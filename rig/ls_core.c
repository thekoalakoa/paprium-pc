/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_rt.h"

#define QUEUE_MAX 4096
#define TRAIL_MAX 64
#define MARKS_MAX 8

typedef struct Event {
    uint8_t kind, size;
    uint32_t address, value;
} Event;

static const LsRef *ref;
static LsOptions opt;
static Event queue[QUEUE_MAX];
static int queue_n, queue_head;
static uint32_t trail[TRAIL_MAX];
static uint64_t trail_n;
static uint32_t current_block;
static int current_live;
static int ram_check_due, stop_due;
static LsStats stats;
LsCounters ls_counters;
static PtCtx *last_ctx;
static char *stack_top;

/* In natural timing: where in the queue each interrupt the reference took begins. */
static struct { int pos, level; } marks[MARKS_MAX];
static int marks_n;

/* Coverage, one bit per even address over the 24-bit bus. */
static uint8_t *visited_t, *visited_f;
/* How often each block ran, one count per even address. */
static uint32_t *block_counts;

/* The replay record: device answers in order, interrupt points, a route's RAM changes. */
static FILE *replay_reads, *replay_irqs, *replay_pokes;

#define POKES_MAX 1024
static struct { uint32_t address, value; int size; } pokes[POKES_MAX];
static int pokes_n;

static const char *const LS_DEV_NAMES[LS_DEV_KINDS] = {"cart", "z80", "io", "video", "other"};

#define DELAY_SITES_MAX 128
static struct { uint32_t pc; int cycles; uint64_t hits, with_irq; } delay_sites[DELAY_SITES_MAX];
static int delay_sites_n;
static uint32_t irq_taken_at = 0xFFFFFFFFu;

static int device_kind(uint32_t a) {
    if (a < 0x10000u) return LS_DEV_CART;
    if (a >= 0xA00000u && a < 0xA10000u) return LS_DEV_Z80;
    if (a >= 0xA10000u && a < 0xA20000u) return LS_DEV_IO;
    if (a >= 0xC00000u && a < 0xC00020u) return LS_DEV_VIDEO;
    return LS_DEV_OTHER;
}

const LsStats *ls_stats(void) {
    return &stats;
}

void ls_init(const LsRef *r, const LsOptions *o) {
    ref = r;
    opt = *o;
    queue_n = queue_head = 0;
    trail_n = 0;
    current_live = 0;
    ram_check_due = stop_due = 0;
    pokes_n = 0;
    marks_n = 0;
    memset(&stats, 0, sizeof(stats));
    memset(&ls_counters, 0, sizeof(ls_counters));
    stack_top = NULL;
    last_ctx = NULL;
    if (!visited_t) {
        visited_t = (uint8_t *)calloc(1, 0x100000);
        visited_f = (uint8_t *)calloc(1, 0x100000);
        block_counts = (uint32_t *)calloc(0x800000, 4);
    }
}

static void dump_state(FILE *f) {
    LsCpu s;
    int i;
    if (!ref || !ref->get || !last_ctx) return;
    ref->get(&s);
    fprintf(f, "reference pc %08X flags NZVC %d%d%d%d x %d imask %d\n", (unsigned)s.pc, s.fn, s.fz, s.fv, s.fc, s.fx, s.imask);
    for (i = 0; i < 8; i++) fprintf(f, "  d%d %08X   a%d %08X\n", i, (unsigned)s.d[i], i, (unsigned)s.a[i]);
    fprintf(f, "translated flags NZVC %d%d%d%d imask %d\n", last_ctx->fn, last_ctx->fz, last_ctx->fv, last_ctx->fc, last_ctx->imask);
    for (i = 0; i < 8; i++) fprintf(f, "  d%d %08X   a%d %08X\n", i, (unsigned)last_ctx->d[i], i, (unsigned)last_ctx->a[i]);
}

/* The report: what differed, where, and the blocks that led there. The tool adds the 68000 text and
 * the generated C lines afterwards, from the block addresses here, and keeps it in the output folder. */
