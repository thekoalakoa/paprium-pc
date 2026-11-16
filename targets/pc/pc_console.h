/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console the game finds: which console it is, which Mega CD unit is on it, a 32X, and CPU MATE. These are lines of paprium_pc.cfg
 * (pc_config.h), each with a default that is the machine this program always was: a Japanese Mega Drive with nothing attached. The two lists are
 * the Pocket core's (its 0.3.0 menu), with the same names in the same order, and each answers the game's own checks as that core does.
 *   console=megadrive|genesis|megadrive2|genesis2|genesis3|nomad|megajet|teradrive|unknown
 *                            Mega Drive, Genesis, Mega Drive 2, Genesis Model 2, Genesis 3, Nomad, Mega Jet, TeraDrive, Unknown. The name carries
 *                            the region: Mega Drive, Mega Drive 2, Mega Jet, TeraDrive and Unknown are Japanese, the others from the USA
 *   megacd=none|megacd|megacd2|wondermega|wondermega2|laseractive|aiwa|cdx|unknown
 *                            None, Mega-CD / Sega CD, Mega-CD 2 / Sega CD 2, Wondermega, Wondermega M2 / X'Eye, LaserActive, Aiwa CSD-GM1, CDX,
 *                            Unknown CD. With any unit but None and Unknown CD the game finds that unit and offers CPU MATE in its Options. The
 *                            Unknown CD's BIOS header carries letters the game does not know, so it starts no Mega CD and offers no CPU MATE.
 *                            Nothing of a Mega CD is there
 *   x32=off|on               a 32X: the game finds one (some throwables become 32X boxes). Nothing of the 32X is there
 *   cpumate=off|on           the CPU MATE partner's brain answers the game through the Mega CD's link. It needs a Mega CD unit the game knows:
 *                            any but None and Unknown CD
 *   cpumate_brain=PATH       where the brain comes from: a copy of Little-Man Firmware's file (little_man_firmware.bin, which carries it),
 *                            or a folder holding brain.bin, cf.bin and rodata.bin. A relative path starts from the settings file's folder.
 *                            With no such line, little_man_firmware.bin beside the program the player started (for the players' build, beside
 *                            Paprium.exe), then beside the settings file; the first place that has it is used (pc_main.c, brain_places). No
 *                            brain is part of this program: see pc_brain.h
 * Lines of earlier versions keep their meaning: console=auto is a Mega Drive; region=us or region=jp with console=auto, megadrive or genesis picks
 * the Genesis or the Mega Drive, as the region decided there; megacd=on is the first Mega-CD (Sega CD) and megacd=off is none. With any other
 * console a region line does nothing, since that console's name carries its region. There is no Europe: a European console differs to the game by
 * running at 50 Hz, which this machine does not do (the team's decision: not needed for now). A region=eu line is refused as any other
 * value the program does not know.
 *
 * How the game tells consoles apart. Once per boot it reads the console's version register (its byte must be the same on both halves of the word),
 * the video chip's status, the cart's register at $1FE4 and the add-ons' identification words, runs a few tests, and keeps one word of what it found.
 * The version register gives it the region (bit 7: Japan or not; the PAL bit it takes from the video chip instead), whether an expansion unit is
 * on (bit 5 clear) and the version number. A console whose version is not 0 has TMSS: the game then checks that clearing bit 0 of $A14101 changes
 * the byte it reads at $000120 to one that is neither 00 nor FF, as a console's TMSS ROM does when it is mapped there. The first Mega Drive and
 * Genesis have version 0 and no TMSS, and are the same console to the game but for the region. The later models are told apart by the cart's own
 * word at $1FE4: its bits 8-10 say Model 2 (all three), Nomad (bit 10), Mega Jet (bits 10 and 9) or Genesis 3 (none), and bit 6 says "unknown" (a
 * console the game does not trust). A TeraDrive is found by a test of 128 KB of video memory, which only it passes. A Mega CD unit is found by the
 * letters and digits of its BIOS's header (the BIOS's window starts at $400000; the game reads its offsets 0x120-0x14F): the units that are a
 * whole console in one (Wondermega, Wondermega M2 / X'Eye, LaserActive, Aiwa CSD-GM1, CDX) are then the console, and the console tests are skipped.
 *
 * The hardware rules, as the Pocket core applies them: a unit that is a whole console sets the console (version 1, TMSS, none of the $1FE4 model
 * bits) and its region: the Aiwa and the Wondermega are Japanese, the CDX is from the USA, and the Wondermega M2 / X'Eye and the LaserActive take
 * the Console list's region, since they were sold in both. The Genesis 3, the Nomad and the Mega Jet have no expansion port, so a separate unit
 * (Mega-CD, Mega-CD 2, Unknown CD) is off with them. The 32X is left as it is set. The TeraDrive's 128 KB answer is on only without a unit that is
 * a whole console.
 *
 * The game reads all of this at boot, so the machine takes a change at its next reset (pc_addons.h).
 */
#ifndef PC_CONSOLE_H
#define PC_CONSOLE_H

#include <stdio.h>
#include <string.h>

/* The region line of earlier versions, still read (see above). */
enum { PC_REGION_AUTO, PC_REGION_US, PC_REGION_JP };
#define PC_REGION_NAMES { "auto", "us", "jp" }

/* The Console list, in the Pocket core's menu order. */
enum { PC_CONSOLE_MEGADRIVE, PC_CONSOLE_GENESIS, PC_CONSOLE_MEGADRIVE2, PC_CONSOLE_GENESIS2, PC_CONSOLE_GENESIS3, PC_CONSOLE_NOMAD, PC_CONSOLE_MEGAJET,
       PC_CONSOLE_TERADRIVE, PC_CONSOLE_UNKNOWN, PC_CONSOLES };
#define PC_CONSOLE_NAMES { "megadrive", "genesis", "megadrive2", "genesis2", "genesis3", "nomad", "megajet", "teradrive", "unknown" }
#define PC_CONSOLE_TITLES { "Mega Drive", "Genesis", "Mega Drive 2", "Genesis Model 2", "Genesis 3", "Nomad", "Mega Jet", "TeraDrive", "Unknown" }

/* The Mega CD list, in the Pocket core's menu order. */
enum { PC_MEGACD_NONE, PC_MEGACD_MEGACD, PC_MEGACD_MEGACD2, PC_MEGACD_WONDERMEGA, PC_MEGACD_WONDERMEGA2, PC_MEGACD_LASERACTIVE, PC_MEGACD_AIWA,
       PC_MEGACD_CDX, PC_MEGACD_UNKNOWN, PC_MEGACDS };
#define PC_MEGACD_NAMES { "none", "megacd", "megacd2", "wondermega", "wondermega2", "laseractive", "aiwa", "cdx", "unknown" }
#define PC_MEGACD_TITLES { "None", "Mega-CD / Sega CD", "Mega-CD 2 / Sega CD 2", "Wondermega", "Wondermega M2 / X'Eye", "LaserActive", "Aiwa CSD-GM1", \
                           "CDX", "Unknown CD" }

