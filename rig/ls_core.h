/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The lockstep core: runs a reference 68000 and the translated program block by block and compares them.
 *
 * The reference goes first through each block. It has the only machine: its device reads get real
 * answers, and those answers wait in a queue. Then the translated block runs. Its device reads pop the
 * queue, so both sides see the same answers by construction, and its writes are compared with the
 * reference's writes, in order. At the next block entry the program counter, every register and the
 * flags that are live there must match.
 *
 * A difference stops the run by default and writes a report. See print_report in ls_core.c.
 */
#ifndef LS_CORE_H
#define LS_CORE_H

#include <stdint.h>

struct PtCtx;

/* The reference CPU's state before its next instruction. */
typedef struct LsCpu {
    uint32_t pc;
    uint32_t d[8];
    uint32_t a[8];
    uint8_t fn, fz, fv, fc;
    uint8_t imask;
    uint8_t fx;                         /* only for the reports: the translated side does not keep X */
} LsCpu;

/* What the lockstep core needs from a reference 68000. Bus events come back through ls_ref_event. */
typedef struct LsRef {
    void (*step)(int instructions);     /* run that many instructions */
    void (*get)(LsCpu *state);
    int (*irq_ready)(void);             /* the level an interrupt would be taken at now, or 0 */
    void (*take_irq)(void);             /* take it now; its bus events arrive through ls_ref_event */
    const uint8_t *(*work_ram)(void);   /* its 64 KB of work RAM, in the same layout as pt_ram, or NULL */
    const uint8_t *(*low_page)(void);   /* its low 64 KB as the 68000 fetches them, same layout, or NULL */
    void (*poke_ram)(uint32_t address, int size, uint32_t value);   /* a route's RAM change, or NULL for no routes */
} LsRef;

enum { LS_READ = 0, LS_WRITE = 1 };

/* Which reads go in the queue. Writes always do. Reads of ROM and work RAM do not, unless strict is set. */
typedef struct LsOptions {
    const char *report_dir;     /* folder for difference reports and run records, or NULL for no files */
    int stop_on_first;          /* stop at the first difference */
    int strict_reads;           /* compare every read, not just device reads */
    uint32_t low_device_end;    /* below this address is device space */
    uint32_t rom_end;
} LsOptions;

void ls_init(const LsRef *ref, const LsOptions *options);

/* From the reference side. */
void ls_ref_event(int kind, int size, uint32_t address, uint32_t value);
/* In natural timing: the reference is taking an interrupt; its bus events follow in the queue. */
void ls_note_irq(int level);

/* From the translated side, through the macros in pt_rt.h. */
void ls_block(struct PtCtx *c, uint32_t pc, int instructions, int live_flags, int safe);
uint32_t ls_read(int size, uint32_t address);
void ls_write(int size, uint32_t address, uint32_t value);
/* Only in a strict reads build: a read of ROM or work RAM, compared in order, and its value too. */
uint32_t ls_read_plain(int size, uint32_t address, uint32_t value);

/* The status register word an interrupt pushes. It holds flags that may be dead at this point, and X,
 * which the translated side does not keep. Only the system byte and the flags live here are compared.
 * Returns the reference's word: the translated side stores that, so the RAM images stay equal. */
uint32_t ls_srframe(uint32_t address, uint32_t value);

/* The bytes of a translated block that lives in the low page must still be what the reference fetches. */
void ls_low_code_check(uint32_t pc, int bytes);
/* The same for a block of RAM code, compared by the caller. */
void ls_ram_code_check(uint32_t pc, int same);

/* From the fallback interpreter: this program address ran here. Used for the run's statistics only. */
void ls_mark_fallback(uint32_t pc);

/* Ask for a compare of the whole work RAM at the next block entry. The driver calls this at each frame
 * end: the reference may be between instructions then, and the next block entry is a clean point. */
void ls_request_ram_check(void);

/* Ask the run to end at the next safe point, after that block's comparisons. A replay stops by counting
 * safe points, so the end must be one. */
void ls_request_stop(void);

/* A route's change to work RAM, on both sides at the same point: the next safe point. Both sides are then
 * between the same two instructions. It goes in the run's record, so a replay makes it too. */
void ls_poke(uint32_t address, int size, uint32_t value);

/* A block after a long run of NOPs has started. Counted per site, with how often an interrupt was taken
 * at that site's safe point: what the wait is for. Written to nop_sites.txt at the end of the run. */
void ls_note_delay(int cycles);

/* How many times the block at this address has run. */
uint32_t ls_block_count(uint32_t pc);

/* After the run. Prints the summary and the RESULT line. Returns 0 for pass, 1 for fail. */
int ls_finish(void);
/* The same, and writes the run's records: final state hash, coverage, replay tail. */
int ls_finish_run(struct PtCtx *c, int stopped);

/* The replay record: the starting state, then device answers and interrupt points as the run goes. */
void ls_record_start(const struct PtCtx *c, uint32_t pc);

enum { LS_DEV_CART, LS_DEV_Z80, LS_DEV_IO, LS_DEV_VIDEO, LS_DEV_OTHER, LS_DEV_KINDS };

typedef struct LsStats {
    uint64_t blocks, instructions, reads, writes, irqs, differences, safe_points;
    uint64_t fallback_instructions;
    uint64_t max_stack;
    uint64_t pokes;
    /* Where the accesses went. Device kinds: see LS_DEV_NAMES in ls_core.c. */
    uint64_t dev_reads[LS_DEV_KINDS], dev_writes[LS_DEV_KINDS];
    uint64_t ram_writes, rom_writes;
    uint64_t plain_ram_reads, plain_rom_reads;     /* counted in a strict reads build only */
    uint64_t irqs_by_level[8];
    char first_kind[32];
} LsStats;
const LsStats *ls_stats(void);

/* Run-time counts of how control moved, for the run's statistics. Only the lockstep build counts. */
typedef struct LsCounters {
    uint64_t guard_hit, guard_miss;     /* constant-loaded indirect calls: the guess was right / wrong */
    uint64_t call_map;                  /* indirect calls and jumps that went through the address map */
    uint64_t switch_hit, switch_miss;   /* switch tables: a known case / the default */
    uint64_t map_hit, map_miss;         /* address map lookups: found / went to the fallback */
    uint64_t unwind;                    /* returns that did not match their caller and reached the dispatcher */
} LsCounters;
extern LsCounters ls_counters;

#endif /* LS_CORE_H */
