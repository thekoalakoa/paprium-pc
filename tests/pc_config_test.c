/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The player's configuration file, the game file's hash and the save file, on made-up data. The hash's expected values are the worked examples of the public standard's test vectors. No SDL here.
 *
 * The test build renames fclose and fflush in every file to the two functions below, so a test can make them fail as a full disk does.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "pc_config.h"
#include "pc_pace.h"
#include "pc_save.h"

static int checks, failed;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

#undef fclose
#undef fflush
int fclose(FILE *f);
int fflush(FILE *f);
static int close_fails, flush_fails;
int test_fclose(FILE *f) { int r = fclose(f); return close_fails ? EOF : r; }
int test_fflush(FILE *f) { int r = fflush(f); return flush_fails ? EOF : r; }

static void hash_is(const char *text, const char *want)
{
    char out[65];
    pc_sha256_hex(text, strlen(text), out);
    CHECK(!strcmp(out, want));
}

static void put_file(const char *name, uint8_t fill, size_t size)
{
    FILE *f = fopen(name, "wb");
    size_t i;
    CHECK(f != NULL);
    if (f) {
        for (i = 0; i < size; i++)
            fputc(fill, f);
        fclose(f);
    }
}

/* ---- a save that cannot be taken at start. Taking any of these as a blank cart would write a blank cart's save over the file as soon as the game saves. */
static void test_save_refused(void)
{
    static uint8_t fw[PC_SAVE_BYTES], back[PC_SAVE_BYTES], other[PC_SAVE_BYTES];
    static const size_t wrong_sizes[] = {0, 1, 2, 2048, PC_SAVE_BYTES_OLD - 1u, PC_SAVE_BYTES_OLD + 1u, PC_SAVE_BYTES - 1u, PC_SAVE_BYTES + 1u, PC_SAVE_BYTES * 2u};
    const char *name = "pc_config_test_refused.sav";
    HANDLE held;
    unsigned i;

    for (i = 0; i < PC_SAVE_BYTES; i++) {
        fw[i] = (uint8_t)(i * 13u + 1u);
        other[i] = (uint8_t)~fw[i];
    }
    /* Only this means a new cart. */
    CHECK(pc_save_load("no_such_save.sav", back) == PC_SAVE_NO_FILE);
    /* A folder or a drive that is gone is not a new cart. */
    CHECK(pc_save_load("no_such_folder/no_such_save.sav", back) == PC_SAVE_UNREADABLE);
    /* A folder with the save's name. */
    CHECK(CreateDirectoryA("pc_config_test_folder.sav", NULL));
    CHECK(pc_save_load("pc_config_test_folder.sav", back) == PC_SAVE_UNREADABLE);
    RemoveDirectoryA("pc_config_test_folder.sav");

    /* Another program holds the file with no sharing, as a backup or a cloud client can. The read fails, the bytes are left alone, and once it lets go the save is there. */
    CHECK(pc_save_store(name, fw) == 0);
    held = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    memset(back, 0x77, sizeof(back));
    CHECK(pc_save_load(name, back) == PC_SAVE_UNREADABLE);
    CHECK(back[0] == 0x77 && back[PC_SAVE_BYTES - 1u] == 0x77);
    /* A write fails too while it is held. */
    CHECK(pc_save_store(name, other) == -1);
    CloseHandle(held);
    remove("pc_config_test_refused.sav.tmp");
    CHECK(pc_save_load(name, back) == PC_SAVE_BYTES && !memcmp(back, fw, sizeof(back)));

    /* Empty, cut short, or too long: read to the end, not a save, and the bytes are left alone. The two good sizes are the ones tested in main. */
    for (i = 0; i < sizeof(wrong_sizes) / sizeof(wrong_sizes[0]); i++) {
        put_file(name, 0x33, wrong_sizes[i]);
        memset(back, 0x77, sizeof(back));
        CHECK(pc_save_load(name, back) == PC_SAVE_WRONG_SIZE);
        CHECK(back[0] == 0x77 && back[PC_SAVE_BYTES - 1u] == 0x77);
    }

    /* The last of the temporary file does not reach the disk. The old save stays, and no temporary file is left. */
    CHECK(pc_save_store(name, fw) == 0);
    close_fails = 1;
    CHECK(pc_save_store(name, other) == -1);
    close_fails = 0;
    CHECK(pc_save_load(name, back) == PC_SAVE_BYTES && !memcmp(back, fw, sizeof(back)));
    CHECK(pc_save_load("pc_config_test_refused.sav.tmp", back) == PC_SAVE_NO_FILE);
    flush_fails = 1;
    CHECK(pc_save_store(name, other) == -1);
    flush_fails = 0;
    CHECK(pc_save_load(name, back) == PC_SAVE_BYTES && !memcmp(back, fw, sizeof(back)));
    CHECK(pc_save_load("pc_config_test_refused.sav.tmp", back) == PC_SAVE_NO_FILE);
    remove(name);
}

/* ---- the copy of the save as it was loaded. A good save in another byte order reads as blank, and the game writes over it: the copy is what is left of it. */
static int file_is(const char *name, const uint8_t *bytes, size_t size)
{
    static uint8_t got[PC_SAVE_BYTES + 16u];
    FILE *f = fopen(name, "rb");
    size_t n;
    if (!f)
        return 0;
    n = fread(got, 1, sizeof(got), f);
    fclose(f);
    return n == size && !memcmp(got, bytes, size);
}

static int no_file(const char *name)
{
    return GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES;
}

static void to_file_layout(uint8_t *out, const uint8_t *fw)
{
    unsigned i;
    for (i = 0; i < PC_SAVE_BYTES; i++)
        out[i] = fw[i ^ 1u];
}

static void put_bytes(const char *name, const uint8_t *bytes, size_t size)
{
    FILE *f = fopen(name, "wb");
    CHECK(f != NULL);
    if (f) {
        fwrite(bytes, 1, size, f);
        fclose(f);
    }
}

/* A copy in the cart's save format, made up: made-up bytes, then the sequence number, the save's number, the byte that makes the XOR 0 and the two closing bytes. In the firmware's byte order. */
static void put_copy(uint8_t *fw, unsigned at, unsigned n, unsigned id, uint32_t sequence, unsigned seed)
{
    unsigned i;
    uint8_t x = 0;
    for (i = 0; i < n - 8u; i++)
        fw[at + i] = (uint8_t)(i * 13u + seed * 7u + 1u);
    for (i = 0; i < 4u; i++)
        fw[at + n - 8u + i] = (uint8_t)(sequence >> (8u * i));
    fw[at + n - 4u] = (uint8_t)id;
    fw[at + n - 3u] = 0;
    fw[at + n - 2u] = 0xC2;
    fw[at + n - 1u] = 0x3F;
    for (i = 0; i < n; i++)
        x ^= fw[at + i];
    fw[at + n - 3u] = x;
}

/* A whole made-up save, two copies, in the file's byte order. */
static void made_up_save(uint8_t *file_out, unsigned seed)
{
    static uint8_t fw[PC_SAVE_BYTES];
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x000u, 0x200u, 4u, 2u + seed, seed);
    put_copy(fw, 0x400u, 0x100u, 1u, 2u, seed + 1u);
    to_file_layout(file_out, fw);
}

/* A made-up save with a copy in each of the cart's four places, as a player's save after some play, in the firmware's byte order. */
static const struct { unsigned number, at, length; } four[4] = {{4u, 0x000u, 0x200u}, {1u, 0x400u, 0x100u}, {2u, 0x600u, 0x100u}, {3u, 0x800u, 0x100u}};

static void four_copies(uint8_t *fw, unsigned seed)
{
    unsigned c;
    memset(fw, 0, PC_SAVE_BYTES);
    for (c = 0; c < 4u; c++)
        put_copy(fw, four[c].at, four[c].length, four[c].number, c ? 2u : 4u + seed, seed + c);
}

/* ---- the check a file must pass to become the backup */
static void test_save_check(void)
{
    static uint8_t file_bytes[PC_SAVE_BYTES], fw[PC_SAVE_BYTES], back[PC_SAVE_BYTES];
    const char *name = "pc_config_test_check.sav";
    unsigned i, c, flips = 0, refused = 0;
    /* A save in the cart's format checks good, with either copy alone. */
    made_up_save(file_bytes, 3u);
    put_bytes(name, file_bytes, PC_SAVE_BYTES);
    CHECK(pc_save_load(name, back) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_GOOD);
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x1F00u, 0x100u, 9u, 77u, 5u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_GOOD);
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x1000u, 8u, 1u, 1u, 0u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_GOOD);
    /* The same copy in save 4's own place is no copy of save 4: the place holds something the cart would not take. */
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x0u, 8u, 1u, 1u, 0u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    /* A copy of the right length in a place, with the wrong save's number. */
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x600u, 0x100u, 3u, 2u, 1u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    /* All four places filled checks good. One bit flipped anywhere in any of the four copies, trailer included, fails the file: every byte of each copy in turn, a different bit each time.
       Before, one good copy anywhere was enough, so a file with one damaged copy and three good ones checked good and became the backup. */
    four_copies(fw, 1u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_GOOD);
    for (c = 0; c < 4u; c++) {
        unsigned k;
        for (k = 0; k < four[c].length; k++) {
            unsigned at = four[c].at + k;
            fw[at] ^= (uint8_t)(1u << ((k + c) & 7u));
            flips++;
            refused += pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE;
            fw[at] ^= (uint8_t)(1u << ((k + c) & 7u));
        }
    }
    printf("a bit flipped in each byte of the four copies in turn: %u of %u files refused\n", refused, flips);
    CHECK(flips == 0x200u + 3u * 0x100u && refused == flips);
    /* A bit flipped in a place left blank: something is there that is no copy. */
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x000u, 0x200u, 4u, 5u, 2u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_GOOD);
    fw[0x6A0u] ^= 0x10u;
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    /* Bytes outside the places and with no trailer are not a copy, and change nothing: a Pocket's save can hold such bytes. */
    four_copies(fw, 2u);
    fw[0x1FFFu] ^= 0xFFu;
    fw[0x0C00u] = 0x5Au;
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_GOOD);
    /* Damage that cancels itself out, the same bit in two bytes of one copy, passes: an XOR cannot see it, the cart's own check either. */
    four_copies(fw, 3u);
    fw[0x010u] ^= 0x04u;
    fw[0x011u] ^= 0x04u;
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_GOOD);
    /* The same save in the other byte order holds no copy the cart would take. */
    for (i = 0; i < PC_SAVE_BYTES; i++)
        fw[i] = file_bytes[i];
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    /* Blank, as a new cart: every byte 0x00 or every byte 0xFF. */
    memset(fw, 0x00, sizeof(fw));
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_BLANK);
    memset(fw, 0xFF, sizeof(fw));
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_BLANK);
    put_bytes(name, fw, PC_SAVE_BYTES);
    CHECK(pc_save_load(name, back) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_BLANK);
    /* Made-up bytes with no copy in them, as a file of something else. */
    for (i = 0; i < PC_SAVE_BYTES; i++)
        fw[i] = (uint8_t)(i * 29u + 7u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    /* A copy whose closing bytes, sequence number or XOR byte are wrong is no copy. */
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x200u, 0x100u, 2u, 5u, 1u);
    fw[0x2FFu] = 0x3E;
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x200u, 0x100u, 2u, 0u, 1u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x200u, 0x100u, 2u, 0xFFFFFFFFu, 1u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    /* Zeros before the trailer, so no length but the copy's own could XOR to 0: with its XOR byte wrong by one bit it is no copy, and right it is one. */
    memset(fw, 0, sizeof(fw));
    fw[0x2F8u] = 5u;
    fw[0x2FCu] = 2u;
    fw[0x2FDu] = 0xFBu;
    fw[0x2FEu] = 0xC2u;
    fw[0x2FFu] = 0x3Fu;
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    fw[0x2FDu] = 0xFAu;
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_GOOD);
    /* A copy longer than the cart allows, 2 KB, whose XOR is 0 only over its whole length, is no copy either. Outside the four places, where a copy's length is not known here. */
    memset(fw, 0, sizeof(fw));
    fw[0x1000u] = 0x5Au;
    fw[0x17F8u] = 5u;
    fw[0x17FCu] = 2u;
    fw[0x17FDu] = 0xA0u;
    fw[0x17FEu] = 0xC2u;
    fw[0x17FFu] = 0x3Fu;
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_NOT_A_SAVE);
    /* The older 4 KB layout has no copies: not blank is good. */
    for (i = 0; i < PC_SAVE_BYTES_OLD; i++)
        fw[i] = (uint8_t)(i * 29u + 7u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES_OLD) == PC_SAVE_GOOD);
    memset(fw, 0, sizeof(fw));
    CHECK(pc_save_check(fw, PC_SAVE_BYTES_OLD) == PC_SAVE_BLANK);
    /* A 4 KB file with one of the cart's copies in its own place is no file of the older layout, which has none: it is an 8 KB save cut short, and does not check, whole copies or not. */
    four_copies(fw, 6u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_GOOD && pc_save_check(fw, PC_SAVE_BYTES_OLD) == PC_SAVE_NOT_A_SAVE);
    memset(fw, 0, sizeof(fw));
    put_copy(fw, 0x600u, 0x100u, 2u, 2u, 1u);
    CHECK(pc_save_check(fw, PC_SAVE_BYTES_OLD) == PC_SAVE_NOT_A_SAVE);
    /* A place blank in a file but whole in its backup: save 2's, wiped to 0x00 or to 0xFF. The file alone checks good, as a save never written would. Blank in the backup too, or whole in the
       file, is no such place. */
    {
        static uint8_t backup[PC_SAVE_BYTES];
        four_copies(backup, 7u);
        memcpy(fw, backup, sizeof(fw));
        CHECK(pc_save_blanked(fw, backup) == 0);
        memset(fw + 0x600u, 0x00, 0x100u);
        CHECK(pc_save_check(fw, PC_SAVE_BYTES) == PC_SAVE_GOOD && pc_save_blanked(fw, backup) == 2);
        memset(fw + 0x600u, 0xFF, 0x100u);
        CHECK(pc_save_blanked(fw, backup) == 2);
        memset(fw, 0x00, 0x200u);
        CHECK(pc_save_blanked(fw, backup) == 4);
        memset(backup, 0x00, 0x200u);
        memset(backup + 0x600u, 0x00, 0x100u);
        CHECK(pc_save_blanked(fw, backup) == 0);
    }
    remove(name);
}

