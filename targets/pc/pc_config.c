/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_config.h. The SHA-256 follows the public standard, FIPS 180-4.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_config.h"
#include "pc_filter.h"

void pc_config_defaults(PcConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->scale = 3;
    c->vsync = 1;
    snprintf(c->renderer, sizeof(c->renderer), "opengl");
    c->sound = 1;
    c->volume = 100;
    c->menu_bar = 1;
    c->arcade_mode = -1;
}

int pc_config_arcade_mode(const PcConfig *c) { return c->arcade_mode >= 0 ? c->arcade_mode : c->arcade_stage ? 1 : 0; }

/* Copies text without the spaces at its two ends. */
static void trimmed(char *to, size_t size, const char *from, size_t length)
{
    while (length && (*from == ' ' || *from == '\t')) {
        from++;
        length--;
    }
    while (length && (from[length - 1] == ' ' || from[length - 1] == '\t' || from[length - 1] == '\r'))
        length--;
    if (length >= size)
        length = size - 1;
    memcpy(to, from, length);
    to[length] = 0;
}

int pc_config_is_preset(const char *value)
{
    size_t n = strlen(value);
    return n > 7 && !_stricmp(value + n - 7, ".slangp");
}

static int on_off(const char *value, int *out)
{
    if (!strcmp(value, "on")) { *out = 1; return 1; }
    if (!strcmp(value, "off")) { *out = 0; return 1; }
    return 0;
}

/* One of a list of words: its place in the list, or -1. */
static int one_of(const char *value, const char *const *words, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(value, words[i]))
            return i;
    return -1;
}

static const char *const cheat_names[3] = {"health", "pills", "lives"};

/* off, or names of cheats with commas between, each once at most. */
static int cheats_value(const char *value, unsigned *out)
{
    unsigned bits = 0;
    if (!strcmp(value, "off")) {
        *out = 0;
        return 1;
    }
    while (*value) {
        char word[16];
        size_t n = strcspn(value, ",");
        int k;
        trimmed(word, sizeof(word), value, n);
        k = one_of(word, cheat_names, 3);
        if (k < 0 || (bits & (1u << k)))
            return 0;
        bits |= 1u << k;
        value += n + (value[n] == ',' ? 1 : 0);
    }
    if (!bits)
        return 0;
    *out = bits;
    return 1;
}

void pc_config_cheats_text(unsigned cheats, char *text, size_t size)
{
    size_t used = 0;
    int k;
    text[0] = 0;
    for (k = 0; k < 3; k++)
        if (cheats & (1u << k))
            used += (size_t)snprintf(text + used, used < size ? size - used : 0, "%s%s", used ? "," : "", cheat_names[k]);
    if (!used)
        snprintf(text, size, "off");
}

/* A key_, pad_ or joy_ line: the action and the input's name, checked later by the part that knows the names (pc_controls.c). An empty name means none. */
static int one_input(const char *action, const char *value, PcConfigKey *list, int *n)
{
    if (*n >= PC_CONFIG_KEYS_MAX || strlen(action) >= PC_CONFIG_NAME || strlen(value) >= PC_CONFIG_NAME)
        return 0;
    strcpy(list[*n].action, action);
    strcpy(list[*n].key, value);
    (*n)++;
    return 1;
}

