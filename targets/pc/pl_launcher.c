/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The players' launcher: the one program a player starts.
 *
 * The players' package holds no part of the game. The first time, the player drops their own copy of the game file (the ROM) on this window or
 * chooses it. The launcher checks it against the hash in tools\rules.json, keeps a copy in the player's folder (below), and builds the game from it
 * on this PC: the firmware's boot tool lists what the cart's boot changes (tools\pc_boot_image.exe), the translator turns the 68000 program into C
 * (tools\translator\pl_translate.py, on the Python in tools\python), and TinyCC (tools\tcc) compiles that C into pc_game.dll there. Then it starts
 * pc_port.exe, the game program, which loads that DLL (pc_game_dll.h). Every later start finds the DLL and its record and starts the game at once.
 * A new version of the package, a changed ROM copy or a damaged DLL is seen from the record, and the game is built again. So is a game built
 * here that is not byte for byte the one the package's makers built and tested: it is built again on every start until it is, so a fresh unzip
 * of the download, after a file of the package was damaged or changed, puts it right on the next start.
 *
 * When the game cannot be built, or Windows will not load the DLL that was built, the game program plays the game through its own interpreter
 * instead (pc_port.exe --interpreter), which needs nothing built. That way plays the same game but is not the build that was tested, so the
 * window (and a message box) says so plainly before the game starts. The team's decision.
 *
 * The player's folder: %LOCALAPPDATA%\Paprium PC, the player's own app-data folder, so the folder the player unzipped (and may copy or zip again)
 * never holds the copy of the game file or the game built from it. Only when that folder cannot be written: my_copy beside this program. It holds
 *   paprium.bin         the player's copy of the game file, as checked (put into the plain byte order when it came in another)
 *   pc_game.dll         the game built on this PC
 *   build_record.txt    what built it: the package's version and build id, the ROM's and the DLL's hashes. Written last, so a build that was cut
 *                       short leaves no record and is done again
 *   launcher_log.txt    what this start did; build_log.txt  the last build's steps, with the tools' own words; game_log.txt  the game's console
 *   build.tmp\          the build's working folder, deleted when it ends
 *   paprium.sav, paprium_pc.cfg, stalls.txt   the game's save, its settings and its freeze log: the launcher names this folder to the game
 *                       program, so nothing is written beside the programs.
 *
 * Usage: Paprium.exe [GAMEFILE]                       a game file named on the command line, or dropped on the program's icon, is taken as chosen
 * Switches for tests and for the people who build the package (a player never needs them):
 *   --headless          no window at all: messages go to the console and the log, and the exit code says what happened
 *   --hidden            the window's whole path, but the window is never shown and no message box opens
 *   --rom FILE          as if the player had chosen FILE
 *   --test-drop FILE    with --hidden: FILE arrives as a drop on the window, through the same message a real drop sends
 *   --no-play           stop once the game is built and checked, without starting it
 *   --game-arg TEXT     one more argument for pc_port.exe, for a test run of the game (more than one may be given). After a Soft Reset the game
 *                       starts again with the same ones, less a key script (--key-script FILE), which belongs to the first start only
 *   --test-free-mb N    act as if the disk had N MB free
 *   --test-fail STEP    act as if STEP failed: boot, translate, compile, move or record (move and record fail as a full disk does; record is the
 *                       writing of the build's record after the built game is in place)
 *   --test-layout       the window is made, never shown, and checked: every control inside it, every text fitting its control; exit 0 or 1
 * Exit codes: 0 the game ran and ended normally (or, with --no-play, it is ready), 2 the package is incomplete (or out of reach below too long a
 * folder path) or no game file was given, 3 the game file was refused, 4 a folder cannot be written or the disk is too full, 5 the build failed,
 * 7 another launcher is running; otherwise the game's own. A build that fails (5, or 4 for want of room) ends the launcher only with --no-play:
 * otherwise the game plays through the interpreter.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "pc_config.h"
#include "pc_game_dll.h"

#define PL_PATH 2048
#define PL_ROM_MAX (64u << 20)     /* a file larger than this is not looked at */
#define PL_HEADER 512u             /* the copier header some dumps carry */

enum { EXIT_OK = 0, EXIT_ARGS = 2, EXIT_REFUSED = 3, EXIT_DISK = 4, EXIT_BUILD = 5, EXIT_BUSY = 7 };
enum { WM_PL_PROGRESS = WM_APP + 1, WM_PL_STATUS, WM_PL_DONE };
enum { ID_CHOOSE = 101, ID_LOG, ID_CLOSE };

/* ---- what the package says: tools\rules.json, written by targets/pc/pc_package.py */
typedef struct Rules {
    char package_version[64];
    char build_id[80];
    char rom_sha256[80];
    char image_check[40];
    char tested_dll_sha256[80];
    char game_exe[64];
    unsigned long long rom_size;
    unsigned need_free_mb;
} Rules;

static Rules rules;
static wchar_t pkg[PL_PATH], tools[PL_PATH], work[PL_PATH], log_path[PL_PATH], build_log_path[PL_PATH];
static int headless, hidden, no_play, work_beside;
static int interpreter;                         /* the game plays through the game program's interpreter: see the top of this file */
static wchar_t build_trouble[1600];             /* why the last build failed, as the player was told */
static int built_unrecorded;                    /* the last build put the game in place, but its record could not be written */
static const wchar_t *test_fail = L"";
static long long test_free_mb = -1;
static const wchar_t *game_args[32];
static int game_args_n;
static FILE *log_file;
static HANDLE build_job;
static HWND win, w_title, w_text, w_where, w_choose, w_progress, w_status, w_log, w_close, w_footer;
static HFONT font_body, font_title;
static volatile LONG building;
static int exit_code = EXIT_OK;
static int done_ok;
static int refused_at_start, told_at_start, test_layout, in_job;
static HANDLE lock = INVALID_HANDLE_VALUE;

/* The longest path, in characters, of this program's folder below which every file of the package stays reachable. The deepest file of the
 * package lies 97 characters below that folder (measured on the package's first version; targets/pc/pc_package.py refuses to make a package
 * with a deeper one), and Windows refuses a path of more than 259 characters to most programs, unzip tools among them, unless the PC allows
 * long paths and the program asks for them. A longer folder is not refused, as it often works; it is written in the log, and a failure then
 * tells the player to move the folder. */
#define PL_MAX_FOLDER 160

/* Where the player's files are, for the window: the text depends on which folder is used. */
static const wchar_t *const where_app_data =
    L"Your copy of the game file, the game built from it, your save and your settings are kept in %LOCALAPPDATA%\\Paprium PC, not in this "
    L"program's folder. To remove everything, delete this program's folder and that one.";
static const wchar_t *const where_beside =
    L"Your app-data folder cannot be written, so your copy of the game file, the game built from it, your save and your settings are kept "
    L"in the folder my_copy, beside this program. To remove everything, delete this program's folder.";
/* What the window's status line says when the game plays through the interpreter, and when the game built here is not the tested one. The
 * longer words, with the reason, go to the log and to the message box. */
static const wchar_t *const status_interpreter_build =
    L"The game could not be built on this PC, so it plays through the program's own interpreter. That way is not the build we tested: small "
    L"things such as timing may differ. The next start tries the build again.";
static const wchar_t *const status_interpreter_load =
    L"The game built on this PC could not be loaded, so it plays through the program's own interpreter. That way is not the build we "
    L"tested: small things such as timing may differ. The next start builds the game again.";
static const wchar_t *const status_interpreter_record =
    L"The game was built on this PC, but its record could not be written, so it plays through the program's own interpreter. That way is "
    L"not the build we tested: small things such as timing may differ. The next start builds the game again.";
static const wchar_t *const status_not_tested =
    L"The game built on this PC is not byte for byte the one we built and tested. If anything looks wrong, unzip the whole download again into "
    L"a new folder and start again.";
static wchar_t last_told[1600];

/* ---- text: wide inside, UTF-8 in files and on the console */
static void to_utf8(const wchar_t *w, char *out, int size)
{
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, NULL, NULL) <= 0)
        out[0] = 0;
}

static void from_utf8(const char *s, wchar_t *out, int size)
{
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, size) <= 0)
        out[0] = 0;
}

static void console_write(const char *utf8)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n;
    if (h && h != INVALID_HANDLE_VALUE)
        WriteFile(h, utf8, (DWORD)strlen(utf8), &n, NULL);
}

/* One line in launcher_log.txt with the time, and on the console in a headless run. */
static void say(const wchar_t *fmt, ...)
{
    wchar_t text[4096];
    char utf8[12288], line[12400];
    SYSTEMTIME t;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(text, 4095, fmt, ap);
    text[4095] = 0;
    va_end(ap);
    to_utf8(text, utf8, sizeof(utf8));
    GetLocalTime(&t);
    snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, utf8);
    if (log_file) {
        fputs(line, log_file);
        fflush(log_file);
    }
    if (headless || hidden) {
        console_write(utf8);
        console_write("\n");
    }
}

/* What the player reads: in the window's status line, or on the console. Also logged. */
static void show(const wchar_t *text)
{
    say(L"%ls", text);
    _snwprintf(last_told, 1599, L"%ls", text);
    if (win && !headless) {
        wchar_t *copy = _wcsdup(text);
        if (copy && !PostMessageW(win, WM_PL_STATUS, 0, (LPARAM)copy))
            free(copy);
    }
}

static void message(const wchar_t *text);

/* A notice the player reads before the game starts: the short words in the window's status line, the whole words in the log (and on the console
 * of a test run), and in a message box, which the player closes to go on (never in a test run). */
static void notice(const wchar_t *short_text, const wchar_t *whole_text)
{
    say(L"%ls", whole_text);
    _snwprintf(last_told, 1599, L"%ls", short_text);
    if (win && !headless) {
        wchar_t *copy = _wcsdup(short_text);
        if (copy && !PostMessageW(win, WM_PL_STATUS, 0, (LPARAM)copy))
            free(copy);
    }
    if (!headless && !hidden)
        MessageBoxW(win, whole_text, L"Paprium on PC", MB_OK | MB_ICONINFORMATION);
}

