/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Compares pr_render with what the exact chip drew on recorded frames. The recordings come from the lockstep bench of the video chip, which plays each
 * state through the real chip model. They come from a run of the game, so they stay outside this repository and are named on the command line.
 *
 *   pr_compare STATES [--cart-layers FILE] [--free-layers FILE] [--rgb FILE] [--swap FILE] [--writes FILE] [--dump FILE] [--lines N] [--skip-swap] [--speed N]
 *
 * STATES         a file of chip states: a 12 byte head whose last word is the count, then per frame the frame number, flags, 32 register bytes, 128 colour
 *                bytes, 80 scroll bytes, 64 KB of VRAM, and 112 bytes of sprite table for the mid-frame table swap. Flag bit 0 says that swap happens.
 *                It can also be a file of PcFrame records (see pc_frame.h), found by its first word, which brings its own writes.
 * --cart-layers  a file of the bench's sprite layers: per frame, number and flags, then the chip's layer and the other side's, 240 by 320 bytes each.
 *                The chip's layer is compared with pr_render's limits on, on all seven bits.
 * --free-layers  the same kind of file, from a run with the Pocket core's no-limit sprite renderer on. Its layer is compared with pr_render's no limit, on opaque pixels.
 * --rgb          the bench's colour output: per frame, number and flags, then both sides' pictures as 32-bit 0x00RRGGBB, 224 by 320. The first must equal
 *                limits on and the second no limit.
 * --swap         the swap's writes as they landed on the bench, one "vline hclock" a line, so the renderer can be given them with their real times. Without
 *                it the lines the swap lands on are left out of the compare and counted apart.
 * --writes FILE  writes to give the renderer, one "frame vline hclock kind addr value" a line, where frame counts the frames compared from 0.
 * --dump FILE    writes pr_render's pictures in the shape of a --rgb file, for looking at the differences.
 * --lines N      prints at most N differing lines (default 40).
 * --skip-swap    leaves the lines the swap lands on out of the compare even with --swap, and counts them apart.
 * --speed N      compares nothing: renders each frame of STATES N times in each mode, with no sprite layer kept, as the program will, and prints the time a picture takes.
 */
#include "../targets/pc/pr_local.h"

#include <stdlib.h>
#include <time.h>

#define SWAP_WORDS 56

static PcFrame frame;
static PcPicture pic_cart, pic_free;
static uint8_t lay_ref[240][320], lay_dut[240][320], keep_cart[240][320];
static uint32_t rgb_ref[224][320], rgb_dut[224][320];
static uint16_t swap_times[SWAP_WORDS][2];
static PrMemory memory_cart, memory_free;

typedef struct Record {
    uint32_t number, flags;
    uint8_t  regs[32], cram[128], vsram[80];
    uint8_t  vram[65536];
    uint8_t  stash[112];
} Record;

static Record record;

typedef struct ExtraWrite { int frame; PcWrite write; } ExtraWrite;
static ExtraWrite *extra_writes;
static unsigned n_extra;

static void add_extra_writes(int compared)
{
    unsigned i;
    for (i = 0; i < n_extra; i++)
        if (extra_writes[i].frame == compared && frame.n_writes < PC_FRAME_WRITES_MAX)
            frame.writes[frame.n_writes++] = extra_writes[i].write;
}

static void read_extra_writes(const char *path)
{
    FILE *f = fopen(path, "r");
    int fr;
    unsigned v, h, k, a, val;
    if (!f) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    while (fscanf(f, "%d %u %u %u %i %i", &fr, &v, &h, &k, (int *)&a, (int *)&val) == 6) {
        extra_writes = (ExtraWrite *)realloc(extra_writes, (n_extra + 1u) * sizeof(ExtraWrite));
        extra_writes[n_extra].frame = fr;
        extra_writes[n_extra].write.vline = (uint16_t)v;
        extra_writes[n_extra].write.hclock = (uint16_t)h;
        extra_writes[n_extra].write.kind = (uint8_t)k;
        extra_writes[n_extra].write.flags = 0;
        extra_writes[n_extra].write.addr = (uint16_t)a;
        extra_writes[n_extra].write.value = (uint16_t)val;
        n_extra++;
    }
    fclose(f);
}

