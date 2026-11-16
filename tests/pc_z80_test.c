/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Z80, the FM chip and the PSG in the machine, on small made-up Z80 programs and hand-counted clocks. tests/pc_machine_test.c includes this file, for its helpers and its small cart. No game byte.
 * The real chips' cores run here: the local configuration's sound_cores names them, and they are pinned in targets/pc/pc_build.py.
 */

/* ---- what the chips send to the mix */
static uint64_t fm_samples, fm_last_t, fm_bad_gaps;
static int32_t fm_left, fm_right;
void pc_sound_fm(uint64_t t, int32_t left, int32_t right)
{
    if (fm_samples && t - fm_last_t != 1008u)
        fm_bad_gaps++;
    fm_samples++;
    fm_last_t = t;
    fm_left = left;
    fm_right = right;
}

static uint64_t psg_changes, psg_last_t, psg_last_gap;
static int32_t psg_level, psg_highest;
void pc_sound_psg(uint64_t t, int32_t level)
{
    psg_changes++;
    psg_last_gap = t - psg_last_t;
    psg_last_t = t;
    psg_level = level;
    if (level > psg_highest)
        psg_highest = level;
}

/* ---- helpers */
#define Z80_RAM 0xA00000u
#define Z80_BUSREQ 0xA11100u
#define Z80_RESET 0xA11200u

/* The 68000 loads a program with the bus held, as the game does, and lets the Z80 go. */
static void z80_load(const uint8_t *code, uint32_t n)
{
    uint32_t i;
    wb(Z80_BUSREQ, 1);
    for (i = 0; i < n; i++)
        wb(Z80_RAM + i, code[i]);
    wb(Z80_BUSREQ, 0);
}

static void z80_go(const uint8_t *code, uint32_t n)
{
    z80_load(code, n);
    wb(Z80_RESET, 1);
}

/* The clock moves on by this much. A stall moves it, so no 68000 clock plays a part. */
static void z_run(uint64_t mclk)
{
    uint64_t t = pc_clock_now() + mclk;
    go_to(t);
    pc_z80_run_to(t);
}

static uint64_t delta(uint64_t now, uint64_t *was)
{
    uint64_t d = now - *was;
    *was = now;
    return d;
}

