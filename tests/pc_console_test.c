/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console the game finds and its add-ons (targets/pc/pc_console.h, pc_addons.h), the CPU MATE brain's seam (pc_brain.h) and the RV32IM
 * interpreter (pc_rv32.h), on made-up data: the settings' lines, the version register, the 32X's and the Mega CD's answers through the machine's
 * bus for every console and unit of the two lists (the table below: what the Pocket core's 0.3.0 source answers, case by case), TMSS's check, the
 * TeraDrive's video memory test, the Mega CD's link with a made-up brain of our own (a few RV32 instructions written here), and the interpreter on
 * programs written here.
 * No game byte and no brain byte. Built and run by tests/test_pc_machine.py, which also builds it with one rule broken at a time and checks that a
 * test here fails, and compares the interpreter with unicorn's RISC-V on random programs (the group "rvfile").
 *
 * It prints one line for each failed check, then the count, then RESULT: PASS or RESULT: FAIL. An argument names one group of checks to run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_rt.h"
#include "pc_addons.h"
#include "pc_brain.h"
#include "pc_cart.h"
#include "pc_clock.h"
#include "pc_config.h"
#include "pc_console.h"
#include "pc_frame.h"
#include "pc_io.h"
#include "pc_rv32.h"
#include "pc_vdp.h"
#include "pc_z80.h"

PtCtx pc_ctx;

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- what the runtime and the rest of the program give the machine: no translated code, no cart */
void pt_watch_hit(int index) { (void)index; }
int pt_interp_only;
u32 pt_fallback(PtCtx *c, u32 pc) { (void)c; (void)pc; return 0xFFFFFFFFu; }
uint32_t pc_cart_r16(uint32_t a, uint64_t t) { (void)a; (void)t; return 0u; }
uint32_t pc_cart_r8(uint32_t a, uint64_t t) { (void)a; (void)t; return 0u; }
void pc_cart_w16(uint32_t a, uint32_t v, uint64_t t) { (void)a; (void)v; (void)t; }
void pc_cart_w8(uint32_t a, uint32_t v, uint64_t t) { (void)a; (void)v; (void)t; }
int pc_cart_holding(void) { return 0; }
void pc_machine_frame_start(uint32_t frame) { pc_vdp_frame_start(frame); }
void pc_machine_frame_end(uint32_t frame) { (void)frame; }
void pc_sound_fm(uint64_t t, int32_t left, int32_t right) { (void)t; (void)left; (void)right; }
void pc_sound_psg(uint64_t t, int32_t level) { (void)t; (void)level; }

static PcFrame *rec;

static void go_to(uint64_t t)
{
    uint64_t now = pc_clock_now();
    if (t > now)
        pc_clock_stall(t - now);
    (void)pt_port_tick();
}

static void machine(const PcConsole *console)
{
    memset(&pc_counts, 0, sizeof(pc_counts));
    memset(&pc_ctx, 0, sizeof(pc_ctx));
    memset(rec, 0, sizeof(*rec));
    pc_ctx.imask = 7;
    pc_frame_rec = rec;
    pc_clock_reset();
    pc_tick_init();
    pc_vdp_reset();
    pc_addons_configure(console);
    pc_addons_power_on();
    pc_io_reset();
    pc_z80_reset();
    pt_irq_level = 0;
    pt_at = 0;
}

/* The Mega CD's windows in the 68000's map, and the places in the brain's mirror that the tests look at */
#define BIOS 0x400000u
#define PRG 0x420000u
#define WORD_RAM 0x600000u
#define TERRAIN (PC_BRAIN_WORD_RAM_OFF + 0x8000u)
#define CAPTURED (PC_BRAIN_PRG_OFF + 0x5F4u)

static uint32_t r8(uint32_t a) { return pt_port_read(1, a); }
static uint32_t r16(uint32_t a) { return pt_port_read(2, a); }
static uint32_t r32(uint32_t a) { return pt_port_read(4, a); }
static void w8(uint32_t a, uint32_t v) { pt_port_write(1, a, v); }
static void w16(uint32_t a, uint32_t v) { pt_port_write(2, a, v); }

static PcConsole console_of(int region, int kind, int x32, int megacd, int cpumate)
{
    PcConsole c;
    memset(&c, 0, sizeof(c));
    c.region = region;
    c.console = kind;
    c.x32 = x32;
    c.megacd = megacd;
    c.cpumate = cpumate;
    return c;
}

/* ---- the settings' lines */
static void test_settings(void)
{
    PcConsole c;
    PcConfig cfg;
    char error[200];
    /* an earlier version's lines keep their meaning: the region decides between the first Mega Drive and the Genesis, megacd=on is the first Mega-CD */
    static const struct { int region, kind, megacd; unsigned version; } table[] = {
        { PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE, 0, 0x20 }, { PC_REGION_AUTO, PC_CONSOLE_GENESIS, 0, 0xA0 },
        { PC_REGION_JP, PC_CONSOLE_MEGADRIVE, 0, 0x20 }, { PC_REGION_JP, PC_CONSOLE_GENESIS, 0, 0x20 },
        { PC_REGION_US, PC_CONSOLE_MEGADRIVE, 0, 0xA0 }, { PC_REGION_US, PC_CONSOLE_GENESIS, 0, 0xA0 }, { PC_REGION_JP, PC_CONSOLE_MEGADRIVE, 1, 0x00 },
        { PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE, 1, 0x00 }, { PC_REGION_US, PC_CONSOLE_MEGADRIVE, 1, 0x80 }, { PC_REGION_AUTO, PC_CONSOLE_GENESIS, 1, 0x80 },
        { PC_REGION_US, PC_CONSOLE_NOMAD, 0, 0xA1 }, { PC_REGION_JP, PC_CONSOLE_NOMAD, 0, 0xA1 }, { PC_REGION_US, PC_CONSOLE_MEGAJET, 0, 0x21 },
    };
    size_t i;
    memset(&c, 0, sizeof(c));
    CHECK(pc_console_line(&c, "region", "us") == 1 && c.region == PC_REGION_US);
    CHECK(pc_console_line(&c, "region", "eu") == 0 && c.region == PC_REGION_US);     /* no Europe (the team's decision) */
    CHECK(pc_console_line(&c, "region", "jp") == 1 && c.region == PC_REGION_JP);
    CHECK(pc_console_line(&c, "region", "auto") == 1 && c.region == PC_REGION_AUTO);
    CHECK(pc_console_line(&c, "region", "usa") == 0 && c.region == PC_REGION_AUTO);
    CHECK(pc_console_line(&c, "console", "genesis") == 1 && c.console == PC_CONSOLE_GENESIS);
    CHECK(pc_console_line(&c, "console", "megadrive") == 1 && c.console == PC_CONSOLE_MEGADRIVE);
    CHECK(pc_console_line(&c, "console", "nomad") == 1 && c.console == PC_CONSOLE_NOMAD);
    CHECK(pc_console_line(&c, "console", "auto") == 1 && c.console == PC_CONSOLE_MEGADRIVE);
    CHECK(pc_console_line(&c, "console", "genesis4") == 0 && c.console == PC_CONSOLE_MEGADRIVE);
    CHECK(pc_console_line(&c, "x32", "on") == 1 && c.x32 == 1);
    CHECK(pc_console_line(&c, "megacd", "on") == 1 && c.megacd == PC_MEGACD_MEGACD && c.megacd == 1);
    CHECK(pc_console_line(&c, "megacd", "off") == 1 && c.megacd == PC_MEGACD_NONE);
    CHECK(pc_console_line(&c, "megacd", "laseractive") == 1 && c.megacd == PC_MEGACD_LASERACTIVE);
    CHECK(pc_console_line(&c, "megacd", "segacd") == 0 && c.megacd == PC_MEGACD_LASERACTIVE);
    {
        /* every name of the two lists is read back as its place in the list */
        static const char *const consoles[] = PC_CONSOLE_NAMES, *const units[] = PC_MEGACD_NAMES;
        int k;
        for (k = 0; k < PC_CONSOLES; k++)
            CHECK(pc_console_line(&c, "console", consoles[k]) == 1 && c.console == k);
        for (k = 0; k < PC_MEGACDS; k++)
            CHECK(pc_console_line(&c, "megacd", units[k]) == 1 && c.megacd == k);
        c.megacd = 1;
    }
    CHECK(pc_console_line(&c, "cpumate", "on") == 1 && c.cpumate == 1);
    CHECK(pc_console_line(&c, "cpumate", "yes") == 0 && c.cpumate == 1);
    CHECK(pc_console_line(&c, "cpumate_brain", "lm\\little_man_firmware.bin") == 1 && !strcmp(c.brain, "lm\\little_man_firmware.bin"));
    CHECK(pc_console_line(&c, "scale", "3") == -1);
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        c = console_of(table[i].region, table[i].kind, 0, table[i].megacd, 0);
        CHECK(pc_console_version(&c) == table[i].version);
    }
    c = console_of(PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE, 0, 0, 0);
    CHECK(!strcmp(pc_console_name(&c), "Mega Drive (Japan)"));
    c.console = PC_CONSOLE_GENESIS;
    CHECK(!strcmp(pc_console_name(&c), "Genesis (USA)"));
    c.megacd = PC_MEGACD_LASERACTIVE;
    CHECK(!strcmp(pc_console_name(&c), "LaserActive (USA)"));
    c.console = PC_CONSOLE_MEGAJET;
    CHECK(!strcmp(pc_console_name(&c), "LaserActive (Japan)"));
    c.megacd = PC_MEGACD_MEGACD;
    CHECK(!strcmp(pc_console_name(&c), "Mega Jet (Japan)"));
    /* through the configuration's own reader: the defaults are all zero, the lines land, a bad one is refused with its line number */
    pc_config_defaults(&cfg);
    CHECK(cfg.console.region == 0 && cfg.console.console == 0 && cfg.console.x32 == 0 && cfg.console.megacd == 0 && cfg.console.cpumate == 0 && !cfg.console.brain[0]);
    CHECK(pc_config_parse("scale=2\nregion=us\nconsole=genesis\nx32=on\nmegacd=on\ncpumate=on\ncpumate_brain=fw/lm.bin\n", &cfg, error, sizeof(error)) == 0);
    CHECK(cfg.scale == 2 && cfg.console.region == PC_REGION_US && cfg.console.console == PC_CONSOLE_GENESIS && cfg.console.x32 && cfg.console.megacd &&
          cfg.console.cpumate && !strcmp(cfg.console.brain, "fw/lm.bin"));
    pc_config_defaults(&cfg);
    CHECK(pc_config_parse("vsync=on\nmegacd=maybe\n", &cfg, error, sizeof(error)) == 2 && strstr(error, "megacd=maybe") != NULL);
    /* region=eu is refused with the same plain words as any value the program does not know */
    pc_config_defaults(&cfg);
    CHECK(pc_config_parse("vsync=on\nregion=eu\n", &cfg, error, sizeof(error)) == 2 && !strcmp(error, "line 2 of the configuration: region=eu"));
    CHECK(pc_config_parse("vsync=on\nregion=mars\n", &cfg, error, sizeof(error)) == 2 && !strcmp(error, "line 2 of the configuration: region=mars"));
    CHECK(cfg.console.region == PC_REGION_AUTO);
}

