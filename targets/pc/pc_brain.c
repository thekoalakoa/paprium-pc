/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_brain.h. What the cart MCU's firmware does around a brain call (its buddy/s1_glue.c and zz_buddy_trap.c), done here for this program:
 * the counter's input word cleared before each call, the restart flag raised before the first call after a release, H2 then H1 with the RV calling
 * convention, the two times read from the counter's output, and the busy time never before the word's.
 */
#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pt_rt.h"
#include "pc_brain.h"
#include "pc_config.h"
#include "pc_rv32.h"

/* ---- the brain's places, as the firmware's entry table (buddy/zz_buddy_brain.c) gives them */
#define CODE_LO         0x00005360u     /* its code and read-only data in the MCU's instruction memory */
#define CODE_HI         0x0000D2ECu
#define ENTRY_H2        0x0000859Cu     /* the AI's turn and its cycle counter */
#define ENTRY_H1        0x00008504u     /* the pad word, in a0 */
#define WRAM            0x80000000u     /* the MCU's work RAM, 48 KB */
#define WRAM_BYTES      0xC000u
#define HELPERS         0x80002C00u     /* the counting helpers, then 64 zero bytes */
#define HELPER_BYTES    5328u
#define TABLES          0x80004110u     /* the image tables */
#define TABLE_BYTES     1336u
#define CNT_OUT         0x80002230u     /* +0 the counted cycles, +4 the word's time, +8 the busy time, +12 an input word, 0 here */
#define FLAG            0x800022BEu     /* the restart flag, a halfword the brain clears itself */
#define T16_KEY         0x80002220u     /* the counter's table key, cleared at every release */
#define SRAM            0x08000000u
#define FLASH           0x04000000u
#define STACK_TOP       0x8000BF00u
#define RETURN_MARK     0xFFFFFFF0u     /* a return address no code has: the call is done when the program counter gets there */
#define CALL_STEPS      50000000u       /* far more than any call takes (pc_brain_report gives the most a call took) */

/* The sub-CPU's start-up pattern in the word memory, written into the mirror at each release, and the tables of the sub-CPU's program that the
   brain keeps in its own work RAM, copied from the program memory's mirror at each release. Both as the firmware's entry table lists them. */
static const struct { uint32_t addr, bytes, fill; } restore_list[4] = {
    { SRAM + 0x20000u, 0x1000u, 0x0000u },      /* the objects, the word memory's first 4 KB; MCU addresses, not a game address */
    { SRAM + 0x24000u, 0x0140u, 0x0000u },      /* the records, 320 bytes at its offset 0x4000; not a game address */
    { SRAM + 0x28000u, 0x1080u, 0x00FFu },      /* the terrain, 4,224 bytes at its offset 0x8000; not a game address */
    { SRAM + 0x30000u, 0x000Au, 0x0000u },      /* the header, 10 bytes at its offset 0x10000; not a game address */
};
static const struct { uint32_t src, dst, bytes; } copy_list[3] = {
    { SRAM + 0x105F4u, 0x80001820u, 0x130u },   /* the program's bytes 0x5F4-0x723; MCU addresses, not a game address */
    { SRAM + 0x109C0u, 0x80001950u, 0x200u },   /* its bytes 0x9C0-0xBBF; not a game address */
    { SRAM + 0x11C92u, 0x80001B50u, 0x0A0u },   /* its bytes 0x1C92-0x1D31; not a game address */
};

/* The SHA-256 of each part as Little-Man Firmware 1.0 carries it. The entry table above belongs to exactly these bytes. */
static const char *const PIN_CODE = "74b349168a403a299ca469f936bcf9f830e3fb357c1f84ac0ef1397c306eb2d0";
static const char *const PIN_HELPERS = "c2bd564659b1e425f391de2e1fc286abfd944e2f5070b7d9786a3bc08d6926f2";
static const char *const PIN_TABLES = "5ce54fe2b6307ffc8cc374c665ee3dae217a65e75c6ebc031c2595e3a47b0ad5";