static void load_frame(int have_swap_times)
{
    unsigned i;
    PcVdpState *s = &frame.start;
    memset(s, 0, sizeof(*s));
    frame.number = record.number;
    frame.n_writes = 0;
    frame.dropped = 0;
    memcpy(s->vram, record.vram, sizeof(s->vram));
    for (i = 0; i < PC_CRAM_WORDS; i++)
        s->cram[i] = (uint16_t)(((unsigned)record.cram[i * 2] << 8 | record.cram[i * 2 + 1]) & 0x0EEEu);
    for (i = 0; i < PC_VSRAM_WORDS; i++)
        s->vsram[i] = (uint16_t)(((unsigned)record.vsram[i * 2] << 8 | record.vsram[i * 2 + 1]) & 0x07FFu);
    memcpy(s->regs, record.regs, PC_VDP_REGS);
    {
        unsigned h40 = s->regs[12] & 1u;
        unsigned table = (unsigned)(s->regs[5] & (h40 ? 0x7Eu : 0x7Fu)) << 9;
        for (i = 0; i < (h40 ? 80u : 64u); i++)
            memcpy(s->sat_cache[i], &s->vram[(table + i * 8u) & 0xFFFFu], 4);
        if ((record.flags & 1u) && have_swap_times) {
            for (i = 0; i < SWAP_WORDS; i++) {
                PcWrite *w = &frame.writes[frame.n_writes++];
                w->vline = swap_times[i][0];
                w->hclock = swap_times[i][1];
                w->kind = PC_WRITE_VRAM;
                w->flags = PC_WF_IN_HINT;
                w->addr = (uint16_t)(table + i * 2u);
                w->value = (uint16_t)((unsigned)record.stash[i * 2] << 8 | record.stash[i * 2 + 1]);
            }
        }
    }
}

typedef struct Tally {
    uint64_t frames, lines, lines_differ, px_differ, near_swap_lines, near_swap_px, frames_differ, first_line_rows;
} Tally;

static int print_left = 40;

static void note(const char *what, uint32_t number, int line, unsigned px, int x0, unsigned want, unsigned got, int near_swap)
{
    if (print_left > 0) {
        print_left--;
        printf("%s frame %u line %d: %u px differ from x %d (want %06X got %06X)%s\n", what, number, line, px, x0, want, got, near_swap ? " near the swap" : "");
    }
}

static void tally_line(const char *what, Tally *t, int line, unsigned px, int x0, unsigned want, unsigned got, int skip_swap, int *any)
{
    int near_swap = (record.flags & 1u) && line >= 46 && line < 60;
    if (!px)
        return;
    if (near_swap) {
        t->near_swap_lines++;
        t->near_swap_px += px;
        if (skip_swap)
            return;
    }
    t->lines_differ++;
    t->px_differ += px;
    *any = 1;
    if (line == 0)
        t->first_line_rows++;
    note(what, record.number, line, px, x0, want, got, near_swap);
}

static void compare_layer(const char *what, Tally *t, const uint8_t (*want)[320], const PcPicture *got, unsigned opaque_only, unsigned width, int skip_swap)
{
    int line, any = 0;
    for (line = 0; line < 224; line++) {
        unsigned x, px = 0, w0 = 0, g0 = 0;
        int x0 = -1;
        for (x = 0; x < width; x++) {
            unsigned a = want[line][x], b = got->sprite_layer[line][x];
            if (opaque_only) {
                if (!(a & 15u)) a = 0;
                if (!(b & 15u)) b = 0;
            }
            if (a != b) {
                if (x0 < 0) {
                    x0 = (int)x;
                    w0 = a;
                    g0 = b;
                }
                px++;
            }
        }
        t->lines++;
        tally_line(what, t, line, px, x0, w0, g0, skip_swap, &any);
    }
    t->frames++;
    t->frames_differ += any;
}