/* ---- every console of the list with every Mega CD unit: what the Pocket core's 0.3.0 source answers, case by case (its paprium_opts with the
   hardware rules, its version register, its $1FE4 model bits, TMSS, the TeraDrive's answer and the unit's eight compared words), in list order:
   the console, then the unit. The words of the unit in effect, or 0 when none is. */
static const struct { unsigned version, model; int tmss, teradrive, unit; uint16_t words[8]; } machines[PC_CONSOLES * PC_MEGACDS] = {
    { 0x20U, 0x0000U, 0, 0, 0, { 0 } },
    { 0x00U, 0x0000U, 0, 0, 1, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0031u, 0x0000u, 0x0000u } },
    { 0x00U, 0x0000U, 0, 0, 2, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0032u, 0x0030u, 0x3000u } },
    { 0x01U, 0x0000U, 1, 0, 3, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 4, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 5, { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 6, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u } },
    { 0x81U, 0x0000U, 1, 0, 7, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u } },
    { 0x00U, 0x0000U, 0, 0, 8, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0033u, 0x0000u, 0x0000u } },
    { 0xA0U, 0x0000U, 0, 0, 0, { 0 } },
    { 0x80U, 0x0000U, 0, 0, 1, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0031u, 0x0000u, 0x0000u } },
    { 0x80U, 0x0000U, 0, 0, 2, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0032u, 0x0030u, 0x3000u } },
    { 0x01U, 0x0000U, 1, 0, 3, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u } },
    { 0x81U, 0x0000U, 1, 0, 4, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u } },
    { 0x81U, 0x0000U, 1, 0, 5, { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 6, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u } },
    { 0x81U, 0x0000U, 1, 0, 7, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u } },
    { 0x80U, 0x0000U, 0, 0, 8, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0033u, 0x0000u, 0x0000u } },
    { 0x21U, 0x0700U, 1, 0, 0, { 0 } },
    { 0x01U, 0x0700U, 1, 0, 1, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0031u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0700U, 1, 0, 2, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0032u, 0x0030u, 0x3000u } },
    { 0x01U, 0x0000U, 1, 0, 3, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 4, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 5, { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 6, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u } },
    { 0x81U, 0x0000U, 1, 0, 7, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u } },
    { 0x01U, 0x0700U, 1, 0, 8, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0033u, 0x0000u, 0x0000u } },
    { 0xA1U, 0x0700U, 1, 0, 0, { 0 } },
    { 0x81U, 0x0700U, 1, 0, 1, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0031u, 0x0000u, 0x0000u } },
    { 0x81U, 0x0700U, 1, 0, 2, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0032u, 0x0030u, 0x3000u } },
    { 0x01U, 0x0000U, 1, 0, 3, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u } },
    { 0x81U, 0x0000U, 1, 0, 4, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u } },
    { 0x81U, 0x0000U, 1, 0, 5, { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 6, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u } },
    { 0x81U, 0x0000U, 1, 0, 7, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u } },
    { 0x81U, 0x0700U, 1, 0, 8, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0033u, 0x0000u, 0x0000u } },
    { 0xA1U, 0x0000U, 1, 0, 0, { 0 } },
    { 0xA1U, 0x0000U, 1, 0, 0, { 0 } },
    { 0xA1U, 0x0000U, 1, 0, 0, { 0 } },
    { 0x01U, 0x0000U, 1, 0, 3, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u } },
    { 0x81U, 0x0000U, 1, 0, 4, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u } },
    { 0x81U, 0x0000U, 1, 0, 5, { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 6, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u } },
    { 0x81U, 0x0000U, 1, 0, 7, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u } },
    { 0xA1U, 0x0000U, 1, 0, 0, { 0 } },
    { 0xA1U, 0x0400U, 1, 0, 0, { 0 } },
    { 0xA1U, 0x0400U, 1, 0, 0, { 0 } },
    { 0xA1U, 0x0400U, 1, 0, 0, { 0 } },
    { 0x01U, 0x0000U, 1, 0, 3, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u } },
    { 0x81U, 0x0000U, 1, 0, 4, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u } },
    { 0x81U, 0x0000U, 1, 0, 5, { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 6, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u } },
    { 0x81U, 0x0000U, 1, 0, 7, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u } },
    { 0xA1U, 0x0400U, 1, 0, 0, { 0 } },
    { 0x21U, 0x0600U, 1, 0, 0, { 0 } },
    { 0x21U, 0x0600U, 1, 0, 0, { 0 } },
    { 0x21U, 0x0600U, 1, 0, 0, { 0 } },
    { 0x01U, 0x0000U, 1, 0, 3, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 4, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 5, { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 6, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u } },
    { 0x81U, 0x0000U, 1, 0, 7, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u } },
    { 0x21U, 0x0600U, 1, 0, 0, { 0 } },
    { 0x20U, 0x0000U, 0, 1, 0, { 0 } },
    { 0x00U, 0x0000U, 0, 1, 1, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0031u, 0x0000u, 0x0000u } },
    { 0x00U, 0x0000U, 0, 1, 2, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0032u, 0x0030u, 0x3000u } },
    { 0x01U, 0x0000U, 1, 0, 3, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 4, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 5, { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 6, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u } },
    { 0x81U, 0x0000U, 1, 0, 7, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u } },
    { 0x00U, 0x0000U, 0, 1, 8, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0033u, 0x0000u, 0x0000u } },
    { 0x20U, 0x0040U, 0, 0, 0, { 0 } },
    { 0x00U, 0x0040U, 0, 0, 1, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0031u, 0x0000u, 0x0000u } },
    { 0x00U, 0x0040U, 0, 0, 2, { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0032u, 0x0030u, 0x3000u } },
    { 0x01U, 0x0000U, 1, 0, 3, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 4, { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 5, { 0x0000u, 0x004Cu, 0x4400u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0000u } },
    { 0x01U, 0x0000U, 1, 0, 6, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u } },
    { 0x81U, 0x0000U, 1, 0, 7, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0032u, 0x3100u } },
    { 0x00U, 0x0040U, 0, 0, 8, { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0033u, 0x0000u, 0x0000u } },
};