#define PC_BRAIN_DEFAULT "little_man_firmware.bin"

typedef struct PcConsole {
    int region;             /* PC_REGION_*: an earlier version's region line */
    int console;            /* PC_CONSOLE_* */
    int x32;
    int megacd;             /* PC_MEGACD_* */
    int cpumate;
    char brain[1200];       /* cpumate_brain as the line gave it; empty for the default */
} PcConsole;

/* The machine those lines make, after the hardware rules: what the game will find. */
typedef struct PcMachine {
    int console;            /* PC_CONSOLE_* as the Console list shows it, an earlier version's region line taken into account */
    int unit;               /* PC_MEGACD_* on the console: none when the console has no expansion port for the unit chosen */
    int combined;           /* the unit is a whole console in one, so it is the console the game finds */
    int dropped;            /* a separate unit was chosen for a console with no expansion port */
    int japan;              /* the version register's region */
    int tmss;               /* version 1, with TMSS */
    unsigned model;         /* the cart's $1FE4 model bits: 6 and 8-10 */
    int teradrive;          /* the 128 KB video memory test passes */
    int x32;
    int cpumate_unit;       /* a unit the game knows, so CPU MATE can work: any but none and the Unknown CD */
} PcMachine;

/* The version register's bits (the console's I/O chip, $A10001). Its PAL bit, 0x40, stays clear: this machine runs at 60 Hz. */
#define PC_VERSION_OVERSEAS 0x80u
#define PC_VERSION_NO_CD    0x20u
#define PC_VERSION_TMSS     0x01u

/* The cart's $1FE4 model bits for each console of the list (bits 10-8 and 6), as the Pocket core's firmware sets them. */
#define PC_MODEL_BITS_MASK  0x0740u

static inline int pc_console_pick(const char *value, const char *const *names, int n, int *out)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!strcmp(value, names[i])) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

/* One line of the configuration: -1 when the name is not one of these, else 1 when the value is good and 0 when it is not. */
static inline int pc_console_line(PcConsole *c, const char *name, const char *value)
{
    static const char *const regions[] = PC_REGION_NAMES, *const consoles[] = PC_CONSOLE_NAMES, *const units[] = PC_MEGACD_NAMES,
                      *const off_on[] = { "off", "on" };
    if (!strcmp(name, "region")) return pc_console_pick(value, regions, (int)(sizeof(regions) / sizeof(regions[0])), &c->region);
    if (!strcmp(name, "console")) {
        if (!strcmp(value, "auto")) {           /* an earlier version's default: a Mega Drive, or what its region line says */
            c->console = PC_CONSOLE_MEGADRIVE;
            return 1;
        }
        return pc_console_pick(value, consoles, PC_CONSOLES, &c->console);
    }
    if (!strcmp(name, "x32")) return pc_console_pick(value, off_on, 2, &c->x32);
    if (!strcmp(name, "megacd")) {
        if (!strcmp(value, "off") || !strcmp(value, "on")) {    /* an earlier version's lines: none, or the first Mega-CD */
            c->megacd = !strcmp(value, "on") ? PC_MEGACD_MEGACD : PC_MEGACD_NONE;
            return 1;
        }
        return pc_console_pick(value, units, PC_MEGACDS, &c->megacd);
    }
    if (!strcmp(name, "cpumate")) return pc_console_pick(value, off_on, 2, &c->cpumate);
    if (!strcmp(name, "cpumate_brain")) {
        if (strlen(value) >= sizeof(c->brain))
            return 0;
        strcpy(c->brain, value);
        return 1;
    }
    return -1;
}

