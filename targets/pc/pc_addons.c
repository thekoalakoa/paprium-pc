/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_addons.h. The 68000's byte write drives its byte on the half of the data bus that matches the address and asserts only that half's strobe:
 * an even address is the upper byte (UDS), an odd one the lower (LDS). Registers here are taken a strobe at a time, as the Pocket's logic takes them.
 */
#include <stdio.h>
#include <string.h>

#include "pt_rt.h"
#include "pc_addons.h"
#include "pc_brain.h"
#include "pc_io.h"
#include "pc_vdp.h"

int pc_addons_on;

/* The Mega CD's windows in the 68000's map: its program's ROM (the BIOS), its program memory's bank and its word memory. */
#define MCD_BIOS        0x400000u
#define MCD_PRG         0x420000u
#define MCD_WORD        0x600000u
#define MCD_END         0x800000u
/* The game's terrain cursor, a word of its own work RAM (offset 0x9920 of the 64 KB at $FF0000), which the brain reads: the Pocket core's logic taps the
   game's writes to it into the brain's mirror (its rtl/PAPRIUM/paprium_snoop.sv). The one address of the game this program names. */
#define GAME_CURSOR     0x9920u

static PcConsole want, now;
static PcMachine mach;                          /* the machine now: the console, its unit and the rules (pc_console.h) */
static int x32, megacd, cpumate;

/* ---- TMSS (a console of version 1). $A14101 bit 0 maps the cart at $000000 (1) or the console's TMSS ROM (0). There is no TMSS ROM here: while
   the bit is 0, the two bytes at $000120 read TMSS_BYTE, which is all the game looks at there, as the Pocket's stub answers. The console's reset
   maps the cart again. */
#define TMSS_BYTE       0x01u                   /* not 00, not FF, and not the byte the game's own header has there */
static int tmss_on, cart_mapped;
static uint64_t n_tmss_writes, n_tmss_reads;

/* ---- the Mega CD's registers as the stub keeps them; cleared with the console */
static uint8_t g1, g2, g3, c0, c1, c2, c3;      /* $A12001, $A12002, $A12003, $A12010-$A12013 */
static uint16_t prg_probe;                      /* $420000 */

/* ---- the link; cleared only at power-on, as the cart's */
static int sres_run, sbrq, last7;               /* the sub-CPU runs (SRES), its bus is requested (SBRQ), bit 7 of the last $A12011 byte */
static int f_send, f_restart, f_aioff;          /* events not yet handled */
static int busy, option_d;                      /* option D: the brain answers, so busy and the word are the real sub-CPU's */
static uint16_t comstat2, pend;
static int armed, word_done;
static uint64_t post_t, word_at, busy_at;     /* the update's time, and when its word and the end of busy are due */

/* ---- the firmware's glue */
enum { ST_NEW, ST_DISABLED, ST_READY };
static int state, restart_pending;

static char brain_status[1600] = "CPU MATE is off";   /* the window's words for CPU MATE: where its brain was read, or why not */
static FILE *trace;                             /* a test's record of what the brain was given and gave: pc_addons_trace */
static uint64_t n_posts, n_calls, n_words, n_restarts, n_aioffs, n_releases, n_ignored, n_other_reads, n_reads, n_writes, n_prg_words, n_wram_words;

/* ---- the Mega CD units' identification words. Only the eight words of the BIOS's header the game compares (offsets 0x120-0x14F from $400000),
   each of them for every unit, so nothing else answers there: the letters 'BO' (at one of three places), 'WO' (a Wondermega), 'L' 'D' (a
   LaserActive), and the model digits at 0x14B and 0x14D-0x14F. No other byte of any BIOS. One row per unit of the list, in PC_MEGACD_* order. */