static void test_models(void)
{
    static const uint16_t offsets[8] = { 0x120u, 0x124u, 0x126u, 0x128u, 0x12Cu, 0x14Au, 0x14Cu, 0x14Eu };
    int k, u, i, wrong = 0;
    for (k = 0; k < PC_CONSOLES; k++) {
        for (u = 0; u < PC_MEGACDS; u++) {
            PcConsole c = console_of(PC_REGION_AUTO, k, 0, u, 0);
            PcMachine m;
            int n = k * PC_MEGACDS + u, ok;
            pc_console_machine(&c, &m);
            machine(&c);
            ok = pc_console_version(&c) == machines[n].version && m.model == machines[n].model && m.tmss == machines[n].tmss &&
                 m.teradrive == machines[n].teradrive && m.unit == machines[n].unit && r8(0xA10001) == machines[n].version &&
                 r16(0xA10000) == (machines[n].version * 0x101u) && m.cpumate_unit == (machines[n].unit != PC_MEGACD_NONE && machines[n].unit != PC_MEGACD_UNKNOWN);
            for (i = 0; i < 8; i++)
                ok = ok && r16(BIOS + offsets[i]) == (machines[n].unit ? machines[n].words[i] : 0xFFFFu);
            ok = ok && r16(BIOS + 0x122u) == 0xFFFFu && r16(BIOS + 0x130u) == 0xFFFFu;          /* a word the game does not compare: nothing there */
            ok = ok && (r16(0xA12022) == (machines[n].unit ? 0x5700u : 0xFFFFu));              /* the gate array answers for every unit */
            if (!ok) {
                wrong++;
                printf("models: console %d, unit %d: version %02X (want %02X), model %03X (want %03X), tmss %d, teradrive %d, unit %d (want %d)\n", k, u,
                       pc_console_version(&c), machines[n].version, m.model, machines[n].model, m.tmss, m.teradrive, m.unit, machines[n].unit);
            }
        }
    }
    CHECK(wrong == 0);
    /* the rules, by name: a console with no port drops a separate unit; a unit that is a whole console sets the console */
    {
        PcConsole c = console_of(PC_REGION_AUTO, PC_CONSOLE_GENESIS3, 1, PC_MEGACD_MEGACD2, 1);
        PcMachine m;
        pc_console_machine(&c, &m);
        CHECK(m.dropped && m.unit == PC_MEGACD_NONE && !m.cpumate_unit && m.x32 && !m.combined);
        c = console_of(PC_REGION_AUTO, PC_CONSOLE_NOMAD, 0, PC_MEGACD_CDX, 1);
        pc_console_machine(&c, &m);
        CHECK(!m.dropped && m.combined && m.unit == PC_MEGACD_CDX && m.cpumate_unit && !m.japan && m.tmss && m.model == 0u);
        c = console_of(PC_REGION_AUTO, PC_CONSOLE_TERADRIVE, 0, PC_MEGACD_UNKNOWN, 1);
        pc_console_machine(&c, &m);
        CHECK(m.teradrive && m.unit == PC_MEGACD_UNKNOWN && !m.cpumate_unit);
        c = console_of(PC_REGION_US, PC_CONSOLE_TERADRIVE, 0, PC_MEGACD_WONDERMEGA2, 0);   /* a region line does nothing with this console */
        pc_console_machine(&c, &m);
        CHECK(!m.teradrive && m.combined && m.japan);
    }
}

