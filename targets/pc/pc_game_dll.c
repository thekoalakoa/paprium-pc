/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Loads the game DLL the launcher built: see pc_game_dll.h.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "pt_rt.h"
#include "pc_game_dll.h"

typedef const PtEntry *(*MapFn)(void);
typedef u32 (*CountFn)(void);
typedef int (*AbiFn)(void);
typedef const char *(*TextFn)(void);

/* The DLL's path as wide characters when it is UTF-8 text, so a folder with any letters works whatever the program's code page. Windows' own error
 * box (a "bad image" box for a damaged file, for one) is turned off while the DLL loads, so the error comes back here, and to the player in our
 * words, and no box of Windows' opens first. The error code is kept across the restore. */
static HMODULE load(const char *path)
{
    wchar_t wide[MAX_PATH * 4];
    UINT mode = GetErrorMode();
    HMODULE h;
    DWORD code;
    SetErrorMode(mode | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (int)(sizeof(wide) / sizeof(wide[0]))) > 0)
        h = LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    else
        h = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    code = GetLastError();
    SetErrorMode(mode);
    SetLastError(code);
    return h;
}

static int exists(const char *path)
{
    wchar_t wide[MAX_PATH * 4];
    DWORD a;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (int)(sizeof(wide) / sizeof(wide[0]))) > 0)
        a = GetFileAttributesW(wide);
    else
        a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Why Windows would not load the DLL, in a sentence after Windows' own words, only when its error code says so; empty otherwise. The launcher names
 * blocked programs by the same codes (pl_launcher.c, blocked_by). program is this program's file name: the DLL imports from PC_GAME_PROGRAM, so a
 * program under another name cannot give the DLL what it needs. */
static const char *refused_because(DWORD code, const char *program)
{
    switch (code) {
    case 225:       /* ERROR_VIRUS_INFECTED */
    case 226:       /* ERROR_VIRUS_DELETED */
        return " A virus scanner blocked it.";
    case 577:       /* ERROR_INVALID_IMAGE_HASH: a signature the rules require is missing */
    case 1260:      /* ERROR_ACCESS_DISABLED_BY_POLICY */
    case 4551:      /* ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION */
    case 4553:      /* ERROR_SYSTEM_INTEGRITY_POLICY_NOT_SIGNED */
        return " Windows' rules for which programs may run blocked it; Smart App Control applies such rules to programs that are not signed.";
    case 193:       /* ERROR_BAD_EXE_FORMAT */
    case 1392:      /* ERROR_FILE_CORRUPT */
        return " The file is damaged, or it is not a program at all.";
    case 126:       /* ERROR_MOD_NOT_FOUND: a file it imports from is not there */
        if (_stricmp(program, PC_GAME_PROGRAM) != 0)
            return " It takes what it needs from this program by the program's file name, " PC_GAME_PROGRAM ", and this program has another name.";
        return "";
    }
    return "";
}

int pc_game_dll_load(const char *path, const char *image_check, char *why, size_t why_size, char *advice, size_t advice_size)
{
    HMODULE h;
    MapFn map;
    CountFn count;
    AbiFn abi;
    TextFn image;
    char program[MAX_PATH * 3] = "";
    const char *name;

    if (!exists(path)) {
        snprintf(why, why_size, "The game has not been built on this PC yet: there is no %s.", path);
        snprintf(advice, advice_size, "Start Paprium with the launcher, which builds it from your own copy of the game.");
        return PC_GAME_DLL_NO_FILE;
    }
    h = load(path);
    if (!h) {
        DWORD code = GetLastError();
        char words[400] = "";
        int n = (int)FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, words, sizeof(words), NULL);
        while (n > 0 && (words[n - 1] == '\n' || words[n - 1] == '\r' || words[n - 1] == ' ' || words[n - 1] == '.'))
            words[--n] = 0;
        GetModuleFileNameA(NULL, program, sizeof(program));
        name = strrchr(program, '\\');
        name = name ? name + 1 : program;
        snprintf(why, why_size, "Windows would not load the game that was built on this PC (%s, error %lu: %s).%s", path, (unsigned long)code,
                 words[0] ? words : "no description", refused_because(code, name));
        if (_stricmp(name, PC_GAME_PROGRAM) != 0)
            /* The built game itself is fine here, and the launcher starts it as it is once the program has its name back. */
            snprintf(advice, advice_size, "Give this program back its name, " PC_GAME_PROGRAM "; it is %s now. Then start the launcher.", name);
        else
            snprintf(advice, advice_size, "The launcher builds it again when you start it.");
        return PC_GAME_DLL_REFUSED;
    }
    map = (MapFn)(void *)GetProcAddress(h, "pt_game_map");
    count = (CountFn)(void *)GetProcAddress(h, "pt_game_map_n");
    abi = (AbiFn)(void *)GetProcAddress(h, "pt_game_abi");
    image = (TextFn)(void *)GetProcAddress(h, "pt_game_image_check");
    if (!map || !count || !abi || !image) {
        FreeLibrary(h);
        snprintf(why, why_size, "%s is not a game this program built: it has none of the names a built game gives this program.", path);
        snprintf(advice, advice_size, "Delete it and start the launcher again.");
        return PC_GAME_DLL_NOT_OURS;
    }
    if (abi() != PT_GAME_ABI) {
        FreeLibrary(h);
        snprintf(why, why_size, "%s was built for another version of this program.", path);
        snprintf(advice, advice_size, "Start the launcher, which builds it again for this one.");
        return PC_GAME_DLL_OTHER_ABI;
    }
    if (image_check && image_check[0] && strcmp(image(), image_check) != 0) {
        FreeLibrary(h);
        snprintf(why, why_size, "%s was built from another copy of the game than this program checks for.", path);
        snprintf(advice, advice_size, "Start the launcher, which builds it again.");
        return PC_GAME_DLL_OTHER_IMAGE;
    }
    pt_game_attach(map(), count());
    return PC_GAME_DLL_OK;
}