/* ---- the bus, by clocks counted by hand */
static void test_z80_bus(void)
{
    const PcZ80Counts *c = pc_z80_counts();
    uint64_t reads, waits, clocks, ym, t0;

    /* A window read costs the table's clocks and the mean wait, and the 68000 pays its share. LD A,(8000h) is 13 clocks, JP 10. */
    {
        static const uint8_t code[] = {0x3A, 0x00, 0x80, 0xC3, 0x00, 0x00};
        reset_all();
        z80_go(code, sizeof(code));
        reads = c->window_reads;
        waits = c->wait_clocks;
        clocks = c->clocks;
        t0 = pc_clock_now();
        {
            uint64_t want = 15u * 3000u * 100u;     /* 4.5 M master clocks. Each read's share moves the clock on as well, and the Z80 follows it */
            double mean, table;
            uint64_t r, w, k, moved;
            z_run(want);
            moved = pc_clock_now() - t0;
            r = delta(c->window_reads, &reads);
            w = delta(c->wait_clocks, &waits);
            k = delta(c->clocks, &clocks);
            printf("window loop: reads %llu, wait %llu, table %llu, clock moved %llu for %llu\n", (unsigned long long)r, (unsigned long long)w, (unsigned long long)k, (unsigned long long)moved, (unsigned long long)want);
            CHECK(r > 10000u);
            mean = (double)w / (double)r;
            CHECK(mean > 2.666 && mean < 2.686);
            table = (double)k / (double)r;
            CHECK(table > 22.99 && table < 23.01);
            CHECK(moved == want + r * PC_Z80_BUS_SHARE_MCLK);
            /* every clock accounted for: the time run is the table's and the wait's, and it reaches the clock to within a line */
            CHECK((k + w) * 15u <= moved + 45u * 15u && (k + w) * 15u + PC_MCLK_LINE + 45u * 15u >= moved);
        }
    }

    /* What the window reads is the cart's, and it is not counted as the 68000's. LD A,(8201h); LD (0100h),A; JR $ */
    {
        static const uint8_t code[] = {0x3A, 0x01, 0x82, 0x32, 0x00, 0x01, 0x18, 0xFE};
        reset_all();
        cart[0x100] = 0x1234;
        z80_go(code, sizeof(code));
        {
            uint64_t kept = pc_counts.cart_reads;
            z_run(15u * 100u);
            CHECK(pc_z80_ram()[0x100] == 0x34);
            CHECK(pc_counts.cart_reads == kept);
        }
    }

    /* A write to the FM chip takes no wait and no share. LD A,2Ah 7; LD (4000h),A 13; JP 10. */
    {
        static const uint8_t code[] = {0x3E, 0x2A, 0x32, 0x00, 0x40, 0xC3, 0x00, 0x00};
        reset_all();
        z80_go(code, sizeof(code));
        waits = c->wait_clocks;
        ym = c->ym_writes;
        t0 = pc_clock_now();
        z_run(15u * 30u * 1000u);
        CHECK(pc_clock_now() - t0 == 15u * 30u * 1000u);
        CHECK(c->wait_clocks == waits);
        CHECK(c->ym_writes - ym >= 999u && c->ym_writes - ym <= 1001u);
    }

    /* The bank register takes nine bits, the first written is the lowest. Bank 1FEh puts the window on work RAM's first 32 KB. */
    {
        static const uint8_t code[] = {0x21, 0x00, 0x60, 0x36, 0x00, 0x36, 0x01, 0x36, 0x01, 0x36, 0x01, 0x36, 0x01, 0x36, 0x01, 0x36, 0x01, 0x36, 0x01, 0x36, 0x01,
                                       0x3A, 0x23, 0x81, 0x32, 0x00, 0x01, 0x18, 0xFE};
        static const uint8_t next[] = {0x3A, 0x23, 0x81, 0x32, 0x01, 0x01, 0x18, 0xFE};
        uint64_t others;
        reset_all();
        others = c->other_pages;
        pt_mem_w8(pt_ram, 0x0123u, 0xA5u);
        z80_go(code, sizeof(code));
        z_run(15u * 300u);
        CHECK(pc_z80_ram()[0x100] == 0xA5);
        CHECK(c->other_pages == others + 1u);
        /* The Z80's reset does not reach the bank register. */
        wb(Z80_RESET, 0);
        z_run(15u * 100u);
        z80_load(next, sizeof(next));
        wb(Z80_RESET, 1);
        z_run(15u * 100u);
        CHECK(pc_z80_ram()[0x101] == 0xA5);
    }

    /* The 68000 holds the bus: the Z80 stops and starts again where it was. LD HL,0100h 10; INC (HL) 11; JP 10. */
    {
        static const uint8_t code[] = {0x21, 0x00, 0x01, 0x34, 0xC3, 0x03, 0x00};
        uint32_t n, a;
        reset_all();
        z80_go(code, sizeof(code));
        z_run(15u * (10u + 21u * 100u));
        n = pc_z80_ram()[0x100];
        CHECK(n >= 99u && n <= 101u);
        wb(Z80_BUSREQ, 1);
        z_run(15u * 21u * 50u);
        CHECK(pc_z80_ram()[0x100] == n);
        /* with the bus held the 68000 reaches the RAM */
        wb(Z80_RAM + 0x200u, 0x66);
        CHECK(pc_z80_ram()[0x200] == 0x66);
        pt_port_write(2, Z80_RAM + 0x202u, 0x7788u);
        CHECK(pc_z80_ram()[0x202] == 0x77);
        pt_port_write(4, Z80_RAM + 0x204u, 0x11223344u);
        CHECK(pc_z80_ram()[0x204] == 0x11 && pc_z80_ram()[0x206] == 0x33);
        CHECK(pt_port_read(1, Z80_RAM + 0x200u) == 0x66);
        wb(Z80_BUSREQ, 0);
        /* with the bus not held it does not, and that counts */
        a = (uint32_t)c->no_grant_writes;
        wb(Z80_RAM + 0x200u, 0x99);
        CHECK(pc_z80_ram()[0x200] == 0x66);
        CHECK(c->no_grant_writes == a + 1u);
        z_run(15u * 21u * 100u);
        n = (uint32_t)pc_z80_ram()[0x100] - n;
        CHECK(n >= 99u && n <= 101u);
    }

    /* The reset line: the Z80 starts again from address 0, with its RAM as it was. */
    {
        static const uint8_t code[] = {0x3A, 0x00, 0x02, 0x3C, 0x32, 0x00, 0x02, 0x18, 0xFE};
        reset_all();
        z80_go(code, sizeof(code));
        z_run(15u * 100u);
        CHECK(pc_z80_ram()[0x200] == 1);
        wb(Z80_RESET, 0);
        z_run(15u * 100u);
        CHECK(pc_z80_ram()[0x200] == 1);
        wb(Z80_RESET, 1);
        z_run(15u * 100u);
        CHECK(pc_z80_ram()[0x200] == 2);
        /* no time passes while the reset holds it: released later, the program has not moved on */
        wb(Z80_RESET, 0);
        z_run(15u * 1000u);
        CHECK(pc_z80_ram()[0x200] == 2);
    }

    /* A window read inside a DMA waits for the DMA's end. */
    {
        static const uint8_t code[] = {0x3A, 0x00, 0x80, 0xC3, 0x00, 0x00};
        uint64_t end, r0;
        reset_all();
        reg(1, 0x14);       /* DMA on, display off: every slot free, so a word takes a known time */
        reg(15, 2);
        reg(19, 0xE8);
        reg(20, 0x03);      /* 1,000 words */
        reg(21, 0x00);
        reg(22, 0x08);
        reg(23, 0x7F);      /* from work RAM */
        z80_go(code, sizeof(code));
        z_run(15u * 2000u);
        reads = c->dma_waits;
        pc_z80_run_to(pc_clock_now());
        cmd(0x21, 0);
        end = pc_vdp_dma_end();
        CHECK(end > pc_vdp_dma_start() + 20000u);
        r0 = c->window_reads;
        pc_z80_run_to(end);
        CHECK(c->window_reads - r0 <= 1u);
        CHECK(c->dma_waits == reads + 1u);
        z_run(15u * 100u);
        CHECK(c->window_reads - r0 >= 2u);
    }

    /* A cart RAM word read within a line of a 68000 write counts: the game must show none of these. */
    {
        static const uint8_t code[] = {0x3A, 0x00, 0x90, 0xC3, 0x00, 0x00};
        uint64_t near;
        reset_all();
        z80_go(code, sizeof(code));
        z_run(15u * 500u);
        near = c->near_cart_writes;
        pt_port_write(2, 0x1000u, 0x4242u);
        z_run(15u * 40u);
        CHECK(c->near_cart_writes > near);
        near = c->near_cart_writes;
        z_run(PC_MCLK_LINE * 2u);       /* the same word goes on being read, but that write is long past */
        z_run(PC_MCLK_LINE);
        {
            uint64_t later = c->near_cart_writes;
            z_run(PC_MCLK_LINE);
            CHECK(c->near_cart_writes == later);
        }
    }

    /* Its one write to the video chip: the first half of a command, at the Z80's own time, so the 68000's next control word is that command's second half. The 68000's block remainder is left alone.
       LD A,F1h; LD (7F05h),A; JR $ */
    {
        static const uint8_t code[] = {0x3E, 0xF1, 0x32, 0x05, 0x7F, 0x18, 0xFE};
        const PcVdpState *s = pc_vdp_state();
        uint64_t chip;
        reset_all();
        reg(10, 0x55);
        z80_go(code, sizeof(code));
        chip = c->chip_writes;
        go_to(pc_clock_now() + 15u * 100u);     /* the clock has passed the Z80's write, and the Z80 has not caught up */
        pt_at = 12u;
        pt_port_write(2, CTRL, 0x8A77u);        /* the access itself brings the Z80 up, so its write goes first */
        pt_at = 0u;
        CHECK(c->chip_writes == chip + 1u);
        CHECK(s->regs[10] == 0x55);             /* so this word was the command's second half, not a register */
        pc_vdp_z80_write(0x7F04u, 0xF1u, pc_clock_now());
        pt_port_read(2, CTRL);                  /* a status read clears the half command */
        w16(CTRL, 0x8A77u);
        CHECK(s->regs[10] == 0x77);
        pt_at = 12u;
        pc_vdp_z80_write(0x7F04u, 0x00u, pc_clock_now());
        CHECK(pt_at == 12u);
        pt_at = 0u;
        pt_port_read(2, CTRL);
    }

    /* What becomes of the next control word after that half command, as the exact chip does on the bench. The step register tells: 4 if the word was a register write, 2 if it was the command's second half. */
    {
        const PcVdpState *s = pc_vdp_state();
        reset_all();
        reg(15, 2);
        reg(15, 4);
        CHECK(s->regs[15] == 4);                /* nothing first: a register write */
        reg(15, 2);
        pc_vdp_z80_write(0x7F05u, 0xF1u, pc_clock_now());
        reg(15, 4);
        CHECK(s->regs[15] == 2);                /* the half command alone: the second half */
        pc_vdp_z80_write(0x7F05u, 0xF1u, pc_clock_now());
        (void)pt_port_read(2, CTRL);
        reg(15, 4);
        CHECK(s->regs[15] == 4);                /* then a status read: a register write */
        reg(15, 2);
        pc_vdp_z80_write(0x7F05u, 0xF1u, pc_clock_now());
        w16(DATA, 0x1234u);
        reg(15, 4);
        CHECK(s->regs[15] == 4);                /* then a word to the data port: a register write */
        reg(15, 2);
        pc_vdp_z80_write(0x7F05u, 0xF1u, pc_clock_now());
        w16(CTRL, 0x0000u);
        reg(15, 4);
        CHECK(s->regs[15] == 4);                /* then a second half word: a register write */
    }

    /* The sound log: the event log's 16-byte records. */
    {
        static const uint8_t code[] = {0x3E, 0x28, 0x32, 0x00, 0x40, 0x3A, 0x10, 0x80, 0x18, 0xFE};
        char path[512];
        uint8_t r[32];
        FILE *f;
        snprintf(path, sizeof(path), "%s/z80_log.bin", scratch);
        reset_all();
        pc_sound_log_open(path);
        z80_go(code, sizeof(code));
        z_run(15u * 100u);
        pc_sound_log_open(NULL);
        f = fopen(path, "rb");
        CHECK(f != NULL);
        if (f) {
            size_t n;
            uint64_t want = 0;
            int ym_seen = 0, read_seen = 0;
            while ((n = fread(r, 1, 16, f)) == 16) {
                uint16_t a;
                memcpy(&a, r + 10, 2);
                if (r[8] == 1 && (r[9] & 4) && a == 0x4000 && r[12] == 0x28)
                    ym_seen = 1;
                if (r[8] == 2 && r[9] == 1 && a == 0x8010)
                    read_seen = 1;
                memcpy(&want, r, 8);
            }
            CHECK(n == 0);
            CHECK(ym_seen && read_seen);
            CHECK(want > 0 && want % 2u == 0u);
            fclose(f);
        }
    }
}