/* ---- TMSS: the game's check at $000120 and the 'SEGA' write, on a console with TMSS and on one without */
static void test_tmss(void)
{
    PcConsole c = console_of(PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE2, 0, 0, 0);
    uint64_t other;
    machine(&c);
    CHECK(pc_addons_on == 1 && r8(0xA10001) == 0x21);
    CHECK(r8(0x120) == 0x00 && r16(0x120) == 0x0000);                /* the cart's bytes (this test's cart reads 0) */
    other = pc_counts.other_writes;
    w8(0xA14000, 'S');
    w16(0xA14000, 0x5345);
    pt_port_write(4, 0xA14000, 0x53454741u);                         /* 'SEGA' */
    CHECK(pc_counts.other_writes == other);                          /* taken */
    w8(0xA14101, 0x00);                                              /* the TMSS ROM mapped */
    CHECK(r8(0x120) == 0x01 && r8(0x121) == 0x01 && r16(0x120) == 0x0101 && r8(0x122) == 0x00 && r8(0x11F) == 0x00);
    CHECK(r32(0x11E) == 0x00000101u);
    w8(0xA14101, 0x01);                                              /* the cart again */
    CHECK(r8(0x120) == 0x00);
    w16(0xA14100, 0x0000);
    CHECK(r8(0x120) == 0x01);
    pc_addons_reset();                                               /* the console's reset maps the cart */
    CHECK(r8(0x120) == 0x00);
    /* a console without TMSS: nothing answers at $A14000, and $000120 is the cart's */
    c = console_of(PC_REGION_AUTO, PC_CONSOLE_GENESIS, 0, 0, 0);
    machine(&c);
    other = pc_counts.other_writes;
    w8(0xA14101, 0x00);
    CHECK(pc_addons_on == 0 && pc_counts.other_writes == other + 1u && r8(0x120) == 0x00);
}

/* ---- the TeraDrive: the game's 128 KB video memory test, as its boot runs it */
static uint32_t vram_test(void)
{
    uint32_t v;
    w16(0xC00004, 0x8004);
    w16(0xC00004, 0x8184);                                           /* 128 KB mode on */
    w16(0xC00004, 0x4000);
    w16(0xC00004, 0x0004);                                           /* a write at $10000 */
    w16(0xC00000, 0x5A02);
    w16(0xC00004, 0x4000);
    w16(0xC00004, 0x0000);                                           /* a write at $0000 */
    w16(0xC00000, 0xA501);
    w16(0xC00004, 0x0000);
    w16(0xC00004, 0x0004);                                           /* a read at $10000 */
    v = r16(0xC00000);
    w16(0xC00004, 0x8104);                                           /* 128 KB mode off */
    return v;
}

static void test_teradrive(void)
{
    PcConsole c = console_of(PC_REGION_AUTO, PC_CONSOLE_TERADRIVE, 0, 0, 0);
    machine(&c);
    CHECK(pc_addons_machine()->teradrive && r8(0xA10001) == 0x20);
    CHECK(vram_test() == 0x5A02);
    w16(0xC00004, 0x0000);
    w16(0xC00004, 0x0000);
    CHECK(r16(0xC00000) == 0xA501);                                  /* the 64 KB memory as on every console */
    c = console_of(PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE, 0, 0, 0);
    machine(&c);
    CHECK(!pc_addons_machine()->teradrive && vram_test() == 0xA501);   /* any other console: $10000 is not there */
    c = console_of(PC_REGION_AUTO, PC_CONSOLE_TERADRIVE, 0, PC_MEGACD_AIWA, 0);
    machine(&c);
    CHECK(!pc_addons_machine()->teradrive && vram_test() == 0xA501);   /* a unit that is a whole console is the console */
}

/* ---- the version register, through the bus, and when a change takes effect */
static void test_version(void)
{
    PcConsole c = console_of(PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE, 0, 0, 0);
    machine(&c);
    CHECK(r8(0xA10001) == 0x20 && r16(0xA10000) == 0x2020);
    CHECK(pc_addons_on == 0);
    CHECK(r16(0xA130EC) == 0xFFFF && r16(BIOS + 0x128u) == 0xFFFF && r16(0xA12022) == 0xFFFF);   /* nothing there */
    c = console_of(PC_REGION_US, PC_CONSOLE_MEGADRIVE, 0, 0, 0);
    machine(&c);
    CHECK(r8(0xA10001) == 0xA0 && r16(0xA10000) == 0xA0A0);
    c = console_of(PC_REGION_US, PC_CONSOLE_MEGADRIVE, 0, 1, 0);
    machine(&c);
    CHECK(r8(0xA10001) == 0x80 && r16(0xA10000) == 0x8080);
    /* a new console takes effect at the next reset, also across the I/O chip's own reset */
    c = console_of(PC_REGION_AUTO, PC_CONSOLE_GENESIS, 0, 0, 0);
    pc_addons_configure(&c);
    CHECK(r8(0xA10001) == 0x80);
    pc_io_reset();
    CHECK(r8(0xA10001) == 0x80);
    pc_addons_reset();
    CHECK(r8(0xA10001) == 0xA0);
    pc_io_reset();
    CHECK(r8(0xA10001) == 0xA0);
    CHECK(pc_addons_console()->console == PC_CONSOLE_GENESIS);
}

/* ---- the 32X */
static void test_x32(void)
{
    PcConsole c = console_of(PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE, 1, 0, 0);
    machine(&c);
    CHECK(pc_addons_on == 1);
    CHECK(r16(0xA130EC) == 0x4D41 && r16(0xA130EE) == 0x5253 && r32(0xA130EC) == 0x4D415253u);
    CHECK(r8(0xA130EC) == 0x4D && r8(0xA130ED) == 0x41 && r8(0xA130EF) == 0x53);
    CHECK(r16(0xA130FC) == 0xFFFF && r16(0xA130F0) == 0xFFFF);
    CHECK(r16(BIOS + 0x128u) == 0xFFFF && r16(0xA12022) == 0xFFFF);  /* no Mega CD with it */
    CHECK(r8(0xA10001) == 0x20);
}

/* ---- the Mega CD's identification and the gate array, as the stub answers them */
static void test_megacd(void)
{
    PcConsole c = console_of(PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE, 0, 1, 0);
    machine(&c);
    CHECK(r8(0xA10001) == 0x00);                                     /* bit 5 clear: a Mega CD is attached */
    CHECK(r16(BIOS + 0x128u) == 0x424F && r8(BIOS + 0x128u) == 'B' && r8(BIOS + 0x129u) == 'O');
    CHECK(r16(BIOS + 0x14Au) == 0x0031 && r8(BIOS + 0x14Bu) == '1');
    CHECK(r16(BIOS + 0x120u) == 0 && r16(BIOS + 0x124u) == 0 && r16(BIOS + 0x126u) == 0 && r16(BIOS + 0x12Cu) == 0 && r16(BIOS + 0x14Cu) == 0 &&
          r16(BIOS + 0x14Eu) == 0);
    CHECK(r16(BIOS + 0x100u) == 0xFFFF && r16(BIOS) == 0xFFFF && r16(WORD_RAM) == 0xFFFF);  /* not compared: nothing there */
    CHECK(r16(PRG + 0x290u) == 0x4D42);
    w16(0x420000, 0x55AA);
    CHECK(r16(0x420000) == 0x55AA);
    w8(PRG + 1u, 0x11);
    CHECK(r16(0x420000) == 0x5511);
    CHECK(r16(0xA12022) == 0x5700 && r8(0xA12022) == 'W');
    w8(0xA12001, 0x03);
    CHECK(r16(0xA12000) == 0x0003 && r8(0xA12001) == 0x03 && r8(0xA12000) == 0);
    w16(0xA12002, 0xFF00);
    CHECK(r16(0xA12002) == 0xFF01);                                  /* RET reads 1 */
    w8(0xA12003, 0x46);
    CHECK(r8(0xA12003) == 0x43 && (r8(0xA12003) & 5) == 1);          /* bit 2 MODE reads 0, bit 0 RET reads 1, the rest as written */
    w16(0xA12010, 0x55AA);
    CHECK(r16(0xA12010) == 0x55AA);
    w8(0xA12011, 0x80);
    CHECK(r8(0xA12021) == 0x80 && r16(0xA12010) == 0x5580);          /* the sub-CPU's echo of $A12011 */
    w16(0xA12012, 0x1234);
    CHECK(r16(0xA12012) == 0x1234 && r16(0xA12030) == 0);
    CHECK(r16(0xA12024) == 0 && (r16(0xA12020) & 0x8000) == 0);      /* no brain: never busy, no word */
    pc_addons_reset();                                               /* the console's reset clears the gate array */
    CHECK(r16(0xA12000) == 0 && r16(0xA12010) == 0);
}