static void print_report(const char *kind, const char *detail) {
    FILE *f = NULL;
    uint64_t i, from;
    stats.differences++;
    if (stats.differences == 1) {
        strncpy(stats.first_kind, kind, sizeof(stats.first_kind) - 1);
    }
    printf("DIFFERENCE %llu: %s at block %06X after %llu blocks, %llu instructions: %s\n",
           (unsigned long long)stats.differences, kind, (unsigned)current_block,
           (unsigned long long)stats.blocks, (unsigned long long)stats.instructions, detail);
    if (opt.report_dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/diff_%04llu.txt", opt.report_dir, (unsigned long long)stats.differences);
        f = fopen(path, "w");
    }
    if (f) {
        fprintf(f, "kind %s\nblock %06X\nblocks %llu\ninstructions %llu\nwrites %llu\nreads %llu\nirqs %llu\ndetail %s\n",
                kind, (unsigned)current_block, (unsigned long long)stats.blocks,
                (unsigned long long)stats.instructions, (unsigned long long)stats.writes,
                (unsigned long long)stats.reads, (unsigned long long)stats.irqs, detail);
        dump_state(f);
        fprintf(f, "trail, oldest first:\n");
        from = trail_n > TRAIL_MAX ? trail_n - TRAIL_MAX : 0;
        for (i = from; i < trail_n; i++) fprintf(f, "  %06X\n", (unsigned)trail[i % TRAIL_MAX]);
        fprintf(f, "queue left, reference side:\n");
        for (i = (uint64_t)queue_head; i < (uint64_t)queue_n; i++) {
            fprintf(f, "  %s size %d address %06X value %08X\n", queue[i].kind == LS_WRITE ? "write" : "read",
                    queue[i].size, (unsigned)queue[i].address, (unsigned)queue[i].value);
        }
        fclose(f);
    }
    if (opt.stop_on_first) {
        fflush(stdout);
        ls_finish();
        fflush(stdout);
        exit(1);
    }
}

static int is_plain_memory(uint32_t address) {
    return address >= PT_RAM_LOW || (address >= opt.low_device_end && address < opt.rom_end);
}

void ls_ref_event(int kind, int size, uint32_t address, uint32_t value) {
    address &= PT_BUS_MASK;
    if (kind == LS_READ && !opt.strict_reads && is_plain_memory(address)) return;
    if (queue_n >= QUEUE_MAX) {
        print_report("queue_overflow", "the reference made too many accesses in one block");
        queue_n = queue_head = 0;
        return;
    }
    if (size == 1) value &= 0xFFu;
    else if (size == 2) value &= 0xFFFFu;
    queue[queue_n].kind = (uint8_t)kind;
    queue[queue_n].size = (uint8_t)size;
    queue[queue_n].address = address;
    queue[queue_n].value = value;
    queue_n++;
}

void ls_note_irq(int level) {
    if (marks_n >= MARKS_MAX) {
        print_report("queue_overflow", "too many interrupts between two blocks");
        return;
    }
    marks[marks_n].pos = queue_n;
    marks[marks_n].level = level;
    marks_n++;
}

static const Event *pop(int kind, int size, uint32_t address, const char *name) {
    char text[160];
    const Event *e;
    if (queue_head >= queue_n) {
        snprintf(text, sizeof(text), "translated %s size %d address %06X, the reference made no more accesses",
                 name, size, (unsigned)address);
        print_report("extra_access", text);
        return NULL;
    }
    e = &queue[queue_head++];
    if (e->kind != kind || e->size != size || e->address != address) {
        snprintf(text, sizeof(text), "translated %s size %d address %06X, reference %s size %d address %06X", name,
                 size, (unsigned)address, e->kind == LS_WRITE ? "write" : "read", e->size, (unsigned)e->address);
        print_report("access_order", text);
        return NULL;
    }
    return e;
}

uint32_t ls_read(int size, uint32_t address) {
    const Event *e = pop(LS_READ, size, address, "read");
    stats.reads++;
    stats.dev_reads[device_kind(address)]++;
    if (e && replay_reads) fwrite(&e->value, 4, 1, replay_reads);
    return e ? e->value : 0;
}

uint32_t ls_read_plain(int size, uint32_t address, uint32_t value) {
    const Event *e = pop(LS_READ, size, address, "plain read");
    if (address >= PT_RAM_LOW) stats.plain_ram_reads++;
    else stats.plain_rom_reads++;
    if (e && e->value != value) {
        char text[160];
        snprintf(text, sizeof(text), "read size %d address %06X, translated %08X, reference %08X", size,
                 (unsigned)address, (unsigned)value, (unsigned)e->value);
        print_report("read_value", text);
    }
    return value;
}