static uint8_t code_mem[CODE_HI - CODE_LO];
static uint8_t wram[WRAM_BYTES];
static uint8_t sram[PC_BRAIN_SRAM_BYTES];
static PcRv32 cpu;
static int loaded, failed, flag_pending;
static uint64_t n_calls, n_releases, steps_max, steps_all;

uint8_t *pc_brain_sram(void) { return sram; }
int pc_brain_ready(void) { return loaded && !failed; }

static uint8_t *at_wram(uint32_t a) { return wram + (a - WRAM); }
static uint8_t *at_sram(uint32_t a) { return sram + (a - SRAM); }

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

void pc_brain_load_parts(const uint8_t *code, size_t code_bytes, const uint8_t *helpers, size_t helper_bytes, const uint8_t *tables, size_t table_bytes)
{
    memset(code_mem, 0, sizeof(code_mem));
    memset(wram, 0, sizeof(wram));
    memcpy(code_mem, code, code_bytes < sizeof(code_mem) ? code_bytes : sizeof(code_mem));
    memcpy(at_wram(HELPERS), helpers, helper_bytes < HELPER_BYTES ? helper_bytes : HELPER_BYTES);
    memcpy(at_wram(TABLES), tables, table_bytes < TABLE_BYTES ? table_bytes : TABLE_BYTES);
    memset(&cpu, 0, sizeof(cpu));
    /* The code may be fetched from the instruction memory's part and from the helpers' window; the window and the tables are never written, as
       the Pocket locks them. The flash window is mapped at the first call, once the game file is in place. */
    pc_rv32_map(&cpu, CODE_LO, CODE_HI - CODE_LO, code_mem, 0, 1);
    pc_rv32_map(&cpu, WRAM, HELPERS - WRAM, wram, 1, 0);
    pc_rv32_map(&cpu, HELPERS, TABLES + TABLE_BYTES - HELPERS, at_wram(HELPERS), 0, 1);
    pc_rv32_map(&cpu, TABLES + TABLE_BYTES, WRAM + WRAM_BYTES - (TABLES + TABLE_BYTES), at_wram(TABLES + TABLE_BYTES), 1, 0);
    pc_rv32_map(&cpu, SRAM, PC_BRAIN_SRAM_BYTES, sram, 1, 0);
    loaded = 1;
    failed = 0;
    flag_pending = 1;
}

/* Reads a whole file of at most most bytes into *data (to be freed). The length is taken from the open file in 64 bits, so a file of 2 GB or more
   is measured right and never taken for a short one; a file longer than most is not read at all. Returns 0 with *data and *size; 1 when the file
   is longer than most, with *size its length and *data NULL; -1 when there is nothing of that name to open; -3 when it is there but cannot be
   opened or read, with errno saying why. */
static int read_all(const char *path, unsigned long long most, uint8_t **data, unsigned long long *size)
{
    FILE *f = fopen(path, "rb");
    long long n;
    int rc = 0, e;
    *data = NULL;
    *size = 0;
    if (!f)
        return errno == ENOENT ? -1 : -3;
    n = _filelengthi64(_fileno(f));
    if (n < 0) {
        rc = -3;
    } else if ((unsigned long long)n > most) {
        *size = (unsigned long long)n;
        rc = 1;
    } else {
        *size = (unsigned long long)n;
        *data = (uint8_t *)malloc((size_t)n + 1u);
        if (!*data || fread(*data, 1, (size_t)n, f) != (size_t)n) {
            free(*data);
            *data = NULL;
            rc = -3;
        }
    }
    e = errno;
    fclose(f);
    errno = e;
    return rc;
}

static int pinned(const uint8_t *b, size_t n, const char *pin)
{
    char hex[65];
    pc_sha256_hex(b, n, hex);
    return !strcmp(hex, pin);
}

