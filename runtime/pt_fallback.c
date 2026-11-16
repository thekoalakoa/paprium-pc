/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The fallback: a small 68000 interpreter for program addresses the address map does not know.
 *
 * It runs one instruction at a time until it reaches an address the map knows, then hands that
 * address back to the dispatcher. It covers every instruction the translator lifts, computes every
 * flag except X, and makes its bus accesses in the same order as the generated code. An instruction
 * that reads X, and the rare forms the translator also refuses, stop the run rather than guess.
 *
 * Every use is logged: one line the first time an address is entered, with the address and the running
 * count, to the file named by pt_fallback_log_path. The tool reads that file back as extra discovery
 * seeds, so a miss only happens once per translation. pt_fallback_write_summary writes the counts per
 * address at the end of a run.
 *
 * Under the lockstep rig each interpreted instruction is a block of one, so this code is checked too.
 * In the replay build with pt_interp_only set, the whole program runs here, which gives the speed
 * comparison with the generated code.
 */
#include <stdio.h>
#include <stdlib.h>

#include "pt_rt.h"

u64 pt_fallback_uses;
const char *pt_fallback_log_path;
int pt_interp_only;

#if defined(PT_REPLAY)
u64 pt_replay_icount;
int pt_replay_icount_event(PtCtx *c);
#endif

/* ---- the log */
#define LOG_SLOTS 4096
static struct { u32 pc; u64 uses; } log_table[LOG_SLOTS];
static int log_overflow;

static void pt_fallback_log(u32 pc) {
    u32 h = (pc * 2654435761u) >> 20, i;
    pt_fallback_uses++;
    pc &= PT_BUS_MASK;
    for (i = 0; i < LOG_SLOTS; i++, h = (h + 1) % LOG_SLOTS) {
        if (log_table[h].uses == 0) {
#if defined(PT_PC_BUILD)
            /* The PC program writes nothing beside itself unless a switch names a file. */
            const char *path = pt_fallback_log_path;
            FILE *f = path ? fopen(path, "a") : NULL;
#else
            const char *path = pt_fallback_log_path ? pt_fallback_log_path : "fallback.log";
            FILE *f = fopen(path, "a");
#endif
            log_table[h].pc = pc;
            log_table[h].uses = 1;
            if (f) {
                fprintf(f, "fallback %06X use %llu\n", (unsigned)pc, (unsigned long long)pt_fallback_uses);
                fclose(f);
            }
            return;
        }
        if (log_table[h].pc == pc) {
            log_table[h].uses++;
            return;
        }
    }
    log_overflow = 1;
}

void pt_fallback_write_summary(const char *path) {
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return;
    for (i = 0; i < LOG_SLOTS; i++) {
        if (log_table[i].uses) fprintf(f, "%06X %llu\n", (unsigned)log_table[i].pc, (unsigned long long)log_table[i].uses);
    }
    if (log_overflow) fprintf(f, "table full\n");
    fclose(f);
}

void pt_fallback_write_summary_to(const char *dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/fallback_summary.txt", dir);
    pt_fallback_write_summary(path);
}

/* ---- operands */
typedef struct Fb {
    PtCtx *c;
    u32 pc;         /* the next word to fetch */
    u32 at;         /* the address of this instruction */
    int known;      /* flags computed since the fallback was entered; only these are compared under the rig */
} Fb;

enum { L_D, L_A, L_MEM, L_IMG, L_IMM };
typedef struct Loc {
    int kind;
    u32 a;          /* register number, address or immediate value */
} Loc;

#define C_ (f->c)
static const u32 mask_of[5] = {0, 0xFFu, 0xFFFFu, 0, 0xFFFFFFFFu};

static inline u32 fetch16(Fb *f) {
    u32 v = IMG_R16(f->pc);
    f->pc += 2;
    return v;
}
static inline u32 fetch32(Fb *f) {
    u32 h = fetch16(f);
    return (h << 16) | fetch16(f);
}
static inline u32 rd(int sz, u32 a) {
    return sz == 1 ? R8(a) : sz == 2 ? R16(a) : R32(a);
}
static inline void wr(int sz, u32 a, u32 v) {
    if (sz == 1) W8(a, v);
    else if (sz == 2) W16(a, v);
    else W32(a, v);
}
static inline u32 img_rd(int sz, u32 a) {
    return sz == 1 ? IMG_R8(a) : sz == 2 ? IMG_R16(a) : IMG_R32(a);
}

/* A brief extension word. Bits 8 to 10 are ignored, as on the 68000. */
static u32 ea_index(Fb *f, u32 base) {
    u32 e = fetch16(f);
    u32 x = (e & 0x8000) ? C_->a[(e >> 12) & 7] : C_->d[(e >> 12) & 7];
    if (!(e & 0x800)) x = SX16(x);
    return base + x + SX8(e);
}

static Loc ea(Fb *f, int mode, int reg, int sz) {
    Loc l;
    l.kind = L_MEM;
    switch (mode) {
    case 0: l.kind = L_D; l.a = (u32)reg; break;
    case 1: l.kind = L_A; l.a = (u32)reg; break;
    case 2: l.a = C_->a[reg]; break;
    case 3: l.a = C_->a[reg]; C_->a[reg] += (sz == 1 && reg == 7) ? 2u : (u32)sz; break;
    case 4: C_->a[reg] -= (sz == 1 && reg == 7) ? 2u : (u32)sz; l.a = C_->a[reg]; break;
    case 5: { u32 d = SX16(fetch16(f)); l.a = C_->a[reg] + d; break; }
    case 6: l.a = ea_index(f, C_->a[reg]); break;
    default:
        switch (reg) {
        case 0: l.a = SX16(fetch16(f)); break;
        case 1: l.a = fetch32(f); break;
        case 2: { u32 base = f->pc; l.kind = L_IMG; l.a = base + SX16(fetch16(f)); break; }
        case 3: { u32 base = f->pc; l.kind = L_IMG; l.a = ea_index(f, base); break; }
        default:
            l.kind = L_IMM;
            if (sz == 1) l.a = fetch16(f) & 0xFF;
            else if (sz == 2) l.a = fetch16(f);
            else l.a = fetch32(f);
            break;
        }
        break;
    }
    return l;
}