static void compare_rgb(const char *what, Tally *t, const uint32_t (*want)[320], const PcPicture *got, unsigned width, int skip_swap)
{
    int line, any = 0;
    for (line = 0; line < 224; line++) {
        unsigned x, px = 0, w0 = 0, g0 = 0;
        int x0 = -1;
        for (x = 0; x < width; x++) {
            if (want[line][x] != got->rgb[line][x]) {
                if (x0 < 0) {
                    x0 = (int)x;
                    w0 = want[line][x];
                    g0 = got->rgb[line][x];
                }
                px++;
            }
        }
        t->lines++;
        tally_line(what, t, line, px, x0, w0, g0, skip_swap, &any);
    }
    t->frames++;
    t->frames_differ += any;
}

static void report(const char *what, const Tally *t)
{
    printf("%-20s frames %llu, lines %llu: lines differing %llu, pixels differing %llu, frames differing %llu; near the swap, lines %llu pixels %llu; on line 0 %llu\n", what,
           (unsigned long long)t->frames, (unsigned long long)t->lines, (unsigned long long)t->lines_differ, (unsigned long long)t->px_differ,
           (unsigned long long)t->frames_differ, (unsigned long long)t->near_swap_lines, (unsigned long long)t->near_swap_px, (unsigned long long)t->first_line_rows);
}

/* Reads one frame of a results file. Returns 0 at the end of the file. */
static int read_layers(FILE *f, uint32_t *number)
{
    uint32_t h[2];
    if (fread(h, 4, 2, f) != 2)
        return 0;
    if (fread(lay_ref, sizeof(lay_ref), 1, f) != 1 || fread(lay_dut, sizeof(lay_dut), 1, f) != 1)
        return 0;
    *number = h[0];
    return 1;
}

static int find_record(FILE *fs, uint32_t count, uint32_t number)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t n2;
        fseek(fs, 12 + (long)i * (long)sizeof(Record), SEEK_SET);
        if (fread(&n2, 4, 1, fs) != 1)
            return 0;
        if (n2 == number) {
            fseek(fs, 12 + (long)i * (long)sizeof(Record), SEEK_SET);
            return fread(&record, sizeof(Record), 1, fs) == 1;
        }
    }
    return 0;
}

/* The time a picture takes, over a whole file. */
static int speed_test(FILE *fs, uint32_t count, int repeats, int have_swap)
{
    PrOptions cart = { PR_LIMITS_CART, 0 }, none = { PR_LIMITS_NONE, 0 };
    double total[2] = { 0, 0 }, worst[2] = { 0, 0 };
    uint32_t i;
    for (i = 0; i < count; i++) {
        int r, mode;
        fseek(fs, 12 + (long)i * (long)sizeof(Record), SEEK_SET);
        if (fread(&record, sizeof(Record), 1, fs) != 1)
            return 2;
        load_frame(have_swap);
        for (mode = 0; mode < 2; mode++) {
            clock_t t0 = clock();
            double spent;
            for (r = 0; r < repeats; r++)
                pr_render_kept(&frame, mode ? &none : &cart, mode ? &memory_free : &memory_cart, mode ? &pic_free : &pic_cart);
            spent = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / repeats;
            total[mode] += spent;
            if (spent > worst[mode])
                worst[mode] = spent;
        }
    }
    printf("speed over %u frames, %d times each: limits on %.0f microseconds a picture, worst frame %.0f; no limit %.0f, worst frame %.0f\n", count, repeats,
           total[0] / count, worst[0], total[1] / count, worst[1]);
    return 0;
}