/* A folder with the three files, as the firmware's build takes them. A test's made-up brain is taken with no check of its hashes. */
static int load_folder(const char *path, int check, char *why, size_t why_size)
{
    static const char *const names[3] = { "brain.bin", "cf.bin", "rodata.bin" };
    static const size_t sizes[3] = { CODE_HI - CODE_LO, HELPER_BYTES, TABLE_BYTES };
    const char *const pins[3] = { PIN_CODE, PIN_HELPERS, PIN_TABLES };
    uint8_t *part[3] = { NULL, NULL, NULL };
    int i, rc = 0;
    for (i = 0; i < 3 && !rc; i++) {
        char file[1400];
        unsigned long long n = 0;
        snprintf(file, sizeof(file), "%s/%s", path, names[i]);
        if (read_all(file, sizes[i], &part[i], &n) < 0) {
            snprintf(why, why_size, "the folder %s holds no %s", path, names[i]);
            rc = -2;
        } else if (n != sizes[i] || (check && !pinned(part[i], n, pins[i]))) {
            snprintf(why, why_size, "%s in %s is not the CPU MATE brain of Little-Man Firmware 1.0", names[i], path);
            rc = -2;
        }
    }
    if (!rc)
        pc_brain_load_parts(part[0], sizes[0], part[1], sizes[1], part[2], sizes[2]);
    for (i = 0; i < 3; i++)
        free(part[i]);
    return rc;
}

/* Little-Man Firmware's file, laid out as its docs/FAFRIUM.md says: 256 KB, every 16-bit pair swapped on disk, the instruction memory's image at
   0x80 and the work RAM's from 0x2C00 at 0x16080 (places in the file, not a game address). Only the brain's three parts are taken; nothing else of the file is used. */
static int load_firmware_file(const char *path, char *why, size_t why_size)
{
    unsigned long long n = 0;
    size_t i;
    uint8_t *disk, *mcu;
    uint32_t kind;
    int rc = read_all(path, 0x40000u, &disk, &n);      /* 256 KB; not a game address */
    if (rc == -1)
        return -1;
    if (rc == -3) {
        snprintf(why, why_size, "%s cannot be read (%s)", path, strerror(errno));
        return -3;
    }
    if (n != 0x40000u) {                        /* 256 KB; not a game address */
        snprintf(why, why_size, "%s is not Little-Man Firmware: that file is 262,144 bytes, this one %llu", path, n);
        free(disk);
        return -2;
    }
    rc = 0;
    mcu = (uint8_t *)malloc(n);
    if (!mcu) {
        free(disk);
        snprintf(why, why_size, "no memory to read %s", path);
        return -2;
    }
    for (i = 0; i < n; i++)
        mcu[i] = disk[i ^ 1u];
    kind = get32(mcu + 0x20u);
    if (get32(mcu) != 0x52464146u) {            /* "FAFR" */
        snprintf(why, why_size, "%s is not Little-Man Firmware", path);
        rc = -2;
    } else if (!(kind & 2u)) {
        snprintf(why, why_size, "%s is a Little-Man Firmware built without the CPU MATE brain", path);
        rc = -2;
    } else {
        const uint8_t *irom = mcu + 0x80u, *w = mcu + 0x16080u - 0x2C00u;     /* w + offset = work RAM byte at that offset; places in the file, not a game address */
        const uint8_t *code = irom + CODE_LO, *helpers = w + (HELPERS - WRAM), *tables = w + (TABLES - WRAM);
        if (!pinned(code, CODE_HI - CODE_LO, PIN_CODE) || !pinned(helpers, HELPER_BYTES, PIN_HELPERS) || !pinned(tables, TABLE_BYTES, PIN_TABLES)) {
            snprintf(why, why_size, "%s carries a CPU MATE brain other than Little-Man Firmware 1.0's, which this program does not know", path);
            rc = -2;
        } else {
            pc_brain_load_parts(code, CODE_HI - CODE_LO, helpers, HELPER_BYTES, tables, TABLE_BYTES);
        }
    }
    free(mcu);
    free(disk);
    return rc;
}