static void test_save_copy(void)
{
    static uint8_t raw[PC_SAVE_BYTES], raw2[PC_SAVE_BYTES], bad[PC_SAVE_BYTES], fw[PC_SAVE_BYTES], fw2[PC_SAVE_BYTES], scratch[PC_SAVE_BYTES], file_of_fw[PC_SAVE_BYTES], file_of_fw2[PC_SAVE_BYTES];
    const char *name = "pc_config_test_copy.sav", *copy = "pc_config_test_copy.sav.bak", *temp = "pc_config_test_copy.sav.tmp", *copy_temp = "pc_config_test_copy.sav.bak.tmp";
    const char *apart = "pc_config_test_copy.sav.failed_check", *kept = "pc_config_test_copy.sav.bak.kept";
    HANDLE held, held_file;
    unsigned i;
    /* The files a run loads, and what the game writes in a run: all saves in the cart's format, but bad, which is made-up bytes with no copy in them. Each holds saves 4 and 1, as a
       cart that has written those two always does after, each in its own place. */
    made_up_save(raw, 1u);
    made_up_save(raw2, 2u);
    memset(fw, 0, sizeof(fw));
    memset(fw2, 0, sizeof(fw2));
    put_copy(fw, 0x000u, 0x200u, 4u, 3u, 5u);
    put_copy(fw, 0x400u, 0x100u, 1u, 3u, 8u);
    put_copy(fw2, 0x000u, 0x200u, 4u, 4u, 6u);
    put_copy(fw2, 0x400u, 0x100u, 1u, 4u, 9u);
    for (i = 0; i < PC_SAVE_BYTES; i++)
        bad[i] = (uint8_t)(i * 29u + 7u);
    to_file_layout(file_of_fw, fw);
    to_file_layout(file_of_fw2, fw2);
    remove(copy);
    remove(apart);
    remove(kept);

    /* The first replacement of a run keeps the file as it was loaded, byte for byte. */
    put_bytes(name, raw, PC_SAVE_BYTES);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES);
    CHECK(no_file(copy));
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, raw, PC_SAVE_BYTES));
    CHECK(file_is(name, file_of_fw, PC_SAVE_BYTES));
    CHECK(no_file(temp) && no_file(copy_temp));
    /* The next one leaves the copy as it is. */
    put_bytes(copy, (const uint8_t *)"left alone", 10);
    CHECK(pc_save_replace(name, fw2) == 0);
    CHECK(file_is(copy, (const uint8_t *)"left alone", 10));
    CHECK(file_is(name, file_of_fw2, PC_SAVE_BYTES));
    /* The next run's first replacement takes that copy's place. */
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, file_of_fw2, PC_SAVE_BYTES));
    /* A save in the older layout, which has no copies, is kept at its own length. */
    put_bytes(name, bad, PC_SAVE_BYTES_OLD);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES_OLD && pc_save_loaded_check() == PC_SAVE_GOOD);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, bad, PC_SAVE_BYTES_OLD));

    /* While the copy cannot be made, nothing is replaced, the newest bytes wait beside the save, and the next try still makes the copy. */
    put_bytes(name, raw, PC_SAVE_BYTES);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES);
    held = CreateFileA(copy, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(pc_save_replace(name, fw) == -1);
    CHECK(pc_save_copy_failed() && pc_save_newest_in_temp());
    CHECK(file_is(name, raw, PC_SAVE_BYTES));
    CHECK(file_is(temp, file_of_fw, PC_SAVE_BYTES));
    CHECK(no_file(copy_temp));
    CloseHandle(held);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(!pc_save_copy_failed() && !pc_save_newest_in_temp());
    CHECK(file_is(copy, raw, PC_SAVE_BYTES));
    CHECK(file_is(name, file_of_fw, PC_SAVE_BYTES));
    CHECK(no_file(temp));

    /* The save itself held: the copy is made, the save stays, and the newest bytes are in the temporary file. */
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES);
    held = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(pc_save_replace(name, fw2) == -1);
    CHECK(!pc_save_copy_failed() && pc_save_newest_in_temp());
    CHECK(file_is(copy, file_of_fw, PC_SAVE_BYTES));
    CHECK(file_is(temp, file_of_fw2, PC_SAVE_BYTES));
    CloseHandle(held);
    remove(temp);
    remove(copy);
    remove(name);

    /* A run that started with no file, and no backup there, has nothing to copy, even straight after a good load in this test. */
    put_bytes(name, raw, PC_SAVE_BYTES);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES);
    remove(name);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_NO_FILE);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(no_file(copy) && no_file(kept));
    CHECK(file_is(name, file_of_fw, PC_SAVE_BYTES));
    remove(name);

    /* A BAD SAVE LEAVES THE LAST GOOD BACKUP ON DISK, in its run and in every run after. Run 1 loads a good save: it becomes the backup. Run 2 loads a file that is not a save, as one bad write
       would leave: the backup stays, a copy of it goes to .bak.kept, and the file is kept apart. Run 3 loads the good save the game wrote in run 2: that one is the backup now, and the backup
       from before run 2 is still in .bak.kept. Runs 4 and 5, good again, change .bak and leave .bak.kept alone. */
    put_bytes(name, raw, PC_SAVE_BYTES);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_GOOD);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, raw, PC_SAVE_BYTES) && no_file(apart) && no_file(kept));
    put_bytes(name, bad, PC_SAVE_BYTES);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_NOT_A_SAVE);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, raw, PC_SAVE_BYTES));
    CHECK(file_is(kept, raw, PC_SAVE_BYTES));
    CHECK(file_is(apart, bad, PC_SAVE_BYTES));
    CHECK(!strcmp(pc_save_copy_ending(), ".failed_check"));
    CHECK(file_is(name, file_of_fw, PC_SAVE_BYTES));
    CHECK(pc_save_replace(name, fw2) == 0);
    CHECK(file_is(copy, raw, PC_SAVE_BYTES) && file_is(apart, bad, PC_SAVE_BYTES) && file_is(kept, raw, PC_SAVE_BYTES));
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_GOOD);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, file_of_fw2, PC_SAVE_BYTES) && file_is(kept, raw, PC_SAVE_BYTES));
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_replace(name, fw2) == 0);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, file_of_fw2, PC_SAVE_BYTES) && file_is(kept, raw, PC_SAVE_BYTES));
    put_bytes(name, raw2, PC_SAVE_BYTES);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, raw2, PC_SAVE_BYTES) && file_is(apart, bad, PC_SAVE_BYTES) && file_is(kept, raw, PC_SAVE_BYTES));
    CHECK(!strcmp(pc_save_copy_ending(), ".bak"));
    /* The same with no file at all in run 2, as a save moved away or lost: .bak stays, its copy goes to .bak.kept, and the good runs after leave that copy alone. The next start without a
       good save keeps the .bak of that moment instead. */
    remove(kept);
    remove(name);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_NO_FILE);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, raw2, PC_SAVE_BYTES) && file_is(kept, raw2, PC_SAVE_BYTES) && !strcmp(pc_save_copy_ending(), ".bak.kept"));
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_GOOD);
    CHECK(pc_save_replace(name, fw2) == 0);
    CHECK(file_is(copy, file_of_fw, PC_SAVE_BYTES) && file_is(kept, raw2, PC_SAVE_BYTES));
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, file_of_fw2, PC_SAVE_BYTES) && file_is(kept, raw2, PC_SAVE_BYTES));
    remove(name);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_NO_FILE && pc_save_replace(name, fw) == 0);
    CHECK(file_is(kept, file_of_fw2, PC_SAVE_BYTES));
    /* One bit flipped in the first of four copies, the rest whole: refused, kept apart, and the backup stays and is kept. */
    {
        static uint8_t four_fw[PC_SAVE_BYTES], damaged[PC_SAVE_BYTES];
        remove(kept);
        remove(apart);
        put_bytes(copy, raw2, PC_SAVE_BYTES);
        four_copies(four_fw, 4u);
        four_fw[0x11u] ^= 0x01u;
        to_file_layout(damaged, four_fw);
        put_bytes(name, damaged, PC_SAVE_BYTES);
        CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_NOT_A_SAVE);
        CHECK(pc_save_replace(name, fw) == 0);
        CHECK(file_is(copy, raw2, PC_SAVE_BYTES) && file_is(kept, raw2, PC_SAVE_BYTES) && file_is(apart, damaged, PC_SAVE_BYTES));
        CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_replace(name, fw2) == 0);
        CHECK(file_is(copy, file_of_fw, PC_SAVE_BYTES) && file_is(kept, raw2, PC_SAVE_BYTES));
    }
    /* EACH OF THE FOUR COPIES DAMAGED IN TURN, and left so for two runs: refused both times, and .bak and .bak.kept are the backup from before after both. */
    {
        static uint8_t four_fw[PC_SAVE_BYTES], damaged[PC_SAVE_BYTES];
        unsigned c, run;
        for (c = 0; c < 4u; c++) {
            remove(kept);
            remove(apart);
            put_bytes(copy, raw2, PC_SAVE_BYTES);
            four_copies(four_fw, 8u + c);
            four_fw[four[c].at + 3u + c] ^= (uint8_t)(1u << c);
            to_file_layout(damaged, four_fw);
            for (run = 0; run < 2u; run++) {
                put_bytes(name, damaged, PC_SAVE_BYTES);
                CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_NOT_A_SAVE && !pc_save_loaded_why()[0]);
                CHECK(pc_save_replace(name, fw) == 0);
                CHECK(file_is(copy, raw2, PC_SAVE_BYTES) && file_is(kept, raw2, PC_SAVE_BYTES) && file_is(apart, damaged, PC_SAVE_BYTES));
            }
        }
    }
    /* A 4 KB CUT of a save with all four copies, which are whole in it: refused, said why, kept apart at its own length, and the backup stays and is kept. */
    {
        static uint8_t four_fw[PC_SAVE_BYTES], whole[PC_SAVE_BYTES];
        remove(kept);
        remove(apart);
        put_bytes(copy, raw2, PC_SAVE_BYTES);
        four_copies(four_fw, 12u);
        to_file_layout(whole, four_fw);
        put_bytes(name, whole, PC_SAVE_BYTES_OLD);
        CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES_OLD && pc_save_loaded_check() == PC_SAVE_NOT_A_SAVE);
        CHECK(!strcmp(pc_save_loaded_why(), "it is 4,096 bytes with the cart's copies in it, an 8 KB save cut short"));
        CHECK(pc_save_replace(name, fw) == 0);
        CHECK(file_is(copy, raw2, PC_SAVE_BYTES) && file_is(kept, raw2, PC_SAVE_BYTES) && file_is(apart, whole, PC_SAVE_BYTES_OLD) && file_is(name, file_of_fw, PC_SAVE_BYTES));
    }
    /* A WIPED PLACE: the backup holds save 2 and the file's place for it is all 0x00, then all 0xFF. Refused, said why, kept apart, and the backup stays and is kept; and so at every start
       while .bak holds save 2 and the file does not, as the game's own writes keep that place blank. With .bak gone, the same file checks good. A place blank in both is no fault. */
    {
        static uint8_t four_fw[PC_SAVE_BYTES], backup_file[PC_SAVE_BYTES], wiped[PC_SAVE_BYTES];
        unsigned k;
        four_copies(four_fw, 13u);
        to_file_layout(backup_file, four_fw);
        for (k = 0; k < 2u; k++) {
            remove(kept);
            remove(apart);
            put_bytes(copy, backup_file, PC_SAVE_BYTES);
            memset(four_fw + 0x600u, k ? 0xFF : 0x00, 0x100u);
            to_file_layout(wiped, four_fw);
            put_bytes(name, wiped, PC_SAVE_BYTES);
            CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_NOT_A_SAVE);
            CHECK(!strcmp(pc_save_loaded_why(), "save 2's place is blank in it but whole in .bak, and the cart never blanks a save it has written"));
            CHECK(pc_save_replace(name, fw) == 0);
            CHECK(file_is(copy, backup_file, PC_SAVE_BYTES) && file_is(kept, backup_file, PC_SAVE_BYTES) && file_is(apart, wiped, PC_SAVE_BYTES));
            CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_NOT_A_SAVE && pc_save_replace(name, fw2) == 0);
            CHECK(file_is(copy, backup_file, PC_SAVE_BYTES) && file_is(kept, backup_file, PC_SAVE_BYTES));
            four_copies(four_fw, 13u);
        }
        remove(copy);
        put_bytes(name, wiped, PC_SAVE_BYTES);
        CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_GOOD && !pc_save_loaded_why()[0]);
        put_bytes(copy, wiped, PC_SAVE_BYTES);
        CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_GOOD);
        CHECK(pc_save_replace(name, fw) == 0 && file_is(copy, wiped, PC_SAVE_BYTES));
    }
    /* .bak READ ONLY. At a good start the copy into it fails: nothing replaced, the newest bytes beside the save, the message names .bak; once writable the next try makes it. At a bad
       start .bak is only read: the run goes on, .bak untouched and kept. .bak a folder at a bad start: it cannot be read to keep, so nothing is replaced, and the message names it. */
    {
        remove(kept);
        remove(apart);
        put_bytes(copy, raw2, PC_SAVE_BYTES);
        put_bytes(name, raw, PC_SAVE_BYTES);
        CHECK(SetFileAttributesA(copy, FILE_ATTRIBUTE_READONLY));
        CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_GOOD);
        CHECK(pc_save_replace(name, fw) == -1 && pc_save_copy_failed() && !strcmp(pc_save_copy_ending(), ".bak") && pc_save_newest_in_temp());
        CHECK(file_is(name, raw, PC_SAVE_BYTES) && file_is(copy, raw2, PC_SAVE_BYTES) && file_is(temp, file_of_fw, PC_SAVE_BYTES));
        CHECK(SetFileAttributesA(copy, FILE_ATTRIBUTE_NORMAL));
        CHECK(pc_save_replace(name, fw) == 0 && file_is(copy, raw, PC_SAVE_BYTES) && file_is(name, file_of_fw, PC_SAVE_BYTES));
        pc_save_forget_temp(name);
        put_bytes(name, bad, PC_SAVE_BYTES);
        CHECK(SetFileAttributesA(copy, FILE_ATTRIBUTE_READONLY));
        CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_NOT_A_SAVE);
        CHECK(pc_save_replace(name, fw) == 0 && file_is(copy, raw, PC_SAVE_BYTES) && file_is(kept, raw, PC_SAVE_BYTES) && file_is(apart, bad, PC_SAVE_BYTES));
        CHECK(SetFileAttributesA(copy, FILE_ATTRIBUTE_NORMAL));
        remove(copy);
        remove(kept);
        CHECK(CreateDirectoryA(copy, NULL));
        put_bytes(name, bad, PC_SAVE_BYTES);
        CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES);
        CHECK(pc_save_replace(name, fw) == -1 && pc_save_copy_failed() && !strcmp(pc_save_copy_ending(), ".bak.kept") && !strcmp(pc_save_copy_source(), ".bak"));
        CHECK(file_is(name, bad, PC_SAVE_BYTES) && no_file(kept));
        RemoveDirectoryA(copy);
        remove(temp);
        put_bytes(copy, file_of_fw, PC_SAVE_BYTES);     /* the backup as the one-bit route above left it, for the route below */
    }
    /* .bak.kept that cannot be written: nothing replaced, the newest bytes beside the save, and the message names the backup and the copy. Once free, the next try makes it. */
    put_bytes(name, bad, PC_SAVE_BYTES);
    put_bytes(kept, (const uint8_t *)"held", 4);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES);
    held_file = CreateFileA(kept, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held_file != INVALID_HANDLE_VALUE);
    CHECK(pc_save_replace(name, fw) == -1);
    CHECK(pc_save_copy_failed() && !strcmp(pc_save_copy_ending(), ".bak.kept") && !strcmp(pc_save_copy_source(), ".bak"));
    CHECK(file_is(name, bad, PC_SAVE_BYTES) && file_is(copy, file_of_fw, PC_SAVE_BYTES) && file_is(temp, file_of_fw, PC_SAVE_BYTES));
    CloseHandle(held_file);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(kept, file_of_fw, PC_SAVE_BYTES) && file_is(apart, bad, PC_SAVE_BYTES) && no_file(temp));
    remove(kept);
    put_bytes(copy, raw2, PC_SAVE_BYTES);
    /* A good save in the other byte order is kept apart too, whole, and the backup stays. */
    {
        static uint8_t other_order[PC_SAVE_BYTES];
        for (i = 0; i < PC_SAVE_BYTES; i++)
            other_order[i] = fw2[i];
        put_bytes(name, other_order, PC_SAVE_BYTES);
        CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_NOT_A_SAVE);
        CHECK(pc_save_replace(name, fw) == 0);
        CHECK(file_is(copy, raw2, PC_SAVE_BYTES) && file_is(apart, other_order, PC_SAVE_BYTES));
    }
    /* A blank file is not kept at all, and the backup stays. */
    memset(scratch, 0, sizeof(scratch));
    put_bytes(name, scratch, PC_SAVE_BYTES);
    remove(apart);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES && pc_save_loaded_check() == PC_SAVE_BLANK);
    CHECK(pc_save_replace(name, fw) == 0);
    CHECK(file_is(copy, raw2, PC_SAVE_BYTES) && no_file(apart));
    /* A file kept apart that cannot be written: nothing replaced, and the message's name for it is the one that failed. */
    put_bytes(name, bad, PC_SAVE_BYTES);
    put_bytes(apart, (const uint8_t *)"held", 4);
    CHECK(pc_save_load(name, scratch) == PC_SAVE_BYTES);
    held = CreateFileA(apart, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(pc_save_replace(name, fw) == -1);
    CHECK(pc_save_copy_failed() && !strcmp(pc_save_copy_ending(), ".failed_check"));
    CHECK(file_is(name, bad, PC_SAVE_BYTES) && file_is(copy, raw2, PC_SAVE_BYTES));
    CloseHandle(held);
    /* What that failure left in the temporary file goes once the save holds the newest bytes again, and only then. */
    CHECK(pc_save_newest_in_temp() && !no_file(temp));
    pc_save_forget_temp(name);
    CHECK(!pc_save_newest_in_temp() && no_file(temp));
    put_bytes(temp, (const uint8_t *)"not ours", 8);
    pc_save_forget_temp(name);
    CHECK(file_is(temp, (const uint8_t *)"not ours", 8));
    remove(temp);
    remove(apart);
    remove(copy);
    remove(kept);
    remove(name);
}