static int one_setting(const char *name, const char *value, PcConfig *c)
{
    if (!strcmp(name, "rom")) { snprintf(c->rom, sizeof(c->rom), "%s", value); return 1; }
    if (!strcmp(name, "save")) { snprintf(c->save, sizeof(c->save), "%s", value); return 1; }
    if (!strcmp(name, "scale")) {
        int n = atoi(value);
        if (n < 1 || n > 16)
            return 0;
        c->scale = n;
        return 1;
    }
    if (!strcmp(name, "display")) {
        static const char *const shapes[PC_SHAPES] = PC_SHAPE_NAMES;
        int k = one_of(value, shapes, PC_SHAPES);
        if (k < 0)
            return 0;
        c->shape = k;
        return 1;
    }
    if (!strcmp(name, "sprite_limits")) {
        if (!strcmp(value, "cart")) c->no_limit = 0;
        else if (!strcmp(value, "none")) c->no_limit = 1;
        else return 0;
        return 1;
    }
    if (!strcmp(name, "vsync")) return on_off(value, &c->vsync);
    if (!strcmp(name, "fullscreen")) return on_off(value, &c->fullscreen);
    if (!strcmp(name, "renderer")) {
        if (strcmp(value, "opengl") && strcmp(value, "direct3d") && strcmp(value, "direct3d11") && strcmp(value, "direct3d12") && strcmp(value, "auto"))
            return 0;
        strcpy(c->renderer, value);
        return 1;
    }
    if (!strcmp(name, "sound")) return on_off(value, &c->sound);
    if (!strcmp(name, "menu_bar")) return on_off(value, &c->menu_bar);
    if (!strcmp(name, "cheats")) return cheats_value(value, &c->cheats);
    if (!strcmp(name, "arcade_stage")) {
        char back[8];
        int n = atoi(value);
        if (!strcmp(value, "off")) {
            c->arcade_stage = 0;
            return 1;
        }
        snprintf(back, sizeof(back), "%d", n);
        if (n < 1 || n > PC_ARCADE_STAGES || strcmp(back, value))
            return 0;
        c->arcade_stage = n;
        return 1;
    }
    if (!strcmp(name, "arcade_mode")) {
        static const char *const modes[] = PC_ARCADE_MODE_NAMES;
        int k = one_of(value, modes, (int)(sizeof(modes) / sizeof(modes[0])));
        if (k < 0)
            return 0;
        c->arcade_mode = k;
        return 1;
    }
    if (!strcmp(name, "arcade_coins")) {
        static const char *const coins[] = PC_ARCADE_COINS_NAMES;
        int k = one_of(value, coins, (int)(sizeof(coins) / sizeof(coins[0])));
        if (k < 0)
            return 0;
        c->arcade_coins = k;
        return 1;
    }
    if (!strcmp(name, "attract_only")) return on_off(value, &c->attract_only);
    if (!strcmp(name, "three_players")) return on_off(value, &c->three_players);
    if (!strcmp(name, "filter")) {
        static const char *const names[] = PC_LOOK_NAMES;
        size_t i;
        /* bvm is another name for the pvm look, which draws a Sony PVM or BVM broadcast monitor: a player may call it either. */
        if (!strcmp(value, "bvm"))
            value = "pvm";
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (!strcmp(value, names[i])) {
                snprintf(c->filter, sizeof(c->filter), "%s", value);
                return 1;
            }
        }
        if (!pc_config_is_preset(value) || strlen(value) >= sizeof(c->filter))
            return 0;
        snprintf(c->filter, sizeof(c->filter), "%s", value);
        return 1;
    }
    if (!strcmp(name, "volume")) {
        int n = atoi(value);
        if (n < 0 || n > 100 || (n == 0 && strcmp(value, "0")))
            return 0;
        c->volume = n;
        return 1;
    }
    if (!strcmp(name, "pad1") || !strcmp(name, "pad2") || !strcmp(name, "pad3")) {
        int port = name[3] - '1';
        if (!strcmp(value, "six")) c->three_button[port] = 0;
        else if (!strcmp(value, "three")) c->three_button[port] = 1;
        else return 0;
        return 1;
    }
    if (!strncmp(name, "key_", 4))
        return one_input(name + 4, value, c->keys, &c->n_keys);
    if (!strncmp(name, "pad_", 4))
        return one_input(name + 4, value, c->pads, &c->n_pads);
    if (!strncmp(name, "joy_", 4))
        return one_input(name + 4, value, c->joys, &c->n_joys);
    if ((!strcmp(name, "controller1") || !strcmp(name, "controller2") || !strcmp(name, "controller3")) && value[0]) {
        /* any, none, or a pad's id and name. The name is only for reading, so a long one is cut. pc_controls.c checks the rest. */
        snprintf(c->controller[name[10] - '1'], sizeof(c->controller[0]), "%.79s", value);   /* PC_CONFIG_VALUE - 1 */
        return 1;
    }
    if (!strcmp(name, "stick1") || !strcmp(name, "stick2") || !strcmp(name, "stick3")) {
        static const char *const sticks[4] = {"off", "left", "right", "both"};
        int i;
        for (i = 0; i < 4; i++) {
            if (!strcmp(value, sticks[i])) {
                c->stick[name[5] - '1'] = 1 + i;
                return 1;
            }
        }
        return 0;
    }
    if (!strcmp(name, "deadzone1") || !strcmp(name, "deadzone2") || !strcmp(name, "deadzone3")) {
        int n = atoi(value);
        if (n < 5 || n > 95)
            return 0;
        c->deadzone[name[8] - '1'] = n;
        return 1;
    }
    {
        int r = pc_console_line(&c->console, name, value);     /* the console's lines: region, console, x32, megacd, cpumate, cpumate_brain */
        if (r >= 0)
            return r;
    }
    return 0;
}

