/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Compiled into the game DLL on the player's PC, beside the generated code: what the DLL hands the program. See pc_game_dll.h.
 *
 * pt_gen.h is the generated code's own header. pt_game_build.h is written by the translation step next to it and defines PT_GAME_IMAGE_CHECK, the
 * hash of the boot-patched image this translation was made from.
 */
#include "pt_gen.h"
#include "pt_game_build.h"

__declspec(dllexport) const PtEntry *pt_game_map(void) { return pt_map; }
__declspec(dllexport) u32 pt_game_map_n(void) { return pt_map_n; }
__declspec(dllexport) int pt_game_abi(void) { return PT_GAME_ABI; }
__declspec(dllexport) const char *pt_game_image_check(void) { return PT_GAME_IMAGE_CHECK; }

#if defined(__TINYC__)
/* TinyCC has no __builtin_popcount: runtime/pt_rt.h sends it here. The number of bits set in v. */
int pt_popcount(unsigned v)
{
    int n = 0;
    while (v) {
        v &= v - 1;
        n++;
    }
    return n;
}
#endif
