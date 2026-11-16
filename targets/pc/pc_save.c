/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_save.h.
 */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#endif

#include "pc_save.h"

/* After a failed open: does the name simply not exist? A missing folder or drive does not count, so this asks the system rather than reading errno, which Windows' C library sets the
   same for both. */
static int no_such_file(const char *path)
{
#ifdef _WIN32
    return GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
#else
    return errno == ENOENT;
#endif
}

/* The file as last loaded, for pc_save_replace's copy, and what the check made of it. */
static uint8_t file[PC_SAVE_BYTES + 1u];    /* one byte over, to see a file that is too long */
static size_t file_bytes;
static int file_check;
static char file_why[160];              /* why the file did not check, when there is more to say than that */
static int copy_kept, newest_in_temp, copy_failed;
static const char *copy_ending, *copy_source;

/* The cart's own check on its save. The cart keeps each save as copies, in the firmware's byte order. A copy is 8 to 1,024 bytes long, starts and ends on a multiple of 4, and ends with 8 bytes:
   a sequence number of 4 bytes, low byte first, that is never 0 and never all ones; the save's number; a byte that makes the XOR of the whole copy 0; then 0xC2 and 0x3F. The cart takes a copy
   only when all of that holds.
   Where each copy goes and how long it is comes from the game's own list, which this program does not hold. But the first copy of each of the saves 1 to 4 has a place of its own, the same on
   every cart: save 4's 512 bytes at the file's start, and saves 1, 2 and 3, 256 bytes each, at 0x400, 0x600 and 0x800 (the firmware's import of an older save relies on the same places). The
   cart writes a save's first copy again and again, so these four places are where the game's saves are. Each of them must be blank, never written, or hold one whole copy of its own save: a
   trailer that is right, the right save's number, and an XOR of 0 over exactly its length. One bit wrong anywhere in such a copy, its trailer included, fails the file.
   A copy anywhere else is found by its trailer alone, and its length is not known here, so it passes when some length the cart allows XORs to 0: a copy damaged inside can still pass there.
   An XOR misses damage that cancels itself out, such as the same bit flipped in two bytes of one copy; the cart's own check misses it too. A blank file, a file of something else, or a save in
   the other byte order has no copy at all. A file in the older 4 KB layout has no trailers, and the firmware takes it in once when it is not blank. A 4 KB file that does hold one of the
   cart's copies in its own place is not that layout: it is an 8 KB save cut short, and it does not check.
   A place left blank checks, as a save never written. A place blank in the file but holding a whole copy in the backup, .bak, is checked in pc_save_load: the cart never blanks a place it has
   written, so such a file has lost a save, and it does not check either. */
#define TRAILER 8u
#define COPY_MAX 1024u

static const struct { unsigned number, at, length; } cart_places[4] = {{4u, 0x000u, 0x200u}, {1u, 0x400u, 0x100u}, {2u, 0x600u, 0x100u}, {3u, 0x800u, 0x100u}};

static int all_one_byte(const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 1; i < n; i++)
        if (b[i] != b[0])
            return 0;
    return b[0] == 0x00 || b[0] == 0xFF;
}

/* The 8 bytes that end at end: the closing bytes and a sequence number the cart takes. */
static int trailer_at(const uint8_t *fw, size_t end)
{
    uint32_t sequence;
    if (fw[end - 2] != 0xC2 || fw[end - 1] != 0x3F)
        return 0;
    sequence = (uint32_t)fw[end - 8] | (uint32_t)fw[end - 7] << 8 | (uint32_t)fw[end - 6] << 16 | (uint32_t)fw[end - 5] << 24;
    return sequence != 0 && sequence != 0xFFFFFFFFu;
}

static uint8_t xor_of(const uint8_t *b, size_t n)
{
    uint8_t x = 0;
    size_t i;
    for (i = 0; i < n; i++)
        x ^= b[i];
    return x;
}

/* One of the four places' own bytes, from the copy's start to its trailer's end. */
static int in_a_place(size_t end)
{
    unsigned i;
    for (i = 0; i < 4u; i++)
        if (end > cart_places[i].at && end <= cart_places[i].at + cart_places[i].length)
            return 1;
    return 0;
}