/* ---- the FM chip and the PSG on the machine's clock */
static void fm(uint32_t part, uint32_t r, uint32_t v)
{
    wb(Z80_RAM + 0x4000u + part * 2u, r);
    z_run(3000u);
    wb(Z80_RAM + 0x4001u + part * 2u, v);
    z_run(3000u);
}

static void test_z80_chips(void)
{
    const PcZ80Counts *c = pc_z80_counts();

    /* One FM sample each 1,008 master clocks, whatever else runs. */
    {
        uint64_t n;
        reset_all();
        fm_samples = 0;
        fm_bad_gaps = 0;
        z_run(1008u * 1000u);
        n = fm_samples;
        CHECK(n >= 999u && n <= 1001u);
        CHECK(fm_bad_gaps == 0);
    }

    /* The chip's DAC reaches the sum, and the reset line clears it. */
    {
        int32_t quiet, loud;
        reset_all();
        wb(Z80_BUSREQ, 1);
        z_run(1008u * 10u);
        quiet = fm_left;
        fm(0, 0x2Bu, 0x80u);
        fm(1, 0xB6u, 0xC0u);
        fm(0, 0x2Au, 0xFFu);
        z_run(1008u * 10u);
        loud = fm_left;
        CHECK(loud > quiet + 300);
        CHECK(fm_left == fm_right);
        wb(Z80_RESET, 1);
        z_run(1008u * 4u);
        wb(Z80_RESET, 0);
        z_run(1008u * 10u);
        CHECK(fm_left < loud - 300);
    }

    /* A status read of the chip reaches the Z80. LD A,(4000h); LD (0100h),A; JR $ */
    {
        static const uint8_t code[] = {0x3A, 0x00, 0x40, 0x32, 0x00, 0x01, 0x18, 0xFE};
        uint64_t reads;
        reset_all();
        reads = c->ym_reads;
        wb(Z80_RAM + 0x100u, 0xEE);     /* no grant yet, so this one goes nowhere */
        z80_go(code, sizeof(code));
        z_run(15u * 100u);
        CHECK(c->ym_reads == reads + 1u);
        CHECK(pc_z80_ram()[0x100] == 0);
    }

    /* The PSG: every attenuation lands on the Pocket's level table. Channel 0, tone period 40h: it flips each 40h x 16 chip clocks of 15 master clocks. */
    {
        static const int32_t levels[16] = {1200, 926, 746, 582, 458, 348, 274, 208, 158, 115, 86, 61, 40, 22, 10, 0};
        int i;
        reset_all();
        pt_port_write(1, 0xC00011u, 0x80u);
        pt_port_write(1, 0xC00011u, 0x04u);
        for (i = 0; i < 16; i++) {
            pt_port_write(1, 0xC00011u, 0x90u | (uint32_t)i);
            psg_highest = 0;
            z_run(15u * 16u * 0x40u * 4u);
            if (psg_highest != levels[i])
                printf("PSG attenuation %d: highest level %d\n", i, (int)psg_highest);
            CHECK(psg_highest == levels[i]);
        }
        /* The flip's time, at full level. */
        pt_port_write(1, 0xC00011u, 0x90u);
        z_run(15u * 16u * 0x40u * 3u);
        psg_changes = 0;
        z_run(15u * 16u * 0x40u * 10u);
        CHECK(psg_changes >= 9u && psg_changes <= 11u);
        printf("PSG flips: %llu, the last gap %llu\n", (unsigned long long)psg_changes, (unsigned long long)psg_last_gap);
        CHECK(psg_last_gap == 15u * 16u * 0x40u);
        /* The Z80 reaches it too, at the same price as the window. LD A,9Fh; LD (7F11h),A; JR $ */
        {
            static const uint8_t code[] = {0x3E, 0x9F, 0x32, 0x11, 0x7F, 0x18, 0xFE};
            uint64_t writes = c->psg_writes, waits = c->wait_clocks, t0;
            z80_go(code, sizeof(code));
            t0 = pc_clock_now();
            z_run(15u * 100u);
            CHECK(c->psg_writes == writes + 1u);
            CHECK(c->wait_clocks >= waits + 2u);
            CHECK(pc_clock_now() - t0 == 15u * 100u + PC_Z80_BUS_SHARE_MCLK);
            z_run(15u * 16u * 0x40u * 2u);
            CHECK(psg_level == 0);
            writes = psg_changes;
            z_run(15u * 16u * 0x40u * 4u);
            CHECK(psg_changes == writes);       /* silent: no more flips */
        }
    }
}

