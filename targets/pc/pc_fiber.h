/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The one thing the firmware's turns need from the host: a second stack to switch to. Declared here by hand because <windows.h> and the firmware's headers do not mix.
 * One host now, Windows. Another host puts its own switch behind these calls.
 */
#ifndef PC_FIBER_H
#define PC_FIBER_H

#include <stddef.h>
#include <stdint.h>

#define PC_FIBER_CALL __attribute__((stdcall))
typedef void *PcFiber;

#ifndef _WINDOWS_       /* a file that holds <windows.h> has these already */
extern void *__attribute__((stdcall)) CreateFiberEx(size_t, size_t, unsigned long, void (__attribute__((stdcall)) *)(void *), void *);
extern void __attribute__((stdcall)) SwitchToFiber(void *);
extern void *__attribute__((stdcall)) ConvertThreadToFiber(void *);
extern int __attribute__((stdcall)) QueryPerformanceCounter(long long *);
extern int __attribute__((stdcall)) QueryPerformanceFrequency(long long *);
#endif

/* On x86-64 the current fiber sits in the thread block at gs:0x20. Same as what <windows.h> does for GetCurrentFiber. */
static inline void *pc_fiber_current(void)
{
    void *f;
    __asm__ __volatile__("movq %%gs:0x20, %0" : "=r"(f));
    return f;
}

static inline void pc_fiber_main_init(void) { ConvertThreadToFiber(NULL); }

static inline PcFiber pc_fiber_create(size_t commit, size_t reserve, void (PC_FIBER_CALL *start)(void *))
{
    return CreateFiberEx(commit, reserve, 1ul /* keep the floating point state */, start, NULL);
}

static inline void pc_fiber_switch(PcFiber f) { SwitchToFiber(f); }

static inline uint64_t pc_fiber_host_time(void)
{
    long long t = 0;
    QueryPerformanceCounter((void *)&t);
    return (uint64_t)t;
}

#endif /* PC_FIBER_H */