int pc_save_check(const uint8_t *fw, size_t n)
{
    size_t end, k;
    unsigned i, copies = 0;
    if (n != PC_SAVE_BYTES && n != PC_SAVE_BYTES_OLD)
        return PC_SAVE_NOT_A_SAVE;
    if (all_one_byte(fw, n))
        return PC_SAVE_BLANK;
    if (n == PC_SAVE_BYTES_OLD) {
        for (i = 0; i < 4u; i++)
            if (trailer_at(fw + cart_places[i].at, cart_places[i].length) && fw[cart_places[i].at + cart_places[i].length - 4u] == cart_places[i].number)
                return PC_SAVE_NOT_A_SAVE;
        return PC_SAVE_GOOD;
    }
    for (i = 0; i < 4u; i++) {
        const uint8_t *place = fw + cart_places[i].at;
        size_t length = cart_places[i].length;
        if (all_one_byte(place, length))
            continue;
        if (!trailer_at(place, length) || place[length - 4u] != cart_places[i].number || xor_of(place, length) != 0)
            return PC_SAVE_NOT_A_SAVE;
        copies++;
    }
    for (end = TRAILER; end <= n; end += 4) {
        uint8_t x = 0;
        int found = 0;
        if (in_a_place(end) || !trailer_at(fw, end))
            continue;
        for (k = 1; k <= COPY_MAX && k <= end && !found; k++) {
            x ^= fw[end - k];
            found = k >= TRAILER && !(k & 3u) && !x;
        }
        if (!found)
            return PC_SAVE_NOT_A_SAVE;
        copies++;
    }
    return copies ? PC_SAVE_GOOD : PC_SAVE_NOT_A_SAVE;
}

/* One of the four places holds one whole copy of its own save, as pc_save_check wants it. */
static int place_whole(const uint8_t *fw, unsigned i)
{
    const uint8_t *place = fw + cart_places[i].at;
    size_t length = cart_places[i].length;
    return trailer_at(place, length) && place[length - 4u] == cart_places[i].number && xor_of(place, length) == 0;
}

int pc_save_blanked(const uint8_t *fw, const uint8_t *backup_fw)
{
    unsigned i;
    for (i = 0; i < 4u; i++)
        if (all_one_byte(fw + cart_places[i].at, cart_places[i].length) && place_whole(backup_fw, i))
            return (int)cart_places[i].number;
    return 0;
}

/* path.bak in the firmware's byte order, as far as it goes, the rest 0: 1, or 0 when there is none or it cannot be read. */
static int backup_read(const char *path, uint8_t *fw)
{
    static uint8_t raw[PC_SAVE_BYTES];
    char name[1100];
    FILE *f;
    size_t n, i;
    int failed;
    if (snprintf(name, sizeof(name), "%s%s", path, PC_SAVE_BACKUP) >= (int)sizeof(name))
        return 0;
    f = fopen(name, "rb");
    if (!f)
        return 0;
    n = fread(raw, 1, sizeof(raw), f);
    failed = ferror(f);
    fclose(f);
    if (failed)
        return 0;
    memset(fw, 0x00, PC_SAVE_BYTES);
    for (i = 0; i < n; i++)
        fw[i ^ 1u] = raw[i];
    return 1;
}

long pc_save_load(const char *path, uint8_t *fw)
{
    static uint8_t backup[PC_SAVE_BYTES];
    FILE *f = fopen(path, "rb");
    size_t n;
    uint32_t i;
    int failed, blanked;
    file_bytes = 0;
    file_check = PC_SAVE_BLANK;
    file_why[0] = 0;
    copy_kept = 0;
    if (!f)
        return no_such_file(path) ? PC_SAVE_NO_FILE : PC_SAVE_UNREADABLE;
    n = fread(file, 1, sizeof(file), f);
    failed = ferror(f);
    fclose(f);
    if (failed)
        return PC_SAVE_UNREADABLE;
    if (n != PC_SAVE_BYTES && n != PC_SAVE_BYTES_OLD)
        return PC_SAVE_WRONG_SIZE;
    file_bytes = n;
    memset(fw, 0x00, PC_SAVE_BYTES);
    for (i = 0; i < (uint32_t)n; i++)
        fw[i ^ 1u] = file[i];
    file_check = pc_save_check(fw, n);
    if (file_check == PC_SAVE_NOT_A_SAVE && n == PC_SAVE_BYTES_OLD)
        snprintf(file_why, sizeof(file_why), "it is 4,096 bytes with the cart's copies in it, an 8 KB save cut short");
    if (file_check == PC_SAVE_GOOD && backup_read(path, backup) && (blanked = pc_save_blanked(fw, backup)) != 0) {
        file_check = PC_SAVE_NOT_A_SAVE;
        snprintf(file_why, sizeof(file_why), "save %d's place is blank in it but whole in " PC_SAVE_BACKUP ", and the cart never blanks a save it has written", blanked);
    }
    return (long)n;
}

int pc_save_loaded_check(void) { return file_check; }
const char *pc_save_loaded_why(void) { return file_why; }

/* Everything written so far on its way to the disk. Without this a full disk could show up only after the rename had replaced a good save. */
static int flush_to_disk(FILE *f)
{
    if (fflush(f) != 0)
        return -1;
#ifdef _WIN32
    if (_commit(_fileno(f)) != 0)
        return -1;
#endif
    return 0;
}

/* A whole file at a temporary name, on its way to the disk, or no file there at all. */
static int write_whole(const char *temp, const uint8_t *bytes, size_t n)
{
    FILE *f = fopen(temp, "wb");
    if (!f)
        return -1;
    if (fwrite(bytes, 1, n, f) != n || flush_to_disk(f) != 0) {
        fclose(f);
        remove(temp);
        return -1;
    }
    if (fclose(f) != 0) {
        remove(temp);
        return -1;
    }
    return 0;
}