/* ---- the pace and the vsync rule, against a model of the run loop and a monitor: pc_pace.h's own pace, the same calls pc_main.c makes. With vsync on, a present here waits for the monitor's next
   refresh, as a blocking present would; with it off, it takes 0.3 ms. A frame the pace leaves unshown has no present. This says what the rules do with such a monitor. Whether a shown window's
   present blocks that way is not something an automated run can see. Times in milliseconds, in a double each, handed to pc_pace.h in nanoseconds: a model, not a measurement. The numbers of a
   player's run come from his freeze log: a 240 Hz monitor, 3 presents of 19 ms that left the game 37 ms behind, one gap of 232 ms in the window's events, and GPU wakes that held one present for
   0.1 to 2.3 seconds. */
#define GAME_FRAME_MS (3420.0 * 262.0 / 53693175.0 * 1000.0)    /* a frame's master clocks, over the master clock's rate */
#define UNITS 1000000.0                                          /* nanoseconds in a millisecond */

typedef struct Monitor {
    double hz;                  /* the monitor's refresh rate */
    double work;                /* each frame's own work */
    int frames;
    int hitch_every;            /* every so many frames, one frame's work takes hitch_ms */
    double hitch_ms;
    int oversleep_every;        /* every so many frames, the sleep before the present runs oversleep_ms past its time */
    double oversleep_ms;
    int slow_first, slow_last;  /* on these frames the present itself takes slow_ms before its wait: a GPU busy, or a slow stretch */
    double slow_ms;
    int slow2_first, slow2_last;    /* a second such stretch */
    int stuck_frame;            /* one present held this long, as a GPU waking up */
    int burst_every;            /* every so many frames, a shown frame's present takes burst_ms more before its wait: slow presents now and then, as a busy GPU gives */
    double burst_ms;
    double stuck_ms;
    int watch_first, watch_last;    /* frames over which the most the game fell behind its pace is kept */
    int heavy_first, heavy_last;    /* on these frames the work takes heavy_ms more: a heavy scene on a slow PC */
    double heavy_ms;
} Monitor;

typedef struct Watched {
    int first_off, first_on, second_off, second_on;    /* frames, or -1 */
    double first_off_ms;        /* the run's time at the first switch off */
    double behind_at;           /* how far behind the pace the game was at frame behind_frame */
    double most_behind;         /* the most it fell behind over the watched frames */
    int offs, ons, on_at_end, unshown, unshown_watched, calm_needed, last_soon, last_at_once, no_returns;
    int64_t drops;
} Watched;

static double ms_of(uint64_t ns) { return (double)ns / UNITS; }

