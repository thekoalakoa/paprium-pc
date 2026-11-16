/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Condition flags as the generated code computes them, and the status register. In generated
 * functions the flags are the local variables fn, fz, fv and fc, and only the ones something can
 * read get computed. sz is the operand size in bits. a, b and r are 32-bit values; the bits above sz
 * do not matter.
 */
#ifndef PT_FLAGS_H
#define PT_FLAGS_H

#define PT_MASK(sz) ((sz) == 32 ? 0xFFFFFFFFu : ((1u << (sz)) - 1u))
#define FL_N(sz, r) (((u32)(r) >> ((sz) - 1)) & 1u)
#define FL_Z(sz, r) (((u32)(r) & PT_MASK(sz)) == 0)
#define FL_CADD(sz, a, b, r) ((((u32)(a) & (u32)(b)) | (((u32)(a) | (u32)(b)) & ~(u32)(r))) >> ((sz) - 1) & 1u)
#define FL_VADD(sz, a, b, r) ((((u32)(a) ^ (u32)(r)) & ((u32)(b) ^ (u32)(r))) >> ((sz) - 1) & 1u)
#define FL_CSUB(sz, a, b, r) (((~(u32)(a) & (u32)(b)) | ((~(u32)(a) | (u32)(b)) & (u32)(r))) >> ((sz) - 1) & 1u)
#define FL_VSUB(sz, a, b, r) ((((u32)(a) ^ (u32)(b)) & ((u32)(a) ^ (u32)(r))) >> ((sz) - 1) & 1u)

/* The status register as the program sees it: supervisor bit set, X reads as zero. */
#define PT_SR_READ() (0x2000u | ((u32)c->imask << 8) | (fn << 3) | (fz << 2) | (fv << 1) | fc)
#define PT_CCR_WRITE(v) do { u32 v_ = (v); fn = (v_ >> 3) & 1u; fz = (v_ >> 2) & 1u; fv = (v_ >> 1) & 1u; fc = v_ & 1u; } while (0)
#define PT_SR_WRITE(v) do { u32 s_ = (v); PT_CCR_WRITE(s_); c->imask = (u8)((s_ >> 8) & 7u); } while (0)

/* Shifts and rotates. They return the result and set C and V; the count is taken modulo 64. */
static inline u32 pt_lsl(int sz, u32 v, u32 n, u32 *fc, u32 *fv) {
    u32 m = PT_MASK(sz);
    n &= 63; v &= m; *fv = 0;
    if (n == 0) { *fc = 0; return v; }
    *fc = n <= (u32)sz ? (v >> (sz - n)) & 1u : 0;
    return n < (u32)sz ? (v << n) & m : 0;
}
static inline u32 pt_lsr(int sz, u32 v, u32 n, u32 *fc, u32 *fv) {
    u32 m = PT_MASK(sz);
    n &= 63; v &= m; *fv = 0;
    if (n == 0) { *fc = 0; return v; }
    *fc = n <= (u32)sz ? (v >> (n - 1)) & 1u : 0;
    return n < (u32)sz ? v >> n : 0;
}
static inline u32 pt_asl(int sz, u32 v, u32 n, u32 *fc, u32 *fv) {
    u32 m = PT_MASK(sz);
    n &= 63; v &= m;
    if (n == 0) { *fc = 0; *fv = 0; return v; }
    if (n >= (u32)sz) *fv = v != 0;
    else {
        u32 top = (u32)((((u64)1 << (n + 1)) - 1) << (sz - n - 1)) & m;
        u32 x = v & top;
        *fv = !(x == 0 || x == top);
    }
    *fc = n <= (u32)sz ? (v >> (sz - n)) & 1u : 0;
    return n < (u32)sz ? (v << n) & m : 0;
}
static inline u32 pt_asr(int sz, u32 v, u32 n, u32 *fc, u32 *fv) {
    u32 m = PT_MASK(sz);
    u32 sign = (v >> (sz - 1)) & 1u;
    n &= 63; v &= m; *fv = 0;
    if (n == 0) { *fc = 0; return v; }
    if (n >= (u32)sz) { *fc = sign; return sign ? m : 0; }
    *fc = (v >> (n - 1)) & 1u;
    return ((v >> n) | (sign ? (m << (sz - n)) : 0)) & m;
}
static inline u32 pt_rol(int sz, u32 v, u32 n, u32 *fc, u32 *fv) {
    u32 m = PT_MASK(sz), k, r;
    n &= 63; v &= m; *fv = 0;
    if (n == 0) { *fc = 0; return v; }
    k = n % (u32)sz;
    r = k ? ((v << k) | (v >> (sz - k))) & m : v;
    *fc = r & 1u;
    return r;
}
static inline u32 pt_ror(int sz, u32 v, u32 n, u32 *fc, u32 *fv) {
    u32 m = PT_MASK(sz), k, r;
    n &= 63; v &= m; *fv = 0;
    if (n == 0) { *fc = 0; return v; }
    k = n % (u32)sz;
    r = k ? ((v >> k) | (v << (sz - k))) & m : v;
    *fc = (r >> (sz - 1)) & 1u;
    return r;
}

/* Division. On overflow the destination stays as it was, N and V are set, Z and C are clear. That is
 * what FX68K, a cycle exact 68000, does on every overflow, the most negative dividend over -1 included.
 * Genesis Plus GX's 68000 core differs in two ways, and the form fuzz allows for both by name: it leaves
 * Z alone on an overflow, and it turns the most negative dividend over -1 into a result of zero.
 * Division by zero would trap; the program has no handler, so it stops here and the report shows where. */
#define PT_DIVU(reg, src, pc) do { \
    u32 n_ = (reg), d_ = (src) & 0xFFFFu; \
    if (d_ == 0) return pt_stopped(c, (pc), "division by zero"); \
    if (n_ / d_ < 0x10000u) { \
        u32 q_ = n_ / d_; \
        (reg) = (q_ & 0xFFFFu) | ((n_ % d_) << 16); \
        fn = FL_N(16, q_); fz = FL_Z(16, q_); fv = 0; fc = 0; \
    } else { fn = 1; fz = 0; fv = 1; fc = 0; } \
} while (0)

#define PT_DIVS(reg, src, pc) do { \
    s32 n_ = (s32)(reg), d_ = (s16)(u16)(src); \
    if (d_ == 0) return pt_stopped(c, (pc), "division by zero"); \
    if ((u32)n_ == 0x80000000u && d_ == -1) { fn = 1; fz = 0; fv = 1; fc = 0; } \
    else { \
        s32 q_ = n_ / d_; \
        if (q_ == (s16)q_) { \
            (reg) = ((u32)q_ & 0xFFFFu) | (((u32)(n_ % d_) & 0xFFFFu) << 16); \
            fn = FL_N(16, q_); fz = FL_Z(16, q_); fv = 0; fc = 0; \
        } else { fn = 1; fz = 0; fv = 1; fc = 0; } \
    } \
} while (0)

#endif /* PT_FLAGS_H */