/* ---- what time the Z80 may run to */
#define CUE_ADDRESS 0x40u

static void handler_cues(void)
{
    wb(Z80_BUSREQ, 1);
    wb(Z80_RAM + CUE_ADDRESS, 0xF1);
    wb(Z80_BUSREQ, 0);
}

static uint64_t handler_sound_plain, handler_sound_rest, handler_sound_again, handler_access;
static uint32_t handler_at_after, handler_chip_writes;

static void handler_reads_the_time(void)
{
    pt_at = 0u;
    handler_sound_plain = pc_sound_time();
    pt_at = 10u;
    handler_sound_rest = pc_sound_time();
    pc_z80_run_to(handler_sound_rest);
    handler_at_after = pt_at;
    handler_chip_writes = (uint32_t)pc_z80_counts()->chip_writes;
    handler_sound_again = pc_sound_time();      /* the Z80's write above moved the clock by its share */
    handler_access = pc_access_time();          /* this one uses up the remainder */
    pt_at = 0u;
}

static void test_z80_time(void)
{
    uint64_t raised;
    /* In the main program with nothing due: the clock. */
    reset_all();
    pt_clock = 500u;
    CHECK(pc_sound_time() == pc_clock_now());
    /* An interrupt the program has masked holds nothing back. An interrupt it has not masked holds the time at its raise until it is taken. */
    order_scene(6);
    pt_clock += 100u;
    CHECK(pc_sound_time() == pc_clock_now());
    order_scene(3);
    raised = line_t(0, 224, PC_VDP_VINT_HCLOCK);
    pt_clock += 100u;
    CHECK(pc_clock_now() > raised + 300u);
    CHECK(pc_sound_time() == raised);
    /* A caller cannot break that: a time asked for past the raise stops at the raise. LD HL,0100h 10; INC (HL) 11; JP 10. */
    {
        static const uint8_t code[] = {0x21, 0x00, 0x01, 0x34, 0xC3, 0x03, 0x00};
        uint32_t held;
        reset_all();
        handler_work = handler_cues;
        pc_ctx.imask = 3;
        pc_ctx.a[7] = SCENE_STACK;
        reg(1, 0x64);
        reg(12, 0x81);
        reg(15, 2);
        z80_go(code, sizeof(code));
        go_to(line_t(0, 224, PC_VDP_VINT_HCLOCK - 100u));
        pt_clock += 40u;
        (void)pt_port_tick();
        CHECK(pt_irq_level == 6);
        pt_clock += 2000u;
        pc_z80_run_to(pc_clock_now());
        held = pc_z80_ram()[0x100];
        pt_clock += 2000u;
        pc_z80_run_to(pc_clock_now());
        CHECK(pc_z80_ram()[0x100] == held);
    }
    /* In a handler: the effective time, less the block's remainder, and the remainder is not used up, not by the time and not by the Z80's write to the video chip. LD A,F1h; LD (7F05h),A; JR $ */
    {
        static const uint8_t code[] = {0x3E, 0xF1, 0x32, 0x05, 0x7F, 0x18, 0xFE};
        uint64_t writes;
        reset_all();
        pc_ctx.imask = 3;
        pc_ctx.a[7] = SCENE_STACK;
        reg(1, 0x64);
        reg(12, 0x81);
        reg(15, 2);
        go_to(line_t(0, 224, PC_VDP_VINT_HCLOCK) - 688u);   /* past the line's start, so no line brings the Z80 up before the handler does */
        handler_work = handler_reads_the_time;
        writes = pc_z80_counts()->chip_writes;
        z80_go(code, sizeof(code));
        pt_clock += 100u;
        (void)pt_port_tick();
        CHECK(pt_irq_level == 6);
        next_safe_point();
        CHECK(handler_entries == 1);
        CHECK(handler_sound_plain - handler_sound_rest >= 65u && handler_sound_plain - handler_sound_rest <= 75u);
        CHECK(handler_at_after == 10u);
        CHECK(handler_chip_writes == writes + 1u);
        CHECK(handler_access == handler_sound_again);
    }
}