static void model_run(const Monitor *m, Watched *r, int behind_frame)
{
    const double refresh = 1000.0 / m->hz;
    double now = 0, last_present = 0, last_point = -1;
    int vsync = 1, n, last_shown = 1, last_behind = 0;
    PcPace pace;
    PcVsyncWatch w;
    memset(&pace, 0, sizeof(pace));
    pc_pace_start(&pace, 0, (uint64_t)(GAME_FRAME_MS * UNITS));
    pc_vsync_watch_start(&w);
    pc_vsync_watch_refresh(&w, (int64_t)(refresh * UNITS));
    memset(r, 0, sizeof(*r));
    r->first_off = r->first_on = r->second_off = r->second_on = -1;
    for (n = 0; n < m->frames; n++) {
        const double work = (m->hitch_every && n && n % m->hitch_every == 0 ? m->hitch_ms : m->work) + (n >= m->heavy_first && n <= m->heavy_last ? m->heavy_ms : 0.0);
        double point, start, behind;
        int shown;
        now += work;
        behind = now - ms_of(pace.next);    /* how far past its own time this frame ended */
        if (n == behind_frame)
            r->behind_at = behind;
        if (n >= m->watch_first && n <= m->watch_last && behind > r->most_behind)
            r->most_behind = behind;
        point = ms_of(pc_pace_point(&pace, (uint64_t)(now * UNITS)));
        if (n >= 60 && last_point >= 0) {
            int action = pc_vsync_watch(&w, vsync, (int64_t)((point - last_point) * UNITS), last_shown ? (int64_t)(last_present * UNITS) : 0, (int64_t)(GAME_FRAME_MS * UNITS), last_shown,
                                        last_behind);
            if (action == PC_VSYNC_TURN_OFF) {
                vsync = 0;
                r->last_soon = w.soon;
                r->last_at_once = w.at_once;
                if (r->first_off < 0) {
                    r->first_off = n;
                    r->first_off_ms = now;
                } else if (r->second_off < 0) {
                    r->second_off = n;
                }
            } else if (action == PC_VSYNC_TURN_ON) {
                vsync = 1;
                if (r->first_on < 0)
                    r->first_on = n;
                else if (r->second_on < 0)
                    r->second_on = n;
            }
        }
        last_point = point;
        last_behind = pace.behind > 0;
        if (now < point)
            now = point;
        if (m->oversleep_every && n && n % m->oversleep_every == 0)
            now += m->oversleep_ms;
        shown = pc_pace_show(&pace, (uint64_t)(now * UNITS));
        start = now;
        if (shown) {
            if ((n >= m->slow_first && n <= m->slow_last) || (n >= m->slow2_first && n <= m->slow2_last))
                now += m->slow_ms;
            if (n == m->stuck_frame)
                now += m->stuck_ms;
            if (m->burst_every && n % m->burst_every == 0)
                now += m->burst_ms;
            if (vsync)
                now = ((double)(int64_t)(now / refresh) + 1.0) * refresh;
            else
                now += 0.3;
        } else {
            r->unshown++;
            if (n >= m->watch_first && n <= m->watch_last)
                r->unshown_watched++;
        }
        last_present = now - start;
        last_shown = shown;
    }
    r->offs = w.offs;
    r->ons = w.ons;
    r->no_returns = w.no_returns;
    r->on_at_end = vsync;
    r->calm_needed = w.calm_needed;
    r->drops = (int64_t)pace.drops;
}

static Monitor plain(double hz, double work)
{
    Monitor m;
    memset(&m, 0, sizeof(m));
    m.hz = hz;
    m.work = work;
    m.frames = 36000;       /* ten minutes */
    m.slow_first = m.slow2_first = m.stuck_frame = m.watch_first = m.heavy_first = -1;
    m.slow_last = m.slow2_last = m.watch_last = m.heavy_last = -2;
    return m;
}