/* One step that replaces the old file, so there is no moment with neither file whole. The plain C rename does not replace a file on Windows. */
static int move_over(const char *temp, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(temp, path) == 0 ? 0 : -1;
#endif
}

static void file_layout(uint8_t *out, const uint8_t *fw)
{
    uint32_t i;
    for (i = 0; i < PC_SAVE_BYTES; i++)
        out[i] = fw[i ^ 1u];
}

int pc_save_store(const char *path, const uint8_t *fw)
{
    static uint8_t out[PC_SAVE_BYTES];
    char temp[1100];
    newest_in_temp = copy_failed = 0;
    file_layout(out, fw);
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp))
        return -1;
    if (write_whole(temp, out, PC_SAVE_BYTES) != 0)
        return -1;
    if (move_over(temp, path) != 0) {
        newest_in_temp = 1;
        return -1;
    }
    return 0;
}

/* bytes, n of them, to path + ending, through a temporary name. 0, or -1 with nothing left at the temporary name. */
static int keep_copy(const char *path, const char *ending, const uint8_t *bytes, size_t n)
{
    char copy[1100], temp[1120];
    if (snprintf(copy, sizeof(copy), "%s%s", path, ending) >= (int)sizeof(copy))
        return -1;
    snprintf(temp, sizeof(temp), "%s.tmp", copy);
    if (write_whole(temp, bytes, n) != 0 || move_over(temp, copy) != 0) {
        remove(temp);
        return -1;
    }
    return 0;
}

/* The backup as it is now, to .bak.kept. No backup: nothing to keep. One that is there and cannot be read: -1, as it cannot be kept. */
static int keep_the_backup(const char *path)
{
    static uint8_t backup[PC_SAVE_BYTES + 1u];
    char name[1100];
    FILE *f;
    size_t n;
    int failed;
    copy_source = PC_SAVE_BACKUP;
    copy_ending = PC_SAVE_BACKUP_KEPT;
    if (snprintf(name, sizeof(name), "%s%s", path, PC_SAVE_BACKUP) >= (int)sizeof(name))
        return -1;
    f = fopen(name, "rb");
    if (!f)
        return no_such_file(name) ? 0 : -1;
    n = fread(backup, 1, sizeof(backup), f);
    failed = ferror(f);
    fclose(f);
    if (failed)
        return -1;
    return keep_copy(path, PC_SAVE_BACKUP_KEPT, backup, n);
}

int pc_save_replace(const char *path, const uint8_t *fw)
{
    /* Once a run, before the first replacement. A file that checked good becomes the backup. Any other start, no file, a blank one or one that did not check, writes a save that checks good
       at the next run and would take the backup's place then, so the backup itself is kept first, as .bak.kept, which only such a run writes. A file that did not check is also kept apart, so it
       is not lost. A blank one is not worth keeping. */
    if (!copy_kept) {
        int failed;
        copy_source = "";
        if (file_check == PC_SAVE_GOOD && file_bytes) {
            copy_ending = PC_SAVE_BACKUP;
            failed = keep_copy(path, PC_SAVE_BACKUP, file, file_bytes) != 0;
        } else {
            failed = keep_the_backup(path) != 0;
            if (!failed && file_check == PC_SAVE_NOT_A_SAVE && file_bytes) {
                copy_source = "";
                copy_ending = PC_SAVE_KEPT_APART;
                failed = keep_copy(path, PC_SAVE_KEPT_APART, file, file_bytes) != 0;
            }
        }
        if (failed) {
            static uint8_t out[PC_SAVE_BYTES];
            char temp[1110];
            /* The old save stays, and the newest bytes wait where a player can find them. The next try makes the copy again. */
            file_layout(out, fw);
            snprintf(temp, sizeof(temp), "%s.tmp", path);
            newest_in_temp = write_whole(temp, out, PC_SAVE_BYTES) == 0;
            copy_failed = 1;
            return -1;
        }
        copy_kept = 1;
    }
    return pc_save_store(path, fw);
}

void pc_save_forget_temp(const char *path)
{
    char temp[1110];
    if (!newest_in_temp)
        return;
    newest_in_temp = 0;
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) < (int)sizeof(temp))
        remove(temp);
}

int pc_save_newest_in_temp(void) { return newest_in_temp; }
int pc_save_copy_failed(void) { return copy_failed; }
const char *pc_save_copy_ending(void) { return copy_ending ? copy_ending : PC_SAVE_BACKUP; }
const char *pc_save_copy_source(void) { return copy_source ? copy_source : ""; }

int pc_save_backup_there(const char *path)
{
    char name[1100];
    if (snprintf(name, sizeof(name), "%s%s", path, PC_SAVE_BACKUP) >= (int)sizeof(name))
        return 0;
#ifdef _WIN32
    return GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES;
#else
    {
        FILE *f = fopen(name, "rb");
        if (f)
            fclose(f);
        return f != NULL;
    }
#endif
}
