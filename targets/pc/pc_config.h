/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The player's configuration: a plain text file, paprium_pc.cfg, beside the program, and the check that the game file is the one the program was built for.
 *
 * Lines read name=value. A line starting with # is a comment. The program writes this file only from its controls screen (pc_controls.h), which then
 * rewrites the controls' lines, and from its menu bar (pc_menu.h), which rewrites only the lines of the settings it changed, each in its own place, and
 * adds a line for a setting the file did not have. Every other line stays as it was.
 *   rom=PATH                the game file. A relative path starts from this file's folder (the program's, unless --config names a file elsewhere)
 *   save=PATH               the cart's battery save, by default paprium.sav beside this file. A relative path starts from this file's folder
 *   scale=N                 the window's size in whole multiples of the picture's 224 lines, by default 3
 *   display=4:3             the picture's shape (pc_window.h): 4:3, the default, the shape a television gives it; square, each pixel square; original
 *                           and corrected, the Pocket core's two Aspect Ratio shapes
 *   sprite_limits=cart|none the cart's limits are the default
 *   vsync=on|off            whether a present waits for the monitor. The game's speed never follows the monitor either way
 *   fullscreen=on|off
 *   renderer=NAME           what draws the window: opengl, the default, or direct3d, direct3d11, direct3d12, or auto for SDL's own pick. See pc_window.c for why, and for what follows when it will not start
 *   pad1=six|three          player 1's pad, pad2 player 2's and pad3 player 3's. Six buttons by default
 *   sound=on|off            on by default
 *   volume=N                the sound's level, 0 to 100, by default 100
 *   filter=NAME             the picture filter, called a look: off, the default, sharp, scanlines, crt, pvm (bvm is taken as pvm too) or curved (see pc_filter.h). Or the path of a player's own RetroArch
 *                           preset, a file ending in .slangp, which needs librashader.dll beside the program. A relative path starts from the program's folder
 *   menu_bar=on|off         the menu bar at the top of the window, on by default. In full screen it is hidden until the mouse touches the top edge or the
 *                           menu key (F10 by default) is pressed. See pc_menu.h
 *   The Pocket core's Scum! menu, all off by default, which is the game as it always was (pc_scum.h):
 *   cheats=off              or a list of health, pills and lives with commas between (health,lives): players take no damage, a Blu pill is not spent, a
 *                           death is not spent. They take effect within a frame of play
 *   arcade_mode=off         or on, multiplayer or stick_only: the Pocket core's Arcade Mode. Read when the game starts: Soft Reset after a change
 *   arcade_coins=1coin      or 2coins or infinite: 1 Coin 1 Credit, 2 Coins 1 Credit or Inf. Credits (0012), with Arcade Mode on or multiplayer
 *   attract_only=off        or on: the attract demo and no game, with Arcade Mode on or multiplayer
 *   three_players=off       or on: 3-Player Mode, player 3 in the EA 4-Way Play, or in the stick's EXT socket with Arcade Mode
 *   arcade_stage=off|1..25  the stage the arcade mode starts on (25 is the Arena), with Arcade Mode on or multiplayer. A file with an arcade_stage line
 *                           and no arcade_mode line, as earlier versions wrote it, means Arcade Mode on
 *   console=megadrive       the console the game finds: console, megacd, x32, cpumate and cpumate_brain (and an earlier version's region line), read
 *                           when it starts. See pc_console.h
 *   key_p1_up=Up            a key, by the name SDL gives it. Empty: no key. See pc_controls.h for the names of the actions
 *   pad_p1_up=dpup          an input on a pad, joy_p1_up=hat0up on a plain joystick, controller1=any, stick1=left, deadzone1=50: see pc_controls.h
 */
#ifndef PC_CONFIG_H
#define PC_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "pc_console.h"

#define PC_CONFIG_KEYS_MAX 64
#define PC_CONFIG_NAME 32
#define PC_CONFIG_VALUE 80

typedef struct PcConfigKey {
    char action[PC_CONFIG_NAME];
    char key[PC_CONFIG_NAME];
} PcConfigKey;

typedef struct PcConfig {
    char rom[1200];
    char save[1200];
    int scale;
    int shape;              /* the picture's shape: PC_SHAPE_*, 0 the 4:3 shape */
    int no_limit;           /* 1: sprites with no limit at start */
    int vsync;
    int fullscreen;
    char renderer[PC_CONFIG_NAME];
    int three_button[3];
    int sound;
    int volume;             /* 0 to 100 */
    char filter[1200];      /* a look's name or a preset file's path, as the line gave it; empty for off */
    int menu_bar;           /* 1: the menu bar, the default */
    unsigned cheats;        /* PC_CHEAT_* bits */
    int arcade_stage;       /* 0 for off, else 1 to PC_ARCADE_STAGES */
    int arcade_mode;        /* PC_ARCADE_* of pc_scum.h, or -1 when no line gave it: see pc_config_arcade_mode */
    int arcade_coins;       /* PC_COINS_* of pc_scum.h */
    int attract_only;
    int three_players;      /* 3-Player Mode */
    int n_keys;
    PcConfigKey keys[PC_CONFIG_KEYS_MAX];
    int n_pads, n_joys;
    PcConfigKey pads[PC_CONFIG_KEYS_MAX];   /* pad_ lines: the action and the input's name */
    PcConfigKey joys[PC_CONFIG_KEYS_MAX];   /* joy_ lines */
    char controller[3][PC_CONFIG_VALUE];    /* controller1 to controller3 as written, or empty when not given */
    int stick[3];                           /* stick1 to stick3: 0 when not given, else 1 + PC_STICK_* (pc_controls.h) */
    int deadzone[3];                        /* deadzone1 to deadzone3 in percent, 0 when not given */
    PcConsole console;                      /* the console the game finds and its add-ons (pc_console.h): all zero is the default */
} PcConfig;

/* The Scum! lines' values. The cheats' bits are the Pocket core's menu values and the patch source's switch bits (patches/patch_source.json). */
enum { PC_CHEAT_HEALTH = 1, PC_CHEAT_PILLS = 2, PC_CHEAT_LIVES = 4 };
#define PC_ARCADE_STAGES 25

/* The picture's shapes, the display line's values: the program's 4:3 (the default) and square pixels, then the Pocket core's Aspect Ratio, Original
   and Corrected (pc_window.h). */
enum { PC_SHAPE_4_3, PC_SHAPE_SQUARE, PC_SHAPE_ORIGINAL, PC_SHAPE_CORRECTED, PC_SHAPES };
#define PC_SHAPE_NAMES {"4:3", "square", "original", "corrected"}
/* The arcade lines' words, in the order of pc_scum.h's values. */
#define PC_ARCADE_MODE_NAMES {"off", "on", "multiplayer", "stick_only"}
#define PC_ARCADE_COINS_NAMES {"1coin", "2coins", "infinite"}

/* Arcade Mode as the settings give it: the arcade_mode line's, or, with none, on when an arcade_stage line chose a stage and off when not, which is
   what an earlier version's arcade_stage line meant. */
int pc_config_arcade_mode(const PcConfig *config);

void pc_config_defaults(PcConfig *config);

/* Applies the text of a configuration to config. Returns 0, or the number of the first line it could not read, with the reason in error. The
   three bytes of a UTF-8 byte order mark at the start of the text are skipped. */
int pc_config_parse(const char *text, PcConfig *config, char *error, size_t error_size);

/* Reads a file. Returns 0; -1 when there is no such file; -2 when a file is there but cannot be read (held by another program, a folder of that
   name), or cannot even be reached (a folder on the way that may not be opened), with the reason in error, in Windows' own words; or a line number as
   pc_config_parse does. A byte order mark at the start is skipped. A file that holds zero bytes, as one saved as UTF-16 does, is refused as line 1,
   with words that say how to save it again. */
int pc_config_load(const char *path, PcConfig *config, char *error, size_t error_size);

/* 1 when a filter line's value names a player's preset file rather than a look: it ends in .slangp, in any case of letters. */
int pc_config_is_preset(const char *value);

/* A configuration's text with its controls replaced: every line of old_text that names a control (key_, pad_ and joy_ lines, pad1 to pad3,
   controller1 to controller3, stick1 to stick3, deadzone1 to deadzone3), and every comment line that block also holds, is left out; the rest keeps its
   order; then block.
   old_text may be NULL, for no file yet. The line ends follow old_text's: CR LF if its first line ends so, else LF. Returns the length written to out,
   or -1 when out_size is too small. A UTF-8 byte order mark at the start of old_text stays at the start, and the first line is read without it. */
int pc_config_merge_controls(const char *old_text, const char *block, char *out, size_t out_size);
/* Whether a setting's name is one of the controls' (as above). */
int pc_config_is_control(const char *name);

/* A configuration's text with some settings set: for each of the n names, the first line that sets it gets the new value in its own place, a later line
   for the same name is left out, and a name with no line gets one at the end. Every other line, comments included, stays as it was. The line ends follow
   old_text's, as pc_config_merge_controls does, and so does a byte order mark. old_text may be NULL. Returns the length written to out, or -1 when out_size
   is too small. */
int pc_config_merge_settings(const char *old_text, const char *const *names, const char *const *values, int n, char *out, size_t out_size);

/* The one way the program writes the settings file: reads it (none is fine), lets change make the new text from the old (old_text is NULL when there is
   no file; change returns the new length, or -1), then writes it through a temporary name, so a cut leaves the old file whole. Only a file that is not
   there counts as no file. Returns 0, or PC_CONFIG_NOT_READ (there but not readable, or holding zero bytes as a UTF-16 file does: such a file is
   never written over), PC_CONFIG_FILE_TOO_LARGE (the file is PC_CONFIG_REWRITE_MAX bytes or more, more than this writer reads; it is left as it is),
   PC_CONFIG_TOO_LARGE (change's text would not fit), PC_CONFIG_NOT_WRITTEN (the temporary file) or PC_CONFIG_NOT_REPLACED (the old file could not be
   replaced). */
enum { PC_CONFIG_NOT_READ = -1, PC_CONFIG_TOO_LARGE = -2, PC_CONFIG_NOT_WRITTEN = -3, PC_CONFIG_NOT_REPLACED = -4, PC_CONFIG_FILE_TOO_LARGE = -5 };
#define PC_CONFIG_REWRITE_MAX 32768     /* 32 KB: pc_config_rewrite reads a settings file only when it is smaller than this */
typedef int (*PcConfigChange)(const char *old_text, char *out, size_t out_size, void *arg);
int pc_config_rewrite(const char *path, PcConfigChange change, void *arg);

/* The cheats as the configuration writes them: "off", or the names with commas between, in the order health, pills, lives. */
void pc_config_cheats_text(unsigned cheats, char *text, size_t size);

/* The SHA-256 of a block of bytes in lower case hexadecimal, 64 characters and a zero. */
void pc_sha256_hex(const void *data, size_t bytes, char out[65]);

#endif /* PC_CONFIG_H */