/* ---- the Z80 against a late handler. The translated program can take an interrupt late, at a safe point, and the handler then works at the earlier effective time. A write of the handler's that the
 * Z80 polls for must reach the Z80 at the chip's order, whatever the lateness. */
#define ORDER_SEEDS 30u

static uint32_t order_rand(uint32_t *state)
{
    *state = *state * 1103515245u + 12345u;
    return *state >> 16;
}

/* The Z80 counts its passes until the cue comes. LD HL,0100h 10; INC (HL) 11; LD A,(0040h) 13; OR A 4; JP Z 10; JR $. A pass is 38 clocks. */
static const uint8_t polling[] = {0x21, 0x00, 0x01, 0x34, 0x3A, 0x40, 0x00, 0xB7, 0xCA, 0x03, 0x00, 0x18, 0xFE};

static void order_setup(void)
{
    reset_all();
    handler_work = handler_cues;
    pc_ctx.imask = 3;
    pc_ctx.a[7] = SCENE_STACK;
    reg(1, 0x64);
    reg(12, 0x81);
    reg(15, 2);
    z80_go(polling, sizeof(polling));
    go_to(line_t(0, 224, PC_VDP_VINT_HCLOCK - 100u));
    pt_clock += 40u;
    (void)pt_port_tick();
    CHECK(pt_irq_level == 6);
    CHECK(handler_entries == 0);
}

