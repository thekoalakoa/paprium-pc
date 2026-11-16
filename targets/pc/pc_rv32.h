/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A small interpreter for the RV32IM instruction set (the 32-bit RISC-V base integer set and its multiply and divide extension), written from the
 * RISC-V unprivileged specification. It runs the CPU MATE brain (pc_brain.h), which the cart's MCU, a NEORV32, runs on the Pocket. Little-endian,
 * as that MCU. No compressed instructions, no CSR or other system instructions, no interrupts: the brain uses none, and such a word stops the run.
 *
 * Memory is a short list of regions. A load or a fetch outside them, a store outside a writable one, a misaligned access, a jump to an address that is
 * not a multiple of 4, or a word that is not RV32IM stops the run with the reason, as an exception would stop the call on the MCU.
 */
#ifndef PC_RV32_H
#define PC_RV32_H

#include <stdint.h>

#define PC_RV32_REGIONS 6

typedef struct PcRv32Region {
    uint32_t base, size;
    uint8_t *mem;           /* the region's bytes in the MCU's byte order */
    int writable, executable;
} PcRv32Region;

enum { PC_RV32_DONE, PC_RV32_FETCH, PC_RV32_LOAD, PC_RV32_STORE, PC_RV32_ILLEGAL, PC_RV32_MISALIGNED, PC_RV32_TOO_LONG };

typedef struct PcRv32 {
    uint32_t x[32];
    uint32_t pc;
    PcRv32Region region[PC_RV32_REGIONS];
    int regions;
    uint64_t steps;         /* instructions run, over every call */
    uint32_t fault_pc, fault_addr, fault_word;
} PcRv32;

/* Adds a region; returns 0, or -1 when the list is full. */
int pc_rv32_map(PcRv32 *cpu, uint32_t base, uint32_t size, uint8_t *mem, int writable, int executable);

/* Calls the routine at entry with ra = ret, and runs until the program counter reaches ret (PC_RV32_DONE) or something stops it (the other
   PC_RV32_* values, with fault_pc, fault_addr and fault_word set). At most max_steps instructions. Every other register is as the caller left it. */
int pc_rv32_call(PcRv32 *cpu, uint32_t entry, uint32_t ret, uint64_t max_steps);

const char *pc_rv32_why(int stop);

#endif /* PC_RV32_H */
