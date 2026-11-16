/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_rv32.h. One instruction at a time, decoded each time: the brain runs a few thousand instructions a call, about once a frame, so a plain
 * interpreter is far inside a frame's time.
 */
#include <string.h>

#include "pc_rv32.h"

int pc_rv32_map(PcRv32 *cpu, uint32_t base, uint32_t size, uint8_t *mem, int writable, int executable)
{
    PcRv32Region *r;
    if (cpu->regions >= PC_RV32_REGIONS)
        return -1;
    r = &cpu->region[cpu->regions++];
    r->base = base;
    r->size = size;
    r->mem = mem;
    r->writable = writable;
    r->executable = executable;
    return 0;
}

const char *pc_rv32_why(int stop)
{
    switch (stop) {
    case PC_RV32_DONE: return "returned";
    case PC_RV32_FETCH: return "an instruction fetched from outside its code";
    case PC_RV32_LOAD: return "a load from outside its memory";
    case PC_RV32_STORE: return "a store outside its writable memory";
    case PC_RV32_ILLEGAL: return "a word that is not an RV32IM instruction";
    case PC_RV32_MISALIGNED: return "a misaligned access or jump";
    case PC_RV32_TOO_LONG: return "a call that did not return in time";
    default: return "an unknown stop";
    }
}

/* The region holding [a, a + n), or 0. Regions never overlap. */
static PcRv32Region *find(PcRv32 *cpu, uint32_t a, uint32_t n)
{
    int i;
    for (i = 0; i < cpu->regions; i++) {
        PcRv32Region *r = &cpu->region[i];
        if (a - r->base < r->size && r->size - (a - r->base) >= n)
            return r;
    }
    return 0;
}