int pc_config_parse(const char *text, PcConfig *c, char *error, size_t error_size)
{
    int line = 0;
    /* A file saved by an editor as "UTF-8 with BOM" starts with the three bytes of the byte order mark, which are not part of its first line. */
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text += 3;
    while (*text) {
        const char *end = strchr(text, '\n');
        size_t length = end ? (size_t)(end - text) : strlen(text);
        char row[1200], name[64], value[1100];
        const char *equals;
        line++;
        trimmed(row, sizeof(row), text, length);
        text += length + (end ? 1 : 0);
        if (!row[0] || row[0] == '#')
            continue;
        equals = strchr(row, '=');
        if (equals) {
            trimmed(name, sizeof(name), row, (size_t)(equals - row));
            trimmed(value, sizeof(value), equals + 1, strlen(equals + 1));
        }
        if (!equals || !one_setting(name, value, c)) {
            if (error)
                snprintf(error, error_size, "line %d of the configuration: %s", line, row);
            return line;
        }
    }
    return 0;
}

/* Why the settings file could not be read, in Windows' own words: held by another program with no sharing, a folder of that name, and so on. there:
   Windows found something by that name; when it could not even look (a folder on the way that this user may not open, a path too long), the file
   may not be there at all, and the words say only that it could not be reached. */
static int cannot_read(const char *path, DWORD code, int there, char *error, size_t error_size)
{
    char words[400] = "";
    int n = (int)FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, words, sizeof(words), NULL);
    while (n > 0 && (words[n - 1] == '\n' || words[n - 1] == '\r' || words[n - 1] == ' ' || words[n - 1] == '.'))
        words[--n] = 0;
    if (error)
        snprintf(error, error_size, there ? "The settings file %s is there, but it could not be read (%s), so the game does not start."
                                          : "The settings file %s could not be reached (%s), so the game does not start.", path,
                 words[0] ? words : "no description");
    return -2;
}

/* A file with zero bytes in it is not the plain text the game reads: most often it was saved as UTF-16, which Notepad calls "Unicode" or "UTF-16 LE".
   Its lines would read as nonsense, so the player is told how to save it again. Returns 1, as a refused first line does. */
static int not_plain_text(const char *path, char *error, size_t error_size)
{
    if (error)
        snprintf(error, error_size, "The settings file %s is not plain text: it holds zero bytes, as a file saved as UTF-16 (Unicode) does, so the game "
                 "does not start. Save it again as UTF-8 (in Notepad: File, Save as, Encoding: UTF-8).", path);
    return 1;
}

int pc_config_load(const char *path, PcConfig *c, char *error, size_t error_size)
{
    FILE *f;
    long n;
    char *text;
    int result;
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return -1;
        return cannot_read(path, code, 0, error, error_size);
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY)
        return cannot_read(path, ERROR_ACCESS_DENIED, 1, error, error_size);
    f = fopen(path, "rb");
    if (!f)
        return cannot_read(path, (DWORD)_doserrno, 1, error, error_size);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = n >= 0 ? (char *)malloc((size_t)n + 1) : NULL;
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
        DWORD code = text ? ERROR_READ_FAULT : ERROR_NOT_ENOUGH_MEMORY;
        fclose(f);
        free(text);
        return cannot_read(path, code, 1, error, error_size);
    }
    fclose(f);
    text[n] = 0;
    if (memchr(text, 0, (size_t)n)) {
        free(text);
        return not_plain_text(path, error, error_size);
    }
    result = pc_config_parse(text, c, error, error_size);
    free(text);
    return result;
}