void ls_ram_code_check(uint32_t pc, int same) {
    if (!same) {
        char text[160];
        snprintf(text, sizeof(text), "the block of RAM code at %06X is not what was translated", (unsigned)pc);
        print_report("code_changed", text);
    }
}

/* Which blocks wrote to a device, and how often: a real 68000 reads before it writes on some forms, and writes the
 * low word of a long read-modify-write first, and those differences can only be seen on a device. */
#define DEVICE_SITES 4096
static struct { uint32_t pc; uint64_t writes[LS_DEV_KINDS]; int used; } device_sites[DEVICE_SITES];

static void note_device_site(uint32_t pc, int kind) {
    uint32_t h = ((pc * 2654435761u) >> 20) % DEVICE_SITES, i;
    for (i = 0; i < DEVICE_SITES; i++, h = (h + 1) % DEVICE_SITES) {
        if (!device_sites[h].used || device_sites[h].pc == pc) {
            device_sites[h].pc = pc;
            device_sites[h].used = 1;
            device_sites[h].writes[kind]++;
            return;
        }
    }
}

void ls_write(int size, uint32_t address, uint32_t value) {
    const Event *e = pop(LS_WRITE, size, address, "write");
    stats.writes++;
    if (address >= PT_RAM_LOW) stats.ram_writes++;
    else if (address >= opt.low_device_end && address < opt.rom_end) stats.rom_writes++;
    else {
        stats.dev_writes[device_kind(address)]++;
        note_device_site(current_block, device_kind(address));
    }
    if (e && e->value != value) {
        char text[160];
        snprintf(text, sizeof(text), "write size %d address %06X, translated %08X, reference %08X", size,
                 (unsigned)address, (unsigned)value, (unsigned)e->value);
        print_report("write_value", text);
    }
}

uint32_t ls_srframe(uint32_t address, uint32_t value) {
    const Event *e = pop(LS_WRITE, 2, address, "status word write");
    uint32_t mask = 0xFF00u | (uint32_t)current_live;
    stats.writes++;
    if (!e) return value;
    if ((e->value & mask) != (value & mask)) {
        char text[160];
        snprintf(text, sizeof(text), "address %06X, live flags %X, translated %04X, reference %04X", (unsigned)address,
                 (unsigned)current_live, (unsigned)value, (unsigned)e->value);
        print_report("sr_frame", text);
    }
    return e->value;
}

void ls_low_code_check(uint32_t pc, int bytes) {
    const uint8_t *low;
    if (!ref->low_page) return;
    low = ref->low_page();
    {
        int k;
        for (k = 0; k < bytes; k++) {
            if (low[(pc + (uint32_t)k) ^ 1] != pt_mem_r8(pt_rom, pc + (uint32_t)k)) {
                char text[160];
                snprintf(text, sizeof(text), "the %d bytes at %06X are not what was translated", bytes, (unsigned)pc);
                print_report("code_changed", text);
                return;
            }
        }
    }
}

void ls_mark_fallback(uint32_t pc) {
    pc &= PT_BUS_MASK;
    visited_f[pc >> 4] |= (uint8_t)(1u << ((pc >> 1) & 7));
    stats.fallback_instructions++;
}

void ls_request_ram_check(void) {
    ram_check_due = 1;
}

void ls_request_stop(void) {
    stop_due = 1;
}

void ls_poke(uint32_t address, int size, uint32_t value) {
    if (pokes_n >= POKES_MAX) {
        print_report("queue_overflow", "too many route pokes at one point");
        return;
    }
    pokes[pokes_n].address = address & PT_BUS_MASK;
    pokes[pokes_n].size = size;
    pokes[pokes_n].value = value;
    pokes_n++;
}

uint32_t ls_block_count(uint32_t pc) {
    return block_counts ? block_counts[(pc & PT_BUS_MASK) >> 1] : 0;
}

void ls_note_delay(int cycles) {
    int i;
    for (i = 0; i < delay_sites_n; i++) {
        if (delay_sites[i].pc == current_block) break;
    }
    if (i == delay_sites_n) {
        if (delay_sites_n >= DELAY_SITES_MAX) return;
        delay_sites[i].pc = current_block;
        delay_sites[i].cycles = cycles;
        delay_sites_n++;
    }
    delay_sites[i].hits++;
    if (irq_taken_at == current_block) delay_sites[i].with_irq++;
    irq_taken_at = 0xFFFFFFFFu;
}

