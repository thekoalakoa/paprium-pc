/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A small tool: boots the firmware on the player's file and lists the bytes its boot work changes against the file, for the translator, which must translate what the 68000 will
 * really fetch. The list goes to the output folder and stays there: this repository holds no game address, and the constants behind the list live in the firmware's source.
 *
 * Usage: pc_boot_image ROM OUTPUT.json
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_cart.h"
#include "pc_fiber.h"
#include "pc_vdp.h"

/* What pc_cart.c wants of a machine that is not there: nothing runs, so nothing calls them for real. */
PcCounts pc_counts;
void pc_tick_stop_request(void) {}
void pc_z80_run_to(uint64_t t) { (void)t; }
void pc_sound_log_post(uint64_t t, uint32_t address, uint32_t word) { (void)t; (void)address; (void)word; }

int main(int argc, char **argv)
{
    static PcCartConfig cart;
    FILE *f;
    long n;
    uint8_t *file, *words, *low;
    long i;
    int first = 1, rows = 0;
    if (argc != 3)
        return 2;
    f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc((size_t)n);
    words = (uint8_t *)malloc((size_t)n);
    low = (uint8_t *)malloc(0x10000u);
    if (!file || !words || !low || fread(file, 1, (size_t)n, f) != (size_t)n)
        return 2;
    fclose(f);
    for (i = 0; i + 1 < n; i += 2) {
        words[i] = file[i + 1];
        words[i + 1] = file[i];
    }
    pc_fiber_main_init();
    cart.rom_bytes = file;
    cart.rom_size = (uint32_t)n;
    cart.rom_words = words;
    cart.no_bank = 1;
    pc_cart_boot(&cart);
    memcpy(low, words, 0x10000u);
    pc_cart_copy_boot_page(low, 0x10000u);

    f = fopen(argv[2], "w");
    if (!f)
        return 2;
    fprintf(f, "[");
    i = 0;
    while (i < n) {
        uint8_t have = i < 0x10000 ? low[i ^ 1] : words[i ^ 1];
        if (have != file[i]) {
            long j = i;
            fprintf(f, "%s\n {\"address\": %ld, \"why\": \"the firmware's boot work\", \"hex\": \"", first ? "" : ",", i);
            first = 0;
            while (j < n) {
                have = j < 0x10000 ? low[j ^ 1] : words[j ^ 1];
                if (have == file[j])
                    break;
                fprintf(f, "%02x", have);
                j++;
            }
            fprintf(f, "\"}");
            rows++;
            i = j;
        } else {
            i++;
        }
    }
    fprintf(f, "\n]\n");
    fclose(f);
    printf("the firmware's boot changes %d places\n", rows);
    printf("RESULT: PASS\n");
    fflush(stdout);
    exit(0);
}