static const uint16_t id_offsets[8] = { 0x120u, 0x124u, 0x126u, 0x128u, 0x12Cu, 0x14Au, 0x14Cu, 0x14Eu };
static const uint16_t id_words[PC_MEGACDS][8] = {
    { 0 },                                                                          /* none: nothing answers */
    { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0031u, 0x0000u, 0x0000u },     /* Mega-CD / Sega CD: 'BO', model '1' */
    { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0032u, 0x0030u, 0x3000u },     /* Mega-CD 2 / Sega CD 2: 'BO', model '2', then '0' '0' */
    { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u },     /* Wondermega: 'WO', 'BO' */
    { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u },     /* Wondermega M2 / X'Eye: 'WO', 'BO', model '2' */
    { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u },     /* LaserActive: 'L' 'D', 'BO' */
    { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u },     /* Aiwa CSD-GM1: 'BO', model '2', then '1' '1' 'C' */
    { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u },     /* CDX: 'BO', model '2', then '2' '1' */
    { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0033u, 0x0000u, 0x0000u },     /* Unknown CD: 'BO', a model digit the game does not know */
};

static int id_word(uint32_t a, uint32_t *v)
{
    int i;
    if (a < MCD_BIOS || a >= MCD_BIOS + 0x200u || mach.unit == PC_MEGACD_NONE)
        return 0;
    for (i = 0; i < 8; i++) {
        if (a - MCD_BIOS == id_offsets[i]) {
            *v = id_words[mach.unit][i];
            return 1;
        }
    }
    return 0;
}

/* Times in master clocks. The Pocket's link counts the sub-CPU's 12.5 MHz from 14 clocks of its 107.39 MHz clock after the write's filtered strobe,
   which comes two of those clocks after the strobe starts, and a release flop adds one: so the word or the busy flag changes at this many of the
   107.39 MHz clocks after the write, 16 + ceil(cycles x 2,147,727 / 250,000), two to a master clock. */
#define CLK_MD_PER_SUB_NUM  2147727u    /* 107.39 MHz / 12.5 MHz = 2,147,727 / 250,000, as the Pocket's link divides it; not a game address */
#define CLK_MD_PER_SUB_DEN  250000u     /* not a game address */
static uint64_t release_time(uint64_t post, uint32_t cycles)
{
    uint64_t clk = 16u + ((uint64_t)cycles * CLK_MD_PER_SUB_NUM + CLK_MD_PER_SUB_DEN - 1u) / CLK_MD_PER_SUB_DEN;
    return post + (clk + 1u) / 2u;
}

/* What the link shows at time t: the word and the busy flag change at their moments. */
static void advance(uint64_t t)
{
    if (!armed)
        return;
    if (!word_done && t >= word_at) {
        if (option_d)
            comstat2 = pend;
        word_done = 1;
    }
    if (t >= busy_at) {
        busy = 0;
        armed = 0;
    }
}

const char *pc_addons_brain_status(void) { return brain_status; }
void pc_addons_set_brain_status(const char *text) { snprintf(brain_status, sizeof(brain_status), "%s", text); }
int pc_addons_brain_ready(void) { return cpumate && pc_brain_ready(); }

/* ---- the firmware's glue: what it does after an event, done here at once (its l3_idle and l3_brain) */
static void call_brain(void)
{
    uint32_t word, dw, db;
    if (!sres_run || sbrq) {                    /* halted or reset since the update: no call */
        n_ignored++;
        return;
    }
    n_calls++;
    {
        /* The game's terrain cursor, a word of its own work RAM that the brain reads at every call: the Pocket's logic copies each write of it into
           the mirror, so the mirror holds the last one written, as work RAM does now. */
        uint16_t cursor = (uint16_t)pt_mem_r16(pt_ram, GAME_CURSOR);
        memcpy(pc_brain_sram() + PC_BRAIN_CURSOR_OFF, &cursor, 2);
    }
    if (trace)
        fprintf(trace, "c %04X\n", (unsigned)pt_mem_r16(pt_ram, GAME_CURSOR));
    if (pc_brain_call(&word, &dw, &db) != 0) {
        state = ST_DISABLED;
        option_d = 0;
        busy = 0;
        armed = 0;
        snprintf(brain_status, sizeof(brain_status), "CPU MATE's brain stopped, so the partner stands still");
        return;
    }
    pend = (uint16_t)word;
    armed = 1;
    word_done = 0;
    word_at = release_time(post_t, dw);
    busy_at = release_time(post_t, db);
    n_words++;
    if (trace)
        fprintf(trace, "a %04X %06X %06X\n", (unsigned)word, (unsigned)dw, (unsigned)db);
}