/* A folder is read as the brain's three files; anything else is opened as the firmware file. The C library's stat is asked only whether the name
   is a folder: it does not take every name Windows opens (a stream of a file, as file.txt:stream), so a name it refuses is still opened as a file,
   and only a name that cannot be opened at all counts as nothing there. */
int pc_brain_load(const char *path, char *why, size_t why_size)
{
    struct _stat64 st;
    why[0] = 0;
    if (_stat64(path, &st) == 0 && (st.st_mode & S_IFDIR))
        return load_folder(path, 1, why, why_size);
    return load_firmware_file(path, why, why_size);
}

int pc_brain_load_test(const char *folder, char *why, size_t why_size)
{
    why[0] = 0;
    return load_folder(folder, 0, why, why_size);
}

void pc_brain_release(void)
{
    int i;
    uint32_t k;
    for (i = 0; i < 4; i++) {
        uint8_t *p = at_sram(restore_list[i].addr);
        uint16_t f = (uint16_t)restore_list[i].fill;
        for (k = 0; k < restore_list[i].bytes; k += 2u)
            memcpy(p + k, &f, 2);
    }
    for (i = 0; i < 3; i++)
        memcpy(at_wram(copy_list[i].dst), at_sram(copy_list[i].src), copy_list[i].bytes);
    put32(at_wram(T16_KEY), 0u);
    flag_pending = 1;
    n_releases++;
}

int pc_brain_call(uint32_t *word, uint32_t *d_word, uint32_t *d_busy)
{
    int stop;
    uint64_t s0 = cpu.steps;
    uint32_t dw, db;
    if (!pc_brain_ready())
        return PC_RV32_FETCH;
    if (cpu.regions < 6)
        pc_rv32_map(&cpu, FLASH, pt_rom_size, pt_rom, 0, 0);
    put32(at_wram(CNT_OUT + 12u), 0u);
    if (flag_pending) {
        uint16_t one = 1u;
        memcpy(at_wram(FLAG), &one, 2);
        flag_pending = 0;
    }
    /* The firmware's wrapper calls the two entries one after the other, with the RV calling convention: a stack, and whatever the registers hold. */
    cpu.x[2] = STACK_TOP;
    cpu.x[10] = ENTRY_H2;
    cpu.x[11] = ENTRY_H1;
    stop = pc_rv32_call(&cpu, ENTRY_H2, RETURN_MARK, CALL_STEPS);
    if (stop == PC_RV32_DONE) {
        cpu.x[2] = STACK_TOP;
        stop = pc_rv32_call(&cpu, ENTRY_H1, RETURN_MARK, CALL_STEPS);
    }
    n_calls++;
    if (cpu.steps - s0 > steps_max)
        steps_max = cpu.steps - s0;
    steps_all += cpu.steps - s0;
    if (stop != PC_RV32_DONE) {
        failed = 1;
        put32(at_wram(T16_KEY), 0u);
        printf("CPU MATE: the brain stopped, %s, at %08X (address %08X, word %08X): it is off for the rest of the run\n", pc_rv32_why(stop),
               (unsigned)cpu.fault_pc, (unsigned)cpu.fault_addr, (unsigned)cpu.fault_word);
        fflush(stdout);
        return stop;
    }
    dw = get32(at_wram(CNT_OUT + 4u)) & 0xFFFFFFu;
    db = get32(at_wram(CNT_OUT + 8u)) & 0xFFFFFFu;
    *word = cpu.x[10] & 0xFFFFu;
    *d_word = dw;
    *d_busy = db < dw ? dw : db;
    return 0;
}

void pc_brain_report(void)
{
    if (!loaded)
        return;
    printf("CPU MATE brain: %s, %llu calls, %llu releases, %llu instructions, at most %llu in one call\n", failed ? "stopped" : "ready",
           (unsigned long long)n_calls, (unsigned long long)n_releases, (unsigned long long)steps_all, (unsigned long long)steps_max);
    fflush(stdout);
}