static u32 lget(Fb *f, Loc l, int sz) {
    switch (l.kind) {
    case L_D: return C_->d[l.a] & mask_of[sz];
    case L_A: return C_->a[l.a] & mask_of[sz];
    case L_MEM: return rd(sz, l.a);
    case L_IMG: return img_rd(sz, l.a);
    default: return l.a;
    }
}

static void lput(Fb *f, Loc l, int sz, u32 v) {
    if (l.kind == L_D) {
        if (sz == 1) C_->d[l.a] = SETLO8(C_->d[l.a], v);
        else if (sz == 2) C_->d[l.a] = SETLO16(C_->d[l.a], v);
        else C_->d[l.a] = v;
    } else if (l.kind == L_A) {
        C_->a[l.a] = v;
    } else {
        wr(sz, l.a, v);
    }
}

/* ---- flags */
static void set_logic(Fb *f, int sz, u32 r) {
    C_->fn = (u8)FL_N(sz * 8, r);
    C_->fz = (u8)FL_Z(sz * 8, r);
    C_->fv = 0;
    C_->fc = 0;
    f->known = 15;
}
static void set_add(Fb *f, int sz, u32 a, u32 b, u32 r) {
    C_->fn = (u8)FL_N(sz * 8, r);
    C_->fz = (u8)FL_Z(sz * 8, r);
    C_->fv = (u8)FL_VADD(sz * 8, a, b, r);
    C_->fc = (u8)FL_CADD(sz * 8, a, b, r);
    f->known = 15;
}
static void set_sub(Fb *f, int sz, u32 a, u32 b, u32 r) {
    C_->fn = (u8)FL_N(sz * 8, r);
    C_->fz = (u8)FL_Z(sz * 8, r);
    C_->fv = (u8)FL_VSUB(sz * 8, a, b, r);
    C_->fc = (u8)FL_CSUB(sz * 8, a, b, r);
    f->known = 15;
}
static u32 get_sr(Fb *f) {
    return 0x2000u | ((u32)C_->imask << 8) | ((u32)C_->fn << 3) | ((u32)C_->fz << 2) | ((u32)C_->fv << 1) | C_->fc;
}
static void set_ccr(Fb *f, u32 v) {
    C_->fn = (u8)((v >> 3) & 1);
    C_->fz = (u8)((v >> 2) & 1);
    C_->fv = (u8)((v >> 1) & 1);
    C_->fc = (u8)(v & 1);
    f->known = 15;
}
static void set_sr(Fb *f, u32 v) {
    set_ccr(f, v);
    C_->imask = (u8)((v >> 8) & 7);
}
static int cond(Fb *f, int cc) {
    u32 fn = C_->fn, fz = C_->fz, fv = C_->fv, fc = C_->fc;
    switch (cc) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !(fc | fz);
    case 3: return fc | fz;
    case 4: return !fc;
    case 5: return fc;
    case 6: return !fz;
    case 7: return fz;
    case 8: return !fv;
    case 9: return fv;
    case 10: return !fn;
    case 11: return fn;
    case 12: return !(fn ^ fv);
    case 13: return fn ^ fv;
    case 14: return !(fz | (fn ^ fv));
    default: return fz | (fn ^ fv);
    }
}

#if !defined(PT_LOCKSTEP) && !defined(PT_REPLAY)
static int pt_fallback_clocks(PtCtx *c, u32 pc);
#endif

#define STEP_ON 0
#define STEP_EXIT 1
#define STOP(why) do { pt_stopped(C_, f->at, why); return STEP_EXIT; } while (0)

static inline void push32(Fb *f, u32 v) {
    C_->a[7] -= 4;
    W32(C_->a[7], v);
}

static void bit_op(Fb *f, int kind, u32 number, int mode, int reg) {
    if (mode == 0) {
        u32 m = 1u << (number & 31), v = C_->d[reg];
        C_->fz = (v & m) == 0;
        if (kind == 1) C_->d[reg] = v ^ m;
        else if (kind == 2) C_->d[reg] = v & ~m;
        else if (kind == 3) C_->d[reg] = v | m;
    } else {
        Loc l = ea(f, mode, reg, 1);
        u32 m = 1u << (number & 7), v = lget(f, l, 1);
        C_->fz = (v & m) == 0;
        if (kind == 1) lput(f, l, 1, v ^ m);
        else if (kind == 2) lput(f, l, 1, v & ~m);
        else if (kind == 3) lput(f, l, 1, v | m);
    }
    f->known |= 4;
}