/* ---- RV32 code for the made-up brains: encodings from the RISC-V unprivileged specification */
static uint32_t rv_r(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) { return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op; }
static uint32_t rv_i(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) { return ((uint32_t)imm & 0xFFFu) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op; }
static uint32_t rv_s(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3)
{
    uint32_t u = (uint32_t)imm;
    return (u >> 5 & 0x7Fu) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (u & 0x1Fu) << 7 | 0x23u;
}
static uint32_t rv_b(int32_t off, uint32_t rs2, uint32_t rs1, uint32_t f3)
{
    uint32_t u = (uint32_t)off;
    return (u >> 12 & 1u) << 31 | (u >> 5 & 0x3Fu) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (u >> 1 & 0xFu) << 8 | (u >> 11 & 1u) << 7 | 0x63u;
}
static uint32_t rv_j(int32_t off, uint32_t rd)
{
    uint32_t u = (uint32_t)off;
    return (u >> 20 & 1u) << 31 | (u >> 1 & 0x3FFu) << 21 | (u >> 11 & 1u) << 20 | (u >> 12 & 0xFFu) << 12 | rd << 7 | 0x6Fu;
}
static uint32_t rv_lui(uint32_t rd, uint32_t v) { return (v & 0xFFFFF000u) | rd << 7 | 0x37u; }
static uint32_t rv_addi(uint32_t rd, uint32_t rs1, int32_t imm) { return rv_i(imm, rs1, 0, rd, 0x13); }
#define RV_RET 0x00008067u                                          /* jalr x0, 0(ra) */

typedef struct Prog { uint32_t w[256]; int n; } Prog;
static void emit(Prog *p, uint32_t w) { p->w[p->n++] = w; }
static void li(Prog *p, uint32_t rd, uint32_t v)                     /* lui + addi, the high part rounded for addi's signed low part */
{
    emit(p, rv_lui(rd, v + 0x800u));
    emit(p, rv_addi(rd, rd, (int32_t)(v << 20) >> 20));
}

enum { ZERO, RA, SP, GP, TP, T0, T1, T2, S0, S1, A0, A1, A2, A3, A4, A5, A6, A7 };
enum { T3 = 28, T4, T5, T6 };

#define CODE_LO 0x5360u
#define H1_AT 0x8504u
#define H2_AT 0x859Cu
#define D_WORD_TEST 4000u
#define D_BUSY_TEST 9000u

static uint8_t brain_code[0xD2ECu - 0x5360u], brain_helpers[5328], brain_tables[1336];

static void put_prog(uint8_t *mem, const Prog *p) { memcpy(mem, p->w, (size_t)p->n * 4u); }

/* A made-up brain. H2 calls a helper in the work RAM's code window, which writes the two times into the counter's output, counts the calls in the
   brain's RAM, keeps the restart flag there and clears it. H1 adds the word memory's first word, the cursor, a word of the game file through the flash
   window, a table word, the first captured program word and the kept flag (as bit 12) into the word. faulty: H2 is a word that is no instruction. */
static void make_brain(int faulty)
{
    Prog h1 = {{0}, 0}, h2 = {{0}, 0}, helper = {{0}, 0};
    memset(brain_code, 0, sizeof(brain_code));
    memset(brain_helpers, 0, sizeof(brain_helpers));
    memset(brain_tables, 0, sizeof(brain_tables));
    /* helper at 0x80002C00 */
    emit(&helper, rv_lui(T2, 0x80002000u));
    li(&helper, T3, D_WORD_TEST);
    emit(&helper, rv_s(0x234, T3, T2, 2));                           /* sw D_WORD, cnt_out + 4 */
    li(&helper, T3, D_BUSY_TEST);
    emit(&helper, rv_s(0x238, T3, T2, 2));                           /* sw D_BUSY, cnt_out + 8 */
    emit(&helper, rv_i(0x400, T2, 2, T4, 0x03));                     /* lw the call count */
    emit(&helper, rv_addi(T4, T4, 1));
    emit(&helper, rv_s(0x400, T4, T2, 2));
    emit(&helper, rv_i(0x2BE, T2, 5, T5, 0x03));                     /* lhu the restart flag */
    emit(&helper, rv_s(0x404, T5, T2, 1));                           /* sh it at 0x80002404 */
    emit(&helper, rv_s(0x2BE, ZERO, T2, 1));                         /* and clear it, as the brain does */
    emit(&helper, RV_RET);
    put_prog(brain_helpers, &helper);
    /* H2 */
    if (faulty) {
        emit(&h2, 0x00000000u);
    } else {
        emit(&h2, rv_addi(SP, SP, -16));
        emit(&h2, rv_s(12, RA, SP, 2));
        emit(&h2, rv_lui(T0, 0x80003000u));
        emit(&h2, rv_addi(T0, T0, -0x400));                          /* 0x80002C00 */
        emit(&h2, rv_i(0, T0, 0, RA, 0x67));                         /* jalr ra, 0(t0) */
        emit(&h2, rv_i(12, SP, 2, RA, 0x03));
        emit(&h2, rv_addi(SP, SP, 16));
        emit(&h2, RV_RET);
    }
    put_prog(brain_code + (H2_AT - CODE_LO), &h2);
    /* H1 */
    emit(&h1, rv_lui(T0, 0x08020000u));
    emit(&h1, rv_i(0, T0, 5, A0, 0x03));                             /* lhu the word memory's $600000 */
    emit(&h1, rv_lui(T1, 0x08012000u));
    emit(&h1, rv_i(0, T1, 5, T2, 0x03));                             /* lhu the cursor */
    emit(&h1, rv_r(0, T2, A0, 0, A0, 0x33));
    emit(&h1, rv_lui(T3, 0x04000000u));
    emit(&h1, rv_i(0x100, T3, 5, T4, 0x03));                         /* lhu the game file's word at $000100 */
    emit(&h1, rv_r(0, T4, A0, 0, A0, 0x33));
    emit(&h1, rv_lui(T5, 0x80004000u));
    emit(&h1, rv_i(0x110, T5, 5, T6, 0x03));                         /* lhu the first table word, 0x80004110 */
    emit(&h1, rv_r(0, T6, A0, 0, A0, 0x33));
    emit(&h1, rv_lui(T5, 0x80002000u));
    emit(&h1, rv_i(-0x7E0, T5, 5, T6, 0x03));                        /* lhu 0x80001820, the first captured word */
    emit(&h1, rv_r(0, T6, A0, 0, A0, 0x33));
    emit(&h1, rv_i(0x404, T5, 5, T6, 0x03));                         /* lhu the kept flag */
    emit(&h1, rv_i(12, T6, 1, T6, 0x13));                            /* slli 12 */
    emit(&h1, rv_r(0, T6, A0, 0, A0, 0x33));
    emit(&h1, RV_RET);
    put_prog(brain_code + (H1_AT - CODE_LO), &h1);
    brain_tables[0] = 0x00;
    brain_tables[1] = 0x50;                                          /* the table word 0x5000 */
    pc_brain_load_parts(brain_code, sizeof(brain_code), brain_helpers, sizeof(brain_helpers), brain_tables, sizeof(brain_tables));
}