static void glue(void)
{
    if (!f_send && !f_aioff)
        return;
    if (state == ST_NEW)
        state = cpumate && pc_brain_ready() ? ST_READY : ST_DISABLED;
    if (state == ST_DISABLED) {
        option_d = 0;
        f_send = f_restart = f_aioff = 0;
        return;
    }
    if (f_restart) {
        restart_pending = 1;
        f_restart = 0;
    }
    if (restart_pending && sres_run && !sbrq) {     /* the release: the sub-CPU's start-up pattern, then the brain's tables */
        restart_pending = 0;
        pc_brain_release();
        n_releases++;
        if (trace)
            fputs("r\n", trace);
    }
    option_d = 1;
    if (!restart_pending && !sbrq)
        f_aioff = 0;
    if (f_send && !restart_pending) {
        f_send = 0;
        call_brain();
    }
}

/* ---- configuration and resets */
void pc_addons_configure(const PcConsole *c) { want = *c; }
const PcConsole *pc_addons_console(void) { return &now; }

const PcMachine *pc_addons_machine(void) { return &mach; }

void pc_addons_reset(void)
{
    now = want;
    pc_console_machine(&now, &mach);
    x32 = mach.x32;
    megacd = mach.unit != PC_MEGACD_NONE;
    cpumate = mach.cpumate_unit && now.cpumate;
    tmss_on = mach.tmss;
    cart_mapped = 1;
    pc_addons_on = x32 || megacd || tmss_on;
    pc_io_set_version(pc_machine_version(&mach));
    pc_vdp_set_teradrive(mach.teradrive);
    g1 = g2 = g3 = c0 = c1 = c2 = c3 = 0;
    prg_probe = 0;
}

void pc_addons_power_on(void)
{
    sres_run = 1;
    sbrq = 0;
    last7 = 0;
    f_send = f_aioff = 0;
    f_restart = 1;                              /* the link's restart flag powers up set */
    busy = option_d = 0;
    comstat2 = pend = 0;
    armed = word_done = 0;
    state = ST_NEW;
    restart_pending = 0;
    pc_addons_reset();
}

/* ---- the 68000's accesses */
static int in_gate(uint32_t a) { return a >= 0xA12000u && a < 0xA12040u; }

/* The gate array's word at a (even) as the stub gives it. */
static uint32_t gate_word(uint32_t a)
{
    switch ((a >> 1) & 31u) {
    case 0: return g1;
    case 1: return (uint32_t)g2 << 8 | (g3 & 0xF8u) | (g3 & 2u) | 1u;      /* bit 0 RET 1, bit 2 MODE 0 */
    case 8: return (uint32_t)c0 << 8 | c1;
    case 9: return (uint32_t)c2 << 8 | c3;
    case 16: return (busy ? 0x8000u : 0u) | c1;                          /* the busy flag, and the sub-CPU's echo of $A12011 */
    case 17: return 0x5700u;                                             /* 'W': the sub-CPU runs */
    case 18: return comstat2;                                            /* the partner's word */
    default: return 0u;
    }
}

/* One word of the add-ons at a (even): 1 with the word when one answers there. */
static int word_at_address(uint32_t a, uint32_t *v)
{
    if (x32 && (a == 0xA130ECu || a == 0xA130EEu)) {
        *v = a == 0xA130ECu ? 0x4D41u : 0x5253u;                         /* "MA" "RS" */
        return 1;
    }
    if (!megacd)
        return 0;
    if (in_gate(a)) {
        *v = gate_word(a);
        return 1;
    }
    if (a == MCD_PRG + 0x290u) {
        *v = 0x4D42u;                                                    /* "MB" */
        return 1;
    }
    if (a == MCD_PRG) {
        *v = prg_probe;
        return 1;
    }
    return id_word(a, v);
}

/* A read of the cart's first 64 KB, value as the cart gave it: with TMSS and $A14101 bit 0 clear, the bytes at $000120 and $000121 are TMSS_BYTE. */
uint32_t pc_addons_low_read(int size, uint32_t a, uint32_t value)
{
    int i;
    if (!tmss_on || cart_mapped)
        return value;
    for (i = 0; i < size; i++) {
        uint32_t byte = a + (uint32_t)i, shift = 8u * (uint32_t)(size - 1 - i);
        if (byte == 0x120u || byte == 0x121u) {
            value = (value & ~(0xFFu << shift)) | TMSS_BYTE << shift;
            n_tmss_reads++;
        }
    }
    return value;
}