int pc_config_is_control(const char *name)
{
    static const char *const names[] = {"pad1", "pad2", "pad3", "controller1", "controller2", "controller3", "stick1", "stick2", "stick3", "deadzone1",
                                        "deadzone2", "deadzone3"};
    size_t i;
    if (!strncmp(name, "key_", 4) || !strncmp(name, "pad_", 4) || !strncmp(name, "joy_", 4))
        return 1;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}

/* One line of text, from *at to its end, without the line end; *at moves past it. Returns 0 at the end of the text. */
static int next_line(const char **at, const char **start, size_t *length)
{
    const char *end;
    if (!**at)
        return 0;
    *start = *at;
    end = strchr(*at, '\n');
    *length = end ? (size_t)(end - *at) : strlen(*at);
    *at += *length + (end ? 1 : 0);
    if (*length && (*start)[*length - 1] == '\r')
        (*length)--;
    return 1;
}

/* Whether a comment line of the old file is one block also writes. */
static int block_has_comment(const char *block, const char *line)
{
    const char *at = block, *start;
    size_t length;
    while (next_line(&at, &start, &length))
        if (length && start[0] == '#' && length == strlen(line) && !memcmp(start, line, length))
            return 1;
    return 0;
}

/* A file saved by an editor as "UTF-8 with BOM" starts with the three bytes of the byte order mark. The merges keep them at the start of the file, and
   read the first line without them, as pc_config_parse does, so that line is known by its name like any other. */