/* The same change on both sides. Only called with both sides between the same two instructions. */
static void apply_pokes(void) {
    int i;
    for (i = 0; i < pokes_n; i++) {
        uint32_t a = pokes[i].address & 0xFFFFu;
        if (ref->poke_ram) ref->poke_ram(pokes[i].address, pokes[i].size, pokes[i].value);
        if (pokes[i].size == 1) pt_mem_w8(pt_ram, a, pokes[i].value);
        else pt_mem_w16(pt_ram, a & 0xFFFEu, pokes[i].value);
        stats.pokes++;
        if (replay_pokes) {
            uint64_t row[4];
            row[0] = stats.safe_points;
            row[1] = stats.instructions;
            row[2] = (uint64_t)pokes[i].address | ((uint64_t)pokes[i].size << 32);
            row[3] = pokes[i].value;
            fwrite(row, 8, 4, replay_pokes);
        }
    }
    pokes_n = 0;
}

static void compare_ram(void) {
    const uint8_t *r;
    int i;
    ram_check_due = 0;
    if (!ref->work_ram) return;
    r = ref->work_ram();
#if !defined(PT_RAM_BYTES)
    if (memcmp(r, pt_ram, sizeof(pt_ram)) == 0) return;
#endif
    /* The reference keeps 16-bit host words, so its byte i is the one at bus address i XOR 1. */
    for (i = 0; i < (int)sizeof(pt_ram); i++) {
        uint32_t mine = pt_mem_r8(pt_ram, (uint32_t)i ^ 1u);
        if (r[i] != mine) {
            char text[160];
            snprintf(text, sizeof(text), "work RAM offset %04X (in the host's layout), translated %02X, reference %02X",
                     (unsigned)i, (unsigned)mine, r[i]);
            print_report("ram_image", text);
            return;
        }
    }
}

static void compare_state(PtCtx *c, uint32_t pc, int live_flags, int entering_irq) {
    LsCpu s;
    char text[160];
    int i;
    ref->get(&s);
    if (!entering_irq && (s.pc & PT_BUS_MASK) != pc) {
        snprintf(text, sizeof(text), "translated entered %06X, reference is at %06X", (unsigned)pc,
                 (unsigned)(s.pc & PT_BUS_MASK));
        print_report("next_pc", text);
    }
    for (i = 0; i < 8; i++) {
        if (s.d[i] != c->d[i]) {
            snprintf(text, sizeof(text), "d%d translated %08X reference %08X", i, (unsigned)c->d[i], (unsigned)s.d[i]);
            print_report("register", text);
        }
        if (s.a[i] != c->a[i] && !(entering_irq && i == 7)) {
            snprintf(text, sizeof(text), "a%d translated %08X reference %08X", i, (unsigned)c->a[i], (unsigned)s.a[i]);
            print_report("register", text);
        }
    }
    if (entering_irq) return;       /* the flags and the mask are compared in the pushed status word */
    if (((live_flags & 8) && s.fn != c->fn) || ((live_flags & 4) && s.fz != c->fz) ||
        ((live_flags & 2) && s.fv != c->fv) || ((live_flags & 1) && s.fc != c->fc)) {
        snprintf(text, sizeof(text), "live flags %X, translated NZVC %d%d%d%d, reference %d%d%d%d", (unsigned)live_flags,
                 c->fn, c->fz, c->fv, c->fc, s.fn, s.fz, s.fv, s.fc);
        print_report("flags", text);
    }
    if (s.imask != c->imask) {
        snprintf(text, sizeof(text), "interrupt mask translated %d reference %d", c->imask, s.imask);
        print_report("imask", text);
    }
}