/* The game is about to play through the game program's interpreter, because the build failed (load_failed 0) or because the game program could
 * not use the game that was built (1; load_why is then the game program's own sentence saying why, or NULL when it printed none). The
 * team's decision: the fallback is on, and the player is told plainly that it is not the tested build. When the game program printed no
 * sentence, the player is pointed at launcher_log.txt, which holds the end of what it printed: game_log.txt is written again by the start
 * through the interpreter. */
static void tell_interpreter(int load_failed, const wchar_t *load_why)
{
    wchar_t text[2600];
    const wchar_t *tail = L"So the game plays now through the game program's own interpreter, which needs nothing built. That way is not the build we "
                          L"tested: it is the same game, but small things such as timing may differ from it.";
    if (load_failed && load_why)
        _snwprintf(text, 2599, L"The game that was built on this PC could not be loaded. The game program said: %ls %ls The next start builds the "
                   L"game again.", load_why, tail);
    else if (load_failed)
        _snwprintf(text, 2599, L"The game that was built on this PC could not be loaded; the details are in %ls\\launcher_log.txt. %ls "
                   L"The next start builds the game again.", work, tail);
    else
        _snwprintf(text, 2599, L"%ls %ls The next start tries the build again.", build_trouble[0] ? build_trouble : L"The game could not be built on this PC.",
                   tail);
    text[2599] = 0;
    interpreter = 1;
    notice(load_failed ? status_interpreter_load : built_unrecorded ? status_interpreter_record : status_interpreter_build, text);
}

/* The game this start built is not byte-equal to the one the package's makers built and tested. Such a game is never started from the cache
 * (cache_good), so every start builds it again and tells the player while it still differs. */
static int not_tested;

static void tell_not_tested(void)
{
    wchar_t text[1200];
    _snwprintf(text, 1199, L"The game built on this PC is not byte for byte the one we built and tested. It may play well, but if anything looks "
               L"wrong, unzip the whole download again into a new folder and start the program from there. Until the build is the tested one, "
               L"each start builds the game again. The details are in %ls.", build_log_path);
    text[1199] = 0;
    notice(status_not_tested, text);
}

static void progress(int percent)
{
    if (win && !headless)
        PostMessageW(win, WM_PL_PROGRESS, (WPARAM)percent, 0);
}

static void join(wchar_t *out, const wchar_t *a, const wchar_t *b)
{
    _snwprintf(out, PL_PATH - 1, L"%ls\\%ls", a, b);
    out[PL_PATH - 1] = 0;
}

static int is_file(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int is_dir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void system_words(DWORD code, wchar_t *out, int size)
{
    int n = (int)FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, out, size, NULL);
    while (n > 0 && (out[n - 1] == L'\n' || out[n - 1] == L'\r' || out[n - 1] == L' ' || out[n - 1] == L'.'))
        out[--n] = 0;
    if (n <= 0)
        _snwprintf(out, size, L"error %lu", (unsigned long)code);
}

/* What blocked a program, named only when Windows' own error code says so: a virus scanner (the codes Windows gives for a file a scanner calls a
 * threat), or Windows' rules for which programs may run, which Smart App Control applies (a policy's block, or a signature the rules require).
 * Any other code is told in the system's words alone, with no guess. The game program says the same for the game it loads (pc_game_dll.c). */
static const wchar_t *blocked_by(DWORD code)
{
    switch (code) {
    case 225:       /* ERROR_VIRUS_INFECTED */
    case 226:       /* ERROR_VIRUS_DELETED */
        return L" A virus scanner blocked it.";
    case 577:       /* ERROR_INVALID_IMAGE_HASH: a signature the rules require is missing */
    case 1260:      /* ERROR_ACCESS_DISABLED_BY_POLICY */
    case 4551:      /* ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION */
    case 4553:      /* ERROR_SYSTEM_INTEGRITY_POLICY_NOT_SIGNED */
        return L" Windows' rules for which programs may run blocked it; Smart App Control applies such rules to programs that are not signed.";
    }
    return L"";
}

/* A whole file, at most limit bytes. Returns NULL and sets *error to the system's code when it cannot. */
static unsigned char *read_file(const wchar_t *path, unsigned long long limit, unsigned long long *size, DWORD *error)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER n;
    unsigned char *b;
    DWORD got, total = 0;
    *error = 0;
    if (h == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return NULL;
    }
    if (!GetFileSizeEx(h, &n)) {
        *error = GetLastError();
        CloseHandle(h);
        return NULL;
    }
    *size = (unsigned long long)n.QuadPart;
    if (*size > limit) {
        CloseHandle(h);
        return NULL;
    }
    b = (unsigned char *)malloc((size_t)*size + 1);
    if (!b) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        CloseHandle(h);
        return NULL;
    }
    while (total < *size) {
        if (!ReadFile(h, b + total, (DWORD)(*size - total), &got, NULL) || got == 0) {
            *error = GetLastError() ? GetLastError() : ERROR_HANDLE_EOF;
            free(b);
            CloseHandle(h);
            return NULL;
        }
        total += got;
    }
    CloseHandle(h);
    b[*size] = 0;
    return b;
}

/* Writes a file whole through a temporary name, so a cut-short write never leaves half a file under the real name. Returns 0 or the system's code. */
static DWORD write_file(const wchar_t *path, const void *data, size_t size)
{
    wchar_t tmp[PL_PATH];
    HANDLE h;
    DWORD put, error = 0;
    _snwprintf(tmp, PL_PATH - 1, L"%ls.part", path);
    tmp[PL_PATH - 1] = 0;
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!WriteFile(h, data, (DWORD)size, &put, NULL) || put != size)
        error = GetLastError() ? GetLastError() : ERROR_DISK_FULL;
    if (!error && !FlushFileBuffers(h))
        error = GetLastError();
    CloseHandle(h);
    if (!error && !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = GetLastError();
    if (error)
        DeleteFileW(tmp);
    return error;
}

/* Deletes a folder and what it holds. A link to another place (a junction or a symbolic link, which Windows calls a reparse point) is removed
 * itself and never followed, so nothing outside the folder is ever deleted, whatever was put inside it. */
static void delete_tree(const wchar_t *path)
{
    wchar_t pattern[PL_PATH], child[PL_PATH];
    WIN32_FIND_DATAW f;
    HANDLE h;
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES)
        return;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (a & FILE_ATTRIBUTE_DIRECTORY)
            RemoveDirectoryW(path);
        else
            DeleteFileW(path);
        return;
    }
    join(pattern, path, L"*");
    h = FindFirstFileW(pattern, &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L".."))
                continue;
            join(child, path, f.cFileName);
            if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                delete_tree(child);
            } else if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(child);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryW(path);
}

static int sha256_of_file(const wchar_t *path, char out[65])
{
    unsigned long long n;
    DWORD error;
    unsigned char *b = read_file(path, 1ull << 31, &n, &error);
    if (!b)
        return -1;
    pc_sha256_hex(b, (size_t)n, out);
    free(b);
    return 0;
}

/* ---- tools\rules.json: a flat JSON file of our own; only its top-level string and number fields are read here */
static int json_field(const char *text, const char *key, char *out, size_t size)
{
    char want[96];
    const char *p, *q;
    size_t n;
    snprintf(want, sizeof(want), "\"%s\"", key);
    p = strstr(text, want);
    if (!p)
        return -1;
    p += strlen(want);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != ':')
        return -1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p == '"') {
        q = strchr(++p, '"');
        if (!q)
            return -1;
    } else {
        q = p;
        while (*q && *q != ',' && *q != '}' && *q != '\n' && *q != '\r' && *q != ' ')
            q++;
    }
    n = (size_t)(q - p);
    if (n >= size)
        return -1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

static int read_rules(wchar_t *missing, int size)
{
    wchar_t path[PL_PATH];
    unsigned long long n;
    DWORD error;
    char *text, number[32];
    join(path, tools, L"rules.json");
    text = (char *)read_file(path, 1u << 20, &n, &error);
    if (!text) {
        _snwprintf(missing, size, L"%ls", path);
        return -1;
    }
    memset(&rules, 0, sizeof(rules));
    if (json_field(text, "package_version", rules.package_version, sizeof(rules.package_version)) ||
        json_field(text, "build_id", rules.build_id, sizeof(rules.build_id)) ||
        json_field(text, "rom_sha256", rules.rom_sha256, sizeof(rules.rom_sha256)) ||
        json_field(text, "image_check", rules.image_check, sizeof(rules.image_check)) ||
        json_field(text, "game_exe", rules.game_exe, sizeof(rules.game_exe)) ||
        json_field(text, "rom_size", number, sizeof(number))) {
        free(text);
        _snwprintf(missing, size, L"%ls (it is damaged)", path);
        return -1;
    }
    rules.rom_size = strtoull(number, NULL, 10);
    json_field(text, "tested_dll_sha256", rules.tested_dll_sha256, sizeof(rules.tested_dll_sha256));
    rules.need_free_mb = json_field(text, "need_free_mb", number, sizeof(number)) ? 96 : (unsigned)strtoul(number, NULL, 10);
    free(text);
    return 0;
}

/* Every piece the build and the game need. A player who unzipped only part of the download is told so before anything is written. */
static int package_complete(wchar_t *missing, int size)
{
    static const wchar_t *const parts[] = {
        L"tools\\pc_boot_image.exe", L"tools\\python\\python.exe", L"tools\\python\\python312.dll", L"tools\\translator\\pl_translate.py",
        L"tools\\translator\\ptool.py", L"tools\\tcc\\tcc.exe", L"tools\\rt\\pt_rt.h", L"tools\\rt\\pt_port.h", L"tools\\rt\\pt_flags.h",
        L"tools\\rt\\pc_game_exports.c", L"tools\\rt\\pc_port_import.def", L"SDL2.dll"
    };
    wchar_t path[PL_PATH], name[80];
    size_t i;
    if (read_rules(missing, size))
        return -1;
    from_utf8(rules.game_exe, name, 80);
    join(path, pkg, name);
    if (!is_file(path)) {
        _snwprintf(missing, size, L"%ls", path);
        return -1;
    }
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        join(path, pkg, parts[i]);
        if (!is_file(path)) {
            _snwprintf(missing, size, L"%ls", path);
            return -1;
        }
    }
    return 0;
}