/* TMSS's registers: 'SEGA' written to $A14000-$A14003, and $A14101, whose bit 0 maps the cart (1) or the TMSS ROM (0). */
static int in_tmss(uint32_t a) { return (a >= 0xA14000u && a < 0xA14004u) || (a & ~1u) == 0xA14100u; }

static void tmss_write(int size, uint32_t a, uint32_t v)
{
    n_tmss_writes++;
    if (size == 1 && a == 0xA14101u)
        cart_mapped = (int)(v & 1u);
    else if (size == 2 && a == 0xA14100u)
        cart_mapped = (int)(v & 1u);
    else if (size == 4 && a == 0xA14100u)
        cart_mapped = (int)((v >> 16) & 1u);
}

int pc_addons_read(int size, uint32_t a, uint64_t t, uint32_t *value)
{
    uint32_t hi, lo;
    a &= PT_BUS_MASK;
    if (megacd)
        advance(t);
    if (size == 4) {
        if (!word_at_address(a, &hi) || !word_at_address(a + 2u, &lo))
            goto nothing;
        *value = hi << 16 | lo;
    } else {
        if (!word_at_address(a & ~1u, &hi))
            goto nothing;
        *value = size == 2 ? hi : (a & 1u) ? (hi & 0xFFu) : (hi >> 8);
    }
    n_reads++;
    return 1;
nothing:
    if (megacd && a >= MCD_BIOS && a < MCD_END)
        n_other_reads++;
    return 0;
}

/* A write of a strobe at a time: upper is the byte at the even address, lower the odd one; -1 for a strobe not asserted. */
static void gate_write(uint32_t a, int upper, int lower, uint64_t t)
{
    switch ((a >> 1) & 31u) {
    case 0:                                                              /* $A12001: the sub-CPU's reset and bus request */
        if (lower >= 0) {
            int old_sbrq = sbrq;
            g1 = (uint8_t)lower;
            sbrq = (lower >> 1) & 1;
            if (lower & 1) {
                sres_run = 1;
            } else if (sres_run) {                                       /* SRES from 1 to 0: the sub-CPU is reset */
                f_restart = 1;
                sres_run = 0;
                busy = 0;
                armed = 0;
                comstat2 = 0;                                            /* the sub-CPU's start clears its word before the game can read it */
                n_restarts++;
            }
            if (!old_sbrq && sbrq) {                                     /* SBRQ from 0 to 1: the game's "AI off" */
                f_aioff = 1;
                busy = 0;
                armed = 0;
                n_aioffs++;
            }
            glue();
        }
        break;
    case 1:
        if (upper >= 0) g2 = (uint8_t)upper;
        if (lower >= 0) g3 = (uint8_t)lower;
        break;
    case 8:                                                              /* $A12010 / $A12011: an update when the low byte says so */
        if (upper >= 0) c0 = (uint8_t)upper;
        if (lower >= 0) {
            int post = (lower & 0x7F) == 2 && ((lower >> 7) & 1) != last7 && sres_run;
            c1 = (uint8_t)lower;
            last7 = (lower >> 7) & 1;
            if (post) {
                n_posts++;
                busy = option_d;
                armed = 0;
                word_done = 0;
                post_t = t;
                f_send = 1;
                glue();
            }
        }
        break;
    case 9:
        if (upper >= 0) c2 = (uint8_t)upper;
        if (lower >= 0) c3 = (uint8_t)lower;
        break;
    default:
        break;
    }
}

/* Into the brain's mirror, each 68000 word as a 16-bit value. */
static void mirror_write(uint32_t off, int upper, int lower)
{
    uint8_t *m = pc_brain_sram() + off;
    uint16_t w;
    memcpy(&w, m, 2);
    if (upper >= 0) w = (uint16_t)((w & 0x00FFu) | (uint32_t)upper << 8);
    if (lower >= 0) w = (uint16_t)((w & 0xFF00u) | (uint32_t)lower);
    memcpy(m, &w, 2);
    if (trace)
        fprintf(trace, "w %05X %04X\n", (unsigned)off, (unsigned)w);
}