static void test_vsync_rule(void)
{
    static const double keep[] = {60.0, 59.95, 59.94, 60.02, 75.0, 120.0, 144.0, 165.0, 240.0};
    static const double work[] = {0.5, 1.5, 3.0, 8.0};
    static const double slow[] = {50.0, 30.0, 24.0, 55.0, 59.0, 59.5, 59.8};
    static const double stuck[] = {108.0, 232.0, 966.0, 2338.0};
    unsigned i, j, k;
    Watched r;
    Monitor m;
    for (i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        for (j = 0; j < sizeof(work) / sizeof(work[0]); j++) {
            /* A monitor as fast as the game or faster: vsync stays, frame after frame, even with a frame doubled now and then, and every frame is shown. */
            m = plain(keep[i], work[j]);
            model_run(&m, &r, -1);
            CHECK(r.offs == 0 && r.unshown == 0);
            /* A hitch now and then does not cost it: two frames' worth, or a long stall, or another program taking the processor while the loop sleeps. */
            m.hitch_every = 977;
            m.hitch_ms = 40.0;
            model_run(&m, &r, -1);
            CHECK(r.offs == 0);
            m.hitch_every = 3001;
            m.hitch_ms = 500.0;
            model_run(&m, &r, -1);
            CHECK(r.offs == 0);
            m = plain(keep[i], work[j]);
            m.oversleep_every = 601;
            m.oversleep_ms = 30.0;
            model_run(&m, &r, -1);
            CHECK(r.offs == 0);
            /* A GPU waking up holds one present, a tenth of a second to more than two seconds: vsync stays. */
            for (k = 0; k < sizeof(stuck) / sizeof(stuck[0]); k++) {
                m = plain(keep[i], work[j]);
                m.frames = 3000;
                m.stuck_frame = 900;
                m.stuck_ms = stuck[k];
                model_run(&m, &r, -1);
                CHECK(r.offs == 0);
            }
            /* A short slow stretch: half a second of presents that each take 25 ms. Vsync stays. */
            m = plain(keep[i], work[j]);
            m.frames = 3000;
            m.slow_first = 900;
            m.slow_last = 929;
            m.slow_ms = 25.0;
            model_run(&m, &r, -1);
            CHECK(r.offs == 0);
            if (r.offs)
                printf("half a second of 25 ms presents turned vsync off: %.2f Hz, %.1f ms of work, at frame %d\n", keep[i], work[j], r.first_off);
        }
    }

    /* The player's run: a 240 Hz monitor, frames of about 8 ms of work, and three presents of 19 ms. Without frames left unshown his log had the game 37 ms behind at the third, and an older rule
       turned vsync off for the rest of the run there. Now, once the game is a whole frame behind, the frames that would show it are not shown, so fewer slow presents happen, and vsync stays. */
    m = plain(240.0, 8.0);
    m.frames = 3000;
    m.slow_first = 900;
    m.slow_last = 902;
    m.slow_ms = 19.0;
    m.watch_first = 900;
    m.watch_last = 1000;
    model_run(&m, &r, 903);
    printf("the player's three presents of 19 ms at 240 Hz: at most %.1f ms behind the pace, vsync went off %d times, %d frames unshown\n", r.most_behind, r.offs, r.unshown);
    CHECK(r.most_behind > 15.0 && r.most_behind < 41.0);
    CHECK(r.offs == 0 && r.unshown >= 1 && r.unshown <= 3);
    /* The same run's 232 ms in the window's events, at frame 3678. */
    m = plain(240.0, 8.0);
    m.frames = 6000;
    m.hitch_every = 3678;
    m.hitch_ms = 232.0;
    model_run(&m, &r, -1);
    CHECK(r.offs == 0);
    /* Six seconds of frames of 100 ms each, as a window held by a drag: every one a hitch, and none counts. */
    for (i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        m = plain(keep[i], 3.0);
        m.frames = 3000;
        m.slow_first = 900;
        m.slow_last = 960;
        m.slow_ms = 100.0;
        model_run(&m, &r, -1);
        CHECK(r.offs == 0);
    }
    /* A present that runs 14 ms long on every fourth frame, made up on the next, for ten minutes on a 240 Hz monitor: the time comes back, so vsync stays. */
    {
        /* The model's slow frames are one run, so this one is built here: frames whose number is 0 modulo 4 present for 14 ms more. */
        const double refresh = 1000.0 / 240.0;
        double now = 0, last_present = 0, last_point = -1;
        int n, offs = 0, last_shown = 1, last_behind = 0;
        PcPace pace;
        PcVsyncWatch w;
        memset(&pace, 0, sizeof(pace));
        pc_pace_start(&pace, 0, (uint64_t)(GAME_FRAME_MS * UNITS));
        pc_vsync_watch_start(&w);
        pc_vsync_watch_refresh(&w, (int64_t)(refresh * UNITS));
        for (n = 0; n < 36000; n++) {
            double point, start;
            int shown;
            now += 3.0;
            point = ms_of(pc_pace_point(&pace, (uint64_t)(now * UNITS)));
            if (n >= 60 && last_point >= 0 && pc_vsync_watch(&w, 1, (int64_t)((point - last_point) * UNITS), last_shown ? (int64_t)(last_present * UNITS) : 0, (int64_t)(GAME_FRAME_MS * UNITS), last_shown,
                                                             last_behind) == PC_VSYNC_TURN_OFF)
                offs++;
            last_point = point;
            last_behind = pace.behind > 0;
            if (now < point)
                now = point;
            shown = pc_pace_show(&pace, (uint64_t)(now * UNITS));
            start = now;
            if (shown) {
                if (n % 4 == 0)
                    now += 14.0;
                now = ((double)(int64_t)(now / refresh) + 1.0) * refresh;
            }
            last_present = now - start;
            last_shown = shown;
        }
        CHECK(offs == 0);
    }
    /* BURSTS OF SLOW PRESENTS on a fast monitor: for two minutes one present in every 5, 10 or 15 takes 25, 30 or 40 ms more, as a busy GPU can make it, 4 to 12 times a second. Each puts the
       game behind, the frames after it catch up, and no present counts more than one refresh: vsync stays, on a 240 Hz monitor at all of these, on a 144 Hz one at one in every 10 or 15. An
       earlier rule counted the whole of each slow present, and not the time given back by the frames left unshown after it, and turned vsync off at 240 Hz here. */
    {
        static const double fast[] = {240.0, 144.0};
        static const int every[] = {5, 10, 15};
        static const double extra[] = {25.0, 30.0, 40.0};
        unsigned e, x;
        int worst = 0;
        for (i = 0; i < sizeof(fast) / sizeof(fast[0]); i++)
            for (e = 0; e < sizeof(every) / sizeof(every[0]); e++)
                for (x = 0; x < sizeof(extra) / sizeof(extra[0]); x++) {
                    if (fast[i] < 200.0 && every[e] < 10)
                        continue;
                    m = plain(fast[i], 8.0);
                    m.frames = 900 + 7200;
                    m.burst_every = every[e];
                    m.burst_ms = extra[x];
                    model_run(&m, &r, -1);
                    CHECK(r.offs == 0);
                    if (r.offs > worst)
                        worst = r.offs;
                }
        printf("bursts of slow presents, 25 to 40 ms longer, at 240 Hz 4 to 12 times a second and at 144 Hz 4 to 6, for two minutes: vsync went off at most %d times\n", worst);
    }
    /* The rule on its own, frame by frame, in microseconds, every frame beginning behind its pace: three quarter seconds of frames 8 ms late with 25 ms presents, then a quarter second where
       one frame is 1 ms late, as the frames' drift against a monitor's refresh can make. Four slow quarters, but not steady: vsync stays. With a fourth quarter like the first three it goes, and
       the line can say the longest present and the longest frame without it. A frame after one left unshown counts as neither: here every other frame loses 8 ms and the one after an unshown
       frame comes 8 ms early, which would give it all back if it counted, as on a monitor much slower than the game. And frames that began on time count nothing, however late they end. */
    {
        const int64_t f = 16688;
        int n, action = PC_VSYNC_KEEP;
        PcVsyncWatch w;
        pc_vsync_watch_start(&w);
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && action == PC_VSYNC_KEEP; n++)
            action = pc_vsync_watch(&w, 1, n < 3 * PC_VSYNC_QUARTER_FRAMES ? f + 8000 : n == 3 * PC_VSYNC_QUARTER_FRAMES ? f + 1000 : f, 25000, f, 1, 1);
        CHECK(action == PC_VSYNC_KEEP);
        pc_vsync_watch_start(&w);
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && action == PC_VSYNC_KEEP; n++)
            action = pc_vsync_watch(&w, 1, f + 8000, n == 7 ? 22000 : 20000, f, 1, 1);
        CHECK(action == PC_VSYNC_TURN_OFF && n == 4 * PC_VSYNC_QUARTER_FRAMES && w.why_loss == 4 * PC_VSYNC_QUARTER_FRAMES * 8000);
        CHECK(w.why_present == 22000 && w.why_rest == f + 8000 - 20000);
        /* With the monitor's refresh known, 4,167 microseconds at 240 Hz, no frame counts more than one refresh, however long its present. */
        pc_vsync_watch_start(&w);
        pc_vsync_watch_refresh(&w, 4167);
        action = PC_VSYNC_KEEP;
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && action == PC_VSYNC_KEEP; n++)
            action = pc_vsync_watch(&w, 1, f + 8000, 20000, f, 1, 1);
        CHECK(action == PC_VSYNC_TURN_OFF && w.why_loss == 4 * PC_VSYNC_QUARTER_FRAMES * 4167);
        pc_vsync_watch_start(&w);
        action = PC_VSYNC_KEEP;
        for (n = 0; n < 8 * PC_VSYNC_QUARTER_FRAMES && action == PC_VSYNC_KEEP; n++)
            action = pc_vsync_watch(&w, 1, n & 1 ? f + 8000 : f - 8000, n & 1 ? 25000 : 0, f, n & 1, 1);
        CHECK(action == PC_VSYNC_TURN_OFF && n == 4 * PC_VSYNC_QUARTER_FRAMES);
        pc_vsync_watch_start(&w);
        action = PC_VSYNC_KEEP;
        for (n = 0; n < 8 * PC_VSYNC_QUARTER_FRAMES && action == PC_VSYNC_KEEP; n++)
            action = pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 0);
        CHECK(action == PC_VSYNC_KEEP);
    }
    /* The wait to come back on: 10 seconds; twice that when vsync goes off again within a minute of coming back; 10 seconds again once it has held for a minute. */
    {
        const int64_t f = 16688;
        int n, k, waits[3];
        PcVsyncWatch w;
        pc_vsync_watch_start(&w);
        for (k = 0; k < 3; k++) {
            int action = PC_VSYNC_KEEP;
            for (n = 0; n < (k == 2 ? PC_VSYNC_SOON_FRAMES : PC_VSYNC_QUARTER_FRAMES); n++)
                pc_vsync_watch(&w, 1, f, 2000, f, 1, 0);
            for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && action == PC_VSYNC_KEEP; n++)
                action = pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 1);
            CHECK(action == PC_VSYNC_TURN_OFF && !w.at_once && !w.no_returns);
            for (n = 1; n < 4 * PC_VSYNC_CALM_MOST && pc_vsync_watch(&w, 0, f, 2000, f, 1, 0) != PC_VSYNC_TURN_ON; n++)
                ;
            waits[k] = n;
        }
        printf("the waits to come back on: %d, %d, %d frames\n", waits[0], waits[1], waits[2]);
        CHECK(waits[0] == PC_VSYNC_CALM_FRAMES && waits[1] == 2 * PC_VSYNC_CALM_FRAMES && waits[2] == PC_VSYNC_CALM_FRAMES);
        /* and it stops growing at its most */
        for (k = 0; k < 10; k++) {
            int action = PC_VSYNC_KEEP;
            for (n = 0; n < PC_VSYNC_QUARTER_FRAMES; n++)
                pc_vsync_watch(&w, 1, f, 2000, f, 1, 0);
            for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && action == PC_VSYNC_KEEP; n++)
                action = pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 1);
            for (n = 1; n < 4 * PC_VSYNC_CALM_MOST && pc_vsync_watch(&w, 0, f, 2000, f, 1, 0) != PC_VSYNC_TURN_ON; n++)
                ;
        }
        CHECK(n == PC_VSYNC_CALM_MOST);
        /* Off again on the first second's evidence after a return, the game behind from the moment vsync was on. With the monitor's refresh not known, or one that may keep the game's
           rate, that is one more off soon after a return: the wait doubles, as a heavy scene or a slow stretch still going when vsync came back on makes it. */
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 1) == PC_VSYNC_KEEP; n++)
            ;
        CHECK(n == 4 * PC_VSYNC_QUARTER_FRAMES - 1 && w.at_once && w.soon && !w.slow_monitor && !w.no_returns);
        for (n = 1; n < 4 * PC_VSYNC_CALM_MOST && pc_vsync_watch(&w, 0, f, 2000, f, 1, 0) != PC_VSYNC_TURN_ON; n++)
            ;
        CHECK(n == PC_VSYNC_CALM_MOST);
        pc_vsync_watch_refresh(&w, 16949);      /* 59 Hz as the system gives it: it may be 59.94 Hz */
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 1) == PC_VSYNC_KEEP; n++)
            ;
        CHECK(w.at_once && !w.slow_monitor && !w.no_returns);
        for (n = 1; n < 4 * PC_VSYNC_CALM_MOST && pc_vsync_watch(&w, 0, f, 2000, f, 1, 0) != PC_VSYNC_TURN_ON; n++)
            ;
        CHECK(n == PC_VSYNC_CALM_MOST);
        /* On a monitor clearly slower than the game, 58 Hz, the same ends the returns for the run, however calm it is after. */
        pc_vsync_watch_refresh(&w, 17241);
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 1) == PC_VSYNC_KEEP; n++)
            ;
        CHECK(n == 4 * PC_VSYNC_QUARTER_FRAMES - 1 && w.at_once && w.slow_monitor && w.no_returns);
        for (n = 0; n < 4 * PC_VSYNC_CALM_MOST; n++)
            CHECK(pc_vsync_watch(&w, 0, f, 2000, f, 1, 0) == PC_VSYNC_KEEP);
        /* A switch the loop found slow ends them too: with vsync on, the next off is the last. */
        pc_vsync_watch_start(&w);
        pc_vsync_watch_no_returns(&w);
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 1) == PC_VSYNC_KEEP; n++)
            ;
        CHECK(w.offs == 1 && !w.at_once);
        for (n = 0; n < 4 * PC_VSYNC_CALM_MOST; n++)
            CHECK(pc_vsync_watch(&w, 0, f, 2000, f, 1, 0) == PC_VSYNC_KEEP);
        /* With vsync off, a frame of two game frames or more within a second of another starts the calm again, as slow frames that recur make: vsync does not come back on among
           them, and does 10 seconds after the last. Frames a little shorter do not, and neither does such a frame on its own every two seconds, as a busy PC makes. */
        pc_vsync_watch_start(&w);
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 1) == PC_VSYNC_KEEP; n++)
            ;
        CHECK(w.offs == 1);
        for (n = 0; n < 4 * PC_VSYNC_CALM_FRAMES; n++)
            CHECK(pc_vsync_watch(&w, 0, n % 10 == 9 ? 2 * f : f, 2000, f, 1, 0) == PC_VSYNC_KEEP);
        for (n = 1; n < 4 * PC_VSYNC_CALM_MOST && pc_vsync_watch(&w, 0, f, 2000, f, 1, 0) != PC_VSYNC_TURN_ON; n++)
            ;
        CHECK(n == PC_VSYNC_CALM_FRAMES);
        pc_vsync_watch_start(&w);
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 1) == PC_VSYNC_KEEP; n++)
            ;
        for (n = 1; n < 4 * PC_VSYNC_CALM_MOST && pc_vsync_watch(&w, 0, n % 10 == 9 ? 2 * f - 1 : f, 2000, f, 1, 0) != PC_VSYNC_TURN_ON; n++)
            ;
        CHECK(n == PC_VSYNC_CALM_FRAMES);
        pc_vsync_watch_start(&w);
        for (n = 0; n < 4 * PC_VSYNC_QUARTER_FRAMES && pc_vsync_watch(&w, 1, f + 8000, 25000, f, 1, 1) == PC_VSYNC_KEEP; n++)
            ;
        for (n = 1; n < 4 * PC_VSYNC_CALM_MOST && pc_vsync_watch(&w, 0, n % 120 == 119 ? 3 * f : f, 2000, f, 1, 0) != PC_VSYNC_TURN_ON; n++)
            ;
        CHECK(n == PC_VSYNC_CALM_FRAMES);
    }

    /* A HEAVY SCENE, OR A LONG SLOW STRETCH, ON A MONITOR THAT KEEPS THE GAME'S RATE. With vsync off the frames end in time, so vsync comes back on while the scene is still going, and goes
       off again at once; that only doubles the wait, and once the scene is over vsync comes back on to stay. An earlier rule ended the tries there for the rest of the run. Here 40 seconds of
       frames 12 ms heavier, then 80 calm seconds, on 60, 75, 144 and 240 Hz monitors; and 30 seconds where every present takes 6, 12 or 15 ms longer, then 60 calm seconds. */
    {
        static const double rates[] = {60.0, 75.0, 144.0, 240.0};
        static const double longer[] = {6.0, 12.0, 15.0};
        int at_once_seen = 0;
        for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
            m = plain(rates[i], 3.0);
            m.frames = 900 + 2400 + 4800;
            m.heavy_first = 900;
            m.heavy_last = 900 + 2399;
            m.heavy_ms = 12.0;
            model_run(&m, &r, -1);
            printf("40 s of frames 12 ms heavier at %.0f Hz: vsync off %d times, back on %d, %s at the end, the tries %s\n", rates[i], r.offs, r.ons, r.on_at_end ? "on" : "off",
                   r.no_returns ? "ended" : "going on");
            CHECK(!r.no_returns && r.on_at_end && r.ons == r.offs);
            at_once_seen += r.offs >= 2 && r.second_off >= 0 && r.second_off <= r.first_on + 4 * PC_VSYNC_QUARTER_FRAMES;
            for (j = 0; j < sizeof(longer) / sizeof(longer[0]); j++) {
                m = plain(rates[i], 8.0);
                m.frames = 900 + 1800 + 3600;
                m.slow_first = 900;
                m.slow_last = 900 + 1799;
                m.slow_ms = longer[j];
                model_run(&m, &r, -1);
                CHECK(!r.no_returns && r.on_at_end);
            }
        }
        CHECK(at_once_seen >= 2);
    }

    /* A MONITOR JUST SLOWER THAN THE GAME, by less than a thousandth: vsync stays, and the game keeps its own rate, a picture left out each time it falls a whole frame behind. Before, the game
       ran at the monitor's rate there and its sound ran dry over and over. */
    {
        static const double close[] = {59.9, 59.865, 59.887};
        for (i = 0; i < sizeof(close) / sizeof(close[0]); i++) {
            m = plain(close[i], 3.0);
            m.watch_first = 600;
            m.watch_last = 36000;
            model_run(&m, &r, -1);
            printf("a %.3f Hz monitor, 10 minutes: vsync off %d times, %d frames unshown, the game at most %.1f ms behind its pace\n", close[i], r.offs, r.unshown, r.most_behind);
            CHECK(r.offs == 0 && r.unshown > 0 && r.most_behind < GAME_FRAME_MS + 0.5);
        }
    }

    /* A LONG STALL AT 60 HZ: after a second held in one present, the stall's time is dropped and the last three frames of it are made up, unshown, at once: the game is back on its pace after the
       frames' own work, not three refreshes later. */
    for (i = 0; i < 2u; i++) {
        m = plain(i ? 240.0 : 60.0, 3.0);
        m.frames = 1200;
        m.stuck_frame = 900;
        m.stuck_ms = 1000.0;
        m.watch_first = 904;
        m.watch_last = 1200;
        model_run(&m, &r, 903);
        printf("a stall of a second at %.0f Hz: the frame after the make-up %.1f ms behind its pace, %d frames unshown, then at most %.1f ms behind\n", m.hz, r.behind_at, r.unshown, r.most_behind);
        CHECK(r.drops == 1 && r.unshown == PC_PACE_MAKE_UP && r.most_behind < 1.5 * GAME_FRAME_MS);
    }

    /* A long slow stretch on a good monitor, five seconds of 25 ms presents: the game keeps its pace, a picture left out when it falls a frame behind; vsync goes off once the presents have held it
       back for a second, and comes back on ten seconds after the stretch ends. Not for the rest of the run. */
    for (i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        m = plain(keep[i], 3.0);
        m.frames = 6000;
        m.slow_first = 900;
        m.slow_last = 1199;
        m.slow_ms = 25.0;
        m.watch_first = 900;
        m.watch_last = 1199;
        model_run(&m, &r, -1);
        if (keep[i] == 60.0 || keep[i] == 240.0)
            printf("five seconds of 25 ms presents at %.0f Hz: vsync off at frame %d, back on at %d; %d of the stretch's 300 frames unshown, the game at most %.1f ms behind its pace\n", keep[i], r.first_off, r.first_on,
                   r.unshown_watched, r.most_behind);
        CHECK(r.offs == 1 && r.ons == 1 && r.on_at_end);
        CHECK(r.first_off > 900 && r.first_off < 1050);
        CHECK(r.first_on >= 1199 + PC_VSYNC_CALM_FRAMES && r.first_on < 1199 + PC_VSYNC_CALM_FRAMES + 10);
        CHECK(r.most_behind < 3.0 * GAME_FRAME_MS && r.unshown_watched >= 60);
    }

    /* TWO SLOW STRETCHES on a good monitor, two seconds of 25 ms presents each. The second comes 40 s after vsync came back on: off again, and back on after twice the wait. 70 s after: off again,
       back on after the first wait. Either way vsync is on at the end: a monitor that keeps the game's rate never keeps vsync off. */
    for (i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        for (j = 0; j < 2u; j++) {
            const int after = j ? 4200 : 2400;
            m = plain(keep[i], 3.0);
            m.frames = 900 + 120 + PC_VSYNC_CALM_FRAMES + after + 120 + 4 * PC_VSYNC_CALM_FRAMES;
            m.slow_first = 900;
            m.slow_last = 1019;
            m.slow2_first = 1019 + PC_VSYNC_CALM_FRAMES + after;
            m.slow2_last = m.slow2_first + 119;
            m.slow_ms = 25.0;
            model_run(&m, &r, -1);
            CHECK(r.offs == 2 && r.ons == 2 && r.on_at_end);
            CHECK(r.last_soon == !j);
            CHECK(r.second_on >= m.slow2_last + (j ? 1 : 2) * PC_VSYNC_CALM_FRAMES && r.second_on < m.slow2_last + (j ? 1 : 2) * PC_VSYNC_CALM_FRAMES + 10);
            if (keep[i] == 240.0)
                printf("two 2 s stretches of 25 ms presents at 240 Hz, the second %d s after vsync came back on: off at %d and %d, back on at %d and %d, on at the end\n", after / 60, r.first_off,
                       r.second_off, r.first_on, r.second_on);
        }
    }

    /* A monitor slower than the game would slow the game: vsync goes. It comes back on to try after 10 seconds. A monitor clearly slower, 55 Hz or less, fails that try on its first second,
       and the tries end for the run; one only a little slower may show it later, and then each time it goes again within a minute the next wait is twice as long. The game keeps its pace all
       through. How soon, and so what it costs, is printed. */
    for (i = 0; i < sizeof(slow) / sizeof(slow[0]); i++) {
        for (j = 0; j < sizeof(work) / sizeof(work[0]); j++) {
            m = plain(slow[i], work[j]);
            m.watch_first = 60;
            m.watch_last = 36000;
            model_run(&m, &r, -1);
            if (j == 2u)
                printf("a %.1f Hz monitor, 3 ms of work: vsync off at frame %d, %.1f s into the run, back on at frame %d, off %d times in 10 minutes, %s; the game at most %.1f ms behind\n",
                       slow[i], r.first_off, r.first_off_ms / 1000.0, r.first_on, r.offs, r.no_returns ? "no more tries after the first failed at once" : "still trying", r.most_behind);
            CHECK(r.first_off >= 60);
            CHECK(r.first_on == r.first_off + PC_VSYNC_CALM_FRAMES && !r.on_at_end);
            if (slow[i] <= 55.0)
                CHECK(r.offs == 2 && r.ons == 1 && r.last_at_once && r.no_returns && r.second_off == r.first_on + 4 * PC_VSYNC_QUARTER_FRAMES);
            else
                CHECK((r.offs == 2 && r.ons == 1 && r.last_at_once && r.no_returns) || (r.offs >= 3 && r.second_on - r.second_off == 2 * PC_VSYNC_CALM_FRAMES));
            CHECK(r.most_behind < 1000.0 / slow[i] + GAME_FRAME_MS);
            if (slow[i] <= 55.0)
                CHECK(r.first_off < 200);
            else if (slow[i] <= 59.5)
                CHECK(r.first_off < 800);
            else
                CHECK(r.first_off < 3000);
        }
        /* With a hitch now and then, it still gets there. A hitch also starts the calm count again, so once the wait is longer than the time between hitches vsync stays off. */
        m = plain(slow[i], 3.0);
        m.hitch_every = 977;
        m.hitch_ms = 500.0;
        model_run(&m, &r, -1);
        CHECK(r.offs >= 1 && r.first_off < 3000 && !r.on_at_end);
    }
}