static uint32_t passes_with_lateness(uint32_t cpu_clocks)
{
    order_setup();
    pt_clock += cpu_clocks;
    (void)pt_port_read(2, CTRL);    /* a status read, which brings the Z80 up as far as it may go */
    next_safe_point();
    CHECK(handler_entries == 1);
    z_run(PC_MCLK_LINE * 4u);
    return (uint32_t)pc_z80_ram()[0x100] | (uint32_t)pc_z80_ram()[0x40] << 8;
}

static void test_z80_order(void)
{
    uint32_t state = 12345u, i, lost = 0, base;
    test_z80_time();
    base = passes_with_lateness(0u);
    CHECK((base >> 8) == 0xF1u);
    for (i = 0; i < ORDER_SEEDS; i++) {
        uint32_t late = 100u + order_rand(&state) % 1000u;
        if (passes_with_lateness(late) != base)
            lost++;
    }
    CHECK(lost == 0);
    printf("the late handler's cue: order lost on %u of %u seeds\n", (unsigned)lost, (unsigned)ORDER_SEEDS);

    /* The main program holds the bus with an interrupt due. The handler goes first, so its release cannot land inside the main program's hold, whatever the safe point's place. */
    lost = 0;
    for (i = 0; i < ORDER_SEEDS; i++) {
        uint32_t late = order_rand(&state) % 400u;
        order_setup();
        pt_clock += late;
        wb(Z80_BUSREQ, 1);
        next_safe_point();
        CHECK(handler_entries == 1);
        wb(Z80_RAM + 0x60u, 0x5Au);
        wb(Z80_BUSREQ, 0);
        if (pc_z80_ram()[0x60] != 0x5A)
            lost++;
    }
    CHECK(lost == 0);
    printf("the main program's hold with a handler due: order lost on %u of %u seeds\n", (unsigned)lost, (unsigned)ORDER_SEEDS);
}