static void write_word(uint32_t a, int upper, int lower, uint64_t t)
{
    if (in_gate(a)) {
        gate_write(a, upper, lower, t);
        return;
    }
    if (a == MCD_PRG) {
        if (upper >= 0) prg_probe = (uint16_t)((prg_probe & 0x00FFu) | (uint32_t)upper << 8);
        if (lower >= 0) prg_probe = (uint16_t)((prg_probe & 0xFF00u) | (uint32_t)lower);
    }
    if (a >= MCD_WORD && a < MCD_WORD + 128u * 1024u) {                  /* the word memory, always mirrored */
        mirror_write(PC_BRAIN_WORD_RAM_OFF + (a - MCD_WORD), upper, lower);
        n_wram_words++;
    } else if (a >= MCD_PRG && a < MCD_PRG + 0x2000u && (g3 >> 6) == 0u && !sres_run) {   /* the program memory, bank 0, the sub-CPU held */
        mirror_write(PC_BRAIN_PRG_OFF + (a - MCD_PRG), upper, lower);
        n_prg_words++;
    }
}

int pc_addons_write(int size, uint32_t a, uint32_t v, uint64_t t)
{
    a &= PT_BUS_MASK;
    if (tmss_on && in_tmss(a)) {
        tmss_write(size, a, v);
        return 1;
    }
    if (!megacd || !(in_gate(a) || (a >= MCD_BIOS && a < MCD_END)))
        return 0;
    advance(t);
    n_writes++;
    if (size == 1) {
        if (a & 1u)
            write_word(a & ~1u, -1, (int)(v & 0xFFu), t);
        else
            write_word(a, (int)(v & 0xFFu), -1, t);
    } else if (size == 2) {
        /* A 68000 reaches words at even addresses only: an odd one is an address error there, and never comes from the game. Kept even here, so a
           word written at the mirror's last byte cannot reach one byte past it. */
        write_word(a & ~1u, (int)((v >> 8) & 0xFFu), (int)(v & 0xFFu), t);
    } else {
        write_word(a & ~1u, (int)((v >> 24) & 0xFFu), (int)((v >> 16) & 0xFFu), t);
        write_word((a & ~1u) + 2u, (int)((v >> 8) & 0xFFu), (int)(v & 0xFFu), t);
    }
    return 1;
}

void pc_addons_trace(void *file) { trace = (FILE *)file; }

void pc_addons_report(void)
{
    if (trace)
        fflush(trace);
    if (!pc_addons_on)
        return;
    {
        static const char *const units[] = PC_MEGACD_TITLES;
        printf("console: %s, version register %02X, $1FE4 model bits %03X, TMSS %s (%llu register writes, %llu reads of $000120 answered), 128 KB video "
               "memory %s, 32X %s, Mega CD %s, CPU MATE %s\n", pc_console_name(&now), pc_machine_version(&mach), mach.model, tmss_on ? "on" : "off",
               (unsigned long long)n_tmss_writes, (unsigned long long)n_tmss_reads, mach.teradrive ? "on" : "off", x32 ? "on" : "off",
               megacd ? units[mach.unit] : "none", cpumate ? (pc_brain_ready() ? "on" : "on, with no brain") : "off");
    }
    if (megacd)
        printf("Mega CD: %llu reads and %llu writes answered, %llu other reads of the Mega CD's 4 MB read as nothing; word memory %llu words, program memory %llu "
               "words mirrored; updates %llu, brain calls %llu, words %llu, sub-CPU resets %llu, AI off %llu, releases %llu, updates while halted %llu\n",
               (unsigned long long)n_reads, (unsigned long long)n_writes, (unsigned long long)n_other_reads, (unsigned long long)n_wram_words,
               (unsigned long long)n_prg_words, (unsigned long long)n_posts, (unsigned long long)n_calls, (unsigned long long)n_words,
               (unsigned long long)n_restarts, (unsigned long long)n_aioffs, (unsigned long long)n_releases, (unsigned long long)n_ignored);
    pc_brain_report();
    fflush(stdout);
}