int main(int argc, char **argv)
{
    const char *states = NULL, *cart_layers = NULL, *free_layers = NULL, *rgb = NULL, *swap = NULL, *dump = NULL;
    FILE *fs, *fc = NULL, *ff = NULL, *fr = NULL, *fd = NULL;
    uint32_t head[3];
    int a, have_swap = 0, failed = 0, skip_swap = 0, is_frames = 0, compared = 0, speed = 0;
    Tally t_cart, t_free, t_rcart, t_rfree;
    PrOptions opt_cart = { PR_LIMITS_CART, 1 }, opt_free = { PR_LIMITS_NONE, 1 };
    double render_clock = 0;
    uint64_t renders = 0, lines_over = 0;

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--cart-layers") && a + 1 < argc) cart_layers = argv[++a];
        else if (!strcmp(argv[a], "--free-layers") && a + 1 < argc) free_layers = argv[++a];
        else if (!strcmp(argv[a], "--rgb") && a + 1 < argc) rgb = argv[++a];
        else if (!strcmp(argv[a], "--swap") && a + 1 < argc) swap = argv[++a];
        else if (!strcmp(argv[a], "--lines") && a + 1 < argc) print_left = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--writes") && a + 1 < argc) read_extra_writes(argv[++a]);
        else if (!strcmp(argv[a], "--dump") && a + 1 < argc) dump = argv[++a];
        else if (!strcmp(argv[a], "--skip-swap")) skip_swap = 1;
        else if (!strcmp(argv[a], "--speed") && a + 1 < argc) speed = atoi(argv[++a]);
        else states = argv[a];
    }
    if (!states) {
        fprintf(stderr, "usage: pr_compare STATES [--cart-layers FILE] [--free-layers FILE] [--rgb FILE] [--swap FILE] [--lines N]\n");
        return 2;
    }
    if (swap) {
        FILE *f = fopen(swap, "r");
        unsigned v, h, k = 0;
        if (!f) {
            fprintf(stderr, "cannot read %s\n", swap);
            return 2;
        }
        while (k < SWAP_WORDS && fscanf(f, "%u %u", &v, &h) == 2) {
            swap_times[k][0] = (uint16_t)v;
            swap_times[k][1] = (uint16_t)h;
            k++;
        }
        fclose(f);
        have_swap = k == SWAP_WORDS;
        if (!have_swap) {
            fprintf(stderr, "%s holds %u times, not %d\n", swap, k, SWAP_WORDS);
            return 2;
        }
    }
    fs = fopen(states, "rb");
    if (!fs || fread(head, 4, 3, fs) != 3) {
        fprintf(stderr, "cannot read %s\n", states);
        return 2;
    }
    if (head[0] == PC_FRAME_FILE_MAGIC) {
        is_frames = 1;
        fseek(fs, 0, SEEK_SET);
    }
    if (dump && !(fd = fopen(dump, "wb"))) {
        fprintf(stderr, "cannot write %s\n", dump);
        return 2;
    }
    skip_swap |= !have_swap;
    if (speed)
        return speed_test(fs, head[2], speed, have_swap);
    if ((cart_layers && !(fc = fopen(cart_layers, "rb"))) || (free_layers && !(ff = fopen(free_layers, "rb"))) || (rgb && !(fr = fopen(rgb, "rb")))) {
        fprintf(stderr, "cannot read a results file\n");
        return 2;
    }
    if (!fc && !ff && !fr) {
        fprintf(stderr, "nothing to compare with\n");
        return 2;
    }
    memset(&t_cart, 0, sizeof(t_cart));
    memset(&t_free, 0, sizeof(t_free));
    memset(&t_rcart, 0, sizeof(t_rcart));
    memset(&t_rfree, 0, sizeof(t_rfree));

    /* The bench may have played only some of the file's frames, so a frame is found by its number. All the results files hold the same frames in the same order. */
    for (;;) {
        uint32_t number = 0, other = 0;
        int have = 0, ended = 0;
        if (fc) {
            if (!read_layers(fc, &number)) {
                ended = 1;
            } else {
                memcpy(keep_cart, lay_ref, sizeof(keep_cart));
                have = 1;
            }
        }
        if (ff && !ended) {
            if (!read_layers(ff, &other)) {
                ended = 1;
            } else {
                if (have && other != number) {
                    fprintf(stderr, "the results files disagree: frame %u against %u\n", number, other);
                    return 2;
                }
                number = other;
                have = 1;
            }
        }
        if (fr && !ended) {
            uint32_t h[2];
            if (fread(h, 4, 2, fr) != 2 || fread(rgb_ref, sizeof(rgb_ref), 1, fr) != 1 || fread(rgb_dut, sizeof(rgb_dut), 1, fr) != 1) {
                ended = 1;
            } else {
                if (have && h[0] != number) {
                    fprintf(stderr, "the results files disagree: frame %u against %u\n", number, h[0]);
                    return 2;
                }
                number = h[0];
            }
        }
        if (ended)
            break;
        if (is_frames) {
            /* Records in the same order as the results */
            if (pc_frame_read(fs, &frame) != 0 || frame.number != number) {
                fprintf(stderr, "frame %u is not next in %s\n", number, states);
                return 2;
            }
            record.flags = 0;
            record.number = number;
        } else {
            if (!find_record(fs, head[2], number)) {
                fprintf(stderr, "frame %u is not in %s\n", number, states);
                return 2;
            }
            load_frame(have_swap);
        }
        add_extra_writes(compared);
        compared++;
        {
            clock_t t0 = clock();
            /* The bench plays the first state twice before it records, and then plays its frames one after the other, so the chip's walk carries its memory across */
            if (compared == 1)
                pr_render_kept(&frame, &opt_cart, &memory_cart, &pic_cart);
            pr_render_kept(&frame, &opt_cart, &memory_cart, &pic_cart);
            pr_render_kept(&frame, &opt_free, &memory_free, &pic_free);
            render_clock += (double)(clock() - t0);
            renders += 2;
            lines_over += pic_cart.lines_over;
            if (pic_cart.lines_over != pic_free.lines_over)
                printf("frame %u: lines over %u limits on, %u no limit\n", number, pic_cart.lines_over, pic_free.lines_over);
        }
        {
            unsigned width = pic_cart.width;
            if (fc)
                compare_layer("layer limits on", &t_cart, (const uint8_t (*)[320])keep_cart, &pic_cart, 0, width, skip_swap);
            if (ff)
                compare_layer("layer no limit", &t_free, (const uint8_t (*)[320])lay_dut, &pic_free, 1, width, skip_swap);
            if (fr) {
                compare_rgb("picture limits on", &t_rcart, (const uint32_t (*)[320])rgb_ref, &pic_cart, width, skip_swap);
                compare_rgb("picture no limit", &t_rfree, (const uint32_t (*)[320])rgb_dut, &pic_free, width, skip_swap);
            }
            if (fd) {
                uint32_t h[2], y;
                h[0] = number;
                h[1] = record.flags;
                fwrite(h, 4, 2, fd);
                for (y = 0; y < 224; y++)
                    fwrite(pic_cart.rgb[y], 4, 320, fd);
                for (y = 0; y < 224; y++)
                    fwrite(pic_free.rgb[y], 4, 320, fd);
            }
        }
    }
    if (fd) fclose(fd);
    if (fc) report("layer limits on", &t_cart);
    if (ff) report("layer no limit", &t_free);
    if (fr) {
        report("picture limits on", &t_rcart);
        report("picture no limit", &t_rfree);
    }
    failed = t_cart.lines_differ || t_free.lines_differ || t_rcart.lines_differ || t_rfree.lines_differ;
    printf("lines over the chip's limits %llu\n", (unsigned long long)lines_over);
    if (renders)
        printf("speed: %.0f microseconds a picture, over %llu pictures\n", render_clock * 1e6 / CLOCKS_PER_SEC / (double)renders, (unsigned long long)renders);
    printf("RESULT: %s\n", failed ? "DIFFER" : "EQUAL");
    return failed ? 1 : 0;
}