/* ---- the game file */
typedef struct RomCheck {
    unsigned char *data;        /* the game file in the plain byte order, rules.rom_size bytes, when accepted */
    const wchar_t *how;         /* how it came, when it needed putting right */
    wchar_t why[1200];          /* the refusal, for the player */
} RomCheck;

static int matches(const unsigned char *b)
{
    char hash[65];
    pc_sha256_hex(b, (size_t)rules.rom_size, hash);
    return !strcmp(hash, rules.rom_sha256);
}

static void swap_pairs(unsigned char *b, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) {
        unsigned char t = b[i];
        b[i] = b[i + 1];
        b[i + 1] = t;
    }
}

/* The interleaved layout of copier dumps: each 16 KB block holds its odd bytes first, then its even bytes. */
static void deinterleave(const unsigned char *in, unsigned char *out, size_t n)
{
    size_t block, i;
    for (block = 0; block + 16384 <= n; block += 16384)
        for (i = 0; i < 8192; i++) {
            out[block + 2 * i + 1] = in[block + i];
            out[block + 2 * i] = in[block + 8192 + i];
        }
}

/* Whether Windows gives this program paths longer than 259 characters: the program's manifest asks for them, and Windows 10 (1607) and later give
 * them when long paths are turned on for the PC. */
static int long_paths_on(void)
{
    typedef BOOLEAN(WINAPI * Fn)(void);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    Fn f = nt ? (Fn)(void *)GetProcAddress(nt, "RtlAreLongPathsEnabled") : NULL;
    return f && f();
}

static int check_rom(const wchar_t *path, RomCheck *r)
{
    unsigned long long n = 0;
    DWORD error;
    unsigned char *b, *t;
    size_t want = (size_t)rules.rom_size;
    wchar_t words[400];
    memset(r, 0, sizeof(*r));
    if (is_dir(path)) {
        _snwprintf(r->why, 1200, L"That is a folder, not a file. Choose the game file inside it. Nothing was changed.");
        return -1;
    }
    b = read_file(path, PL_ROM_MAX, &n, &error);
    if (!b) {
        if ((error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND || error == ERROR_FILENAME_EXCED_RANGE) && wcslen(path) >= MAX_PATH &&
            !long_paths_on()) {
            /* The program asks Windows for long paths (its manifest), but Windows gives them only where long paths are turned on. */
            _snwprintf(r->why, 1200, L"That file's path is %u characters long, and Windows on this PC lets programs use paths of at most 259. Move "
                       L"the file to a folder with a shorter path, such as C:\\Games, and choose it again. Nothing was changed.", (unsigned)wcslen(path));
        } else if (error) {
            system_words(error, words, 400);
            _snwprintf(r->why, 1200, L"That file cannot be read (%ls). Nothing was changed.", words);
        } else {
            _snwprintf(r->why, 1200, L"That file is %llu MB, far larger than the game file. It is not a Paprium game file. Nothing was changed.", n >> 20);
        }
        return -1;
    }
    if (n == 0) {
        _snwprintf(r->why, 1200, L"That file is empty. It is not a Paprium game file. Nothing was changed.");
    } else if (n >= 4 && b[0] == 'P' && b[1] == 'K' && (b[2] == 3 || b[2] == 5 || b[2] == 7)) {
        _snwprintf(r->why, 1200, L"That is a zip file. Unzip it first, then choose the game file that was inside. Nothing was changed.");
    } else if ((n >= 6 && !memcmp(b, "7z\xBC\xAF\x27\x1C", 6)) || (n >= 6 && !memcmp(b, "Rar!\x1A\x07", 6)) || (n >= 2 && b[0] == 0x1F && b[1] == 0x8B)) {
        _snwprintf(r->why, 1200, L"That is a packed file (7-Zip, RAR or gzip). Unpack it first, then choose the game file that was inside. Nothing was changed.");
    } else if (n == want) {
        if (matches(b)) {
            r->data = b;
            return 0;
        }
        swap_pairs(b, want);
        if (matches(b)) {
            r->data = b;
            r->how = L"with each pair of bytes swapped, as some dumping tools write it";
            return 0;
        }
        _snwprintf(r->why, 1200, L"That file is the size of the game file, but it is not the copy of Paprium this program knows. It may be another version "
                   L"of the game, a changed copy (a hack or a translation) or a damaged copy. Nothing was changed.");
    } else if (n == want + PL_HEADER) {
        t = (unsigned char *)malloc(want);
        if (t) {
            memcpy(t, b + PL_HEADER, want);
            if (matches(t)) {
                free(b);
                r->data = t;
                r->how = L"with a copier's 512-byte header in front";
                return 0;
            }
            deinterleave(b + PL_HEADER, t, want);
            if (matches(t)) {
                free(b);
                r->data = t;
                r->how = L"in a copier's interleaved layout, with its header";
                return 0;
            }
            memcpy(t, b + PL_HEADER, want);
            swap_pairs(t, want);
            if (matches(t)) {
                free(b);
                r->data = t;
                r->how = L"with a copier's header and each pair of bytes swapped";
                return 0;
            }
            free(t);
        }
        _snwprintf(r->why, 1200, L"That file is not the copy of Paprium this program knows. It may be another version of the game, a changed copy or a "
                   L"damaged copy. Nothing was changed.");
    } else {
        _snwprintf(r->why, 1200, L"That is not a Paprium game file this program knows: it is %llu bytes, and the game file is %llu bytes. It may be another "
                   L"game, a copy that was cut short, or another kind of file. Nothing was changed.", n, rules.rom_size);
    }
    free(b);
    return -1;
}

/* ---- the folder for the player's files */
static int writable(const wchar_t *folder)
{
    wchar_t probe[PL_PATH];
    HANDLE h;
    CreateDirectoryW(folder, NULL);
    if (!is_dir(folder))
        return 0;
    join(probe, folder, L".write_test");
    h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    CloseHandle(h);
    return 1;
}

/* The player's folder: %LOCALAPPDATA%\Paprium PC first, my_copy beside this program only when that cannot be written. The app-data folder is
 * found from the LOCALAPPDATA variable, as Windows sets it for every user (and as tests point it elsewhere), in Windows' full form (back
 * slashes, no "." or ".." parts), as the build's relative paths need. */
static int choose_work_folder(void)
{
    wchar_t given[PL_PATH], base[PL_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", given, PL_PATH);
    if (n && n < PL_PATH) {
        n = GetFullPathNameW(given, PL_PATH, base, NULL);
        if (!n || n >= PL_PATH)
            base[0] = 0;
    }
    if (n && n < PL_PATH && base[0] && is_dir(base)) {
        join(work, base, L"Paprium PC");
        if (writable(work))
            return 0;
    }
    join(work, pkg, L"my_copy");
    if (!writable(work))
        return -1;
    work_beside = 1;
    return 0;
}

/* ---- the record of the last build */
typedef struct Record {
    char package_version[64], build_id[80], rom_sha256[80], image_check[40], dll_sha256[80];
} Record;

static int record_field(const char *text, const char *key, char *out, size_t size)
{
    char want[64];
    const char *p, *e;
    size_t n;
    snprintf(want, sizeof(want), "\n%s=", key);
    p = strstr(text, want);
    if (!p)
        return -1;
    p += strlen(want);
    e = strpbrk(p, "\r\n");
    n = e ? (size_t)(e - p) : strlen(p);
    if (n >= size)
        return -1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

static int read_record(Record *r)
{
    wchar_t path[PL_PATH];
    unsigned long long n;
    DWORD error;
    char *body, *text;
    join(path, work, L"build_record.txt");
    body = (char *)read_file(path, 1u << 16, &n, &error);
    if (!body)
        return -1;
    text = (char *)malloc((size_t)n + 2);
    if (!text) {
        free(body);
        return -1;
    }
    text[0] = '\n';
    memcpy(text + 1, body, (size_t)n + 1);
    free(body);
    memset(r, 0, sizeof(*r));
    if (record_field(text, "package_version", r->package_version, sizeof(r->package_version)) ||
        record_field(text, "build_id", r->build_id, sizeof(r->build_id)) ||
        record_field(text, "rom_sha256", r->rom_sha256, sizeof(r->rom_sha256)) ||
        record_field(text, "image_check", r->image_check, sizeof(r->image_check)) ||
        record_field(text, "dll_sha256", r->dll_sha256, sizeof(r->dll_sha256))) {
        free(text);
        return -1;
    }
    free(text);
    return 0;
}

/* Is the player's copy of the game here, and is it still the right one? */
static int have_rom(void)
{
    wchar_t path[PL_PATH];
    unsigned long long n;
    DWORD error;
    unsigned char *b;
    int ok;
    join(path, work, L"paprium.bin");
    b = read_file(path, PL_ROM_MAX, &n, &error);
    if (!b)
        return 0;
    ok = n == rules.rom_size && matches(b);
    free(b);
    return ok;
}

/* Is the built game here, built by this version of the package from this game file, unchanged since, and the one the package's makers built and
 * tested? Says why not. */
static int cache_good(wchar_t *why, int size)
{
    Record r;
    wchar_t dll[PL_PATH];
    char hash[65];
    if (read_record(&r)) {
        join(dll, work, L"pc_game.dll");
        _snwprintf(why, size, is_file(dll) ? L"the last build did not finish, or its game could not be loaded, so the game is built again"
                                           : L"the game has not been built on this PC yet");
        return 0;
    }
    if (strcmp(r.package_version, rules.package_version) || strcmp(r.build_id, rules.build_id)) {
        _snwprintf(why, size, L"the game was built by another version of this package, so it is built again for this one");
        return 0;
    }
    if (strcmp(r.rom_sha256, rules.rom_sha256) || strcmp(r.image_check, rules.image_check)) {
        _snwprintf(why, size, L"the game was built from another game file");
        return 0;
    }
    join(dll, work, L"pc_game.dll");
    if (sha256_of_file(dll, hash) || strcmp(hash, r.dll_sha256)) {
        _snwprintf(why, size, L"the built game is missing or has changed since it was built");
        return 0;
    }
    /* A game that is not the tested one is built again, not started: the files it was built from may have been damaged or changed, and the
     * player may since have unzipped the download again, as the notice about it advises. The record cannot tell, as it names no package folder
     * and no package file. The player is told again only when the new build differs too. */
    if (rules.tested_dll_sha256[0] && strcmp(hash, rules.tested_dll_sha256)) {
        _snwprintf(why, size, L"the built game is not byte for byte the one the package's makers built and tested, so it is built again");
        return 0;
    }
    return 1;
}

/* ---- the build */
static unsigned long long free_mb(const wchar_t *folder)
{
    ULARGE_INTEGER avail;
    if (test_free_mb >= 0)
        return (unsigned long long)test_free_mb;
    if (!GetDiskFreeSpaceExW(folder, &avail, NULL, NULL))
        return ~0ull;
    return avail.QuadPart >> 20;
}

/* A path TinyCC can be given: relative to its working folder, so the folder names above it never reach it. TinyCC reads its command line in the
 * system's code page, not in UTF-8 as this program does, so the relative path is used only when every character of it is plain ASCII (a space
 * is fine: the caller puts the path in quotes). TinyCC also opens its files by joining its working folder and this path, and Windows refuses a
 * path of more than 259 characters, so the path is used only when the deepest file TinyCC opens under it stays inside that. Returns NULL when
 * the path can be used, or why it cannot, for the log. */
#define PL_TCC_DEEPEST 40        /* "\include\sec_api\sys\timeb_s.h" and the like: the longest name under tools\tcc, with room to spare */
static const wchar_t *path_for_tinycc(const wchar_t *from_folder, const wchar_t *to, int to_is_dir, char *out, int size)
{
    wchar_t rel[MAX_PATH];
    const wchar_t *c;
    if (wcslen(from_folder) >= MAX_PATH || wcslen(to) >= MAX_PATH ||
        !PathRelativePathToW(rel, from_folder, FILE_ATTRIBUTE_DIRECTORY, to, to_is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL))
        return L"there is no relative way from the build folder to it (another drive, or a path too long to compare)";
    for (c = rel; *c; c++)
        if (*c < 0x20 || *c > 0x7E || *c == L'"')
            return L"the way from the build folder to it has letters outside plain ASCII, which TinyCC cannot read from its command line";
    if (wcslen(from_folder) + 1 + wcslen(rel) + PL_TCC_DEEPEST > 259)
        return L"the way from the build folder to it would make TinyCC's deepest file paths longer than 259 characters";
    if (WideCharToMultiByte(CP_UTF8, 0, rel, -1, out, size, NULL, NULL) <= 0)
        return L"the way from the build folder to it could not be written as text";
    return NULL;
}

static int copy_folder(const wchar_t *from, const wchar_t *to)
{
    wchar_t pattern[PL_PATH], a[PL_PATH], b[PL_PATH];
    WIN32_FIND_DATAW f;
    HANDLE h;
    int bad = 0;
    CreateDirectoryW(to, NULL);
    join(pattern, from, L"*");
    h = FindFirstFileW(pattern, &f);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L".."))
            continue;
        join(a, from, f.cFileName);
        join(b, to, f.cFileName);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            bad |= copy_folder(a, b);
        else if (!CopyFileW(a, b, FALSE))
            bad = -1;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return bad;
}