static int line0(Fb *f, u32 op) {
    int mode = (op >> 3) & 7, reg = op & 7, rr = (op >> 9) & 7, s = (op >> 6) & 3;
    static const int szs[3] = {1, 2, 4};
    if (op & 0x100) {
        if (mode == 1) STOP("movep is not handled");
        bit_op(f, s, C_->d[rr], mode, reg);
        return STEP_ON;
    }
    if (rr == 4) {
        u32 n = fetch16(f) & 0xFF;
        bit_op(f, s, n, mode, reg);
        return STEP_ON;
    }
    if ((op & 0xFF) == 0x3C && (rr == 0 || rr == 1 || rr == 5)) {
        u32 v = fetch16(f) & 0xFF, sr = get_sr(f) & 0xFF;
        set_ccr(f, rr == 0 ? sr | v : rr == 1 ? sr & v : sr ^ v);
        return STEP_ON;
    }
    if ((op & 0xFF) == 0x7C && (rr == 0 || rr == 1 || rr == 5)) {
        u32 v = fetch16(f), sr = get_sr(f);
        set_sr(f, rr == 0 ? sr | v : rr == 1 ? sr & v : sr ^ v);
        return STEP_ON;
    }
    if (s == 3 || rr == 7) STOP("not a 68000 instruction");
    {
        int sz = szs[s];
        u32 imm = sz == 1 ? (fetch16(f) & 0xFF) : sz == 2 ? fetch16(f) : fetch32(f);
        Loc l = ea(f, mode, reg, sz);
        u32 v = lget(f, l, sz), r;
        switch (rr) {
        case 0: r = v | imm; lput(f, l, sz, r); set_logic(f, sz, r); break;
        case 1: r = v & imm; lput(f, l, sz, r); set_logic(f, sz, r); break;
        case 2: r = v - imm; set_sub(f, sz, v, imm, r); lput(f, l, sz, r); break;
        case 3: r = v + imm; set_add(f, sz, v, imm, r); lput(f, l, sz, r); break;
        case 5: r = v ^ imm; lput(f, l, sz, r); set_logic(f, sz, r); break;
        case 6: r = v - imm; set_sub(f, sz, v, imm, r); break;
        default: STOP("not a 68000 instruction");
        }
    }
    return STEP_ON;
}

static int line_move(Fb *f, u32 op) {
    int hi = op >> 12, sz = hi == 1 ? 1 : hi == 3 ? 2 : 4;
    int mode = (op >> 3) & 7, reg = op & 7, rr = (op >> 9) & 7, om = (op >> 6) & 7;
    Loc s = ea(f, mode, reg, sz);
    u32 v = lget(f, s, sz);
    if (om == 1) {
        if (sz == 1) STOP("movea.b");
        C_->a[rr] = sz == 2 ? SX16(v) : v;
        return STEP_ON;
    }
    if (om == 4 && sz == 4) {       /* a long to -(An) writes its low word first, as the reference does */
        Loc d = ea(f, om, rr, 4);
        W16(d.a + 2, v & 0xFFFF);
        W16(d.a, v >> 16);
    } else {
        Loc d = ea(f, om, rr, sz);
        lput(f, d, sz, v);
    }
    set_logic(f, sz, v);
    return STEP_ON;
}

static void movem(Fb *f, u32 op) {
    int mode = (op >> 3) & 7, reg = op & 7, sz = (op & 0x40) ? 4 : 2, i;
    u32 mask = fetch16(f);
    if (op & 0x400) {
        u32 a;
        int img = 0;
        if (mode == 3) {
            a = C_->a[reg];
        } else {
            Loc l = ea(f, mode, reg, sz);
            a = l.a;
            img = l.kind == L_IMG;
        }
        for (i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                u32 v = img ? img_rd(sz, a) : rd(sz, a);
                if (sz == 2) v = SX16(v);
                if (i < 8) C_->d[i] = v;
                else C_->a[i - 8] = v;
                a += (u32)sz;
            }
        }
        if (mode == 3) C_->a[reg] = a;
        (void)R16(a);               /* the 68000's extra read at the end: a bus read in the reference, even after a PC relative list */
    } else if (mode == 4) {
        u32 a = C_->a[reg];
        for (i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                u32 v = i < 8 ? C_->a[7 - i] : C_->d[15 - i];
                a -= (u32)sz;
                if (sz == 4) {
                    W16(a + 2, v & 0xFFFF);
                    W16(a, v >> 16);
                } else {
                    W16(a, v & 0xFFFF);
                }
            }
        }
        C_->a[reg] = a;
    } else {
        Loc l = ea(f, mode, reg, sz);
        u32 a = l.a;
        for (i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                u32 v = i < 8 ? C_->d[i] : C_->a[i - 8];
                if (sz == 4) W32(a, v);
                else W16(a, v & 0xFFFF);
                a += (u32)sz;
            }
        }
    }
}

