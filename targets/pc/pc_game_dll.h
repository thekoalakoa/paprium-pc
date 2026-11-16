/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The translated game as a DLL, for the players' package.
 *
 * The package holds no translated game. The launcher (pl_launcher.c) builds pc_game.dll on the player's PC from the player's own ROM, with the
 * translator, the Python and the TinyCC compiler that come in the package, and this program loads it at start. The program is built for this with
 * PT_GAME_FROM_DLL; the developers' build links the generated code in as before and never uses this file.
 *
 * What the DLL and the program share:
 *   - the DLL exports pt_game_map and pt_game_map_n (the generated code's address map), pt_game_abi (PT_GAME_ABI from runtime/pt_rt.h as the DLL
 *     was built) and pt_game_image_check (the hash of the boot-patched image the translation was made from). They are defined in
 *     pc_game_exports.c, which is compiled into the DLL.
 *   - the DLL imports the runtime's functions and data from this program, by the program's file name. pc_build.py's GAME_DLL_DATA and
 *     GAME_DLL_FUNCTIONS list them, and its game_dll_def() writes them as the .def files for the program's link and for TinyCC.
 *     So the program must keep the file name it was built with: a renamed program cannot load the DLL, and the message says so.
 */
#ifndef PC_GAME_DLL_H
#define PC_GAME_DLL_H

#include <stddef.h>

/* The DLL's place when the command line names none, below the program's folder. The launcher always names the DLL it built (in the player's
 * folder, see pl_launcher.c), so this is for a run of the program by hand. */
#define PC_GAME_DLL_DEFAULT "my_copy\\pc_game.dll"

enum {
    PC_GAME_DLL_OK = 0,
    PC_GAME_DLL_NO_FILE = 1,        /* there is no such file: the game was never built, or was deleted */
    PC_GAME_DLL_REFUSED = 2,        /* Windows would not load it: a damaged or blocked file, or a renamed program */
    PC_GAME_DLL_NOT_OURS = 3,       /* it loaded but has none of the four exports */
    PC_GAME_DLL_OTHER_ABI = 4,      /* built for another version of this program */
    PC_GAME_DLL_OTHER_IMAGE = 5     /* translated from another image than the one this program checks the game file against */
};

/* The game program's file name, as the DLL imports from it: targets/pc/pc_build.py's GAME_EXE (tests/test_launcher.py checks that the two agree). */
#define PC_GAME_PROGRAM "pc_port.exe"

/* Started by the launcher (--from-launcher), the game program prints this, then the sentence saying why it could not use the DLL, on one line of its
 * console. The launcher finds that line in game_log.txt and tells the player those words before the game plays through the interpreter. */
#define PC_GAME_DLL_SAID "the game DLL could not be used: "

/* Loads the DLL at path (UTF-8 or the code page's text), checks it against image_check (16 hexadecimal digits, or empty for no check) and hands its
 * map to the runtime. Returns one of the codes above. On failure why holds a sentence saying what went wrong, naming a virus scanner or Smart App
 * Control only when Windows' own error code says one of them blocked the file, and advice a sentence saying what a player who started the program
 * by hand can do. Windows' own error box never opens while the DLL loads: the error is only returned. */
int pc_game_dll_load(const char *path, const char *image_check, char *why, size_t why_size, char *advice, size_t advice_size);

#endif /* PC_GAME_DLL_H */