static FILE *build_log;
static wchar_t tool_start_trouble[300];        /* why the last tool could not be started at all, in the system's words */
static const wchar_t *tool_start_blocked = L"";  /* and what blocked it, when Windows' code says so (see blocked_by) */

/* Runs one tool with no window, its output into build_log.txt, inside the job that ends it if the launcher ends. The progress bar moves from
 * first to last as the tool's own PROGRESS lines say, or with the time it usually takes. Returns the tool's exit code, or -1 when the tool could
 * not be started at all (tool_start_trouble then says why). */
static int run_tool(const wchar_t *label, wchar_t *command, const wchar_t *folder, int first, int last, double usual_seconds)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd, wr;
    DWORD code = 0, avail, got;
    char buffer[4096], line[512];
    int line_n = 0, shown = first, told = -1;
    ULONGLONG start = GetTickCount64();
    char utf8[8192];
    to_utf8(command, utf8, sizeof(utf8));
    fprintf(build_log, "\n== %s\n", utf8);
    fflush(build_log);
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = NULL;
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, folder, &si, &pi)) {
        DWORD e = GetLastError();
        wchar_t words[300];
        system_words(e, words, 300);
        say(L"%ls: could not start (%ls)", label, words);
        _snwprintf(tool_start_trouble, 299, L"%ls", words);
        tool_start_trouble[299] = 0;
        tool_start_blocked = blocked_by(e);
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    if (build_job && !in_job)
        AssignProcessToJobObject(build_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    for (;;) {
        int ended = 0;
        avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL))
            avail = 0;
        if (avail) {
            DWORD i;
            if (!ReadFile(rd, buffer, avail < sizeof(buffer) ? avail : sizeof(buffer), &got, NULL) || !got)
                continue;
            fwrite(buffer, 1, got, build_log);
            for (i = 0; i < got; i++) {
                if (buffer[i] == '\n' || line_n == (int)sizeof(line) - 1) {
                    int p;
                    line[line_n] = 0;
                    if (sscanf(line, "PROGRESS %d", &p) == 1 && p >= 0 && p <= 100)
                        told = p;
                    line_n = 0;
                } else if (buffer[i] != '\r') {
                    line[line_n++] = buffer[i];
                }
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 60) == WAIT_OBJECT_0)
            ended = 1;
        {
            double t = (double)(GetTickCount64() - start) / 1000.0;
            /* Half the way in the usual time, three quarters in three times that, never quite to the end. */
            double usual = usual_seconds > 0.1 ? usual_seconds : 0.1;
            int by_time = first + (int)((last - first) * (t / (t + usual)) * 0.95);
            int by_tool = told >= 0 ? first + (last - first) * told / 100 : first;
            int now = by_tool > by_time ? by_tool : by_time;
            if (now > shown) {
                shown = now;
                progress(shown);
            }
        }
        if (ended) {
            while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail && ReadFile(rd, buffer, avail < sizeof(buffer) ? avail : sizeof(buffer), &got, NULL) && got)
                fwrite(buffer, 1, got, build_log);
            break;
        }
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    fprintf(build_log, "\n== exit code %lu, %.1f s\n", (unsigned long)code, (double)(GetTickCount64() - start) / 1000.0);
    fflush(build_log);
    say(L"%ls: exit code %lu, %.1f s", label, (unsigned long)code, (double)(GetTickCount64() - start) / 1000.0);
    progress(last);
    return (int)code;
}

static int fail_here(const wchar_t *step)
{
    return !wcscmp(test_fail, step);
}

/* Two folders' paths can be too long for the build. This program's folder: the package's deepest file lies 97 characters below it, so a path
 * of more than PL_MAX_FOLDER is long (see there). The player's folder, where the build works: its deepest file is a header of TinyCC's,
 * copied into the build folder when TinyCC cannot be given its own folder, "\build.tmp\tcc\include\sec_api\sys\timeb_s.h", 44 characters
 * below it, and TinyCC opens no path of more than 259 characters, so a player's folder of more than 259 - 44 = 215 is long. my_copy beside
 * this program never is unless this program's folder is long too, which then says it. */
#define PL_WORK_DEEPEST 44
static int program_folder_long(void) { return wcslen(pkg) > PL_MAX_FOLDER; }
static int player_folder_long(void) { return !work_beside && wcslen(work) + PL_WORK_DEEPEST > 259; }

/* A build step failed: the player is told what failed and where the details are, and the words are kept for the interpreter's notice. When a
 * folder's path is long enough to be the cause, it is named: this program's folder, which the player can move, or the player's folder in the
 * app-data folder, whose place Windows sets for each user. */
static int build_failed(const wchar_t *what)
{
    wchar_t where[1300] = L"";
    int n = 0;
    if (player_folder_long()) {
        n = _snwprintf(where, 1299, L" The folder that keeps your files, %ls, has a path %u characters long, and the tools the build uses may not "
                       L"reach their files below such a path. That folder is your app-data folder, whose place Windows sets for each user, so "
                       L"this program cannot choose a shorter one.", work, (unsigned)wcslen(work));
        if (n < 0)
            n = 1299;       /* cut short: nothing more fits */
    }
    if (program_folder_long() && n < 1299)
        _snwprintf(where + n, (size_t)(1299 - n), L" This program's folder path is %u characters long, and the tools the build uses may not reach their "
                   L"files below such a path: move the folder to a shorter path, such as C:\\Games\\Paprium PC, and start again.", (unsigned)wcslen(pkg));
    where[1299] = 0;
    if (where[0])
        _snwprintf(build_trouble, 1599, L"%ls The game was not changed.%ls The details are in %ls.", what, where, build_log_path);
    else
        _snwprintf(build_trouble, 1599, L"%ls The game was not changed. The details are in %ls. You can start again; if it fails the same way, send "
                   L"us that file.", what, build_log_path);
    build_trouble[1599] = 0;
    show(build_trouble);
    return EXIT_BUILD;
}

/* A tool of the build failed (rc its exit code, or -1 when it could not be started at all). who names the tool for a start that failed; what is
 * the sentence for the player; plain says the same without naming the game file, for when a folder's path is long: the path is then the likely
 * cause, and build_failed says so, so the game file is not blamed. */
static int step_failed(int rc, const wchar_t *who, const wchar_t *what, const wchar_t *plain)
{
    wchar_t first[800];
    if (rc == -1)
        _snwprintf(first, 799, L"%ls could not be started (%ls).%ls", who, tool_start_trouble, tool_start_blocked);
    else
        _snwprintf(first, 799, L"%ls", program_folder_long() || player_folder_long() ? plain : what);
    first[799] = 0;
    return build_failed(first);
}