static int line4(Fb *f, u32 op) {
    int mode = (op >> 3) & 7, reg = op & 7, rr = (op >> 9) & 7, s = (op >> 6) & 3;
    static const int szs[3] = {1, 2, 4};
    switch (op) {
    case 0x4E71: return STEP_ON;
    case 0x4E75: {
        u32 v = R32(C_->a[7]);
        C_->a[7] += 4;
        f->pc = v;
        return STEP_ON;
    }
    case 0x4E73: {
        u32 sr = R16(C_->a[7]), v;
        C_->a[7] += 2;
        v = R32(C_->a[7]);
        C_->a[7] += 4;
        f->pc = v;
        set_sr(f, sr);
        C_->rte_flag = 1;
        return STEP_EXIT;
    }
    case 0x4E70: STOP("reset is not handled");
    case 0x4E72: STOP("stop is not handled");
    case 0x4E76: STOP("trapv is not handled");
    case 0x4E77: STOP("rtr is not handled");
    case 0x4AFC: STOP("illegal");
    default: break;
    }
    if ((op & 0xFFF0) == 0x4E40) STOP("trap is not handled");
    if ((op & 0xFFF8) == 0x4E50) {
        u32 d = SX16(fetch16(f));
        push32(f, C_->a[reg]);      /* link a7 pushes the stack pointer from before the push, as FX68K does */
        C_->a[reg] = C_->a[7];
        C_->a[7] += d;
        return STEP_ON;
    }
    if ((op & 0xFFF8) == 0x4E58) {
        u32 v;
        C_->a[7] = C_->a[reg];
        v = R32(C_->a[7]);
        C_->a[7] += 4;
        C_->a[reg] = v;
        return STEP_ON;
    }
    if ((op & 0xFFF0) == 0x4E60) {
        if (op & 8) C_->a[reg] = C_->usp;
        else C_->usp = C_->a[reg];
        return STEP_ON;
    }
    if ((op & 0xFF80) == 0x4E80) {  /* jsr, jmp */
        Loc l = ea(f, mode, reg, 4);
        if (!(op & 0x40)) push32(f, f->pc);
        f->pc = l.a;
        return STEP_ON;
    }
    if ((op & 0xFFC0) == 0x40C0) {  /* move from SR */
        Loc l = ea(f, mode, reg, 2);
        lput(f, l, 2, get_sr(f));
        return STEP_ON;
    }
    if ((op & 0xFFC0) == 0x44C0 || (op & 0xFFC0) == 0x46C0) {    /* move to CCR, to SR */
        Loc l = ea(f, mode, reg, 2);
        u32 v = lget(f, l, 2);
        if (op & 0x200) set_sr(f, v);
        else set_ccr(f, v & 0xFF);
        return STEP_ON;
    }
    if ((op & 0xFFF8) == 0x4840) {
        C_->d[reg] = ROT16(C_->d[reg]);
        set_logic(f, 4, C_->d[reg]);
        return STEP_ON;
    }
    if ((op & 0xFFF8) == 0x4880) {
        u32 v = SX8(C_->d[reg]);
        C_->d[reg] = SETLO16(C_->d[reg], v);
        set_logic(f, 2, v);
        return STEP_ON;
    }
    if ((op & 0xFFF8) == 0x48C0) {
        C_->d[reg] = SX16(C_->d[reg]);
        set_logic(f, 4, C_->d[reg]);
        return STEP_ON;
    }
    if ((op & 0xFFC0) == 0x4840) {
        Loc l = ea(f, mode, reg, 4);
        push32(f, l.a);
        return STEP_ON;
    }
    if ((op & 0xFFC0) == 0x4800) STOP("nbcd reads X");
    if ((op & 0xFB80) == 0x4880) {
        movem(f, op);
        return STEP_ON;
    }
    if ((op & 0xF1C0) == 0x41C0) {
        Loc l = ea(f, mode, reg, 4);
        C_->a[rr] = l.a;
        return STEP_ON;
    }
    if ((op & 0xF1C0) == 0x4180) STOP("chk is not handled");
    if ((op & 0xFFC0) == 0x4AC0) {  /* tas: on this machine the write half does not reach memory */
        Loc l = ea(f, mode, reg, 1);
        u32 v = lget(f, l, 1);
        set_logic(f, 1, v);
        if (l.kind == L_D) lput(f, l, 1, v | 0x80);
        return STEP_ON;
    }
    if ((op >> 8) == 0x40 && s != 3) STOP("negx reads X");
    if (s == 3) STOP("not a 68000 instruction");
    {
        int sz = szs[s];
        Loc l;
        u32 v, r;
        switch (op >> 8) {
        case 0x42:
            l = ea(f, mode, reg, sz);
            lput(f, l, sz, 0);
            C_->fn = 0; C_->fz = 1; C_->fv = 0; C_->fc = 0;
            f->known = 15;
            return STEP_ON;
        case 0x44:
            l = ea(f, mode, reg, sz);
            v = lget(f, l, sz);
            r = 0u - v;
            set_sub(f, sz, 0, v, r);
            lput(f, l, sz, r);
            return STEP_ON;
        case 0x46:
            l = ea(f, mode, reg, sz);
            r = ~lget(f, l, sz);
            lput(f, l, sz, r);
            set_logic(f, sz, r);
            return STEP_ON;
        case 0x4A:
            l = ea(f, mode, reg, sz);
            set_logic(f, sz, lget(f, l, sz));
            return STEP_ON;
        default:
            break;
        }
    }
    STOP("not a 68000 instruction");
}

static int line5(Fb *f, u32 op) {
    int mode = (op >> 3) & 7, reg = op & 7, rr = (op >> 9) & 7, s = (op >> 6) & 3;
    static const int szs[3] = {1, 2, 4};
    if (s == 3) {
        int cc = (op >> 8) & 15;
        if (mode == 1) {
            u32 base = f->pc, d = SX16(fetch16(f));
            if (!cond(f, cc)) {
                C_->d[reg] = SETLO16(C_->d[reg], C_->d[reg] - 1);
                if ((C_->d[reg] & 0xFFFFu) != 0xFFFFu) f->pc = base + d;
            }
            return STEP_ON;
        }
        {
            Loc l = ea(f, mode, reg, 1);
            lput(f, l, 1, cond(f, cc) ? 0xFFu : 0u);
        }
        return STEP_ON;
    }
    {
        int sz = szs[s];
        u32 q = rr ? (u32)rr : 8u;
        if (mode == 1) {
            if (op & 0x100) C_->a[reg] -= q;
            else C_->a[reg] += q;
            return STEP_ON;
        }
        {
            Loc l = ea(f, mode, reg, sz);
            u32 v = lget(f, l, sz), r;
            if (op & 0x100) {
                r = v - q;
                set_sub(f, sz, v, q, r);
            } else {
                r = v + q;
                set_add(f, sz, v, q, r);
            }
            lput(f, l, sz, r);
        }
    }
    return STEP_ON;
}