static int has_bom(const char *text) { return (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF; }

int pc_config_merge_controls(const char *old_text, const char *block, char *out, size_t out_size)
{
    const char *at = old_text ? old_text : "", *start, *first_end = strchr(at, '\n');
    const char *eol = first_end && first_end > at && first_end[-1] == '\r' ? "\r\n" : "\n";
    size_t used = 0, kept_end = 0, bom = 0, length;
    char line[1200], name[64];
#define PUT(text, n) do { if (used + (n) >= out_size) return -1; memcpy(out + used, (text), (n)); used += (n); } while (0)
    if (has_bom(at)) {
        PUT(at, 3);
        at += 3;
        kept_end = bom = 3;
    }
    while (next_line(&at, &start, &length)) {
        int keep = 1;
        trimmed(line, sizeof(line), start, length);
        if (line[0] == '#') {
            keep = !block_has_comment(block, line);
        } else if (line[0]) {
            const char *equals = strchr(line, '=');
            if (equals) {
                trimmed(name, sizeof(name), line, (size_t)(equals - line));
                keep = !pc_config_is_control(name);
            }
        }
        if (!keep)
            continue;
        PUT(start, length);
        PUT(eol, strlen(eol));
        if (line[0])
            kept_end = used;        /* blank lines at the end are left out, and one is put back before the block */
    }
    used = kept_end;
    if (used > bom)
        PUT(eol, strlen(eol));
    at = block;
    while (next_line(&at, &start, &length)) {
        PUT(start, length);
        PUT(eol, strlen(eol));
    }
#undef PUT
    if (used >= out_size)
        return -1;
    out[used] = 0;
    return (int)used;
}

/* The name a setting's line sets, or "" for a comment, a blank line or a line with no '='. */
static void line_name(const char *start, size_t length, char *name, size_t size)
{
    char line[1200];
    const char *equals;
    trimmed(line, sizeof(line), start, length);
    name[0] = 0;
    if (!line[0] || line[0] == '#')
        return;
    equals = strchr(line, '=');
    if (equals)
        trimmed(name, size, line, (size_t)(equals - line));
}

int pc_config_merge_settings(const char *old_text, const char *const *names, const char *const *values, int n, char *out, size_t out_size)
{
    const char *at = old_text ? old_text : "", *start, *first_end = strchr(at, '\n');
    const char *eol = first_end && first_end > at && first_end[-1] == '\r' ? "\r\n" : "\n";
    size_t used = 0, kept_end = 0, length;
    char name[64];
    unsigned char done[64];
    int k;
    if (n < 0 || n > 64)
        return -1;
    memset(done, 0, sizeof(done));
#define PUT(text, count) do { if (used + (count) >= out_size) return -1; memcpy(out + used, (text), (count)); used += (count); } while (0)
    if (has_bom(at)) {
        PUT(at, 3);
        at += 3;
        kept_end = 3;
    }
    while (next_line(&at, &start, &length)) {
        line_name(start, length, name, sizeof(name));
        for (k = 0; name[0] && k < n; k++)
            if (!strcmp(name, names[k]))
                break;
        if (name[0] && k < n) {
            if (done[k])
                continue;           /* a second line for the same setting: the first one now holds its value */
            done[k] = 1;
            PUT(names[k], strlen(names[k]));
            PUT("=", 1);
            PUT(values[k], strlen(values[k]));
        } else {
            PUT(start, length);
        }
        PUT(eol, strlen(eol));
        if (length)
            kept_end = used;        /* blank lines at the end go, as pc_config_merge_controls leaves them */
    }
    used = kept_end;
    for (k = 0; k < n; k++) {
        if (done[k])
            continue;
        PUT(names[k], strlen(names[k]));
        PUT("=", 1);
        PUT(values[k], strlen(values[k]));
        PUT(eol, strlen(eol));
    }
#undef PUT
    if (used >= out_size)
        return -1;
    out[used] = 0;
    return (int)used;
}

int pc_config_rewrite(const char *path, PcConfigChange change, void *arg)
{
    static char merged[2 * PC_CONFIG_REWRITE_MAX];
    char *old = NULL, temp[1300];
    FILE *f;
    long n = 0;
    int length;
    /* Only a file that is not there is no file: one that is there but cannot be opened (held by another program, a folder of that name) is never
       replaced by the change alone. */
    f = fopen(path, "rb");
    if (!f) {
        DWORD a = GetFileAttributesA(path), code = GetLastError();
        if (a != INVALID_FILE_ATTRIBUTES || (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND))
            return PC_CONFIG_NOT_READ;
    } else {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        /* A file this large was not written by the program: it is told as too large, which is the true reason, rather than as unreadable. */
        if (n >= PC_CONFIG_REWRITE_MAX) {
            fclose(f);
            return PC_CONFIG_FILE_TOO_LARGE;
        }
        old = n >= 0 ? (char *)malloc((size_t)n + 1) : NULL;
        if (!old || fread(old, 1, (size_t)n, f) != (size_t)n) {
            fclose(f);
            free(old);
            return PC_CONFIG_NOT_READ;
        }
        old[n] = 0;
        fclose(f);
        /* A file saved as UTF-16 holds zero bytes: read as text it would end at the first one, and the rest of it would be lost. */
        if (memchr(old, 0, (size_t)n)) {
            free(old);
            return PC_CONFIG_NOT_READ;
        }
    }
    length = change(old, merged, sizeof(merged), arg);
    free(old);
    if (length < 0)
        return PC_CONFIG_TOO_LARGE;
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    f = fopen(temp, "wb");
    if (!f || fwrite(merged, 1, (size_t)length, f) != (size_t)length || fflush(f) != 0) {
        if (f)
            fclose(f);
        remove(temp);
        return PC_CONFIG_NOT_WRITTEN;
    }
    fclose(f);
    if (!MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(temp);
        return PC_CONFIG_NOT_REPLACED;
    }
    return 0;
}

/* ---- SHA-256 */

static const uint32_t K[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu,
    0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu, 0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u, 0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 64; i++) {
        uint32_t s1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t t1 = k + s1 + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t s0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void pc_sha256_hex(const void *data, size_t bytes, char out[65])
{
    uint32_t h[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
    const uint8_t *p = (const uint8_t *)data;
    uint8_t tail[128];
    size_t left = bytes, tail_bytes;
    uint64_t bits = (uint64_t)bytes * 8u;
    int i;
    while (left >= 64) {
        sha_block(h, p);
        p += 64;
        left -= 64;
    }
    memset(tail, 0, sizeof(tail));
    if (left)
        memcpy(tail, p, left);
    tail[left] = 0x80u;
    tail_bytes = left < 56 ? 64 : 128;
    for (i = 0; i < 8; i++)
        tail[tail_bytes - 1 - (size_t)i] = (uint8_t)(bits >> (i * 8));
    sha_block(h, tail);
    if (tail_bytes == 128)
        sha_block(h, tail + 64);
    for (i = 0; i < 8; i++)
        snprintf(out + i * 8, 9, "%08x", (unsigned)h[i]);
}