static inline uint32_t sext(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    v &= (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return (v ^ m) - m;
}

static inline uint32_t imm_i(uint32_t w) { return (uint32_t)((int32_t)w >> 20); }
static inline uint32_t imm_s(uint32_t w) { return (uint32_t)((int32_t)(w & 0xFE000000u) >> 20) | ((w >> 7) & 0x1Fu); }
static inline uint32_t imm_b(uint32_t w)
{
    return (uint32_t)((int32_t)(w & 0x80000000u) >> 19) | ((w & 0x80u) << 4) | ((w >> 20) & 0x7E0u) | ((w >> 7) & 0x1Eu);
}
static inline uint32_t imm_j(uint32_t w)
{
    return (uint32_t)((int32_t)(w & 0x80000000u) >> 11) | (w & 0xFF000u) | ((w >> 9) & 0x800u) | ((w >> 20) & 0x7FEu);     /* bit masks, not a game address */
}

static uint32_t mulh_ss(uint32_t a, uint32_t b) { return (uint32_t)((uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32); }
static uint32_t mulh_su(uint32_t a, uint32_t b) { return (uint32_t)((uint64_t)((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32); }
static uint32_t mulh_uu(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); }

/* Division as RISC-V defines it: by zero gives all ones (or the dividend for the remainder), and the one signed overflow gives the dividend. */
static uint32_t div_s(uint32_t a, uint32_t b)
{
    if (b == 0u) return 0xFFFFFFFFu;
    if (a == 0x80000000u && b == 0xFFFFFFFFu) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static uint32_t rem_s(uint32_t a, uint32_t b)
{
    if (b == 0u) return a;
    if (a == 0x80000000u && b == 0xFFFFFFFFu) return 0u;
    return (uint32_t)((int32_t)a % (int32_t)b);
}
static uint32_t div_u(uint32_t a, uint32_t b) { return b ? a / b : 0xFFFFFFFFu; }
static uint32_t rem_u(uint32_t a, uint32_t b) { return b ? a % b : a; }

#define STOP(why, addr) do { cpu->fault_pc = pc; cpu->fault_addr = (addr); cpu->fault_word = w; cpu->pc = pc; return (why); } while (0)

int pc_rv32_call(PcRv32 *cpu, uint32_t entry, uint32_t ret, uint64_t max_steps)
{
    uint32_t *x = cpu->x;
    uint32_t pc = entry;
    uint64_t left = max_steps;
    PcRv32Region *code = 0;
    x[1] = ret;
    cpu->fault_pc = cpu->fault_addr = cpu->fault_word = 0u;
    for (;;) {
        uint32_t w = 0u, rd, rs1, rs2, a, v, next;
        if (pc == ret) {
            cpu->pc = pc;
            return PC_RV32_DONE;
        }
        if (!left--)
            STOP(PC_RV32_TOO_LONG, pc);
        if (pc & 3u)
            STOP(PC_RV32_MISALIGNED, pc);
        if (!code || pc - code->base >= code->size || code->size - (pc - code->base) < 4u) {
            code = find(cpu, pc, 4u);
            if (!code || !code->executable)
                STOP(PC_RV32_FETCH, pc);
        }
        memcpy(&w, code->mem + (pc - code->base), 4);
        cpu->steps++;
        rd = (w >> 7) & 31u;
        rs1 = (w >> 15) & 31u;
        rs2 = (w >> 20) & 31u;
        next = pc + 4u;
        switch (w & 0x7Fu) {
        case 0x37:                                      /* LUI */
            v = w & 0xFFFFF000u;
            break;
        case 0x17:                                      /* AUIPC */
            v = pc + (w & 0xFFFFF000u);
            break;
        case 0x6F:                                      /* JAL */
            v = next;
            next = pc + imm_j(w);
            if (next & 3u)
                STOP(PC_RV32_MISALIGNED, next);
            break;
        case 0x67:                                      /* JALR */
            if ((w >> 12) & 7u)
                STOP(PC_RV32_ILLEGAL, pc);
            v = next;
            next = (x[rs1] + imm_i(w)) & ~1u;
            if (next & 3u)
                STOP(PC_RV32_MISALIGNED, next);
            break;
        case 0x63: {                                    /* branches */
            uint32_t s = x[rs1], t = x[rs2];
            int take;
            switch ((w >> 12) & 7u) {
            case 0: take = s == t; break;
            case 1: take = s != t; break;
            case 4: take = (int32_t)s < (int32_t)t; break;
            case 5: take = (int32_t)s >= (int32_t)t; break;
            case 6: take = s < t; break;
            case 7: take = s >= t; break;
            default: STOP(PC_RV32_ILLEGAL, pc);
            }
            if (take) {
                next = pc + imm_b(w);
                if (next & 3u)
                    STOP(PC_RV32_MISALIGNED, next);
            }
            pc = next;
            continue;
        }
        case 0x03: {                                    /* loads */
            uint32_t f3 = (w >> 12) & 7u, n = (f3 & 3u) == 0 ? 1u : (f3 & 3u) == 1 ? 2u : 4u;
            PcRv32Region *r;
            if (f3 == 3u || f3 > 5u)
                STOP(PC_RV32_ILLEGAL, pc);
            a = x[rs1] + imm_i(w);
            if (a & (n - 1u))
                STOP(PC_RV32_MISALIGNED, a);
            r = find(cpu, a, n);
            if (!r)
                STOP(PC_RV32_LOAD, a);
            v = 0u;
            memcpy(&v, r->mem + (a - r->base), n);
            if (f3 == 0u) v = sext(v, 8);
            else if (f3 == 1u) v = sext(v, 16);
            break;
        }
        case 0x23: {                                    /* stores */
            uint32_t f3 = (w >> 12) & 7u, n = f3 == 0 ? 1u : f3 == 1 ? 2u : 4u;
            PcRv32Region *r;
            if (f3 > 2u)
                STOP(PC_RV32_ILLEGAL, pc);
            a = x[rs1] + imm_s(w);
            if (a & (n - 1u))
                STOP(PC_RV32_MISALIGNED, a);
            r = find(cpu, a, n);
            if (!r || !r->writable)
                STOP(PC_RV32_STORE, a);
            v = x[rs2];
            memcpy(r->mem + (a - r->base), &v, n);
            pc = next;
            continue;
        }
        case 0x13: {                                    /* operations with an immediate */
            uint32_t s = x[rs1], i = imm_i(w), sh = rs2;
            switch ((w >> 12) & 7u) {
            case 0: v = s + i; break;
            case 2: v = (int32_t)s < (int32_t)i; break;
            case 3: v = s < i; break;
            case 4: v = s ^ i; break;
            case 6: v = s | i; break;
            case 7: v = s & i; break;
            case 1:
                if (w >> 25)
                    STOP(PC_RV32_ILLEGAL, pc);
                v = s << sh;
                break;
            default:
                if ((w >> 25) == 0x00u) v = s >> sh;
                else if ((w >> 25) == 0x20u) v = (uint32_t)((int32_t)s >> sh);
                else STOP(PC_RV32_ILLEGAL, pc);
                break;
            }
            break;
        }
        case 0x33: {                                    /* register operations, and multiply and divide */
            uint32_t s = x[rs1], t = x[rs2], f7 = w >> 25, f3 = (w >> 12) & 7u;
            if (f7 == 0x01u) {
                switch (f3) {
                case 0: v = s * t; break;
                case 1: v = mulh_ss(s, t); break;
                case 2: v = mulh_su(s, t); break;
                case 3: v = mulh_uu(s, t); break;
                case 4: v = div_s(s, t); break;
                case 5: v = div_u(s, t); break;
                case 6: v = rem_s(s, t); break;
                default: v = rem_u(s, t); break;
                }
            } else if (f7 == 0x00u) {
                switch (f3) {
                case 0: v = s + t; break;
                case 1: v = s << (t & 31u); break;
                case 2: v = (int32_t)s < (int32_t)t; break;
                case 3: v = s < t; break;
                case 4: v = s ^ t; break;
                case 5: v = s >> (t & 31u); break;
                case 6: v = s | t; break;
                default: v = s & t; break;
                }
            } else if (f7 == 0x20u && f3 == 0u) {
                v = s - t;
            } else if (f7 == 0x20u && f3 == 5u) {
                v = (uint32_t)((int32_t)s >> (t & 31u));
            } else {
                STOP(PC_RV32_ILLEGAL, pc);
            }
            break;
        }
        case 0x0F:                                      /* FENCE and FENCE.I: one hart, one memory, nothing to order */
            pc = next;
            continue;
        default:
            STOP(PC_RV32_ILLEGAL, pc);
        }
        if (rd)
            x[rd] = v;
        pc = next;
    }
}