static void do_div(Fb *f, int is_signed, int rr, u32 src) {
    u32 fn = C_->fn, fz = C_->fz, fv = C_->fv, fc = C_->fc;
    PtCtx *c = C_;
    u32 r_ = 0;
    if (!(src & 0xFFFF)) {
        pt_stopped(c, f->at, "division by zero");
        return;
    }
    if (is_signed) {
        s32 n_ = (s32)c->d[rr], d_ = (s16)(u16)src;
        if ((u32)n_ == 0x80000000u && d_ == -1) {
            fn = 1; fz = 0; fv = 1; fc = 0;
        } else {
            s32 q_ = n_ / d_;
            if (q_ == (s16)q_) {
                c->d[rr] = ((u32)q_ & 0xFFFFu) | (((u32)(n_ % d_) & 0xFFFFu) << 16);
                fn = FL_N(16, q_); fz = FL_Z(16, q_); fv = 0; fc = 0;
            } else {
                fn = 1; fz = 0; fv = 1; fc = 0;
            }
        }
    } else {
        u32 n_ = c->d[rr], d_ = src & 0xFFFFu;
        if (n_ / d_ < 0x10000u) {
            u32 q_ = n_ / d_;
            c->d[rr] = (q_ & 0xFFFFu) | ((n_ % d_) << 16);
            fn = FL_N(16, q_); fz = FL_Z(16, q_); fv = 0; fc = 0;
        } else {
            fn = 1; fz = 0; fv = 1; fc = 0;
        }
    }
    (void)r_;
    c->fn = (u8)fn; c->fz = (u8)fz; c->fv = (u8)fv; c->fc = (u8)fc;
    f->known = 15;
}

static int line_logic(Fb *f, u32 op, int is_and) {
    int mode = (op >> 3) & 7, reg = op & 7, rr = (op >> 9) & 7, om = (op >> 6) & 7, s = (op >> 6) & 3;
    static const int szs[3] = {1, 2, 4};
    if (om == 3 || om == 7) {
        Loc l = ea(f, mode, reg, 2);
        u32 v = lget(f, l, 2);
        if (is_and) {
            u32 r = om == 3 ? (C_->d[rr] & 0xFFFFu) * v : (u32)((s32)(s16)(u16)C_->d[rr] * (s32)(s16)(u16)v);
            C_->d[rr] = r;
            set_logic(f, 4, r);
        } else {
            do_div(f, om == 7, rr, v);
            if (C_->stopped) return STEP_EXIT;
        }
        return STEP_ON;
    }
    if ((op & 0x1F0) == 0x100) STOP(is_and ? "abcd reads X" : "sbcd reads X");
    if (is_and) {
        int x = op & 0x1F8;
        if (x == 0x140 || x == 0x148 || x == 0x188) {
            u32 *p = x == 0x148 ? &C_->a[rr] : &C_->d[rr];
            u32 *q = x == 0x140 ? &C_->d[reg] : &C_->a[reg];
            u32 t = *p;
            *p = *q;
            *q = t;
            return STEP_ON;
        }
    }
    {
        int sz = szs[s];
        if (om < 3) {
            Loc l = ea(f, mode, reg, sz);
            u32 v = lget(f, l, sz);
            u32 r = is_and ? (C_->d[rr] & v) : (C_->d[rr] | v);
            lput(f, (Loc){L_D, (u32)rr}, sz, r);
            set_logic(f, sz, r);
        } else {
            Loc l = ea(f, mode, reg, sz);
            u32 v = lget(f, l, sz);
            u32 r = is_and ? (C_->d[rr] & v) : (C_->d[rr] | v);
            lput(f, l, sz, r);
            set_logic(f, sz, r);
        }
    }
    return STEP_ON;
}

static int line_arith(Fb *f, u32 op, int is_add) {
    int mode = (op >> 3) & 7, reg = op & 7, rr = (op >> 9) & 7, om = (op >> 6) & 7, s = (op >> 6) & 3;
    static const int szs[3] = {1, 2, 4};
    if (om == 3 || om == 7) {
        int sz = om == 3 ? 2 : 4;
        Loc l = ea(f, mode, reg, sz);
        u32 v = lget(f, l, sz);
        if (sz == 2) v = SX16(v);
        if (is_add) C_->a[rr] += v;
        else C_->a[rr] -= v;
        return STEP_ON;
    }
    {
        int sz = szs[s];
        if (om < 3) {
            Loc l = ea(f, mode, reg, sz);
            u32 b = lget(f, l, sz), a = C_->d[rr] & mask_of[sz], r = is_add ? a + b : a - b;
            if (is_add) set_add(f, sz, a, b, r);
            else set_sub(f, sz, a, b, r);
            lput(f, (Loc){L_D, (u32)rr}, sz, r);
            return STEP_ON;
        }
        if (mode == 0 || mode == 1) STOP(is_add ? "addx reads X" : "subx reads X");
        {
            Loc l = ea(f, mode, reg, sz);
            u32 a = lget(f, l, sz), b = C_->d[rr] & mask_of[sz], r = is_add ? a + b : a - b;
            if (is_add) set_add(f, sz, a, b, r);
            else set_sub(f, sz, a, b, r);
            lput(f, l, sz, r);
        }
    }
    return STEP_ON;
}