/* The console as the Console list shows it: an earlier version's region line picks between the first Mega Drive and the Genesis. */
static inline int pc_console_choice(const PcConsole *c)
{
    if ((c->console == PC_CONSOLE_MEGADRIVE || c->console == PC_CONSOLE_GENESIS) && c->region != PC_REGION_AUTO)
        return c->region == PC_REGION_US ? PC_CONSOLE_GENESIS : PC_CONSOLE_MEGADRIVE;
    return c->console >= 0 && c->console < PC_CONSOLES ? c->console : PC_CONSOLE_MEGADRIVE;
}

/* The machine the lines make, with the hardware rules (see the top of this file). */
static inline void pc_console_machine(const PcConsole *c, PcMachine *m)
{
    /* per console: Japanese, TMSS, the $1FE4 model bits, no expansion port */
    static const struct { int japan, tmss; unsigned model; int no_port; } kinds[PC_CONSOLES] = {
        { 1, 0, 0x000u, 0 },    /* Mega Drive */
        { 0, 0, 0x000u, 0 },    /* Genesis */
        { 1, 1, 0x700u, 0 },    /* Mega Drive 2 */
        { 0, 1, 0x700u, 0 },    /* Genesis Model 2 */
        { 0, 1, 0x000u, 1 },    /* Genesis 3 */
        { 0, 1, 0x400u, 1 },    /* Nomad */
        { 1, 1, 0x600u, 1 },    /* Mega Jet */
        { 1, 0, 0x000u, 0 },    /* TeraDrive */
        { 1, 0, 0x040u, 0 },    /* Unknown */
    };
    int unit = c->megacd >= 0 && c->megacd < PC_MEGACDS ? c->megacd : PC_MEGACD_NONE;
    int separate = unit == PC_MEGACD_MEGACD || unit == PC_MEGACD_MEGACD2 || unit == PC_MEGACD_UNKNOWN;
    memset(m, 0, sizeof(*m));
    m->console = pc_console_choice(c);
    m->dropped = separate && kinds[m->console].no_port;
    m->unit = m->dropped ? PC_MEGACD_NONE : unit;
    m->combined = m->unit >= PC_MEGACD_WONDERMEGA && m->unit <= PC_MEGACD_CDX;
    if (m->unit == PC_MEGACD_CDX)
        m->japan = 0;
    else if (m->unit == PC_MEGACD_AIWA || m->unit == PC_MEGACD_WONDERMEGA)
        m->japan = 1;
    else
        m->japan = kinds[m->console].japan;
    m->tmss = m->combined ? 1 : kinds[m->console].tmss;
    m->model = m->combined ? 0u : kinds[m->console].model;
    m->teradrive = m->console == PC_CONSOLE_TERADRIVE && !m->combined;
    m->x32 = c->x32 != 0;
    m->cpumate_unit = m->unit != PC_MEGACD_NONE && m->unit != PC_MEGACD_UNKNOWN;
}

/* The version register as the game reads it: its region, whether a Mega CD unit is on, and version 1 (TMSS) or 0. The defaults give 0x20, the value
   this program always had. */
static inline unsigned pc_machine_version(const PcMachine *m)
{
    return (m->japan ? 0u : PC_VERSION_OVERSEAS) | (m->unit == PC_MEGACD_NONE ? PC_VERSION_NO_CD : 0u) | (m->tmss ? PC_VERSION_TMSS : 0u);
}

static inline unsigned pc_console_version(const PcConsole *c)
{
    PcMachine m;
    pc_console_machine(c, &m);
    return pc_machine_version(&m);
}

/* The console's name with its region, for messages: a unit that is a whole console names the console. */
static inline const char *pc_console_name(const PcConsole *c)
{
    static const char *const consoles[] = PC_CONSOLE_TITLES, *const units[] = PC_MEGACD_TITLES;
    static char name[80];
    PcMachine m;
    pc_console_machine(c, &m);
    snprintf(name, sizeof(name), "%s (%s)", m.combined ? units[m.unit] : m.console == PC_CONSOLE_UNKNOWN ? "Unknown console" : consoles[m.console],
             m.japan ? "Japan" : "USA");
    return name;
}

#endif /* PC_CONSOLE_H */