/* The game's Mega CD start, as its own sequence goes: the sub-CPU's bus request and reset, the program's tables while it is held, then its release. */
static void boot_megacd(void)
{
    w16(0xA12002, 0xFF00);
    w8(0xA12001, 3);
    w8(0xA12001, 2);
    w8(0xA12001, 0);
    w8(0xA12003, 0);
    w16(PRG + 0x5F4u, 0x0700);                                       /* the first captured word of the program, bank 0, the sub-CPU held */
    w8(0xA12003, 0x40);
    w16(PRG + 0x5F6u, 0x1111);                                       /* bank 1: not captured */
    w8(0xA12003, 0);
    w8(0xA12001, 1);                                                 /* released */
}

static uint64_t release_mclk(uint32_t cycles)                       /* the test's own figure: 16 + ceil(cycles x 8.590908) clocks of 107.39 MHz, two to a master clock */
{
    uint64_t clk = 16u + ((uint64_t)cycles * 2147727u + 249999u) / 250000u;    /* clock figures, not a game address */
    return (clk + 1u) / 2u;
}

static uint32_t post(void)                                           /* the game's update: bit 7 of the echo turned over, low bits 2 */
{
    uint32_t v = ((r8(0xA12021) ^ 0x80u) & 0x80u) + 2u;
    w8(0xA12011, v);
    return v;
}

/* ---- the link, with a made-up brain */
static void test_link(void)
{
    PcConsole c = console_of(PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE, 0, 1, 1);
    uint64_t t;
    uint32_t want, word1;
    make_brain(0);
    machine(&c);
    pt_mem_w16(pt_rom, 0x100u, 0x0300u);
    pt_mem_w16(pt_ram, 0x9920u, 0x0002u);
    go_to(10000);
    boot_megacd();
    CHECK(pc_brain_ready());
    /* the release wrote the sub-CPU's start-up pattern into the mirror: the terrain is $00FF words */
    CHECK(pc_brain_sram()[TERRAIN] == 0xFF && pc_brain_sram()[TERRAIN + 1u] == 0x00 && pc_brain_sram()[TERRAIN + 0x1080u] == 0x00);
    /* the word memory as the game writes it, each 68000 word a 16-bit value: a byte write lands in its own half */
    w16(WORD_RAM, 0x0011);
    CHECK(pc_brain_sram()[PC_BRAIN_WORD_RAM_OFF] == 0x11 && pc_brain_sram()[PC_BRAIN_WORD_RAM_OFF + 1u] == 0x00);
    w8(WORD_RAM + 1u, 0x10);
    w8(WORD_RAM, 0x00);
    CHECK(pc_brain_sram()[PC_BRAIN_WORD_RAM_OFF] == 0x10 && pc_brain_sram()[PC_BRAIN_WORD_RAM_OFF + 1u] == 0x00);
    w16(0x600000, 0x0011);
    /* not an update: low bits other than 2, or bit 7 unchanged */
    w8(0xA12011, 0x83);
    w8(0xA12011, 0x82);
    CHECK((r16(0xA12020) & 0x8000) == 0);
    /* an update: busy at once; the word at its moment and not a master clock before; busy until its own */
    go_to(20000);
    t = pc_clock_now();
    post();
    CHECK((r16(0xA12020) & 0x8000) != 0 && r16(0xA12024) == 0);
    want = (0x0011u + 0x0002u + 0x0300u + 0x5000u + 0x0700u + 0x1000u) & 0xFFFFu;    /* the flag was raised for the first call after the release */
    go_to(t + release_mclk(D_WORD_TEST) - 1u);
    CHECK(r16(0xA12024) == 0);
    go_to(t + release_mclk(D_WORD_TEST));
    word1 = r16(0xA12024);
    CHECK(word1 == want);
    CHECK(r8(0xA12024) == (want >> 8) && r8(0xA12025) == (want & 0xFFu));
    CHECK((r16(0xA12020) & 0x8000) != 0);
    go_to(t + release_mclk(D_BUSY_TEST) - 1u);
    CHECK((r16(0xA12020) & 0x8000) != 0);
    go_to(t + release_mclk(D_BUSY_TEST));
    CHECK((r16(0xA12020) & 0x8000) == 0 && r16(0xA12024) == want);
    /* the next update: the flag is down now, the cursor moved */
    pt_mem_w16(pt_ram, 0x9920u, 0x0006u);
    t = pc_clock_now() + 100u;
    go_to(t);
    post();
    go_to(t + release_mclk(D_WORD_TEST));
    CHECK(r16(0xA12024) == ((want - 0x1000u + 4u) & 0xFFFFu));
    /* AI off in the middle: busy drops at once, the word in flight never comes, the last one stays */
    go_to(t + release_mclk(D_BUSY_TEST) + 10u);
    pt_mem_w16(pt_ram, 0x9920u, 0x0008u);
    t = pc_clock_now();
    post();
    go_to(t + 100u);
    w8(0xA12001, 3);
    CHECK((r16(0xA12020) & 0x8000) == 0);
    go_to(t + release_mclk(D_BUSY_TEST) + 10u);
    CHECK(r16(0xA12024) == ((want - 0x1000u + 4u) & 0xFFFFu));
    /* an update while the sub-CPU's bus is requested: the Pocket's link raises busy as for any update, and the halted brain answers nothing, so
       busy stays up and no word comes until the sub-CPU's next reset (the game sends none then: it requests the bus only to stop the partner) */
    t = pc_clock_now();
    post();
    CHECK((r16(0xA12020) & 0x8000) != 0);
    go_to(t + release_mclk(D_BUSY_TEST) + 10u);
    CHECK((r16(0xA12020) & 0x8000) != 0 && r16(0xA12024) == ((want - 0x1000u + 4u) & 0xFFFFu));
    /* the sub-CPU's reset clears its word and busy; after its release the pattern is written again and the flag is raised again */
    w8(0xA12001, 2);
    CHECK(r16(0xA12024) == 0 && (r16(0xA12020) & 0x8000) == 0);
    w16(WORD_RAM + 0x8000u, 0x1234);
    w8(0xA12001, 0);
    w8(0xA12001, 1);
    CHECK(pc_brain_sram()[TERRAIN] == 0xFF && pc_brain_sram()[TERRAIN + 1u] == 0x00);
    CHECK(pc_brain_sram()[PC_BRAIN_WORD_RAM_OFF] == 0x00);           /* the objects' pattern is 0 */
    w16(0x600000, 0x0011);
    pt_mem_w16(pt_ram, 0x9920u, 0x0002u);
    t = pc_clock_now() + 50u;
    go_to(t);
    post();
    go_to(t + release_mclk(D_WORD_TEST));
    CHECK(r16(0xA12024) == want);
    /* an update on top of one still in flight: the first one's word never comes */
    go_to(t + release_mclk(D_BUSY_TEST) + 10u);
    pt_mem_w16(pt_ram, 0x9920u, 0x0003u);
    t = pc_clock_now();
    post();
    go_to(t + 200u);
    pt_mem_w16(pt_ram, 0x9920u, 0x0005u);
    post();
    go_to(t + release_mclk(D_WORD_TEST) + 10u);
    CHECK(r16(0xA12024) == want);                                    /* the first one's word (cursor 3) was dropped; the second's is not due yet */
    go_to(t + 200u + release_mclk(D_WORD_TEST));
    CHECK(r16(0xA12024) == ((want - 0x1000u + 3u) & 0xFFFFu));
    /* the program memory is captured only in bank 0 with the sub-CPU held */
    CHECK(pc_brain_sram()[CAPTURED] == 0x00 && pc_brain_sram()[CAPTURED + 1u] == 0x07 && pc_brain_sram()[CAPTURED + 2u] == 0x00);
}