static int lineB(Fb *f, u32 op) {
    int mode = (op >> 3) & 7, reg = op & 7, rr = (op >> 9) & 7, om = (op >> 6) & 7, s = (op >> 6) & 3;
    static const int szs[3] = {1, 2, 4};
    if (om == 3 || om == 7) {
        int sz = om == 3 ? 2 : 4;
        Loc l = ea(f, mode, reg, sz);
        u32 b = lget(f, l, sz), a = C_->a[rr], r;
        if (sz == 2) b = SX16(b);
        r = a - b;
        set_sub(f, 4, a, b, r);
        return STEP_ON;
    }
    {
        int sz = szs[s];
        if (om < 3) {
            Loc l = ea(f, mode, reg, sz);
            u32 b = lget(f, l, sz), a = C_->d[rr] & mask_of[sz];
            set_sub(f, sz, a, b, a - b);
            return STEP_ON;
        }
        if (mode == 1) {        /* cmpm */
            Loc x = ea(f, 3, reg, sz);
            u32 b = lget(f, x, sz);
            Loc y = ea(f, 3, rr, sz);
            u32 a = lget(f, y, sz);
            set_sub(f, sz, a, b, a - b);
            return STEP_ON;
        }
        {
            Loc l = ea(f, mode, reg, sz);
            u32 r = lget(f, l, sz) ^ (C_->d[rr] & mask_of[sz]);
            lput(f, l, sz, r);
            set_logic(f, sz, r);
        }
    }
    return STEP_ON;
}

static u32 do_shift(int kind, int left, int bits, u32 v, u32 n, u32 *fc, u32 *fv) {
    switch (kind) {
    case 0: return left ? pt_asl(bits, v, n, fc, fv) : pt_asr(bits, v, n, fc, fv);
    case 1: return left ? pt_lsl(bits, v, n, fc, fv) : pt_lsr(bits, v, n, fc, fv);
    default: return left ? pt_rol(bits, v, n, fc, fv) : pt_ror(bits, v, n, fc, fv);
    }
}

static int lineE(Fb *f, u32 op) {
    int mode = (op >> 3) & 7, reg = op & 7, rr = (op >> 9) & 7, s = (op >> 6) & 3, left = (op & 0x100) != 0;
    static const int szs[3] = {1, 2, 4};
    u32 fc = 0, fv = 0, r;
    if (s == 3) {
        Loc l;
        if (op & 0x800) STOP("not a 68000 instruction");
        if (rr == 2) STOP("roxl and roxr read X");
        l = ea(f, mode, reg, 2);
        r = do_shift(rr == 3 ? 2 : rr, left, 16, lget(f, l, 2), 1, &fc, &fv);
        lput(f, l, 2, r);
        set_logic(f, 2, r);
        C_->fc = (u8)fc;
        C_->fv = (u8)fv;
        return STEP_ON;
    }
    {
        int sz = szs[s], kind = (op >> 3) & 3;
        u32 n = (op & 0x20) ? (C_->d[rr] & 63) : (rr ? (u32)rr : 8u);
        if (kind == 2) STOP("roxl and roxr read X");
        r = do_shift(kind == 3 ? 2 : kind, left, sz * 8, C_->d[reg] & mask_of[sz], n, &fc, &fv);
        lput(f, (Loc){L_D, (u32)reg}, sz, r);
        set_logic(f, sz, r);
        C_->fc = (u8)fc;
        C_->fv = (u8)fv;
    }
    return STEP_ON;
}

/* One instruction. On STEP_ON, f->pc is the next instruction. On STEP_EXIT, control leaves the fallback. */
static int step(Fb *f) {
    u32 op;
    if ((f->pc & PT_BUS_MASK) >= pt_rom_size && (f->pc & PT_BUS_MASK) < PT_RAM_LOW) {
        pt_stopped(C_, f->pc & PT_BUS_MASK, "the program counter left the program");
        return STEP_EXIT;
    }
    f->at = f->pc;
    op = fetch16(f);
    switch (op >> 12) {
    case 0: return line0(f, op);
    case 1: case 2: case 3: return line_move(f, op);
    case 4: return line4(f, op);
    case 5: return line5(f, op);
    case 6: {
        int c6 = (op >> 8) & 15;
        u32 base = f->pc, d = op & 0xFF;
        if (d == 0xFF) STOP("not a 68000 instruction");
        d = d == 0 ? SX16(fetch16(f)) : SX8(d);
        if (c6 == 1) {
            push32(f, f->pc);
            f->pc = base + d;
        } else if (c6 == 0 || cond(f, c6)) {
            f->pc = base + d;
        }
        return STEP_ON;
    }
    case 7: {
        u32 v = SX8(op);
        if (op & 0x100) STOP("not a 68000 instruction");
        C_->d[(op >> 9) & 7] = v;
        set_logic(f, 4, v);
        return STEP_ON;
    }
    case 8: return line_logic(f, op, 0);
    case 9: return line_arith(f, op, 0);
    case 0xB: return lineB(f, op);
    case 0xC: return line_logic(f, op, 1);
    case 0xD: return line_arith(f, op, 1);
    case 0xE: return lineE(f, op);
    default: STOP("line A or line F");
    }
}