/* The controls' lines in the file (pc_config_merge_controls): the controls screen rewrites them and keeps every other line as the player wrote it. */
static void test_controls_lines(void)
{
    static const char block[] = "# ---- Controls. A comment the block writes.\nkey_p1_a=Space\npad_p1_a=x\ncontroller1=any\n";
    static const char old_lf[] = "# my notes\nrom=D:/my copy.md\nkey_p1_a=Z\n# ---- Controls. A comment the block writes.\njoy_p2_up=hat0up\npad1=three\n"
                                 "controller2=none\nstick1=left\ndeadzone2=40\nscale=4\n\n\n";
    static const char want_lf[] = "# my notes\nrom=D:/my copy.md\nscale=4\n\n# ---- Controls. A comment the block writes.\nkey_p1_a=Space\npad_p1_a=x\ncontroller1=any\n";
    static char out[4096], again[4096];
    char crlf[1024], want_crlf[1024];
    const char *from;
    char *to;
    int n;
    PcConfig c;
    char error[200];
    /* No file yet: the block alone. */
    n = pc_config_merge_controls(NULL, block, out, sizeof(out));
    CHECK(n == (int)strlen(block) && !strcmp(out, block));
    /* Every control line goes, wherever it was, and the block's own comment with it; the rest stays in its order; blank lines at the end become one. */
    n = pc_config_merge_controls(old_lf, block, out, sizeof(out));
    CHECK(n == (int)strlen(want_lf) && !strcmp(out, want_lf));
    /* Twice gives the same as once. */
    CHECK(pc_config_merge_controls(out, block, again, sizeof(again)) == n && !strcmp(out, again));
    /* A file with CR LF line ends keeps them, the block's lines too. */
    for (from = old_lf, to = crlf; *from; from++) {
        if (*from == '\n')
            *to++ = '\r';
        *to++ = *from;
    }
    *to = 0;
    for (from = want_lf, to = want_crlf; *from; from++) {
        if (*from == '\n')
            *to++ = '\r';
        *to++ = *from;
    }
    *to = 0;
    n = pc_config_merge_controls(crlf, block, out, sizeof(out));
    CHECK(n == (int)strlen(want_crlf) && !strcmp(out, want_crlf));
    /* Too small a buffer is refused, never cut. */
    CHECK(pc_config_merge_controls(old_lf, block, out, 40) == -1);
    /* A player's own comment stays even when it starts like the block's; a line that is not a setting stays too. */
    n = pc_config_merge_controls("# ---- Controls, my own words\nnot a setting\n", block, out, sizeof(out));
    CHECK(n > 0 && !strncmp(out, "# ---- Controls, my own words\nnot a setting\n\n# ---- Controls. A", 63));
    /* The names the controls own. */
    CHECK(pc_config_is_control("key_p1_up") && pc_config_is_control("pad_pause") && pc_config_is_control("joy_p2_mode") && pc_config_is_control("pad2"));
    CHECK(pc_config_is_control("controller1") && pc_config_is_control("stick2") && pc_config_is_control("deadzone1"));
    CHECK(!pc_config_is_control("rom") && !pc_config_is_control("scale") && !pc_config_is_control("pad4") && !pc_config_is_control("padding"));
    CHECK(pc_config_is_control("pad3") && pc_config_is_control("controller3") && pc_config_is_control("stick3") && pc_config_is_control("deadzone3"));
    CHECK(!pc_config_is_control("three_players") && !pc_config_is_control("arcade_mode"));
    /* The new lines parse: an empty key is none; a pad's id and name; the stick and the dead zone; each refused out of its range. */
    pc_config_defaults(&c);
    CHECK(pc_config_parse("key_p1_a=\npad_p1_b=a\njoy_p2_c=button3\ncontroller1=0300000000000000000000000000000000 My pad\nstick2=both\ndeadzone1=35\n", &c, error, sizeof(error)) == 0);
    CHECK(c.n_keys == 1 && !strcmp(c.keys[0].key, "") && c.n_pads == 1 && !strcmp(c.pads[0].action, "p1_b") && c.n_joys == 1 && !strcmp(c.joys[0].key, "button3"));
    CHECK(!strcmp(c.controller[0], "0300000000000000000000000000000000 My pad") && c.controller[1][0] == 0);
    CHECK(c.stick[0] == 0 && c.stick[1] == 4 && c.deadzone[0] == 35 && c.deadzone[1] == 0);
    CHECK(pc_config_parse("stick1=sideways\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("deadzone1=96\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("controller1=\n", &c, error, sizeof(error)) == 1);
}

/* The menu bar's lines (pc_config.h): the Scum! cheats as a list, the arcade stage, the console's settings, the bar itself; and the menu's writer, which
   sets each setting in its own line and keeps every other line as the player wrote it. */
static void test_menu_lines(void)
{
    static const char old_lf[] = "# my notes\nrom=D:/my copy.md\ncheats=pills\nscale=4\n# a note about the stage\narcade_stage=3\ncheats=lives\n\n\n";
    static const char *const names[] = {"cheats", "region", "arcade_stage"}, *const values[] = {"health,lives", "jp", "25"};
    static const char want_lf[] = "# my notes\nrom=D:/my copy.md\ncheats=health,lives\nscale=4\n# a note about the stage\narcade_stage=25\nregion=jp\n";
    static char out[2048], crlf[1024], want_crlf[1024], text[64];
    const char *from;
    char *to;
    PcConfig c;
    char error[200];
    int n, k;
    pc_config_defaults(&c);
    CHECK(c.menu_bar == 1 && c.cheats == 0 && c.arcade_stage == 0 && c.console.region == PC_REGION_AUTO && c.console.console == PC_CONSOLE_MEGADRIVE && !c.console.x32 &&
          !c.console.megacd && !c.console.cpumate);
    CHECK(pc_config_parse("cheats=health,lives\narcade_stage=25\nregion=us\nconsole=genesis\nx32=on\nmegacd=on\ncpumate=on\nmenu_bar=off\n", &c, error, sizeof(error)) == 0);
    CHECK(c.cheats == (PC_CHEAT_HEALTH | PC_CHEAT_LIVES) && c.arcade_stage == 25 && c.console.region == PC_REGION_US && c.console.console == PC_CONSOLE_GENESIS);
    CHECK(c.console.x32 && c.console.megacd && c.console.cpumate && !c.menu_bar);
    CHECK(pc_config_parse("cheats = pills , health \narcade_stage=off\nregion=auto\nconsole=megadrive\n", &c, error, sizeof(error)) == 0);
    CHECK(c.cheats == (PC_CHEAT_HEALTH | PC_CHEAT_PILLS) && c.arcade_stage == 0 && c.console.region == PC_REGION_AUTO && c.console.console == PC_CONSOLE_MEGADRIVE);
    CHECK(pc_config_parse("cheats=off\n", &c, error, sizeof(error)) == 0 && c.cheats == 0);
    CHECK(pc_config_parse("arcade_stage=1\n", &c, error, sizeof(error)) == 0 && c.arcade_stage == 1);
    /* Refused, each by its line: a name that is not a cheat, one twice, an empty list, a stage out of 1 to 25 or not a plain number, a region or a
       console the menu does not have, an on/off that is neither. */
    CHECK(pc_config_parse("cheats=health,money\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("cheats=lives,lives\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("cheats=\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("arcade_stage=26\n", &c, error, sizeof(error)) == 1 && pc_config_parse("arcade_stage=0\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("arcade_stage=03\n", &c, error, sizeof(error)) == 1 && pc_config_parse("arcade_stage=5x\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("region=usa\n", &c, error, sizeof(error)) == 1 && pc_config_parse("console=nomad2\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("console=nomad\nmegacd=cdx\n", &c, error, sizeof(error)) == 0 && c.console.console == PC_CONSOLE_NOMAD && c.console.megacd == PC_MEGACD_CDX);
    CHECK(pc_config_parse("megacd=sega\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("region=eu\n", &c, error, sizeof(error)) == 1 && !strcmp(error, "line 1 of the configuration: region=eu"));
    CHECK(pc_config_parse("megacd=yes\n", &c, error, sizeof(error)) == 1 && pc_config_parse("menu_bar=1\n", &c, error, sizeof(error)) == 1);
    /* The cheats as the file writes them, and back. */
    for (k = 0; k < 8; k++) {
        pc_config_cheats_text((unsigned)k, text, sizeof(text));
        snprintf(out, sizeof(out), "cheats=%s\n", text);
        pc_config_defaults(&c);
        CHECK(pc_config_parse(out, &c, error, sizeof(error)) == 0 && c.cheats == (unsigned)k);
    }
    pc_config_cheats_text(7, text, sizeof(text));
    CHECK(!strcmp(text, "health,pills,lives"));
    pc_config_cheats_text(0, text, sizeof(text));
    CHECK(!strcmp(text, "off"));
    /* The writer: the first line of each name gets the value in its place, a second one goes, a missing one is added at the end; comments, order and
       the player's other lines stay; blank lines at the end go. */
    n = pc_config_merge_settings(old_lf, names, values, 3, out, sizeof(out));
    CHECK(n == (int)strlen(want_lf) && !strcmp(out, want_lf));
    /* Twice gives the same; no file yet gives the lines alone. */
    {
        static char again[2048];
        CHECK(pc_config_merge_settings(out, names, values, 3, again, sizeof(again)) == n && !strcmp(again, out));
    }
    CHECK(pc_config_merge_settings(NULL, names, values, 1, out, sizeof(out)) == (int)strlen("cheats=health,lives\n") && !strcmp(out, "cheats=health,lives\n"));
    /* CR LF stays CR LF. */
    for (from = old_lf, to = crlf; *from; from++) {
        if (*from == '\n')
            *to++ = '\r';
        *to++ = *from;
    }
    *to = 0;
    for (from = want_lf, to = want_crlf; *from; from++) {
        if (*from == '\n')
            *to++ = '\r';
        *to++ = *from;
    }
    *to = 0;
    n = pc_config_merge_settings(crlf, names, values, 3, out, sizeof(out));
    CHECK(n == (int)strlen(want_crlf) && !strcmp(out, want_crlf));
    /* Too small is refused, never cut; a comment that names a setting is no setting; the controls' lines stay for the controls screen. */
    CHECK(pc_config_merge_settings(old_lf, names, values, 3, out, 30) == -1);
    n = pc_config_merge_settings("# cheats=lives in a comment\nkey_p1_a=Z\n", names, values, 1, out, sizeof(out));
    CHECK(n > 0 && !strcmp(out, "# cheats=lives in a comment\nkey_p1_a=Z\ncheats=health,lives\n"));
    /* What the writer writes, the parser reads. */
    pc_config_defaults(&c);
    n = pc_config_merge_settings(old_lf, names, values, 3, out, sizeof(out));
    CHECK(pc_config_parse(out, &c, error, sizeof(error)) == 0 && c.cheats == (PC_CHEAT_HEALTH | PC_CHEAT_LIVES) && c.console.region == PC_REGION_JP && c.arcade_stage == 25);
}

/* The one writer of the settings file (pc_config_rewrite), on a real file: through a temporary name; a file that cannot be read is left alone. */
static int change_to_hello(const char *old_text, char *out, size_t out_size, void *arg)
{
    (void)arg;
    snprintf(out, out_size, "%shello=1\n", old_text ? old_text : "");
    return (int)strlen(out);
}

static int change_refused(const char *old_text, char *out, size_t out_size, void *arg)
{
    (void)old_text;
    (void)out;
    (void)out_size;
    (void)arg;
    return -1;
}

/* The Pocket core's arcade entries and player 3 (pc_config.h, pc_scum.h): each line, its words, its refusals, and what an earlier version's file meant. */
static void test_arcade_lines(void)
{
    PcConfig c;
    char error[200];
    pc_config_defaults(&c);
    CHECK(c.arcade_mode == -1 && c.arcade_coins == 0 && !c.attract_only && !c.three_players && pc_config_arcade_mode(&c) == 0);
    /* An earlier version's file: an arcade_stage line and no arcade_mode line meant the arcade mode at that stage, so it means Arcade Mode On. */
    CHECK(pc_config_parse("arcade_stage=6\n", &c, error, sizeof(error)) == 0 && pc_config_arcade_mode(&c) == 1 && c.arcade_stage == 6);
    CHECK(pc_config_parse("arcade_stage=off\n", &c, error, sizeof(error)) == 0 && pc_config_arcade_mode(&c) == 0);
    /* With an arcade_mode line, the stage only picks the stage, before it or after it. */
    pc_config_defaults(&c);
    CHECK(pc_config_parse("arcade_mode=off\narcade_stage=6\n", &c, error, sizeof(error)) == 0 && pc_config_arcade_mode(&c) == 0 && c.arcade_stage == 6);
    pc_config_defaults(&c);
    CHECK(pc_config_parse("arcade_stage=25\narcade_mode=multiplayer\n", &c, error, sizeof(error)) == 0 && pc_config_arcade_mode(&c) == 2 && c.arcade_stage == 25);
    CHECK(pc_config_parse("arcade_mode=on\n", &c, error, sizeof(error)) == 0 && pc_config_arcade_mode(&c) == 1);
    CHECK(pc_config_parse("arcade_mode=stick_only\narcade_stage=off\n", &c, error, sizeof(error)) == 0 && pc_config_arcade_mode(&c) == 3);
    CHECK(pc_config_parse("arcade_coins=2coins\nattract_only=on\nthree_players=on\n", &c, error, sizeof(error)) == 0 && c.arcade_coins == 1 && c.attract_only &&
          c.three_players);
    CHECK(pc_config_parse("arcade_coins=infinite\n", &c, error, sizeof(error)) == 0 && c.arcade_coins == 2);
    CHECK(pc_config_parse("arcade_coins=1coin\nattract_only=off\nthree_players=off\n", &c, error, sizeof(error)) == 0 && c.arcade_coins == 0 && !c.attract_only &&
          !c.three_players);
    CHECK(pc_config_parse("arcade_mode=stick\n", &c, error, sizeof(error)) == 1 && pc_config_parse("arcade_mode=On\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("arcade_coins=3coins\n", &c, error, sizeof(error)) == 1 && pc_config_parse("attract_only=1\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("three_players=yes\n", &c, error, sizeof(error)) == 1);
    /* Player 3's own lines, as players 1 and 2 have them; no fourth. */
    pc_config_defaults(&c);
    CHECK(pc_config_parse("pad3=three\ncontroller3=none\nstick3=right\ndeadzone3=40\nkey_p3_a=Space\n", &c, error, sizeof(error)) == 0);
    CHECK(c.three_button[2] == 1 && !strcmp(c.controller[2], "none") && c.stick[2] == 3 && c.deadzone[2] == 40 && c.n_keys == 1 &&
          !strcmp(c.keys[0].action, "p3_a"));
    CHECK(pc_config_parse("pad4=six\n", &c, error, sizeof(error)) == 1 && pc_config_parse("controller4=any\n", &c, error, sizeof(error)) == 1);
}

static void test_rewrite(void)
{
    const char *name = "pc_config_test_rewrite.cfg";
    char back[256];
    FILE *f;
    size_t n;
    remove(name);
    CHECK(pc_config_rewrite(name, change_to_hello, NULL) == 0);
    f = fopen(name, "rb");
    n = f ? fread(back, 1, sizeof(back) - 1, f) : 0;
    if (f)
        fclose(f);
    back[n] = 0;
    CHECK(!strcmp(back, "hello=1\n"));
    CHECK(pc_config_rewrite(name, change_to_hello, NULL) == 0);
    f = fopen(name, "rb");
    n = f ? fread(back, 1, sizeof(back) - 1, f) : 0;
    if (f)
        fclose(f);
    back[n] = 0;
    CHECK(!strcmp(back, "hello=1\nhello=1\n"));
    CHECK(pc_config_rewrite(name, change_refused, NULL) == PC_CONFIG_TOO_LARGE);
    f = fopen(name, "rb");
    n = f ? fread(back, 1, sizeof(back) - 1, f) : 0;
    if (f)
        fclose(f);
    back[n] = 0;
    CHECK(!strcmp(back, "hello=1\nhello=1\n"));
    f = fopen("pc_config_test_rewrite.cfg.tmp", "rb");
    CHECK(f == NULL);
    if (f)
        fclose(f);
    remove(name);
    /* Only a file that is not there is no file: a folder of that name, which cannot be opened as a file, is refused as unreadable, and nothing is
       written beside it. */
    CreateDirectoryA("pc_config_test_rewrite_folder.cfg", NULL);
    CHECK(pc_config_rewrite("pc_config_test_rewrite_folder.cfg", change_to_hello, NULL) == PC_CONFIG_NOT_READ);
    f = fopen("pc_config_test_rewrite_folder.cfg.tmp", "rb");
    CHECK(f == NULL);
    if (f)
        fclose(f);
    RemoveDirectoryA("pc_config_test_rewrite_folder.cfg");
    /* A file of 32 KB or more is more than the writer reads: it is told as too large, not as unreadable, and left as it is. */
    f = fopen(name, "wb");
    if (f) {
        for (n = 0; n < PC_CONFIG_REWRITE_MAX; n += 8)
            fwrite("# a lot\n", 1, 8, f);
        fclose(f);
    }
    CHECK(pc_config_rewrite(name, change_to_hello, NULL) == PC_CONFIG_FILE_TOO_LARGE);
    n = 0;
    f = fopen(name, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        fclose(f);
    }
    CHECK(n == PC_CONFIG_REWRITE_MAX);
    f = fopen("pc_config_test_rewrite.cfg.tmp", "rb");
    CHECK(f == NULL);
    if (f)
        fclose(f);
    remove(name);
    /* A file saved as UTF-16 is never merged into: read as text it would end at its first zero byte, and the rest would be lost. */
    f = fopen(name, "wb");
    if (f) {
        fwrite("\xFF\xFEs\0c\0a\0l\0e\0=\0" "2\0\r\0\n\0", 1, 20, f);
        fclose(f);
    }
    CHECK(pc_config_rewrite(name, change_to_hello, NULL) == PC_CONFIG_NOT_READ);
    f = fopen(name, "rb");
    n = f ? fread(back, 1, sizeof(back) - 1, f) : 0;
    if (f)
        fclose(f);
    CHECK(n == 20 && !memcmp(back, "\xFF\xFEs\0c\0a\0l\0e\0", 12));
    remove(name);
}

/* A file saved "UTF-8 with BOM" (the three bytes of the byte order mark at its start): both merges keep the mark where it was and read the first line
   without it, as the parser does, so a first line that sets what they change is changed in its place, not added a second time. A file saved as UTF-16,
   which holds zero bytes, is refused with words that say how to save it again. A settings file that cannot even be reached is told apart from one that
   is there. */
static void test_text_forms(void)
{
    static const char block[] = "# ---- Controls.\nkey_p1_start=K\n";
    static const char *const names[] = {"volume"}, *const values[] = {"75"};
    static char out[2048];
    char error[600];
    PcConfig c;
    FILE *f;
    int n;
    n = pc_config_merge_controls("\xEF\xBB\xBFkey_p1_start=Space\r\nscale=2\r\n", block, out, sizeof(out));
    CHECK(n > 0 && !strcmp(out, "\xEF\xBB\xBFscale=2\r\n\r\n# ---- Controls.\r\nkey_p1_start=K\r\n"));
    n = pc_config_merge_controls("\xEF\xBB\xBFkey_p1_start=Space\n", block, out, sizeof(out));
    CHECK(n > 0 && !strcmp(out, "\xEF\xBB\xBF# ---- Controls.\nkey_p1_start=K\n"));
    n = pc_config_merge_settings("\xEF\xBB\xBFvolume=50\r\ncheats=pills\r\n", names, values, 1, out, sizeof(out));
    CHECK(n > 0 && !strcmp(out, "\xEF\xBB\xBFvolume=75\r\ncheats=pills\r\n"));
    pc_config_defaults(&c);
    CHECK(pc_config_parse(out, &c, error, sizeof(error)) == 0 && c.volume == 75 && c.cheats == PC_CHEAT_PILLS);
    n = pc_config_merge_settings("\xEF\xBB\xBF", names, values, 1, out, sizeof(out));
    CHECK(n > 0 && !strcmp(out, "\xEF\xBB\xBFvolume=75\n"));
    /* UTF-16, with its mark and without it */
    f = fopen("pc_config_test_utf16.cfg", "wb");
    if (f) {
        fwrite("\xFF\xFEs\0c\0a\0l\0e\0=\0" "2\0\r\0\n\0", 1, 20, f);
        fclose(f);
    }
    error[0] = 0;
    CHECK(pc_config_load("pc_config_test_utf16.cfg", &c, error, sizeof(error)) == 1 && strstr(error, "UTF-16") && strstr(error, "Save it again as UTF-8"));
    printf("a settings file saved as UTF-16: %s\n", error);
    f = fopen("pc_config_test_utf16.cfg", "wb");
    if (f) {
        fwrite("s\0c\0a\0l\0e\0=\0" "2\0", 1, 14, f);
        fclose(f);
    }
    CHECK(pc_config_load("pc_config_test_utf16.cfg", &c, error, sizeof(error)) == 1 && strstr(error, "UTF-16"));
    remove("pc_config_test_utf16.cfg");
    /* a name Windows cannot even look up: the file may not be there, so it is not said to be */
    error[0] = 0;
    CHECK(pc_config_load("pc_config_test_bad<name>.cfg", &c, error, sizeof(error)) == -2 && strstr(error, "could not be reached (") && !strstr(error, "is there"));
    printf("a settings file that cannot be reached: %s\n", error);
}

int main(void)
{
    PcConfig c;
    char error[200], out[65];
    static char million[1000000];   /* not a game address: the standard asks for a million letters */
    static uint8_t fw[PC_SAVE_BYTES], back[PC_SAVE_BYTES];
    FILE *f;
    uint8_t raw[4096];
    unsigned i;

    test_menu_lines();
    test_arcade_lines();
    test_rewrite();
    test_text_forms();
    hash_is("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    hash_is("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    hash_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    memset(million, 'a', sizeof(million));
    pc_sha256_hex(million, sizeof(million), out);
    CHECK(!strcmp(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

    pc_config_defaults(&c);
    CHECK(c.scale == 3 && c.vsync == 1 && c.shape == PC_SHAPE_4_3 && !c.no_limit && !c.fullscreen && !c.rom[0]);
    CHECK(!strcmp(c.renderer, "opengl"));
    /* An old file with no such line keeps the default. Each name is taken, and any other is refused by its line. */
    CHECK(pc_config_parse("scale=3\n", &c, error, sizeof(error)) == 0 && !strcmp(c.renderer, "opengl"));
    CHECK(pc_config_parse("renderer=direct3d\n", &c, error, sizeof(error)) == 0 && !strcmp(c.renderer, "direct3d"));
    CHECK(pc_config_parse("renderer=direct3d11\n", &c, error, sizeof(error)) == 0 && !strcmp(c.renderer, "direct3d11"));
    CHECK(pc_config_parse("renderer=direct3d12\n", &c, error, sizeof(error)) == 0 && !strcmp(c.renderer, "direct3d12"));
    CHECK(pc_config_parse("renderer=auto\n", &c, error, sizeof(error)) == 0 && !strcmp(c.renderer, "auto"));
    CHECK(pc_config_parse("renderer=opengl\n", &c, error, sizeof(error)) == 0 && !strcmp(c.renderer, "opengl"));
    CHECK(pc_config_parse("scale=2\nrenderer=vulkan\n", &c, error, sizeof(error)) == 2 && strstr(error, "vulkan"));
    CHECK(!strcmp(c.renderer, "opengl"));
    pc_config_defaults(&c);
    CHECK(pc_config_parse("# a comment\n\nrom = D:/games/my copy.md \r\nscale=4\ndisplay=square\nsprite_limits=none\nvsync=off\nfullscreen=on\npad2=three\nkey_p1_a=Space\nsave=my.sav\n", &c, error, sizeof(error)) == 0);
    CHECK(!strcmp(c.rom, "D:/games/my copy.md"));
    CHECK(!strcmp(c.save, "my.sav"));
    CHECK(c.scale == 4 && c.shape == PC_SHAPE_SQUARE && c.no_limit == 1 && c.vsync == 0 && c.fullscreen == 1);
    CHECK(c.three_button[0] == 0 && c.three_button[1] == 1);
    CHECK(c.n_keys == 1 && !strcmp(c.keys[0].action, "p1_a") && !strcmp(c.keys[0].key, "Space"));
    CHECK(pc_config_parse("display=4:3\nsprite_limits=cart\n", &c, error, sizeof(error)) == 0 && c.shape == PC_SHAPE_4_3 && c.no_limit == 0);
    /* The Pocket core's Aspect Ratio: original and corrected, after the program's own two. */
    CHECK(pc_config_parse("display=original\n", &c, error, sizeof(error)) == 0 && c.shape == PC_SHAPE_ORIGINAL);
    CHECK(pc_config_parse("display=corrected\n", &c, error, sizeof(error)) == 0 && c.shape == PC_SHAPE_CORRECTED);
    CHECK(pc_config_parse("display=Original\n", &c, error, sizeof(error)) == 1 && pc_config_parse("display=10:7\n", &c, error, sizeof(error)) == 1);
    CHECK(c.sound == 1 && c.volume == 100);
    CHECK(pc_config_parse("sound=off\nvolume=35\nkey_mute=F4\n", &c, error, sizeof(error)) == 0 && c.sound == 0 && c.volume == 35);
    CHECK(pc_config_parse("volume=0\n", &c, error, sizeof(error)) == 0 && c.volume == 0);
    CHECK(pc_config_parse("volume=101\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("volume=loud\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("sound=maybe\n", &c, error, sizeof(error)) == 1);

    /* Every kind of bad line is refused, by its number, and named. */
    pc_config_defaults(&c);
    CHECK(pc_config_parse("scale=2\nthis is not a setting\n", &c, error, sizeof(error)) == 2 && strstr(error, "this is not a setting"));
    CHECK(pc_config_parse("nonsense=1\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("scale=0\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("display=wide\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("sprite_limits=some\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("vsync=maybe\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("pad1=four\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_load("no_such_file_here.cfg", &c, error, sizeof(error)) == -1);
    /* A settings file that is there but cannot be read is told apart from no file, with Windows' words: a folder of that name, and a file another
       program holds open with no sharing. Once let go, the same file reads. */
    CreateDirectoryA("pc_config_test_folder.cfg", NULL);
    error[0] = 0;
    CHECK(pc_config_load("pc_config_test_folder.cfg", &c, error, sizeof(error)) == -2 && strstr(error, "pc_config_test_folder.cfg is there, but it could not be read ("));
    printf("a folder for a settings file: %s\n", error);
    RemoveDirectoryA("pc_config_test_folder.cfg");
    f = fopen("pc_config_test_held.cfg", "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("scale=2\n", f);
        fclose(f);
    }
    {
        HANDLE held = CreateFileA("pc_config_test_held.cfg", GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(held != INVALID_HANDLE_VALUE);
        error[0] = 0;
        CHECK(pc_config_load("pc_config_test_held.cfg", &c, error, sizeof(error)) == -2 && strstr(error, "could not be read (") && strstr(error, "so the game does not start"));
        printf("a settings file held by another program: %s\n", error);
        if (held != INVALID_HANDLE_VALUE)
            CloseHandle(held);
    }
    pc_config_defaults(&c);
    CHECK(pc_config_load("pc_config_test_held.cfg", &c, error, sizeof(error)) == 0 && c.scale == 2);
    remove("pc_config_test_held.cfg");
    /* A file saved as "UTF-8 with BOM": the three bytes of the byte order mark are not part of its first line, a comment or a setting. */
    pc_config_defaults(&c);
    CHECK(pc_config_parse("\xEF\xBB\xBF# The game's settings\nscale=2\n", &c, error, sizeof(error)) == 0 && c.scale == 2);
    CHECK(pc_config_parse("\xEF\xBB\xBF" "scale=4\n", &c, error, sizeof(error)) == 0 && c.scale == 4);
    CHECK(pc_config_parse("scale=2\n\xEF\xBB\xBF" "scale=4\n", &c, error, sizeof(error)) == 2);      /* only at the start */

    /* The save: the firmware's byte i is the file's byte i ^ 1; a kill between the temporary file and the rename leaves the old one; a file in the older layout reads as far as it goes. */
    for (i = 0; i < PC_SAVE_BYTES; i++)
        fw[i] = (uint8_t)(i * 7u + 3u);
    CHECK(pc_save_store("pc_config_test.sav", fw) == 0);
    f = fopen("pc_config_test.sav", "rb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fread(raw, 1, 4, f) == 4 && raw[0] == fw[1] && raw[1] == fw[0] && raw[2] == fw[3] && raw[3] == fw[2]);
        fclose(f);
    }
    CHECK(pc_save_load("pc_config_test.sav", back) == PC_SAVE_BYTES);
    CHECK(!memcmp(fw, back, PC_SAVE_BYTES));
    /* A second save replaces the first, and leaves no temporary file. */
    fw[100] ^= 0xFFu;
    CHECK(pc_save_store("pc_config_test.sav", fw) == 0);
    CHECK(pc_save_load("pc_config_test.sav", back) == PC_SAVE_BYTES);
    CHECK(!memcmp(fw, back, PC_SAVE_BYTES));
    CHECK(pc_save_load("pc_config_test.sav.tmp", back) == PC_SAVE_NO_FILE);
    /* A kill while the temporary file is being written leaves that file half done, and the save itself whole. */
    f = fopen("pc_config_test.sav.tmp", "wb");
    if (f) {
        fputs("half a file", f);
        fclose(f);
    }
    CHECK(pc_save_load("pc_config_test.sav", back) == PC_SAVE_BYTES);
    CHECK(!memcmp(fw, back, PC_SAVE_BYTES));
    /* And the next save goes straight through that leftover. */
    CHECK(pc_save_store("pc_config_test.sav", fw) == 0);
    CHECK(pc_save_load("pc_config_test.sav", back) == PC_SAVE_BYTES);
    CHECK(!memcmp(fw, back, PC_SAVE_BYTES));
    f = fopen("pc_config_test_short.sav", "wb");
    if (f) {
        memset(raw, 0x5A, sizeof(raw));
        fwrite(raw, 1, sizeof(raw), f);
        fclose(f);
    }
    CHECK(pc_save_load("pc_config_test_short.sav", back) == (long)sizeof(raw));
    CHECK(back[0] == 0x5A && back[4095] == 0x5A && back[4096] == 0 && back[PC_SAVE_BYTES - 1] == 0);
    remove("pc_config_test.sav");
    remove("pc_config_test_short.sav");
    test_save_refused();
    test_save_check();
    test_save_copy();
    test_vsync_rule();
    test_controls_lines();

    printf("%d checks, %d failed\n", checks, failed);
    printf("RESULT: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