void ls_block(PtCtx *c, uint32_t pc, int instructions, int live_flags, int safe) {
    int level;
    char probe;
    last_ctx = c;
    if (!stack_top) stack_top = &probe;
    if (stack_top - &probe > (long long)stats.max_stack) stats.max_stack = (uint64_t)(stack_top - &probe);

    /* Natural timing: the reference already took an interrupt after the block before this one. */
    if (marks_n) {
        char text[160];
        int i;
        if (queue_head != marks[0].pos) {
            snprintf(text, sizeof(text), "%d accesses before an interrupt were not made here", marks[0].pos - queue_head);
            print_report("missing_access", text);
            queue_head = marks[0].pos;
        }
        current_block = pc;
        current_live = live_flags;
        compare_state(c, pc, live_flags, 1);
        level = marks[0].level;
        for (i = 1; i < marks_n; i++) marks[i - 1] = marks[i];
        marks_n--;
        stats.irqs++;
        stats.irqs_by_level[level & 7]++;
        pt_irq_level = (u32)level;
        return;
    }

    /* Everything the reference did in the block before this one must have been done here too. */
    if (queue_head < queue_n) {
        char text[160];
        snprintf(text, sizeof(text), "%d accesses of the reference were not made here, first at %06X",
                 queue_n - queue_head, (unsigned)queue[queue_head].address);
        print_report("missing_access", text);
    }
    queue_n = queue_head = 0;
    current_block = pc;
    current_live = live_flags;
    trail[trail_n % TRAIL_MAX] = pc;
    trail_n++;
    compare_state(c, pc, live_flags, 0);
    if (ram_check_due) compare_ram();
    if (stop_due && safe) {
        fflush(stdout);
        exit(ls_finish_run(c, 0));
    }
    if (safe) stats.safe_points++;
    if (safe && pokes_n) apply_pokes();

    /* Interrupts are taken at safe points only, on both sides. If the machine asks for one now, the
     * reference takes it first and the translated side follows through pt_take_irq. */
    if (safe && ref->irq_ready) {
        level = ref->irq_ready();
        if (level) {
            ref->take_irq();
            irq_taken_at = pc;
            stats.irqs++;
            stats.irqs_by_level[level & 7]++;
            if (replay_irqs) {
                uint64_t row[4];
                row[0] = stats.safe_points;
                row[1] = stats.instructions;
                row[2] = (uint64_t)level;
                row[3] = queue_n ? queue[queue_n - 1].value : 0;    /* the status word it pushed */
                fwrite(row, 8, 4, replay_irqs);
            }
            pt_irq_level = (u32)level;
            return;
        }
    }
    visited_t[pc >> 4] |= (uint8_t)(1u << ((pc >> 1) & 7));
    block_counts[pc >> 1]++;
    ref->step(instructions);
    stats.blocks++;
    stats.instructions += (uint64_t)instructions;
}