/* ---- time, in the speed build: the same times as tool/cycles68k.py for what a block charges at its start, which tests/test_all.py checks form
 * by form. Here the run is already known, so a branch and a shift are charged what they really take. A multiply or a divide is charged its fixed part only:
 * this interpreter is rare, and nothing about it is timed that closely. */
#if !defined(PT_LOCKSTEP) && !defined(PT_REPLAY)
static int ea_class(int mode, int reg) {
    return mode < 7 ? mode : 7 + reg;   /* d, a, ind, post, pre, disp, idx, absw, absl, pcd, pci, imm */
}
static int ea_time(int cls, int sz) {
    static const u8 bw[12] = {0, 0, 4, 4, 6, 8, 10, 8, 12, 8, 10, 4};
    static const u8 lg[12] = {0, 0, 8, 8, 10, 12, 14, 12, 16, 12, 14, 8};
    return cls < 12 ? (sz == 4 ? lg[cls] : bw[cls]) : 0;
}
#define EA(sz) ea_time(ea_class(mode, reg), (sz))

/* The time of one instruction before the run decides anything: op and the word after it. */
int pt_fallback_static_clocks(u32 op, u32 next) {
    int mode = (op >> 3) & 7, reg = op & 7, rr = (op >> 9) & 7, om = (op >> 6) & 7, s = (op >> 6) & 3;
    int sz = s == 0 ? 1 : s == 1 ? 2 : 4, ismem = !(mode == 0 || mode == 1 || (mode == 7 && reg == 4));
    int count;
    switch (op >> 12) {
    case 0:
        if (op & 0x100) {
            if (mode == 1) return s & 1 ? 24 : 16;
            if (mode == 0) return s == 0 ? 6 : s == 2 ? 8 : 6;
            return (s == 0 ? 4 : 8) + EA(1);
        }
        if (rr == 4) {
            if (mode == 0) return s == 0 ? 10 : (s == 2 ? 12 : 10) + ((next & 0x1F) >= 16 ? 2 : 0);
            return (s == 0 ? 8 : 12) + EA(1);
        }
        if ((op & 0xBF) == 0x3C) return 20;
        if (mode == 0) {
            if (sz != 4) return 8;
            return rr == 6 ? 14 : 16;
        }
        return rr == 6 ? (sz == 4 ? 12 : 8) + EA(sz) : (sz == 4 ? 20 : 12) + EA(sz);
    case 1: case 2: case 3: {
        int msz = (op >> 12) == 1 ? 1 : (op >> 12) == 3 ? 2 : 4;
        int dst = ea_class(om, rr);
        static const u8 dbw[9] = {0, 0, 4, 4, 4, 8, 10, 8, 12};
        static const u8 dl[9] = {0, 0, 8, 8, 8, 12, 14, 12, 16};
        return 4 + EA(msz) + (dst < 9 ? (msz == 4 ? dl[dst] : dbw[dst]) : 0);
    }
    case 4:
        switch (op) {
        case 0x4E71: return 4;
        case 0x4E72: return 4;
        case 0x4E75: return 16;
        case 0x4E73: case 0x4E77: return 20;
        case 0x4AFC: return 34;
        default: break;
        }
        if ((op & 0xFFF0) == 0x4E40) return 34;
        if ((op & 0xFFF8) == 0x4E50) return 16;
        if ((op & 0xFFF8) == 0x4E58) return 12;
        if ((op & 0xFFF0) == 0x4E60) return 4;
        if ((op & 0xFF80) == 0x4E80) {
            static const u8 jmp[11] = {0, 0, 8, 0, 0, 10, 14, 10, 12, 10, 14};
            static const u8 jsr[11] = {0, 0, 16, 0, 0, 18, 22, 18, 20, 18, 22};
            int cls = ea_class(mode, reg);
            return cls < 11 ? ((op & 0x40) ? jmp[cls] : jsr[cls]) : 0;
        }
        if ((op & 0xFFC0) == 0x40C0) return mode == 0 ? 6 : 8 + EA(2);
        if ((op & 0xFFC0) == 0x44C0 || (op & 0xFFC0) == 0x46C0) return 12 + EA(2);
        if ((op & 0xFFF8) == 0x4840 || (op & 0xFFF8) == 0x4880 || (op & 0xFFF8) == 0x48C0) return 4;
        if ((op & 0xFFC0) == 0x4840) {
            static const u8 pea[11] = {0, 0, 12, 0, 0, 16, 20, 16, 20, 16, 20};
            int cls = ea_class(mode, reg);
            return cls < 11 ? pea[cls] : 0;
        }
        if ((op & 0xFB80) == 0x4880) {
            int each = ((op & 0x40) ? 8 : 4) * __builtin_popcount(next & 0xFFFF), cls = ea_class(mode, reg);
            if (op & 0x400) {
                static const u8 ld[11] = {0, 0, 12, 12, 0, 16, 18, 16, 20, 16, 18};
                return each + (cls < 11 ? ld[cls] : 0);
            } else {
                static const u8 st[9] = {0, 0, 8, 0, 8, 12, 14, 12, 16};
                return each + (cls < 9 ? st[cls] : 0);
            }
        }
        if ((op & 0xF1C0) == 0x41C0) {
            static const u8 lea[11] = {0, 0, 4, 0, 0, 8, 12, 8, 12, 8, 12};
            int cls = ea_class(mode, reg);
            return cls < 11 ? lea[cls] : 0;
        }
        if ((op & 0xFFC0) == 0x4AC0) return mode == 0 ? 4 : 10 + EA(1);
        if ((op >> 8) == 0x4A) return 4 + EA(sz);
        return mode == 0 ? (sz == 4 ? 6 : 4) : (sz == 4 ? 12 : 8) + EA(sz);     /* clr, neg, not */
    case 5:
        if (s == 3) {
            if (mode == 1) return 10;
            return mode == 0 ? 4 : 8 + EA(1);
        }
        if (mode == 0) return sz == 4 ? 8 : 4;
        if (mode == 1) return 8;
        return (sz == 4 ? 12 : 8) + EA(sz);
    case 6: {
        int c6 = (op >> 8) & 15;
        if (c6 == 0) return 10;
        if (c6 == 1) return 18;
        return (op & 0xFF) ? 8 : 12;
    }
    case 7:
        return 4;
    case 0xC:
        if ((op & 0x1F8) == 0x140 || (op & 0x1F8) == 0x148 || (op & 0x1F8) == 0x188) return 6;
        /* fall through */
    case 8:
        if (om == 3 || om == 7) return ((op >> 12) == 0xC ? 38 : 0) + EA(2);
        if (om < 3) return sz == 4 ? (ismem ? 6 : 8) + EA(4) : 4 + EA(sz);
        return (sz == 4 ? 12 : 8) + EA(sz);
    case 9: case 0xD:
        if (om == 3) return 8 + EA(2);
        if (om == 7) return (ismem ? 6 : 8) + EA(4);
        if (om < 3) return sz == 4 ? (ismem ? 6 : 8) + EA(4) : 4 + EA(sz);
        return (sz == 4 ? 12 : 8) + EA(sz);
    case 0xB:
        if (om == 3 || om == 7) return 6 + EA(om == 3 ? 2 : 4);
        if (om < 3) return (sz == 4 ? 6 : 4) + EA(sz);
        if (mode == 1) return sz == 4 ? 20 : 12;
        if (mode == 0) return sz == 4 ? 8 : 4;
        return (sz == 4 ? 12 : 8) + EA(sz);
    case 0xE:
        if (s == 3) return 8 + EA(2);
        count = (op & 0x20) ? 0 : (rr ? rr : 8);
        return (sz == 4 ? 8 : 6) + 2 * count;
    default:
        return 4;
    }
}