/* ---- no brain, and a brain that stops: the stub's own answers */
static void test_no_brain(void)
{
    PcConsole c = console_of(PC_REGION_AUTO, PC_CONSOLE_MEGADRIVE, 0, 1, 0);
    uint64_t t;
    make_brain(0);
    machine(&c);                                                     /* cpumate=off */
    go_to(10000);
    boot_megacd();
    w16(0x600000, 0x0011);
    t = pc_clock_now();
    post();
    CHECK((r16(0xA12020) & 0x8000) == 0);
    go_to(t + release_mclk(D_BUSY_TEST) + 10u);
    CHECK(r16(0xA12024) == 0 && !pc_addons_brain_ready());
    c.cpumate = 1;
    make_brain(1);
    machine(&c);
    go_to(10000);
    boot_megacd();
    t = pc_clock_now();
    post();
    CHECK(!pc_brain_ready() && (r16(0xA12020) & 0x8000) == 0);
    go_to(t + release_mclk(D_BUSY_TEST) + 10u);
    CHECK(r16(0xA12024) == 0);
    /* a brain file that is not there, and one that is not Little-Man Firmware */
    {
        char why[300];
        FILE *f;
        CHECK(pc_brain_load("no_such_brain_file.bin", why, sizeof(why)) == -1);
        f = fopen("not_firmware.bin", "wb");
        if (f) {
            fputs("not a firmware file", f);
            fclose(f);
        }
        CHECK(pc_brain_load("not_firmware.bin", why, sizeof(why)) == -2 && strstr(why, "not Little-Man Firmware") != NULL);
        remove("not_firmware.bin");
    }
}

/* ---- the interpreter on programs of our own */
static uint8_t code_mem[4096], data_mem[4096];

static int run_prog(PcRv32 *cpu, const Prog *p, uint64_t steps)
{
    memset(cpu, 0, sizeof(*cpu));
    memset(code_mem, 0, sizeof(code_mem));
    put_prog(code_mem, p);
    pc_rv32_map(cpu, 0x1000u, sizeof(code_mem), code_mem, 0, 1);
    pc_rv32_map(cpu, 0x2000u, sizeof(data_mem), data_mem, 1, 0);
    cpu->x[SP] = 0x2800u;
    return pc_rv32_call(cpu, 0x1000u, 0xFFFFFFF0u, steps);
}