void ls_record_start(const PtCtx *c, uint32_t pc) {
    char path[512];
    FILE *f;
    if (!opt.report_dir) return;
#if defined(PT_RAM_BYTES)
    return;     /* a record is in the reference's layout, and a run in this one is compared as it goes */
#endif
    snprintf(path, sizeof(path), "%s/replay_start.bin", opt.report_dir);
    f = fopen(path, "wb");
    if (f) {
        fwrite(&pc, 4, 1, f);
        fwrite(&pt_rom_size, 4, 1, f);
        fwrite(c, sizeof(*c), 1, f);
        fwrite(pt_ram, sizeof(pt_ram), 1, f);
        fwrite(pt_rom, pt_rom_size, 1, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/replay_reads.bin", opt.report_dir);
    replay_reads = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/replay_irqs.bin", opt.report_dir);
    replay_irqs = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/replay_pokes.bin", opt.report_dir);
    replay_pokes = fopen(path, "wb");
}

int ls_finish(void) {
    int k;
    printf("writes_ram %llu writes_rom %llu plain_ram_reads %llu plain_rom_reads %llu pokes %llu\n",
           (unsigned long long)stats.ram_writes, (unsigned long long)stats.rom_writes,
           (unsigned long long)stats.plain_ram_reads, (unsigned long long)stats.plain_rom_reads,
           (unsigned long long)stats.pokes);
    for (k = 0; k < LS_DEV_KINDS; k++) {
        printf("device %s reads %llu writes %llu\n", LS_DEV_NAMES[k], (unsigned long long)stats.dev_reads[k],
               (unsigned long long)stats.dev_writes[k]);
    }
    for (k = 1; k < 8; k++) {
        if (stats.irqs_by_level[k]) printf("interrupts level %d: %llu\n", k, (unsigned long long)stats.irqs_by_level[k]);
    }
    printf("blocks %llu instructions %llu writes %llu device_reads %llu interrupts %llu safe_points %llu differences %llu\n",
           (unsigned long long)stats.blocks, (unsigned long long)stats.instructions,
           (unsigned long long)stats.writes, (unsigned long long)stats.reads, (unsigned long long)stats.irqs,
           (unsigned long long)stats.safe_points, (unsigned long long)stats.differences);
    printf("fallback_instructions %llu fallback_entries %llu max_c_stack_bytes %llu\n",
           (unsigned long long)stats.fallback_instructions, (unsigned long long)pt_fallback_uses,
           (unsigned long long)stats.max_stack);
    printf("guard_hit %llu guard_miss %llu call_map %llu switch_hit %llu switch_miss %llu map_hit %llu map_miss %llu unwind %llu\n",
           (unsigned long long)ls_counters.guard_hit, (unsigned long long)ls_counters.guard_miss,
           (unsigned long long)ls_counters.call_map, (unsigned long long)ls_counters.switch_hit,
           (unsigned long long)ls_counters.switch_miss, (unsigned long long)ls_counters.map_hit,
           (unsigned long long)ls_counters.map_miss, (unsigned long long)ls_counters.unwind);
    if (stats.differences) {
        printf("RESULT: FAIL: %s\n", stats.first_kind);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}

int ls_finish_run(PtCtx *c, int stopped) {
    int rc;
    if (opt.report_dir) {
        char path[512];
        FILE *f;
        uint64_t h = 0xCBF29CE484222325ull;
        uint32_t i;
        const uint8_t *p;
        snprintf(path, sizeof(path), "%s/visited.bin", opt.report_dir);
        f = fopen(path, "wb");
        if (f) {
            fwrite(visited_t, 1, 0x100000, f);
            fwrite(visited_f, 1, 0x100000, f);
            fclose(f);
        }
        snprintf(path, sizeof(path), "%s/block_counts.bin", opt.report_dir);
        f = fopen(path, "wb");
        if (f) {                        /* address, count: only the blocks that ran */
            for (i = 0; i < 0x800000u; i++) {
                if (block_counts[i]) {
                    uint32_t row[2];
                    row[0] = i * 2;
                    row[1] = block_counts[i];
                    fwrite(row, 4, 2, f);
                }
            }
            fclose(f);
        }
        pt_fallback_write_summary_to(opt.report_dir);
        snprintf(path, sizeof(path), "%s/device_sites.txt", opt.report_dir);
        f = fopen(path, "w");
        if (f) {
            int k;
            for (k = 0; k < DEVICE_SITES; k++) {
                if (device_sites[k].used) {
                    int j;
                    fprintf(f, "%06X", (unsigned)device_sites[k].pc);
                    for (j = 0; j < LS_DEV_KINDS; j++) fprintf(f, " %s %llu", LS_DEV_NAMES[j], (unsigned long long)device_sites[k].writes[j]);
                    fprintf(f, "\n");
                }
            }
            fclose(f);
        }
        snprintf(path, sizeof(path), "%s/nop_sites.txt", opt.report_dir);
        f = fopen(path, "w");
        if (f) {
            int k;
            for (k = 0; k < delay_sites_n; k++) {
                fprintf(f, "%06X cycles %d hits %llu interrupt_taken %llu\n", (unsigned)delay_sites[k].pc, delay_sites[k].cycles,
                        (unsigned long long)delay_sites[k].hits, (unsigned long long)delay_sites[k].with_irq);
            }
            fclose(f);
        }
        if (replay_reads) fclose(replay_reads);
        if (replay_irqs) fclose(replay_irqs);
        if (replay_pokes) fclose(replay_pokes);
        replay_reads = replay_irqs = replay_pokes = NULL;
        for (i = 0; i < sizeof(pt_ram); i++) { h ^= pt_mem_r8(pt_ram, i ^ 1u); h *= 0x100000001B3ull; }
        p = (const uint8_t *)c->d;
        for (i = 0; i < sizeof(c->d) + sizeof(c->a); i++) { h ^= p[i]; h *= 0x100000001B3ull; }
        snprintf(path, sizeof(path), "%s/replay_end.txt", opt.report_dir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "safe_points %llu instructions %llu reads %llu irqs %llu state %016llX\n",
                    (unsigned long long)stats.safe_points, (unsigned long long)stats.instructions,
                    (unsigned long long)stats.reads, (unsigned long long)stats.irqs, (unsigned long long)h);
            fclose(f);
        }
    }
    rc = ls_finish();
    if (stopped && !stats.differences) {
        printf("RESULT: the translated side stopped before the end of the route\n");
        rc = 1;
    }
    fflush(stdout);
    return rc;
}
