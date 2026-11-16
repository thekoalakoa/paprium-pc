/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Pocket core's Scum! menu on the PC: the cheats, Arcade Mode, Arcade Coins, Attract Only, the arcade stage and 3-Player Mode, as the Pocket
 * core's 0.3.0 release has them.
 *
 * THE CHEATS (cheats= in the configuration, and the menu bar's Scum! menu). Health: players take no damage, and a player's throws do no damage to
 * enemies. Pills: using a Blu pill (Down + A) does not spend one. Lives: dying does not spend a life. Any mix of the three. They are the patch
 * source's switches (patches/patch_source.json, tool/patchsrc.py): the translated game holds the game's own code and the patched code at each of their
 * places, and takes the patched code while a switch is on. A change is taken at the switch point, the last instruction of the game's pad read, which
 * runs once a frame in play, as the Pocket core takes it there: so a switch never changes between a patched place and the code it calls. All three off,
 * the game runs as it always did, byte for byte. A run with no translated game (the players' build playing through its interpreter, see
 * pc_game_dll.h) has no patched code, so the cheats cannot be used there.
 *
 * ARCADE MODE (arcade_mode=). The game's hidden ARCADE PAPRIUM mode comes from the game's own arcade code: a Grand Stick III, the game's own arcade
 * stick, plugged into port 1 (pc_io.h). As on the Pocket core:
 *   Off          no stick: the two pads, or the EA 4-Way Play with 3-Player Mode
 *   On           the stick with its ARCADE switch, Arcade Coins' and Attract Only's switches; player 1's Mode button is its coin slot, and with 3-Player
 *                Mode player 3's pad is in its EXT socket
 *   Multiplayer  On with the stick's 3 PLAYERS switch, and player 3's pad in its EXT socket
 *   Stick Only   the stick with every switch off, as a player's own stick: the normal game, and Mode is the stick's own button; with 3-Player Mode
 *                player 3's pad is in its EXT socket
 * The game swallows the first coin after power-on, as on the cart. ARCADE COINS: 1 Coin 1 Credit (no switch), 2 Coins 1 Credit (the TWO COINS
 * switch) or Inf. Credits (0012) (the INF. CREDITS switch, which the game answers by stopping on its red 0012 screen at start: that is what the game
 * does with that switch). ATTRACT ONLY: the DEMO switch, the attract demo with no game. Both act only with Arcade Mode On or Multiplayer. 3-PLAYER
 * MODE: with Arcade Mode Off, an EA 4-Way Play on the two ports, players 1, 2 and 3 in its sockets A, B and C, so the game's own three-player modes
 * work (its ARENA, with Arena open); with the stick, player 3's pad in its EXT socket. The game looks for the stick and the adapter when it starts, and
 * reads the stick's switches then, so these four need a Soft Reset.
 *
 * THE ARCADE STAGE (arcade_stage= 1 to 25). The stage the game's arcade mode starts on: the stage word krikzz found, which the game reads when an arcade
 * game starts: 1 to 24 are its stages 0x01 to 0x18, and 25, the Arena, is 0x1E. It acts only while Arcade Mode is On or Multiplayer; otherwise, and with
 * no stage chosen, the word is the game's own, its first stage. As on the Pocket core, the word follows the menu and is taken at the switch point, so a
 * stage chosen while the game runs starts the next arcade game there. Without the translated game there is no switch point: the stage word is then
 * the one the run started with.
 */
#ifndef PC_SCUM_H
#define PC_SCUM_H

#include <stdint.h>

#include "pc_io.h"

/* The menu's values, in the Pocket core's order. */
enum { PC_ARCADE_OFF, PC_ARCADE_ON, PC_ARCADE_MULTIPLAYER, PC_ARCADE_STICK_ONLY, PC_ARCADE_MODES };
enum { PC_COINS_1_1, PC_COINS_2_1, PC_COINS_INFINITE, PC_COINS_SETTINGS };

typedef struct PcArcade {
    int mode;               /* PC_ARCADE_* */
    int coins;              /* PC_COINS_* */
    int attract;            /* Attract Only */
    int three_players;      /* 3-Player Mode */
    int stage;              /* 0 for none, else 1 to PC_ARCADE_STAGES (pc_config.h) */
} PcArcade;

/* The stage names, 1 to PC_ARCADE_STAGES (pc_config.h), as the Pocket core's menu lists them. NULL for any other number. */
const char *pc_scum_stage_name(int stage);

/* What the ports hold for these settings, as the Pocket core's core_top.sv makes them. */
void pc_scum_ports(const PcArcade *arcade, PcIoPorts *ports);

/* At start, after the game file is loaded and checked: the settings the run starts with. The ports follow arcade from now until the program ends; the
   stage word is set at once. interpreter: 1 when no translated game runs, so the cheats cannot be used. */
void pc_scum_start(unsigned cheats, const PcArcade *arcade, int interpreter);

/* What the player asks for now. The cheats and the stage word are taken at the game's next switch point; the ports only by the game's next start
   (a Soft Reset). */
void pc_scum_set_cheats(unsigned cheats);
void pc_scum_set_arcade(const PcArcade *arcade);
unsigned pc_scum_cheats(void);          /* asked for */
unsigned pc_scum_cheats_in_use(void);   /* in use since the last switch point */
void pc_scum_arcade(PcArcade *out);             /* asked for */
void pc_scum_arcade_at_start(PcArcade *out);    /* the one this run started with */
int pc_scum_stage_served(void);         /* the stage word's stage now: 0 for the game's own */
int pc_scum_cheats_available(void);     /* 0 in a run with no translated game */

#endif /* PC_SCUM_H */