static void test_rv32(void)
{
    PcRv32 cpu;
    Prog p = {{0}, 0};
    /* arithmetic, shifts, compares */
    li(&p, A0, 0x80000000u);
    li(&p, A1, 0xFFFFFFFFu);
    emit(&p, rv_r(0x20, A1, A0, 5, A2, 0x33));                       /* sra: 0x80000000 >> 31 arithmetic = -1 */
    emit(&p, rv_r(0x00, A1, A0, 5, A3, 0x33));                       /* srl: = 1 */
    emit(&p, rv_r(0x01, A1, A0, 4, A4, 0x33));                       /* div: the one overflow gives the dividend */
    emit(&p, rv_r(0x01, ZERO, A0, 5, A5, 0x33));                     /* divu by zero: all ones */
    emit(&p, rv_r(0x01, ZERO, A1, 6, A6, 0x33));                     /* rem by zero: the dividend */
    emit(&p, rv_r(0x01, A1, A0, 6, A7, 0x33));                       /* rem overflow: 0 */
    emit(&p, rv_r(0x01, A1, A0, 1, T0, 0x33));                       /* mulh(-2^31, -1) = 0 */
    emit(&p, rv_r(0x01, A1, A0, 3, T1, 0x33));                       /* mulhu(2^31, 2^32 - 1) = 0x7FFFFFFF */
    emit(&p, rv_r(0x01, A1, A0, 2, T2, 0x33));                       /* mulhsu(-2^31, 2^32 - 1) = 0x80000000 */
    emit(&p, rv_r(0x00, A1, A0, 2, S0, 0x33));                       /* slt -2^31 < -1 = 1 */
    emit(&p, rv_r(0x00, A1, A0, 3, S1, 0x33));                       /* sltu 2^31 < 2^32-1 = 1 */
    emit(&p, rv_i(-1, A0, 3, T3, 0x13));                             /* sltiu with -1 = all ones: 1 */
    emit(&p, rv_i(0x400 | 4, A0, 5, T4, 0x13));                      /* srai 4 */
    emit(&p, RV_RET);
    CHECK(run_prog(&cpu, &p, 1000) == PC_RV32_DONE);
    CHECK(cpu.x[A2] == 0xFFFFFFFFu && cpu.x[A3] == 1u && cpu.x[A4] == 0x80000000u && cpu.x[A5] == 0xFFFFFFFFu && cpu.x[A6] == 0xFFFFFFFFu && cpu.x[A7] == 0u);
    CHECK(cpu.x[T0] == 0u && cpu.x[T1] == 0x7FFFFFFFu && cpu.x[T2] == 0x80000000u && cpu.x[S0] == 1u && cpu.x[S1] == 1u && cpu.x[T3] == 1u && cpu.x[T4] == 0xF8000000u);
    /* loads and stores, sign and zero extension, little-endian */
    p.n = 0;
    li(&p, A0, 0x8081F0F1u);
    emit(&p, rv_s(0, A0, SP, 2));
    emit(&p, rv_i(0, SP, 0, A1, 0x03));                              /* lb  0xF1 -> -15 */
    emit(&p, rv_i(0, SP, 4, A2, 0x03));                              /* lbu 0xF1 */
    emit(&p, rv_i(2, SP, 1, A3, 0x03));                              /* lh  0x8081 -> sign */
    emit(&p, rv_i(2, SP, 5, A4, 0x03));                              /* lhu */
    emit(&p, rv_s(5, A0, SP, 0));                                    /* sb */
    emit(&p, rv_i(4, SP, 2, A5, 0x03));
    emit(&p, rv_j(8, ZERO));                                         /* jal over one word */
    emit(&p, rv_addi(A6, ZERO, 1));
    emit(&p, rv_b(8, ZERO, ZERO, 1));                                /* bne zero, zero: not taken */
    emit(&p, rv_addi(A7, ZERO, 7));
    emit(&p, rv_b(-4, A7, A7, 5));                                   /* bge a7, a7: always taken, back to the addi, for ever */
    emit(&p, RV_RET);
    memset(data_mem, 0, sizeof(data_mem));
    CHECK(run_prog(&cpu, &p, 1000) == PC_RV32_TOO_LONG);             /* the backward bge loops: the step limit stops it */
    CHECK(cpu.x[A1] == 0xFFFFFFF1u && cpu.x[A2] == 0xF1u && cpu.x[A3] == 0xFFFF8081u && cpu.x[A4] == 0x8081u && cpu.x[A5] == 0x0000F100u && cpu.x[A6] == 0u);
    /* the stops */
    p.n = 0;
    emit(&p, rv_i(0, ZERO, 2, A0, 0x03));                            /* lw from 0 */
    CHECK(run_prog(&cpu, &p, 10) == PC_RV32_LOAD && cpu.fault_addr == 0u && cpu.fault_pc == 0x1000u);
    p.n = 0;
    emit(&p, rv_s(1, ZERO, SP, 2));                                  /* sw misaligned */
    CHECK(run_prog(&cpu, &p, 10) == PC_RV32_MISALIGNED);
    p.n = 0;
    li(&p, A0, 0x1000u);
    emit(&p, rv_s(0, ZERO, A0, 2));                                  /* sw into the code: not writable */
    CHECK(run_prog(&cpu, &p, 10) == PC_RV32_STORE);
    p.n = 0;
    emit(&p, 0x00000073u);                                           /* ecall */
    CHECK(run_prog(&cpu, &p, 10) == PC_RV32_ILLEGAL);
    p.n = 0;
    li(&p, A0, 0x2000u);
    emit(&p, rv_i(0, A0, 0, ZERO, 0x67));                            /* jalr into data: no code there */
    CHECK(run_prog(&cpu, &p, 10) == PC_RV32_FETCH);
    p.n = 0;
    emit(&p, rv_j(2, ZERO));                                         /* jal to 0x1002 */
    CHECK(run_prog(&cpu, &p, 10) == PC_RV32_MISALIGNED);
    p.n = 0;
    emit(&p, rv_addi(ZERO, ZERO, 5));                                /* x0 stays 0 */
    emit(&p, RV_RET);
    CHECK(run_prog(&cpu, &p, 10) == PC_RV32_DONE && cpu.x[0] == 0u);
    /* signed division by zero; jalr drops bit 0 of its target */
    p.n = 0;
    li(&p, A0, 1234u);
    emit(&p, rv_r(0x01, ZERO, A0, 4, A1, 0x33));                     /* div by zero: all ones */
    li(&p, A2, 0x1000u + 7u * 4u + 1u);                              /* the address of the second addi below (word 7), plus 1 */
    emit(&p, rv_i(0, A2, 0, ZERO, 0x67));                            /* jalr x0, 0(a2) */
    emit(&p, rv_addi(A3, ZERO, 1));                                  /* skipped */
    emit(&p, rv_addi(A4, ZERO, 2));                                  /* lands here */
    emit(&p, RV_RET);
    CHECK(run_prog(&cpu, &p, 20) == PC_RV32_DONE && cpu.x[A1] == 0xFFFFFFFFu && cpu.x[A3] == 0u && cpu.x[A4] == 2u);
}

/* ---- a program from a file, for the comparison with unicorn: words of code at 0x1000, then 32 registers, then 4 KB of data at 0x2000. Prints
   the stop, the registers and the data. */
static void test_rv_file(const char *path)
{
    static uint32_t buf[2048];
    PcRv32 cpu;
    FILE *f = fopen(path, "rb");
    size_t n, i, words;
    int stop;
    if (!f) {
        CHECK(!"rvfile: no file");
        return;
    }
    n = fread(buf, 4, sizeof(buf) / 4, f);
    fclose(f);
    words = buf[0];
    CHECK(n >= 1 + words + 32 + 1024 && words <= 1024);
    if (n < 1 + words + 32 + 1024 || words > 1024)
        return;
    memset(&cpu, 0, sizeof(cpu));
    memset(code_mem, 0, sizeof(code_mem));
    memcpy(code_mem, buf + 1, words * 4u);
    memcpy(cpu.x, buf + 1 + words, 32 * 4);
    cpu.x[0] = 0;
    memcpy(data_mem, buf + 1 + words + 32, 4096);
    pc_rv32_map(&cpu, 0x1000u, sizeof(code_mem), code_mem, 0, 1);
    pc_rv32_map(&cpu, 0x2000u, sizeof(data_mem), data_mem, 1, 0);
    stop = pc_rv32_call(&cpu, 0x1000u, cpu.x[1], 100000);          /* a step limit, not a game address */
    printf("stop %d\n", stop);
    for (i = 0; i < 32; i++)
        printf("x%u %08X\n", (unsigned)i, (unsigned)cpu.x[i]);
    for (i = 0; i < 4096; i += 4) {
        uint32_t v;
        memcpy(&v, data_mem + i, 4);
        printf("m%03X %08X\n", (unsigned)i, (unsigned)v);
    }
}

static const struct { const char *name; void (*run)(void); } groups[] = {
    { "settings", test_settings }, { "version", test_version }, { "x32", test_x32 }, { "megacd", test_megacd },
    { "models", test_models }, { "tmss", test_tmss }, { "teradrive", test_teradrive },
    { "link", test_link }, { "nobrain", test_no_brain }, { "rv32", test_rv32 },
};

int main(int argc, char **argv)
{
    uint32_t i;
    const char *only = argc > 1 ? argv[1] : NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    rec = (PcFrame *)malloc(sizeof(PcFrame));
    pt_rom = (u8 *)calloc(1, 0x10000u);
    pt_rom_size = 0x10000u;
    pt_rom_bus_end = 0x10000u;
    pt_low_device_end = 0x10000u;
    if (only && !strcmp(only, "rvfile") && argc > 2) {
        test_rv_file(argv[2]);
    } else {
        for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
            if (!only || !strcmp(only, "all") || !strcmp(only, groups[i].name))
                groups[i].run();
    }
    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fails ? 1 : 0;
}
