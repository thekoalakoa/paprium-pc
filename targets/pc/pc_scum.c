/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_scum.h.
 */
#include <stddef.h>
#include <string.h>

#include "pt_rt.h"
#include "pc_config.h"
#include "pc_io.h"
#include "pc_scum.h"

/* The arcade start-stage word (krikzz): the game reads it at start; the ROM holds 0x0001, the first stage. The patch source names it too
   (patches/patch_source.json, its word arcade_stage), which checks it against the player's ROM; tests/test_all.py checks that the two agree. */
#define STAGE_WORD 0x0B0A14u
/* The Arena is the game's stage 0x1E; the menu's 25. */
#define ARENA_MENU 25
#define ARENA_GAME 0x1Eu

static const char *const stage_names[PC_ARCADE_STAGES] = {
    "Block 888", "Intercom", "MTS", "Bion Mart", "Roof-Top", "Police HQ", "Ice Factory", "Reformatory", "Sanophix PU", "Central Park", "Green Cross",
    "The Docks", "Golden Of Seas", "Deep Storage", "Aquaworld", "Blue Bar", "Powder-Zone", "Bohpal B-2", "Juvanium", "Sky Spa", "Digniti KTV", "The Vault",
    "Sanophix Labs", "The Solarium", "Arena"
};

static unsigned cheats_asked;
static PcArcade asked, started;
static int stage_in_word, no_translation;
static uint32_t stage_word_rom;

const char *pc_scum_stage_name(int stage) { return stage >= 1 && stage <= PC_ARCADE_STAGES ? stage_names[stage - 1] : NULL; }

/* The Pocket core's core_top.sv: the stick for On, Multiplayer and Stick Only; its switches only for On and Multiplayer, where Mode is its coin slot;
   player 3 in its EXT socket for Multiplayer, or with 3-Player Mode; the EA 4-Way Play for 3-Player Mode with Arcade Mode Off. No NO HIT FX: it only
   changes a signal the game sends back to the stick. */
void pc_scum_ports(const PcArcade *a, PcIoPorts *p)
{
    int arcade = a->mode == PC_ARCADE_ON || a->mode == PC_ARCADE_MULTIPLAYER;
    memset(p, 0, sizeof(*p));
    p->stick = a->mode != PC_ARCADE_OFF;
    p->stick_coin = arcade;
    p->stick_ext = p->stick && (a->mode == PC_ARCADE_MULTIPLAYER || a->three_players);
    p->tap = a->three_players && a->mode == PC_ARCADE_OFF;
    if (arcade)
        p->switches = PC_STICK_ARCADE | (a->coins == PC_COINS_INFINITE ? PC_STICK_INF_CREDITS : 0u) | (a->attract ? PC_STICK_DEMO : 0u) |
                      (a->coins == PC_COINS_2_1 ? PC_STICK_TWO_COINS : 0u) | (a->mode == PC_ARCADE_MULTIPLAYER ? PC_STICK_3_PLAYERS : 0u);
}

/* The stage word's stage for these settings: the chosen stage while Arcade Mode is On or Multiplayer, otherwise none. */
static int served_stage(const PcArcade *a) { return a->mode == PC_ARCADE_ON || a->mode == PC_ARCADE_MULTIPLAYER ? a->stage : 0; }

/* The stage word for a menu stage: the ROM's own for 0, 1-24 as they are, 25 as the Arena's stage number. */
static void stage_word_set(int stage)
{
    if (stage == stage_in_word || pt_rom_size < STAGE_WORD + 2u)      /* a made-up program of a test may be smaller */
        return;
    pt_mem_w16(pt_rom, STAGE_WORD, stage == 0 ? stage_word_rom : stage == ARENA_MENU ? ARENA_GAME : (uint32_t)stage);
    stage_in_word = stage;
}

/* At the game's switch point, as the Pocket core takes the stage there: the stage the menu asks for now. */
static void at_switch_point(void) { stage_word_set(served_stage(&asked)); }

/* Before the 68000 first runs: the ports the game will find at start, the stage word, the cheats at the first switch point. */
void pc_scum_start(unsigned cheats, const PcArcade *arcade, int interpreter)
{
    PcIoPorts ports;
    no_translation = interpreter;
    stage_word_rom = pt_rom_size >= STAGE_WORD + 2u ? pt_mem_r16(pt_rom, STAGE_WORD) : 0u;
    stage_in_word = 0;
    started = *arcade;
    pc_scum_set_cheats(cheats);
    pc_scum_set_arcade(arcade);
    stage_word_set(served_stage(arcade));
    pt_switch_point_hook = at_switch_point;
    pc_scum_ports(arcade, &ports);
    pc_io_set_ports(&ports);
    pc_io_reset();
}

void pc_scum_set_cheats(unsigned cheats)
{
    cheats_asked = cheats & (PC_CHEAT_HEALTH | PC_CHEAT_PILLS | PC_CHEAT_LIVES);
    pt_switches_next = no_translation ? 0u : cheats_asked;
}

void pc_scum_set_arcade(const PcArcade *a)
{
    asked = *a;
    if (asked.mode < 0 || asked.mode >= PC_ARCADE_MODES)
        asked.mode = PC_ARCADE_OFF;
    if (asked.coins < 0 || asked.coins >= PC_COINS_SETTINGS)
        asked.coins = PC_COINS_1_1;
    if (asked.stage < 0 || asked.stage > PC_ARCADE_STAGES)
        asked.stage = 0;
    asked.attract = asked.attract != 0;
    asked.three_players = asked.three_players != 0;
}

unsigned pc_scum_cheats(void) { return cheats_asked; }
unsigned pc_scum_cheats_in_use(void) { return pt_switches; }
void pc_scum_arcade(PcArcade *out) { *out = asked; }
void pc_scum_arcade_at_start(PcArcade *out) { *out = started; }
int pc_scum_stage_served(void) { return stage_in_word; }
int pc_scum_cheats_available(void) { return !no_translation; }