static int pt_fallback_clocks(PtCtx *c, u32 pc) {
    u32 op, a = pc & PT_BUS_MASK;
    int v;
    if (a >= pt_rom_size && a < PT_RAM_LOW) return 0;   /* the step that follows stops the run */
    op = IMG_R16(a);
    v = pt_fallback_static_clocks(op, IMG_R16(a + 2));
    if ((op >> 12) == 6 && ((op >> 8) & 15) > 1) {
        Fb probe;
        probe.c = c;
        if (cond(&probe, (op >> 8) & 15)) v = 10;
    } else if ((op >> 12) == 0xE && ((op >> 6) & 3) != 3 && (op & 0x20)) {
        v += 2 * (int)(c->d[(op >> 9) & 7] & 63);
    }
    return v;
}
#endif

#if defined(PT_PC_BUILD)
#define PT_LOW_STAYS(pc) (pt_low_from_ram && ((pc) & PT_BUS_MASK) < pt_low_device_end)
#else
#define PT_LOW_STAYS(pc) 0
#endif

/* Exactly one instruction at pc, with no log and no interrupt check: for the form fuzz, which checks this
 * interpreter one instruction at a time. Returns the next address. */
u32 pt_fallback_one(PtCtx *c, u32 pc) {
    Fb fb;
    Fb *f = &fb;
    f->c = c;
    f->pc = pc;
    f->known = 0;
#if defined(PT_LOCKSTEP)
    ls_block(c, pc & PT_BUS_MASK, 1, 0, 1);
#endif
    if (step(f) != STEP_ON && c->stopped) return 0xFFFFFFFFu;
    return f->pc;
}

u32 pt_fallback(PtCtx *c, u32 pc) {
    Fb fb;
    Fb *f = &fb;
    int first = 1;
    if (!pt_interp_only) pt_fallback_log(pc);
    f->c = c;
    f->pc = pc;
    f->known = 0;
    for (;;) {
        u32 here = f->pc;
        if (here == pt_run_exit_pc) return here;
        if (!first && !pt_interp_only && pt_map_has(here) && !PT_LOW_STAYS(here)) return here;
        first = 0;
#if defined(PT_LOCKSTEP)
        for (;;) {
            u32 r;
            ls_block(c, here & PT_BUS_MASK, 1, f->known, 1);
            if (!(pt_irq_level > c->imask)) break;
            r = pt_take_irq(c, here);
            if (r != here) return r;
            f->known = 15;
        }
        ls_mark_fallback(here);
#elif defined(PT_REPLAY)
        if (pt_interp_only) {
            while (pt_replay_icount_event(c)) {
                u32 r = pt_take_irq(c, here);
                if (r != here) return r;
            }
            pt_replay_icount++;
        } else {
            while (--pt_replay_left == 0 && pt_replay_event(c)) {
                u32 r = pt_take_irq(c, here);
                if (r != here) return r;
            }
        }
#else
        while (pt_irq_level > c->imask) {
            u32 r = pt_take_irq(c, here);
            if (r != here) return r;
        }
        /* Each instruction is charged as it runs, so a handler that falls in here must not read what is left of a translated block. */
        pt_at = 0;
        pt_clock += pt_fallback_clocks(c, here);
        if (pt_clock >= pt_deadline && pt_port_tick()) return pt_halted(c);
#endif
        if (step(f) != STEP_ON) {
            return c->stopped ? 0xFFFFFFFFu : f->pc;
        }
    }
}