/* A failure of the build for want of room or of a folder it can write: told in the system's own words, kept for the interpreter's notice. The
 * caller says whether anything in the player's folder changed (changed then holds the sentence that says what). */
static int build_cannot_write(const wchar_t *what, DWORD error, const wchar_t *changed)
{
    wchar_t words[300];
    system_words(error, words, 300);
    _snwprintf(build_trouble, 1599, L"%ls (%ls). %ls", what, words, changed ? changed : L"Nothing was changed.");
    build_trouble[1599] = 0;
    show(build_trouble);
    return EXIT_DISK;
}

/* The whole build, in the build folder. On success the player's folder holds the new pc_game.dll and its record; on failure nothing there has
 * changed but the logs. */
static int build_game(void)
{
    wchar_t tmp[PL_PATH], gen[PL_PATH], command[8192], a[PL_PATH], b[PL_PATH], rom[PL_PATH], dll[PL_PATH], target[PL_PATH], tcc_dir[PL_PATH];
    wchar_t pattern[PL_PATH];
    char tcc_b[MAX_PATH * 2], names[4096], hash[65], record[1200];
    WIN32_FIND_DATAW f;
    HANDLE h;
    int rc, used;
    unsigned long long room = free_mb(work);
    DWORD error;
    const wchar_t *tcc_why;
    ULONGLONG start = GetTickCount64();

    build_trouble[0] = 0;
    built_unrecorded = 0;
    if (room < rules.need_free_mb) {
        _snwprintf(build_trouble, 1599, L"The disk with %ls has %llu MB free, and building the game needs about %u MB. Free some space and start again. "
                   L"Nothing was changed.", work, room, rules.need_free_mb);
        show(build_trouble);
        return EXIT_DISK;
    }
    join(build_log_path, work, L"build_log.txt");
    build_log = _wfopen(build_log_path, L"wbN");     /* N: not inherited by the tools */
    if (!build_log)
        return build_cannot_write(L"The log of the build cannot be written", GetLastError(), NULL);
    fprintf(build_log, "build of package %s (build id %s)\n", rules.package_version, rules.build_id);
    join(tmp, work, L"build.tmp");
    if (is_dir(tmp))
        delete_tree(tmp);
    if (!CreateDirectoryW(tmp, NULL)) {
        error = GetLastError();
        fclose(build_log);
        return build_cannot_write(L"The build's folder cannot be made", error, NULL);
    }

    show(L"Reading the cart chip's start ...");
    progress(4);
    join(a, tools, L"pc_boot_image.exe");
    join(rom, work, L"paprium.bin");
    /* The game file by a name relative to the build folder, which is inside the player's folder: plain letters, whatever the folders above. */
    _snwprintf(command, 8191, L"\"%ls\" ..\\paprium.bin boot_image.json", a);
    rc = fail_here(L"boot") ? 1 : run_tool(L"the boot tool", command, tmp, 4, 8, 0.5);
    if (rc) {
        fclose(build_log);
        delete_tree(tmp);
        return step_failed(rc, L"The boot tool, tools\\pc_boot_image.exe,", L"The cart chip's start could not be read from your game file.",
                           L"The cart chip's start could not be read.");
    }

    show(L"Translating the game (about 10 seconds) ...");
    join(a, tools, L"python\\python.exe");
    join(b, tools, L"translator\\pl_translate.py");
    {
        wchar_t r[PL_PATH];
        join(r, tools, L"rules.json");
        _snwprintf(command, 8191, L"\"%ls\" -I -B -X utf8 \"%ls\" --rules \"%ls\" --rom \"%ls\" --boot boot_image.json --out tr", a, b, r, rom);
    }
    rc = fail_here(L"translate") ? 1 : run_tool(L"the translation", command, tmp, 8, 80, 8.0);
    if (rc) {
        fclose(build_log);
        delete_tree(tmp);
        return step_failed(rc, L"The translator's Python, tools\\python\\python.exe,",
                           rc == 3 ? L"The translator did not accept your game file." : L"The translation of the game failed.",
                           L"The translation of the game failed.");
    }

    show(L"Building the game (a few seconds) ...");
    /* TinyCC gets only names relative to its working folder: the generated code's folder, where the runtime's headers are copied too. */
    join(gen, tmp, L"tr\\gen");
    {
        static const wchar_t *const rt_files[] = {L"pt_rt.h", L"pt_port.h", L"pt_flags.h", L"pc_game_exports.c", L"pc_port_import.def"};
        size_t i;
        for (i = 0; i < sizeof(rt_files) / sizeof(rt_files[0]); i++) {
            join(a, tools, L"rt");
            join(b, a, rt_files[i]);
            join(a, gen, rt_files[i]);
            if (!CopyFileW(b, a, FALSE)) {
                fclose(build_log);
                delete_tree(tmp);
                return build_failed(L"The runtime's files could not be copied for the build.");
            }
        }
    }
    join(tcc_dir, tools, L"tcc");
    tcc_why = path_for_tinycc(gen, tcc_dir, 1, tcc_b, sizeof(tcc_b));
    if (tcc_why) {
        /* The package's folder is on another drive, or the way to it has letters outside plain ASCII or is too long: give TinyCC a copy of its
         * own files here. */
        join(a, tcc_dir, L"include");
        join(b, tmp, L"tcc\\include");
        join(pattern, tmp, L"tcc");
        CreateDirectoryW(pattern, NULL);
        if (copy_folder(a, b)) {
            fclose(build_log);
            delete_tree(tmp);
            return build_failed(L"TinyCC's own files could not be copied for the build.");
        }
        join(a, tcc_dir, L"lib");
        join(b, tmp, L"tcc\\lib");
        if (copy_folder(a, b)) {
            fclose(build_log);
            delete_tree(tmp);
            return build_failed(L"TinyCC's own files could not be copied for the build.");
        }
        snprintf(tcc_b, sizeof(tcc_b), "..\\..\\tcc");
        say(L"TinyCC's files were copied into the build folder, as its own folder cannot be named to it: %ls", tcc_why);
    }
    /* The generated files in name order, then the exports, then the program's import list. */
    used = 0;
    names[0] = 0;
    {
        wchar_t *list[512];
        int n = 0, i, j;
        join(pattern, gen, L"gen_*.c");
        h = FindFirstFileW(pattern, &f);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (n < 512)
                    list[n++] = _wcsdup(f.cFileName);
            } while (FindNextFileW(h, &f));
            FindClose(h);
        }
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && wcscmp(list[j - 1], list[j]) > 0; j--) {
                wchar_t *t = list[j];
                list[j] = list[j - 1];
                list[j - 1] = t;
            }
        for (i = 0; i < n; i++) {
            char one[MAX_PATH];
            to_utf8(list[i], one, sizeof(one));
            if (used + (int)strlen(one) + 2 < (int)sizeof(names))
                used += snprintf(names + used, sizeof(names) - used, " %s", one);
            free(list[i]);
        }
        if (n == 0) {
            fclose(build_log);
            delete_tree(tmp);
            return build_failed(L"The translation wrote no code.");
        }
    }
    join(a, tools, L"tcc\\tcc.exe");
    {
        wchar_t wb[MAX_PATH * 2], wnames[4096];
        from_utf8(tcc_b, wb, MAX_PATH * 2);
        from_utf8(names, wnames, 4096);
        /* -B in quotes: the relative path may hold spaces (a folder under Program Files). */
        _snwprintf(command, 8191, L"\"%ls\" \"-B%ls\" -shared -w -DPT_PC_BUILD -DPT_GAME_DLL -I. -o ..\\..\\pc_game.dll%ls pc_game_exports.c pc_port_import.def",
                   a, wb, wnames);
    }
    rc = fail_here(L"compile") ? 1 : run_tool(L"TinyCC", command, gen, 80, 96, 2.5);
    join(dll, tmp, L"pc_game.dll");
    if (rc || !is_file(dll)) {
        fclose(build_log);
        delete_tree(tmp);
        return step_failed(rc, L"TinyCC, tools\\tcc\\tcc.exe,", L"TinyCC could not build the game from the translation.",
                           L"TinyCC could not build the game from the translation.");
    }
    if (sha256_of_file(dll, hash)) {
        fclose(build_log);
        delete_tree(tmp);
        return build_failed(L"The built game could not be read back.");
    }
    if (rules.tested_dll_sha256[0])
        say(L"the built game is %ls the one the package's makers built and tested (SHA-256 %hs)",
            strcmp(hash, rules.tested_dll_sha256) ? L"NOT byte-equal to" : L"byte-equal to", hash);
    else
        say(L"the built game's SHA-256 is %hs; the package names no tested build to compare it with", hash);
    not_tested = rules.tested_dll_sha256[0] && strcmp(hash, rules.tested_dll_sha256);

    /* Into place: the DLL, then the record, so a stop between them leaves a DLL with no matching record, which is built again next time. A move
     * that fails leaves the player's folder as it was (the build folder is inside it, so the move is a rename that happens whole or not at all);
     * a record that cannot be written after the move leaves the new game with no record, and the player is told that. */
    join(target, work, L"pc_game.dll");
    error = fail_here(L"move") ? ERROR_DISK_FULL : 0;
    if (!error && !MoveFileExW(dll, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
        error = GetLastError();
    if (error) {
        fclose(build_log);
        delete_tree(tmp);
        return build_cannot_write(L"The built game could not be put in place", error, NULL);
    }
    snprintf(record, sizeof(record), "package_version=%s\nbuild_id=%s\nrom_sha256=%s\nimage_check=%s\ndll_sha256=%s\nmatches_tested_build=%s\n",
             rules.package_version, rules.build_id, rules.rom_sha256, rules.image_check, hash,
             !rules.tested_dll_sha256[0] ? "not known" : strcmp(hash, rules.tested_dll_sha256) ? "no" : "yes");
    join(a, work, L"build_record.txt");
    error = fail_here(L"record") ? ERROR_DISK_FULL : write_file(a, record, strlen(record));
    if (error) {
        DeleteFileW(a);
        fclose(build_log);
        delete_tree(tmp);
        built_unrecorded = 1;
        return build_cannot_write(L"The built game is in place, but its record could not be written", error,
                                  L"The next start builds the game again.");
    }
    fprintf(build_log, "\nbuilt in %.1f s, pc_game.dll SHA-256 %s\n", (double)(GetTickCount64() - start) / 1000.0, hash);
    fclose(build_log);
    build_log = NULL;
    delete_tree(tmp);
    say(L"the game was built in %.1f s", (double)(GetTickCount64() - start) / 1000.0);
    progress(100);
    return EXIT_OK;
}

/* Takes a chosen game file: checks it and keeps the copy. Returns EXIT_OK, or the refusal's code with the reason told. */
static int take_rom(const wchar_t *path)
{
    RomCheck r;
    wchar_t target[PL_PATH], text[1400];
    DWORD error;
    unsigned long long room = free_mb(work);
    say(L"game file chosen: %ls", path);
    show(L"Checking the game file ...");
    progress(1);
    if (check_rom(path, &r)) {
        show(r.why);
        return EXIT_REFUSED;
    }
    if (r.how)
        say(L"it came %ls; the copy is kept in the plain byte order", r.how);
    join(target, work, L"paprium.bin");
    if (!have_rom()) {
        /* Room is asked for only when the copy is written; a copy already kept writes nothing here, and the build checks its own room. */
        if (room < rules.need_free_mb + (rules.rom_size >> 20)) {
            free(r.data);
            _snwprintf(text, 1400, L"The disk with %ls has %llu MB free, and the copy of the game file and the build need about %llu MB. Free some space and "
                       L"start again. Nothing was changed.", work, room, rules.need_free_mb + (rules.rom_size >> 20));
            show(text);
            return EXIT_DISK;
        }
        error = write_file(target, r.data, (size_t)rules.rom_size);
        if (error) {
            wchar_t words[300];
            system_words(error, words, 300);
            free(r.data);
            _snwprintf(text, 1400, L"Your copy of the game file could not be kept in %ls (%ls). Nothing was changed.", work, words);
            show(text);
            return EXIT_DISK;
        }
    }
    free(r.data);
    say(L"the game file checks good; the copy is %ls", target);
    return EXIT_OK;
}

/* ---- the game */
#define PL_GAME_SOFT_RESET 8        /* the game's exit code for its menu bar's Soft Reset (pc_main.c): start it again, the same way. No other exit of
                                       the game uses it; 7, a fault of the cart's chip program, is reported as a fault */

static int start_game_once(int again);

/* The game, started again after each Soft Reset it asks for, as at power-on. */
static int start_game(void)
{
    int rc = start_game_once(0);
    while (rc == PL_GAME_SOFT_RESET) {
        say(L"Soft Reset: the game starts again");
        rc = start_game_once(1);
    }
    return rc;
}

/* again: a start after a Soft Reset. It gets the same arguments as the first start but a test's key script (--game-arg --key-script FILE), which
 * belongs to the first start only, as in the game program's own restart (pc_main.c): played again, it would ask for the same Soft Reset again. */
static int start_game_once(int again)
{
    wchar_t exe[PL_PATH], name[80], command[8192], rom[PL_PATH], dll[PL_PATH], logp[PL_PATH];
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE out;
    DWORD code = 0;
    int used, i;
    from_utf8(rules.game_exe, name, 80);
    join(exe, pkg, name);
    join(rom, work, L"paprium.bin");
    join(dll, work, L"pc_game.dll");
    /* --from-launcher: the game program leaves the words about a DLL it cannot load to the launcher, which then plays through the interpreter, and
     * asks this launcher for a Soft Reset (exit code PL_GAME_SOFT_RESET) instead of starting itself again. The interpreter's start passes it too, for the
     * Soft Reset: without it the game would start a copy of itself that this launcher no longer waits for. */
    if (interpreter)
        used = _snwprintf(command, 8191, L"\"%ls\" --rom \"%ls\" --interpreter --from-launcher", exe, rom);
    else
        used = _snwprintf(command, 8191, L"\"%ls\" --rom \"%ls\" --game-dll \"%ls\" --from-launcher", exe, rom, dll);
    {
        /* The game's settings, save and freeze log are kept in the player's folder too, so nothing is written beside the programs. The
         * settings file is the launcher's to start, only when there is none: a file that is there but cannot be read here (too large for this
         * look, or held by another program) is the player's and is never written over; the game reads it itself, and its save then still goes
         * in this folder, as a relative or missing save line starts from the settings file's folder (pc_main.c). A line naming the save is added
         * to a file that has none, so the save stays in this folder. */
        wchar_t cfg[PL_PATH], save[PL_PATH], stalls[PL_PATH];
        char s8[PL_PATH * 3], text[PL_PATH * 3 + 300];
        unsigned long long n = 0;
        DWORD error;
        char *old;
        join(cfg, work, L"paprium_pc.cfg");
        join(save, work, L"paprium.sav");
        join(stalls, work, L"stalls.txt");
        to_utf8(save, s8, sizeof(s8));
        old = (char *)read_file(cfg, 1u << 20, &n, &error);
        if (!old && error == ERROR_FILE_NOT_FOUND) {
            snprintf(text, sizeof(text), "# The game's settings: see READ_ME.txt. Written by the launcher; the save is kept beside this file.\nsave=%s\n", s8);
            write_file(cfg, text, strlen(text));
        } else if (!old) {
            wchar_t words[300];
            if (error)
                system_words(error, words, 300);
            else
                _snwprintf(words, 300, L"it is %llu bytes, more than the launcher reads", n);
            say(L"the settings file %ls could not be read here (%ls), so it is left as it is; the game reads it itself", cfg, words);
        } else if (memchr(old, 0, (size_t)n)) {
            /* Saved as UTF-16 (Notepad's "Unicode"), or not text at all: a line written in UTF-8 would only spoil it more. The game tells the player
             * how to save it again (pc_config.c). */
            say(L"the settings file %ls holds zero bytes, as a file saved as UTF-16 does, so it is left as it is; the game says what to do", cfg);
            free(old);
        } else {
            const char *p = old;
            int has_save = 0;
            /* A file saved by an editor as "UTF-8 with BOM" starts with the three bytes of the byte order mark, which the game skips too. */
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
                p += 3;
            while (*p) {
                const char *e = strchr(p, '\n');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                while (len && (*p == ' ' || *p == '\t')) {
                    p++;
                    len--;
                }
                if (len >= 4 && !strncmp(p, "save", 4)) {
                    const char *q = p + 4;
                    while (*q == ' ' || *q == '\t')
                        q++;
                    if (*q == '=')
                        has_save = 1;
                }
                p += len + (e ? 1 : 0);
                if (!e)
                    break;
            }
            if (!has_save) {
                char *all = (char *)malloc((size_t)n + strlen(s8) + 16);
                if (all) {
                    size_t k = (size_t)n;
                    memcpy(all, old, k);
                    if (k && all[k - 1] != '\n')
                        all[k++] = '\n';
                    k += (size_t)sprintf(all + k, "save=%s\n", s8);
                    error = write_file(cfg, all, k);
                    if (!error) {
                        say(L"the settings file named no save, so a line naming %ls was added to it", save);
                    } else {
                        /* Another program may hold the file and still let others read it. The game then reads it as it is, and its save goes
                         * beside it, in this folder, by the save's default name (pc_main.c). */
                        wchar_t words[300];
                        system_words(error, words, 300);
                        say(L"the settings file names no save, and the line naming %ls could not be added to it (%ls); the game reads the file as it is, "
                            L"and keeps its save beside it", save, words);
                    }
                    free(all);
                }
            }
            free(old);
        }
        used += _snwprintf(command + used, 8191 - used, L" --config \"%ls\" --stall-log \"%ls\"", cfg, stalls);
    }
    for (i = 0; i < game_args_n && used < 8000; i++) {
        if (again && !wcscmp(game_args[i], L"--key-script") && i + 1 < game_args_n) {
            i++;
            continue;
        }
        used += _snwprintf(command + used, 8191 - used, L" \"%ls\"", game_args[i]);
    }
    command[8191] = 0;
    join(logp, work, L"game_log.txt");
    out = CreateFileW(logp, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (out != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = out;
        si.hStdInput = NULL;
    }
    say(L"starting the game: %ls", command);
    /* The game holds the launcher's lock too, so a second launcher stays out while the game runs even if this one is ended from outside. */
    if (lock != INVALID_HANDLE_VALUE)
        SetHandleInformation(lock, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    /* Out of the launcher's job, so the game never ends with the launcher. A job above ours may forbid that: then the game starts in it. Its working
     * folder is the player's folder, so nothing it might write by a relative name lands in this program's folder (the game finds its own files
     * beside itself, whatever the working folder). */
    if (!(in_job && CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, NULL, work, &si, &pi)) &&
        !CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, work, &si, &pi)) {
        wchar_t words[300], text[800];
        DWORD e = GetLastError();
        system_words(e, words, 300);
        if (out != INVALID_HANDLE_VALUE)
            CloseHandle(out);
        _snwprintf(text, 799, L"The game program %ls could not be started (%ls).%ls", exe, words, blocked_by(e));
        text[799] = 0;
        show(text);
        return -1;
    }
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    if (lock != INVALID_HANDLE_VALUE)
        SetHandleInformation(lock, HANDLE_FLAG_INHERIT, 0);
    say(L"the game runs as process %lu", (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    say(L"the game ended with exit code %lu; its console is in %ls", (unsigned long)code, logp);
    if (code == 6 && !interpreter) {
        /* The game program could not use the DLL: forget the record, so the next start builds the game again. */
        wchar_t rec[PL_PATH];
        join(rec, work, L"build_record.txt");
        DeleteFileW(rec);
        say(L"the game program could not load the built game; the record was removed, so the next start builds it again");
    } else if (code != 0 && code != 2 && code != 3 && code != PL_GAME_SOFT_RESET) {
        /* 2 and 3 are the game's own refusals, which it tells the player itself. Anything else is a fault: a halt the game program caught (4),
         * a crash (5, or Windows' own code), and the player is pointed at what the game printed. */
        wchar_t text[1200];
        _snwprintf(text, 1199, L"The game stopped because of a fault (exit code %lu). What it printed is in %ls. If it happens again, please send us "
                   L"that file and launcher_log.txt from the same folder.", (unsigned long)code, logp);
        text[1199] = 0;
        show(text);
        message(text);
    }
    return (int)code;
}

/* ---- the window */
static DWORD WINAPI build_thread(void *unused)
{
    int rc;
    (void)unused;
    rc = build_game();
    PostMessageW(win, WM_PL_DONE, (WPARAM)rc, 0);
    return 0;
}

static void begin_build(void)
{
    HANDLE t;
    if (InterlockedExchange(&building, 1))
        return;
    EnableWindow(w_choose, FALSE);
    ShowWindow(w_progress, SW_SHOWNA);
    SendMessageW(w_progress, PBM_SETPOS, 0, 0);
    t = CreateThread(NULL, 0, build_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        PostMessageW(win, WM_PL_DONE, (WPARAM)EXIT_BUILD, 0);
}

static void file_given(const wchar_t *path)
{
    int rc;
    if (building)
        return;
    rc = take_rom(path);
    if (rc == EXIT_OK) {
        wchar_t why[300];
        if (cache_good(why, 300)) {
            /* the game is built from this file already, and it is the tested one */
            done_ok = 1;
            DestroyWindow(win);
            return;
        }
        begin_build();
        return;
    }
    exit_code = rc;
    if (hidden)
        PostMessageW(win, WM_CLOSE, 0, 0);
}

static void choose_file(void)
{
    wchar_t path[PL_PATH] = L"";
    OPENFILENAMEW o;
    memset(&o, 0, sizeof(o));
    o.lStructSize = sizeof(o);
    o.hwndOwner = win;
    o.lpstrFilter = L"Game files (*.md, *.bin, *.gen, *.smd)\0*.md;*.bin;*.gen;*.smd\0All files\0*.*\0";
    o.lpstrFile = path;
    o.nMaxFile = PL_PATH;
    o.lpstrTitle = L"Choose your own copy of the Paprium game file";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o))
        file_given(path);
}

static HWND control(const wchar_t *kind, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, win, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_DROPFILES: {
        wchar_t path[PL_PATH];
        UINT n = DragQueryFileW((HDROP)wp, 0xFFFFFFFFu, NULL, 0);
        path[0] = 0;
        if (n >= 1)
            DragQueryFileW((HDROP)wp, 0, path, PL_PATH);
        DragFinish((HDROP)wp);
        if (n > 1)
            show(L"Drop one file: the game file. Nothing was changed.");
        else if (path[0])
            file_given(path);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CHOOSE)
            choose_file();
        else if (LOWORD(wp) == ID_LOG)
            ShellExecuteW(h, L"open", is_file(build_log_path) ? build_log_path : log_path, NULL, NULL, SW_SHOWNORMAL);
        else if (LOWORD(wp) == ID_CLOSE)
            PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_PL_PROGRESS:
        SendMessageW(w_progress, PBM_SETPOS, wp, 0);
        return 0;
    case WM_PL_STATUS:
        SetWindowTextW(w_status, (const wchar_t *)lp);
        free((void *)lp);
        return 0;
    case WM_PL_DONE:
        InterlockedExchange(&building, 0);
        if ((int)wp == EXIT_OK) {
            done_ok = 1;
            if (not_tested)
                tell_not_tested();
            show(L"Starting the game ...");
            DestroyWindow(h);
        } else if (!no_play && have_rom()) {
            /* The build failed, but the player's copy is kept: the game plays through the interpreter, and the window says so first. */
            ShowWindow(w_progress, SW_HIDE);
            tell_interpreter(0, NULL);
            done_ok = 1;
            DestroyWindow(h);
        } else {
            exit_code = (int)wp;
            ShowWindow(w_progress, SW_HIDE);
            EnableWindow(w_choose, TRUE);
            if (hidden)
                PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CLOSE:
        if (building) {
            if (hidden)
                return 0;
            if (MessageBoxW(h, L"The game is being built. Stop the build?", L"Paprium", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* Builds an HDROP holding one file, as Explorer does, for --test-drop: the drop then takes the same path through the window as a real one. */
static void post_test_drop(const wchar_t *path)
{
    size_t n = wcslen(path);
    HGLOBAL g = GlobalAlloc(GHND, sizeof(DROPFILES) + (n + 2) * sizeof(wchar_t));
    DROPFILES *d;
    if (!g)
        return;
    d = (DROPFILES *)GlobalLock(g);
    d->pFiles = sizeof(DROPFILES);
    d->fWide = TRUE;
    memcpy((char *)d + sizeof(DROPFILES), path, (n + 1) * sizeof(wchar_t));
    GlobalUnlock(g);
    PostMessageW(win, WM_DROPFILES, (WPARAM)g, 0);
}

/* --test-layout, with --hidden: each control lies inside the window's client area, and each text fits its control at the window's font when
 * Windows wraps it there, the longest status text included. Says what does not, and returns 0 or 1. The window is never shown. */
static int layout_check(void)
{
    static const wchar_t *const longest_status =
        L"That file is the size of the game file, but it is not the copy of Paprium this program knows. It may be another version of the game, a "
        L"changed copy (a hack or a translation) or a damaged copy. Nothing was changed.";
    HWND parts[] = {w_title, w_text, w_where, w_choose, w_progress, w_status, w_log, w_close, w_footer};
    RECT client, r;
    HDC dc = GetDC(win);
    int i, bad = 0;
    GetClientRect(win, &client);
    for (i = 0; i < (int)(sizeof(parts) / sizeof(parts[0])); i++) {
        wchar_t text[600], cls[32];
        POINT p0, p1;
        int w, h, need, k;
        GetWindowRect(parts[i], &r);
        p0.x = r.left; p0.y = r.top; p1.x = r.right; p1.y = r.bottom;
        ScreenToClient(win, &p0);
        ScreenToClient(win, &p1);
        w = p1.x - p0.x;
        h = p1.y - p0.y;
        if (p0.x < 0 || p0.y < 0 || p1.x > client.right || p1.y > client.bottom) {
            say(L"layout: control %d at %ld,%ld-%ld,%ld lies outside the client area %ld x %ld", i, p0.x, p0.y, p1.x, p1.y, client.right, client.bottom);
            bad = 1;
        }
        GetClassNameW(parts[i], cls, 32);
        if (_wcsicmp(cls, L"Static"))
            continue;
        SelectObject(dc, (HGDIOBJ)SendMessageW(parts[i], WM_GETFONT, 0, 0));
        /* The status line is measured with the longest refusal and the four notices before a game, the folder line with both of its texts. */
        for (k = 0; k < 5; k++) {
            if ((k && parts[i] != w_where && parts[i] != w_status) || (k > 1 && parts[i] == w_where))
                break;
            GetWindowTextW(parts[i], text, 600);
            if (parts[i] == w_status)
                _snwprintf(text, 599, L"%ls", k == 0 ? longest_status : k == 1 ? status_interpreter_build : k == 2 ? status_interpreter_load
                                              : k == 3 ? status_interpreter_record : status_not_tested);
            else if (parts[i] == w_where)
                _snwprintf(text, 599, L"%ls", k ? where_beside : where_app_data);
            SetRect(&r, 0, 0, w, 0);
            DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            need = r.bottom;
            say(L"layout: text %d needs %d px of %d", i, need, h);
            if (need > h)
                bad = 1;
        }
    }
    ReleaseDC(win, dc);
    say(L"layout: %ls", bad ? L"something does not fit" : L"every control and text fits");
    DestroyWindow(win);
    return bad;
}

static int run_window(int build_now, const wchar_t *drop)
{
    WNDCLASSW c;
    MSG msg;
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    memset(&c, 0, sizeof(c));
    c.lpfnWndProc = window_proc;
    c.hInstance = GetModuleHandleW(NULL);
    c.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    c.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    c.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    c.lpszClassName = L"PapriumLauncher";
    RegisterClassW(&c);
    font_body = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    font_title = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    win = CreateWindowExW(WS_EX_ACCEPTFILES, c.lpszClassName, L"Paprium on PC", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, 660, 480, NULL, NULL, c.hInstance, NULL);
    if (!win)
        return EXIT_ARGS;
    w_title = control(L"STATIC", L"Paprium on PC", 0, 24, 16, 600, 34, 0, font_title);
    w_text = control(L"STATIC", L"Drop your own copy of the Paprium game file (the ROM) on this window, or choose it. The program checks it, keeps a "
                     L"copy and builds the game from it on this PC. The first time takes about ten to twenty seconds; every start after that "
                     L"is at once.", 0, 24, 56, 600, 64, 0, font_body);
    /* Where the player's files are, and so what to delete to remove everything: the second folder when the program's own cannot be written. */
    w_where = control(L"STATIC", work_beside ? where_beside : where_app_data, 0, 24, 124, 600, 64, 0, font_body);
    w_choose = control(L"BUTTON", L"Choose the game file ...", BS_PUSHBUTTON | WS_TABSTOP, 24, 196, 220, 34, ID_CHOOSE, font_body);
    w_progress = control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 24, 242, 600, 18, 0, font_body);
    SendMessageW(w_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    ShowWindow(w_progress, SW_HIDE);
    w_status = control(L"STATIC", L"", 0, 24, 268, 600, 82, 0, font_body);
    w_log = control(L"BUTTON", L"Show the log", BS_PUSHBUTTON | WS_TABSTOP, 24, 358, 140, 30, ID_LOG, font_body);
    w_close = control(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 524, 358, 100, 30, ID_CLOSE, font_body);
    w_footer = control(L"STATIC", L"Paprium is named here with no affiliation and no commercial use. See NOTICES.txt.", 0, 24, 398, 600, 22, 0, font_body);
    DragAcceptFiles(win, TRUE);
    if (test_layout)
        return layout_check();
    if (!hidden) {
        ShowWindow(win, SW_SHOWNORMAL);
        UpdateWindow(win);
    }
    if (refused_at_start || told_at_start)
        SetWindowTextW(w_status, last_told);
    if (build_now) {
        show(L"Building the game ...");
        begin_build();
    } else if (drop) {
        post_test_drop(drop);
    } else if (hidden) {
        say(L"a hidden window was asked for with no file to drop: nothing to do");
        exit_code = EXIT_ARGS;
        PostMessageW(win, WM_CLOSE, 0, 0);
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(win, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    win = NULL;
    return done_ok ? EXIT_OK : (exit_code ? exit_code : EXIT_ARGS);
}

/* A message box for the player, before or after the window. Never in a test run. */
static void message(const wchar_t *text)
{
    if (!headless && !hidden)
        MessageBoxW(NULL, text, L"Paprium", MB_OK | MB_ICONWARNING);
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t why[1200], lockp[PL_PATH], missing[PL_PATH + 40], text[2400];
    const wchar_t *rom_arg = NULL, *drop = NULL;
    DWORD n;
    int i, rc, cache;

    for (i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--headless")) headless = 1;
        else if (!wcscmp(argv[i], L"--hidden")) hidden = 1;
        else if (!wcscmp(argv[i], L"--no-play")) no_play = 1;
        else if (!wcscmp(argv[i], L"--rom") && i + 1 < argc) rom_arg = argv[++i];
        else if (!wcscmp(argv[i], L"--test-drop") && i + 1 < argc) drop = argv[++i];
        else if (!wcscmp(argv[i], L"--game-arg") && i + 1 < argc) { if (game_args_n < 32) game_args[game_args_n++] = argv[++i]; else i++; }
        else if (!wcscmp(argv[i], L"--test-free-mb") && i + 1 < argc) test_free_mb = _wtoi64(argv[++i]);
        else if (!wcscmp(argv[i], L"--test-fail") && i + 1 < argc) test_fail = argv[++i];
        else if (!wcscmp(argv[i], L"--test-layout")) { test_layout = 1; hidden = 1; }
        else if (argv[i][0] != L'-' && !rom_arg) rom_arg = argv[i];
        else {
            console_write("bad argument\n");
            return EXIT_ARGS;
        }
    }
    if (headless)
        hidden = 0;
    n = GetModuleFileNameW(NULL, pkg, PL_PATH);
    if (!n || n >= PL_PATH)
        return EXIT_ARGS;
    *wcsrchr(pkg, L'\\') = 0;
    join(tools, pkg, L"tools");

    if (package_complete(missing, PL_PATH + 40)) {
        if (wcslen(pkg) > PL_MAX_FOLDER)
            /* A file past Windows' path limit looks missing although it may be there, and an unzip tool may have left it out. */
            _snwprintf(text, 2400, L"This folder is not complete, or Windows cannot reach all of it: %ls is missing. The folder's path is %u characters "
                       L"long, too long for the deepest files of the program. Unzip the whole download again into a folder with a shorter path, "
                       L"such as C:\\Games\\Paprium PC.", missing, (unsigned)wcslen(pkg));
        else
            _snwprintf(text, 2400, L"This folder is not complete: %ls is missing. Unzip the whole download again, into a new folder.", missing);
        console_write("incomplete package\n");
        message(text);
        if (headless || hidden) {
            char t8[7200];
            to_utf8(text, t8, sizeof(t8));
            console_write(t8);
            console_write("\n");
        }
        return EXIT_ARGS;
    }
    if (choose_work_folder()) {
        _snwprintf(text, 2400, L"Neither %%LOCALAPPDATA%%\\Paprium PC nor %ls\\my_copy can be written, so the game cannot be built. Move this "
                   L"folder somewhere you can write, such as your Documents, and start again.", pkg);
        console_write("no folder can be written\n");
        message(text);
        return EXIT_DISK;
    }
    join(log_path, work, L"launcher_log.txt");
    join(build_log_path, work, L"build_log.txt");
    /* One launcher at a time: the lock first, so a second one touches none of the first one's files, its log included. */
    join(lockp, work, L"launcher.lock");
    lock = CreateFileW(lockp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (lock == INVALID_HANDLE_VALUE) {
        console_write("another launcher is running\n");
        message(L"Paprium's launcher is already running, or the game is. Close it first.");
        return EXIT_BUSY;
    }
    log_file = _wfopen(log_path, L"wbN");      /* N: the tools this starts do not inherit the log */
    say(L"Paprium launcher, package %hs, in %ls; the player's files are in %ls", rules.package_version, pkg, work);
    if (work_beside)
        say(L"the app-data folder cannot be written, so the player's files are kept in %ls, beside the program; to remove everything, the "
            L"program's folder is deleted", work);
    else
        say(L"the player's files are kept in the app-data folder %ls, so this program's own folder is never written; to remove everything, that "
            L"folder is deleted too", work);
    if (wcslen(pkg) > PL_MAX_FOLDER)
        say(L"this folder's path is %u characters long, more than %d: Windows may not reach the program's deepest files", (unsigned)wcslen(pkg),
            PL_MAX_FOLDER);
    if (player_folder_long())
        say(L"the player's folder's path is %u characters long, more than %d: TinyCC may not reach the build's deepest files below it",
            (unsigned)wcslen(work), 259 - PL_WORK_DEEPEST);
    /* The build's tools must end when the launcher ends, however it ends. The launcher joins a job that is closed, and so ends everything in
     * it, when the launcher's last handle goes: every tool it starts is then inside the job from its first instruction, with no moment
     * between its start and its joining. The game breaks away from the job (see start_game), so it lives on its own. */
    build_job = CreateJobObjectW(NULL, NULL);
    if (build_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION j;
        memset(&j, 0, sizeof(j));
        j.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        if (SetInformationJobObject(build_job, JobObjectExtendedLimitInformation, &j, sizeof(j)) && AssignProcessToJobObject(build_job, GetCurrentProcess()))
            in_job = 1;
        say(in_job ? L"the build's tools run in the launcher's job" : L"the launcher could not join a job of its own (error %lu); each tool joins one as it starts",
            (unsigned long)GetLastError());
    }

    if (test_layout)
        return run_window(0, NULL);
    if (rom_arg) {
        /* A file named on the command line, or dropped on the program's icon: checked first. A refusal ends a test run; a player's window shows it
         * and waits for another file. */
        rc = take_rom(rom_arg);
        if (rc != EXIT_OK) {
            if (headless || hidden)
                return rc;
            refused_at_start = 1;
        }
    }
    why[0] = 0;
    {
        /* A kept copy that no longer checks good (changed or damaged on the disk) is not used: the player is told why the window asks again. */
        wchar_t kept[PL_PATH];
        join(kept, work, L"paprium.bin");
        if (!refused_at_start && is_file(kept) && !have_rom()) {
            _snwprintf(text, 2400, L"The copy of the game file kept in %ls has changed since it was checked, so it is not used. Drop your own copy of "
                       L"the game file on this window again, or choose it.", work);
            show(text);
            told_at_start = 1;
        }
    }
    cache = have_rom() && cache_good(why, 1200);
    if (!cache && why[0] && have_rom())
        say(L"building: %ls", why);
    if (cache && !refused_at_start && !drop) {
        /* Only the tested game starts from the cache: one that is not is built again (cache_good), and the build tells the player if it differs. */
        say(L"the game is built and unchanged: starting it from the cache");
        rc = EXIT_OK;
    } else if (headless) {
        if (!have_rom()) {
            say(L"no game file: give one with --rom");
            return EXIT_ARGS;
        }
        rc = build_game();
        if (rc == EXIT_OK && not_tested && !no_play)
            tell_not_tested();
        else if (rc != EXIT_OK && !no_play) {
            tell_interpreter(0, NULL);
            rc = EXIT_OK;
        }
    } else {
        /* A refused file shows its reason in the window even when the game is built, so the player sees why. */
        rc = run_window(!cache && have_rom() && !refused_at_start && !drop, drop);
    }
    if (rc != EXIT_OK) {
        say(L"stopped with exit code %d", rc);
        return rc;
    }
    if (no_play) {
        say(L"ready; not started (--no-play)");
        return EXIT_OK;
    }
    rc = start_game();
    if (rc == 6 && !interpreter) {
        /* Windows would not load the DLL (or it was not this program's): what the game program said goes into the launcher's log, then the game
         * plays through the interpreter. */
        wchar_t logp[PL_PATH], said[4200], why_w[2100];
        unsigned long long n = 0;
        DWORD error;
        char *text;
        const wchar_t *why_load = NULL;
        join(logp, work, L"game_log.txt");
        text = (char *)read_file(logp, 1u << 20, &n, &error);
        if (text) {
            /* The game program's sentence about the DLL is its line that starts with PC_GAME_DLL_SAID (pc_game_dll.h); its last 4,000 bytes go
             * into this log whole. */
            const char *line = strstr(text, PC_GAME_DLL_SAID);
            const char *tail = n > 4000 ? text + n - 4000 : text;
            from_utf8(tail, said, 4200);
            say(L"the game program said: %ls", said);
            if (line) {
                char one[2048];
                size_t k = strcspn(line + strlen(PC_GAME_DLL_SAID), "\r\n");
                if (k >= sizeof(one))
                    k = sizeof(one) - 1;
                memcpy(one, line + strlen(PC_GAME_DLL_SAID), k);
                one[k] = 0;
                from_utf8(one, why_w, 2100);
                if (why_w[0])
                    why_load = why_w;
            }
            free(text);
        } else {
            wchar_t words[300];
            if (error)
                system_words(error, words, 300);
            else
                _snwprintf(words, 300, L"it is %llu bytes, more than the launcher reads", n);
            say(L"what the game program printed, in %ls, could not be read here (%ls)", logp, words);
        }
        tell_interpreter(1, why_load);
        rc = start_game();
    }
    if (rc == -1) {
        message(last_told);
        rc = EXIT_BUILD;
    }
    CloseHandle(lock);
    return rc;
}
