/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PC program's start-up and its run loop.
 *
 * TO PLAY: run the program with nothing on its command line, or with the game file's path. It reads paprium_pc.cfg beside it (see pc_config.h), checks the game file against the
 * hash it was built for, loads paprium.sav from beside it, opens the window and paces the game at 59.92 Hz from the PC's timer, whatever the monitor does. The key list goes to the console.
 * F4, by default, opens the controls screen over the paused game (pc_controls.h): every key and pad input, and each player's pad. It saves into paprium_pc.cfg when it closes.
 * F6, by default, changes the picture filter (pc_filter.h); that key is on the controls screen's Program keys page with the others.
 * The menu bar at the top of the window (pc_menu.h) holds every option of those keys, each named with its key, the Scum! settings (pc_scum.h), the
 * console settings, Soft Reset and Console Reset. F10, by default, opens it. Soft Reset ends this run and starts the program again as it was started:
 * a new power-on of the whole machine, the cart's chip too, with the save and the settings as they are now. Console Reset is the console's own reset
 * button, as the Pocket core's 0.3.0 release gives it (console_reset below): the 68000 starts again, the Z80 and the FM chip wait until the game
 * starts them again, and everything else runs on.
 *
 * TESTS AND TOOLS: with --run-dir or --headless the program opens no window and does not start SDL at all: pads from a script or a route program, N frames, logs to a folder. With
 * --hidden-window it goes through all of SDL's path but the showing of the window, so an automated run can test it on a desktop someone else is using. No wall clock enters a log,
 * so the same pads give byte-equal logs.
 *
 * Usage: pc --rom FILE --run-dir DIR --frames N [--image-check HEX] [--pad FILE] [--route DLL] [--route-arg TEXT] [--save FILE] [--save-raw FILE] [--save-out FILE]
 *           [--fw-options HEX] [--fw-step N] [--fw-no-bank] [--three-button PORT] [--dump-frames FILE] [--dump-range FIRST-LAST] [--shot FRAME]... [--render]
 *           [--fallback-log FILE] [--no-limit] [--flip-limits N] [--shot-every N] [--shot-free] [--no-logs] [--paced] [--log-word HEXADDR]... [--config FILE]
 *           [--hidden-window] [--key-script FILE] [--window-shot FRAME]... [--sound-file FILE] [--sound-log FILE]
 *           [--game-dll FILE] [--interpreter] [--from-launcher]      (the players' build only, see pc_game_dll.h)
 *           [--console NAME=VALUE]...
 *
 * --pad          lines "first last mask": frames and the pad bits of pc_route.h, for port 1
 * --route        a route program, see pc_route.h. It replaces --pad
 * --save         the cart's save, in the Pocket's .sav layout
 * --save-raw     the cart's save, in the firmware's own byte order
 * --save-out     writes the cart's save here at the end, in the Pocket's layout
 * --dump-frames  writes each frame in --dump-range as a PcFrame record, for the renderer's tests
 * --shot         a picture of that frame from the renderer, as a PPM file in the run folder
 * --render       runs the renderer on every frame and adds its picture's hash to the log
 * --no-limit    the renderer starts with no sprite limits
 * --flip-limits  flips the sprite limits every N frames, as a player's key does
 * --shot-every   a picture of every Nth frame, as --shot does
 * --shot-free    each picture is also written as drawn with no sprite limits, as shot_N_free.ppm
 * --no-logs      keeps no frame log, for a speed test: hashing the work RAM and the picture every frame costs more than most of what is being measured
 * --paced        paces a headless run at 59.92 Hz, as the window does
 * --game-dll     the players' build: the game DLL the launcher built, by default my_copy\pc_game.dll beside the program. Any other build has the game linked in
 * --interpreter  the players' build: every instruction runs in the fallback and no game DLL is loaded. It plays the same game, but its timing model
 *                is finer than the translated code's, so its logs are not byte-equal to the translated build's. The launcher uses it when the game
 *                cannot be built or its DLL cannot be loaded, and tells the player so
 * --from-launcher  the players' build: a DLL that cannot be loaded is reported on the console only (exit 6), as the launcher tells the player
 *                itself and then starts the game again with --interpreter. A Soft Reset is left to the launcher too (exit 8)
 * --log-word     adds a word of work RAM to each line of frames.txt. The address stays on the command line
 * --hidden-window  a window that is never shown, through SDL's real path
 * --key-script   lines "frame down|up KEY": key presses, by SDL's name for the key, pushed into SDL's event queue at those frames. Each pass of a pause counts as a frame here.
 *                Lines "frame move X,Y" and "frame click X,Y" move the mouse there, or press its left button there, in the window's pixels
 * --window-shot  what the window's back buffer held at that frame, as win_N.ppm, and for the 4:3 shape its whole-number enlargement as large_N.ppm
 * --pause-shot   the same during a pause or the controls screen, at that count of the key script's clock, as pause_N.ppm. Up to 16 of these
 * --controls-db  a test: the controller database to read, in place of gamecontrollerdb.txt beside the program (see pc_input.c)
 * --menu-dump    the menu bar's menus and items as they stand at the end of the run, on the console: title, label, key, what it does, tick and grey
 * --sound-file   the sound as 48 kHz stereo in a WAV file. See pc_sound.h
 * --sound-log    every write to the sound chips and every Z80 read of the cart, in a file. See pc_sound.h
 * --sound-device  which audio device, by the names in pc_audio.h, or none. A headless run has none unless asked, and a hidden window has SDL's dummy driver
 * --sound-no-hold  the output sampled at 48 kHz in place of the Pocket's 24 kHz. See pc_mix.h
 * --test-restart FRAME  a test: the 68000 restarts at that frame's end, as after the cart's hold, with no hold
 * --data-patch HEXADDR:HEXBYTES  a test: changes a data word of the program after the image check, as a route that picks a stage does. The address stays on the command line
 * --cheats LIST  the cheats= line of the configuration, for a run that reads none (pc_scum.h): off, or health, pills, lives with commas between
 * --arcade-stage N  the arcade_stage= line of the configuration, for a run that reads none: off or 1 to 25
 * --arcade-mode NAME, --arcade-coins NAME, --attract-only on|off, --three-players on|off  the configuration's lines of those names, for a run that
 *                reads none (pc_scum.h)
 * --console-reset FRAME  a test: the console's reset button at that frame's end, as the Game menu's Console Reset. Up to 16 of these
 * --io-log FILE  the ports' log (pc_io.h): every access to the pads' ports, with its time
 * --mode-log FILE  the video chip's picture size frame by frame: a line when the 320 or 256 pixel mode or the 224 or 240 line mode at a frame's start
 *                changes, and for each frame where a write changes either during the picture; the totals at the end
 * --stall-log FILE  the freeze log goes here (see pc_stall.h). Without this it is stalls.txt in the run folder when there is one, beside the program when not
 * --no-stall-log  no freeze log and no second thread: a test that the log changes nothing in the game
 * --stall-limit MS  a test: a pass longer than this gets a line, in place of 100 ms
 * --test-renderer-fail NAMES  a test: these renderers, with commas between, act as if they will not start. See pc_window.c. The name vsyncslow makes each switch of vsync take 300 ms in
 *                the renderer, as a switch of Direct3D 9's did on one PC. The name vsync makes the renderer refuse to switch vsync
 * --test-window WxH  a test: the window's size in place of the configuration's scale, as a hidden window cannot go full screen
 * --test-save-change FRAME  a test: the last byte of the cart's save, outside the places pc_save.c checks, changes at that frame, as when the game saves, so the save's path runs on a made-up program. Up to 8 of these: a second one
 *                puts the byte back
 * --test-stall FRAME[-LAST]:PART:MS  a test: the main thread sleeps at that frame, or on each frame from FRAME to LAST, in that part, as the log names it. Up to 256 of these. A sleep in the
 *                present stands in for a slow monitor or a GPU waking up. Nothing in the logs may change. A frame the pace leaves unshown has no present, so a sleep planted there does not happen
 * --console      one of the console's lines of paprium_pc.cfg (region, console, x32, megacd, cpumate, cpumate_brain: see pc_console.h), after the file's own, so a
 *                headless run can set them too
 * --test-brain FOLDER  a test: a made-up CPU MATE brain of the test's own, brain.bin, cf.bin and rodata.bin in FOLDER, taken without the hashes of the real one
 * --cpumate-trace FILE  a test: what the CPU MATE brain is given and gives, call by call, as pc_addons_trace writes it
 * --test-monitor HZ  a test: while vsync is on, each present first waits for the next refresh of a made-up monitor at HZ, as a shown window's present waits for a real one. A hidden window
 *                never meets its monitor's refresh, so this is how an automated run sees what a monitor slower or faster than the game does to the pace and the sound
 *
 * In the run folder: frames.txt, one line a frame: the frame, a hash of work RAM, the master clock, the first and last V counter value the program read and how many, the watched
 * blocks' first lines and counts when the translation has any, and the device counts of the frame. cart.txt, one line a frame from the firmware. report.txt, the totals.
 *
 * Exit 0 with RESULT: PASS, or with RESULT: FAIL and a reason: the program stopped, or the firmware reached a fatal halt.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_rt.h"
#include "pc_addons.h"
#include "pc_brain.h"
#include "pc_cart.h"
#include "pc_clock.h"
#include "pc_config.h"
#include "pc_controls.h"
#include "pc_fiber.h"
#include "pc_filter.h"
#include "pc_frame.h"
#if defined(PT_GAME_FROM_DLL)
#include "pc_game_dll.h"
#endif
#include "pc_io.h"
#include "pc_menu.h"
#include "pc_pace.h"
#include "pc_route.h"
#include "pc_save.h"
#include "pc_scum.h"
#include "pc_stall.h"
#include "pc_audio.h"
#include "pc_mix.h"
#include "pc_sound.h"
#include "pc_vdp.h"
#include "pc_window.h"
#include "pc_z80.h"
#include "pr_local.h"

PtCtx pc_ctx;

extern const char *pt_fallback_log_path;

/* Written by the build, in the output folder: what this program was translated from. */
extern const char pc_built_rom_sha256[];
extern const char pc_built_image_check[];

static FILE *f_frames, *f_cart;
static int frames_wanted;
static uint32_t frames_done;
static int run_done;
static int test_restart_frame = -1, test_restart_now;
static const char *data_patches[16];
static int data_patches_n;
static const char *run_dir;
static PcFrame *frame_rec;
static long long host_freq = 1;
static int use_window, window_hidden;
static volatile LONG quit_asked;
static int no_logs;

#define LOG_WORDS_MAX 8
static uint32_t log_words[LOG_WORDS_MAX];
static int log_words_n;

/* ---- blocks the local configuration asked to hear about */
#define WATCH_MAX 16
static uint32_t watch_count[WATCH_MAX];
static int32_t watch_line[WATCH_MAX];
static int watch_n;

void pt_watch_hit(int index)
{
    if (index >= WATCH_MAX)
        return;
    if (!watch_count[index]++)
        watch_line[index] = (int32_t)pc_time_vline(pc_clock_now());
    if (index >= watch_n)
        watch_n = index + 1;
}

/* ---- pads */
typedef struct PadRow { int first, last; unsigned mask; } PadRow;
static PadRow pad_rows[4096];
static int pad_n;
static PcRouteFrameFn route_frame;
static PcRoutePad3Fn route_pad3;
static PcRouteIO route_io;
static FILE *route_log;

static uint32_t pad_of_route(uint32_t b)
{
    static const struct { uint32_t route, pad; } map[12] = {
        {PC_ROUTE_B, PC_PAD_B}, {PC_ROUTE_A, PC_PAD_A}, {PC_ROUTE_MODE, PC_PAD_MODE}, {PC_ROUTE_START, PC_PAD_START}, {PC_ROUTE_UP, PC_PAD_UP}, {PC_ROUTE_DOWN, PC_PAD_DOWN},
        {PC_ROUTE_LEFT, PC_PAD_LEFT}, {PC_ROUTE_RIGHT, PC_PAD_RIGHT}, {PC_ROUTE_C, PC_PAD_C}, {PC_ROUTE_Y, PC_PAD_Y}, {PC_ROUTE_X, PC_PAD_X}, {PC_ROUTE_Z, PC_PAD_Z}
    };
    uint32_t out = 0;
    int i;
    for (i = 0; i < 12; i++)
        if (b & map[i].route)
            out |= map[i].pad;
    return out;
}

static void load_pad(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "no pad script %s\n", path);
        exit(2);
    }
    while (pad_n < 4096 && fscanf(f, "%d %d %x", &pad_rows[pad_n].first, &pad_rows[pad_n].last, &pad_rows[pad_n].mask) == 3)
        pad_n++;
    fclose(f);
}

static void load_route(const char *path, const char *argument)
{
    HMODULE m = LoadLibraryA(path);
    PcRouteInitFn init;
    if (!m) {
        fprintf(stderr, "cannot load route %s (%lu)\n", path, (unsigned long)GetLastError());
        exit(2);
    }
    init = (PcRouteInitFn)GetProcAddress(m, "route_init");
    route_frame = (PcRouteFrameFn)GetProcAddress(m, "route_frame");
    route_pad3 = (PcRoutePad3Fn)GetProcAddress(m, "route_pad3");
    if (!route_frame) {
        fprintf(stderr, "no route_frame in %s\n", path);
        exit(2);
    }
    if (init && init(argument ? argument : "")) {
        fprintf(stderr, "the route refused its argument\n");
        exit(2);
    }
}

static void route_log_frame(const PcRouteIO *io, uint32_t pad3)
{
    static unsigned last[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    int k;
    if (!route_log)
        return;
    if (io->pad[0] != last[0] || io->pad[1] != last[1] || (route_pad3 && pad3 != last[2])) {
        if (route_pad3)
            fprintf(route_log, "%d pad %03X %03X %03X\n", io->frame, io->pad[0], io->pad[1], pad3);
        else
            fprintf(route_log, "%d pad %03X %03X\n", io->frame, io->pad[0], io->pad[1]);
        last[0] = io->pad[0];
        last[1] = io->pad[1];
        last[2] = pad3;
    }
    for (k = 0; k < io->n_pokes; k++)
        fprintf(route_log, "%d poke %06X %u %X\n", io->frame, (unsigned)io->pokes[k].address, (unsigned)io->pokes[k].size, (unsigned)io->pokes[k].value);
    if (io->note[0])
        fprintf(route_log, "%d note %s\n", io->frame, io->note);
}

static int shots[256], shots_n;
static int shot_next;
static int shot_every, shot_free;
static uint32_t input_pad[3];

/* The pads, and on test routes the changes to work RAM, for the frame about to run. Player 3 is plugged in while a route gives a third pad, or in a
   window while the controls give player 3 a pad or a key (pc_controls.h). */
static void pads_for_frame(int frame)
{
    pc_io_log_time(pc_clock_now());
    pc_io_set_present(2, route_pad3 != NULL || (use_window && pc_input_player_present(2)));
    if (route_frame) {
        PcRouteIO *io = &route_io;
        uint32_t pad3;
        int k;
        io->frame = frame;
        io->ram = pt_ram;
        io->rom = pt_rom;
        io->rom_size = pt_rom_size;
        io->pad[0] = io->pad[1] = 0;
        io->n_pokes = 0;
        io->shot = io->end = 0;
        io->note[0] = 0;
        route_frame(io);
        pad3 = route_pad3 ? route_pad3() : 0u;
        pc_io_set_pad(0, pad_of_route(io->pad[0]) | input_pad[0]);
        pc_io_set_pad(1, pad_of_route(io->pad[1]) | input_pad[1]);
        pc_io_set_pad(2, pad_of_route(pad3) | input_pad[2]);
        route_log_frame(io, pad3);
        for (k = 0; k < io->n_pokes; k++) {
            uint32_t a = io->pokes[k].address & 0xFFFFu;
            if (io->pokes[k].size == 1)
                pt_mem_w8(pt_ram, a, io->pokes[k].value);
            else
                pt_mem_w16(pt_ram, a & 0xFFFEu, io->pokes[k].value);
        }
        shot_next = io->shot;
        if (io->end)
            frames_wanted = frame + 1;
    } else {
        unsigned m = 0;
        int i;
        for (i = 0; i < pad_n; i++)
            if (frame >= pad_rows[i].first && frame <= pad_rows[i].last)
                m |= pad_rows[i].mask;
        pc_io_set_pad(0, pad_of_route(m) | input_pad[0]);
        pc_io_set_pad(1, input_pad[1]);
        pc_io_set_pad(2, input_pad[2]);
    }
}

/* ---- hashes */
#define FNV0 0xCBF29CE484222325ull
static uint64_t fnv(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

/* ---- pictures */
static PcPicture *picture, *picture_free;
static int render_all;
static FILE *f_dump;
static int dump_first, dump_last = -1;
static PrOptions render_options;
static PrMemory render_memory;     /* the chip's sprite walk carries this from frame to frame; cleared only when the machine is reset */
static int flip_limits;
static int window_shots[64], window_shots_n;

static void write_ppm(const char *path, const uint32_t *pixels, unsigned stride, unsigned width, unsigned height)
{
    FILE *f = fopen(path, "wb");
    unsigned x, y;
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint32_t c = pixels[y * stride + x];
            uint8_t rgb[3];
            rgb[0] = (uint8_t)(c >> 16);
            rgb[1] = (uint8_t)(c >> 8);
            rgb[2] = (uint8_t)c;
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void write_shot(uint32_t frame)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/shot_%06u.ppm", run_dir, (unsigned)frame);
    write_ppm(path, &picture->rgb[0][0], PC_PICTURE_W, picture->width, picture->height);
    if (shot_free) {
        static const PrOptions none = {PR_LIMITS_NONE, 0};
        pr_render(frame_rec, &none, picture_free);
        snprintf(path, sizeof(path), "%s/shot_%06u_free.ppm", run_dir, (unsigned)frame);
        write_ppm(path, &picture_free->rgb[0][0], PC_PICTURE_W, picture_free->width, picture_free->height);
    }
}

/* ---- host time, for the speed figures on the program's standard output. None of this enters a log. */
#define HOST_BUCKETS 4096
typedef struct HostStat { uint64_t total, worst, count; uint32_t buckets[HOST_BUCKETS]; } HostStat;  /* buckets of 10 microseconds */
static HostStat stat_work, stat_render, stat_present, stat_wait;
static uint64_t host_mark;

static void stat_add(HostStat *s, uint64_t ticks)
{
    uint64_t us = ticks * 1000000u / (uint64_t)host_freq;   /* not a game address: microseconds in a second */
    uint64_t b = us / 10u;
    s->total += us;
    s->count++;
    if (us > s->worst)
        s->worst = us;
    s->buckets[b < HOST_BUCKETS ? b : HOST_BUCKETS - 1]++;
}

static uint64_t stat_part(const HostStat *s, double part)
{
    uint64_t seen = 0, want = (uint64_t)((double)s->count * part);
    int i;
    for (i = 0; i < HOST_BUCKETS; i++) {
        seen += s->buckets[i];
        if (seen > want)
            return (uint64_t)i * 10u;
    }
    return s->worst;
}

static void stat_print(const char *name, const HostStat *s)
{
    if (s->count)
        printf("  %-8s mean %llu, half under %llu, 99 %% under %llu, 99.9 %% under %llu, worst %llu\n", name, (unsigned long long)(s->total / s->count), (unsigned long long)stat_part(s, 0.5),
               (unsigned long long)stat_part(s, 0.99), (unsigned long long)stat_part(s, 0.999), (unsigned long long)s->worst);
}

/* ---- pacing: 53,693,175 / (3,420 x 262) frames a second, from the PC's timer. The game never follows the monitor: pc_pace.h has the rules, how a late frame goes unshown so the game
   catches up by running its frames, and how a stall's time is dropped with only its last three frames made up, at once and unshown but the last, so the sound's queue (pc_audio.c) is full
   again within the frames' own work. */
static int paced;
static uint64_t pace_period;
static PcPace pace;
static PcVsyncWatch vsync_watch;
static int vsync_watched;           /* vsync was asked for and the renderer started with it */
static uint64_t last_present_ticks, last_pace_point;
static int last_shown = 1;          /* the last frame was shown */
static int last_behind;             /* the last frame's pace point was past its due time: the next frame begins behind the pace */
static double test_monitor_hz;      /* a test: each present with vsync on waits for the next refresh of a made-up monitor at this rate */
#define VSYNC_WATCH_FROM_FRAME 60   /* the first presents of a run set up the window, and say nothing about the monitor */

static void title_update(void);

static void pace_reset(void)
{
    pc_pace_start(&pace, pc_fiber_host_time(), pace_period);
    last_pace_point = 0;
    last_shown = 1;
    last_behind = 0;
    pc_vsync_watch_again(&vsync_watch);
}

static long long ms_of_ticks(int64_t ticks) { return (long long)(ticks * 1000 / host_freq); }

/* A switch of vsync that takes longer than this in the renderer ends the returns for the run (pc_pace.h). In hidden test runs on one PC a switch took up to 15 ms on OpenGL and Direct3D 9,
   but some Direct3D 9 switches took 0.7 to 2.1 s, each a stop of the game and its sound. */
#define VSYNC_SWITCH_SLOW_MS 250

/* pc_pace.h's rule said to switch: do it, and say why, in the freeze log and on the console. */
static void vsync_switch(int action)
{
    uint64_t start = pc_fiber_host_time();
    int on = action == PC_VSYNC_TURN_ON, failed, slow;
    long long took;
    char note[PC_STALL_NOTE_CHARS], again[200], after[160];
    pc_stall_part(PC_STALL_VSYNC_SWITCH);
    failed = pc_window_set_vsync(on) != 0;
    pc_stall_part(PC_STALL_PACE);
    took = ms_of_ticks((int64_t)(pc_fiber_host_time() - start));
    slow = !failed && took > VSYNC_SWITCH_SLOW_MS;
    if (slow)
        pc_vsync_watch_no_returns(&vsync_watch);
    if (failed) {
        /* The rule thinks it switched. Rather than let it think so and ask again every second, this run stops watching: vsync stays as it is. */
        vsync_watched = 0;
        snprintf(note, sizeof(note), "vsync could not be switched %s: the renderer refused, so it stays %s, and this run does not try again", on ? "back on" : "off", on ? "off" : "on");
    } else if (!on) {
        /* Why now, then what follows. Only the second part says whether vsync comes back, so the two never disagree. */
        if (vsync_watch.at_once)
            snprintf(again, sizeof(again), "it had come back on only a second before, and the game fell behind again at once");
        else if (vsync_watch.soon)
            snprintf(again, sizeof(again), "it had come back on only %d seconds before, as on a monitor slower than the game, or with the PC slow again so soon", vsync_watch.on_for / 60);
        else
            snprintf(again, sizeof(again), "%s", vsync_watch.offs > 1 ? "it had held for more than a minute since it last came back on" : "the first time in this run");
        if (slow)
            snprintf(after, sizeof(after), "this switch took over %d ms, so it stays off for the rest of the run", VSYNC_SWITCH_SLOW_MS);
        else if (vsync_watch.no_returns && vsync_watch.at_once && vsync_watch.slow_monitor)
            snprintf(after, sizeof(after), "this monitor's refresh, %.0f Hz, is slower than the game's 59.92, so it stays off for the rest of the run", (double)host_freq / (double)vsync_watch.refresh);
        else if (vsync_watch.no_returns)
            snprintf(after, sizeof(after), "an earlier switch took over %d ms, so it stays off for the rest of the run", VSYNC_SWITCH_SLOW_MS);
        else if (vsync_watch.soon)
            snprintf(after, sizeof(after), "so its wait doubles, up to about 5 minutes: it comes back on after %d seconds of steady frames", vsync_watch.calm_needed / 60);
        else
            snprintf(after, sizeof(after), "it comes back on after %d seconds of steady frames", vsync_watch.calm_needed / 60);
        snprintf(note, sizeof(note), "vsync switched off, which took %lld ms: for a second the game stayed behind its pace with vsync on, losing %lld ms more than it made up, no frame counting for more than its present; "
                 "the longest present then took %lld ms, the longest frame without it %lld ms; %s; %s",
                 took, ms_of_ticks(vsync_watch.why_loss), ms_of_ticks(vsync_watch.why_present), ms_of_ticks(vsync_watch.why_rest), again, after);
    } else {
        snprintf(note, sizeof(note), "vsync switched back on, which took %lld ms: %d seconds of steady frames with short presents; it has gone off %d times in this run%s", took,
                 vsync_watch.calm_needed / 60, vsync_watch.offs, slow ? "; this switch took so long that once vsync goes off again, it stays off" : "");
    }
    pc_stall_note(note);
    printf("At frame %u: %s.\n", (unsigned)(frames_done - 1u), note);
    title_update();
}

/* The monitor's refresh, for the vsync rule: a test's made-up monitor, or the one that holds the window, looked at again once a second, as the window can be moved to another. */
static void vsync_refresh_update(void)
{
    double hz = test_monitor_hz > 0 ? test_monitor_hz : (double)pc_window_refresh_hz();
    pc_vsync_watch_refresh(&vsync_watch, hz > 0 ? (int64_t)((double)host_freq / hz + 0.5) : 0);
}

static void pace_wait(void)
{
    uint64_t now = pc_fiber_host_time();
    uint64_t point = pc_pace_point(&pace, now);     /* when this frame's wait ends */
    if (vsync_watched && frames_done % 60u == 0)
        vsync_refresh_update();
    if (vsync_watched && frames_done >= VSYNC_WATCH_FROM_FRAME && last_pace_point) {
        int action = pc_vsync_watch(&vsync_watch, pc_window_vsync_on(), (int64_t)(point - last_pace_point), last_shown ? (int64_t)last_present_ticks : 0, (int64_t)pace_period, last_shown,
                                    last_behind);
        if (action != PC_VSYNC_KEEP)
            vsync_switch(action);
    }
    last_pace_point = point;
    last_behind = pace.behind > 0;
    pc_audio_pace((uint32_t)(pace.behind * (uint64_t)PC_AUDIO_RATE / host_freq));     /* the sound's queue keeps its extra until all is calm again (pc_audio.h) */
    for (;;) {
        int64_t left;
        now = pc_fiber_host_time();
        left = (int64_t)(point - now);
        if (left <= 0)
            break;
        /* Sleep while there is plenty of time, and spin only for the last moment. */
        if (left * 1000 > host_freq * 3)
            Sleep(1);
    }
}

/* A test: the present waits for the next refresh of a monitor at test_monitor_hz, counted from the timer's zero, as a present with vsync on waits for a real one. */
static void test_monitor_wait(void)
{
    uint64_t refresh = (uint64_t)((double)host_freq / test_monitor_hz + 0.5), now = pc_fiber_host_time(), until = (now / refresh + 1u) * refresh;
    for (;;) {
        int64_t left;
        now = pc_fiber_host_time();
        left = (int64_t)(until - now);
        if (left <= 0)
            break;
        if (left * 1000 > host_freq * 3)
            Sleep(1);
    }
}

/* ---- the window's half of a frame */
static int paused;
static FILE *key_script;
static int key_next_frame = -1, key_next_down, key_next_mouse;     /* key_next_mouse: 1 a move, 2 a click */
static char key_next_name[64];

static void key_script_read(void)
{
    char line[200];
    key_next_frame = -1;
    while (key_script && fgets(line, sizeof(line), key_script)) {
        char word[16];
        int consumed = 0, frame;
        size_t n;
        if (sscanf(line, "%d %15s %n", &frame, word, &consumed) < 2 || !consumed)
            continue;
        snprintf(key_next_name, sizeof(key_next_name), "%s", line + consumed);
        n = strlen(key_next_name);
        while (n && (key_next_name[n - 1] == '\n' || key_next_name[n - 1] == '\r' || key_next_name[n - 1] == ' '))
            key_next_name[--n] = 0;
        key_next_down = !strcmp(word, "down");
        key_next_mouse = !strcmp(word, "move") ? 1 : !strcmp(word, "click") ? 2 : 0;
        key_next_frame = frame;
        return;
    }
}

/* The key script's clock is the frame, plus one for each pass of a pause, so a script can end the pause it started. */
static int script_ticks;

static void key_script_due(int now)
{
    while (key_next_frame >= 0 && key_next_frame <= now) {
        int x, y;
        if (key_next_mouse && sscanf(key_next_name, "%d,%d", &x, &y) == 2)
            pc_input_push_mouse(x, y, key_next_mouse == 2);
        else if (pc_input_push_key(key_next_name, key_next_down) != 0)
            printf("the key script names a key SDL does not know: %s\n", key_next_name);
        key_script_read();
    }
}

/* The Aspect Ratio list's names, by PC_SHAPE_* (pc_config.h): the window's title says the shape in use. */
static const char *const shape_titles[PC_SHAPES] = {"4:3", "Square pixels", "Original", "Corrected"};

static void title_update(void)
{
    char text[300];
    /* The title says square pixels in small letters, as it always has. */
    snprintf(text, sizeof(text), "Paprium - sprites: %s - %s - %s - filter %s%s%s", render_options.limits == PR_LIMITS_CART ? "cart limits" : "no limit",
             pc_window_shape() == PC_SHAPE_SQUARE ? "square pixels" : shape_titles[pc_window_shape()], pc_window_renderer(), pc_window_filter_label(),
             paused ? " - PAUSED" : "", pc_window_vsync_dropped() ? " - vsync off" : "");
    pc_window_set_title(text);
    pc_stall_state(pc_window_vsync_on(), paused);
}

/* The controls screen: it pauses the game while it is open, and when it closes the game goes on as it was, with the pads' three or six buttons as the
   screen left them. --three-button keeps a port at three for a test. */
static int controls_paused_it;
static int three_button_arg[3];

static void controls_open(void)
{
    controls_paused_it = !paused;
    if (!paused) {
        paused = 1;
        pc_audio_paused(1);
    }
    pc_controls_screen_show(picture);
}

static void controls_closed(void)
{
    int i;
    for (i = 0; i < 3; i++)
        pc_io_set_three_button(i, three_button_arg[i] || pc_input_three_button(i));
    pc_window_present(picture);       /* the game's own picture again, for a pause that goes on */
    if (controls_paused_it) {
        paused = 0;
        pc_audio_paused(0);
        if (paced)
            pace_reset();
    }
}


/* ---- the menu bar (pc_menu.h). Its menus are filled from the program's state each time the window's events have run, so a tick or a grey item always
   shows what is true now. An item that does what a key does gives that key's action, and window_events carries it out as it does the key's. The
   settings it changes are written into the settings file through the one writer the controls screen uses too (pc_config.h). */
static PcConfig *settings;          /* the settings the run started with, as the menu has changed them since */
static char config_full[1100];      /* the settings file's full path, once the window's path has read it (empty in a headless run) */
static int menu_was_open, menu_paused_it, soft_reset_asked, started_by_launcher;
static int menu_stage_list, menu_renderer_list, menu_console_list, menu_megacd_list, menu_arcade_list, menu_coins_list;
/* The console's reset button: asked for by the Game menu or a test, done in the run loop once the 68000 has stopped (console_reset). console_locks:
   how many times a Console Reset left the console locked up (console_locked). run_stack_top: the C stack where the run loop calls the 68000. */
static int console_reset_asked, console_reset_frames[16], console_reset_frames_n, console_resets_done, console_locks;
static uintptr_t run_stack_top;

static const char *const renderer_names[] = {"opengl", "direct3d", "direct3d11", "direct3d12", "auto"};
static const char *const renderer_titles[] = {"OpenGL", "Direct3D 9", "Direct3D 11", "Direct3D 12", "Auto (SDL's choice)"};

/* The Console menu's two lists, the Pocket core's: the names are the settings file's (pc_console.h), the titles what the menu shows, in the same order. */
static const char *const console_names[] = PC_CONSOLE_NAMES, *const console_titles[] = PC_CONSOLE_TITLES;
static const char *const megacd_names[] = PC_MEGACD_NAMES, *const megacd_titles[] = PC_MEGACD_TITLES;
typedef char console_titles_match[sizeof(console_titles) == sizeof(console_names) && sizeof(console_names) == PC_CONSOLES * sizeof(char *) ? 1 : -1];
typedef char megacd_titles_match[sizeof(megacd_titles) == sizeof(megacd_names) && sizeof(megacd_names) == PC_MEGACDS * sizeof(char *) ? 1 : -1];

/* The Scum! menu's Arcade Mode and Arcade Coins, the Pocket core's names (pc_scum.h), and the settings file's words for them (pc_config.h). */
static const char *const arcade_titles[PC_ARCADE_MODES] = {"Off", "On", "Multiplayer", "Stick Only"};
static const char *const coins_titles[PC_COINS_SETTINGS] = {"1 Coin 1 Credit", "2 Coins 1 Credit", "Inf. Credits (0012)"};
static const char *const arcade_names[PC_ARCADE_MODES] = PC_ARCADE_MODE_NAMES, *const coins_names[PC_COINS_SETTINGS] = PC_ARCADE_COINS_NAMES;
static const char *const shape_names[PC_SHAPES] = PC_SHAPE_NAMES;

/* Where the hardware rules change a choice (pc_console.h), the few plain words that say so, for the menu and its messages: a unit that is a whole
   console, a console with no port for a separate unit, and CPU MATE with no unit the game knows. Each kind is one sentence; out is "" when none
   applies. */
static void console_rule_words(const PcConsole *c, int with_cpumate, char *out, size_t size)
{
    PcMachine m;
    size_t used = 0;
    out[0] = 0;
    pc_console_machine(c, &m);
    if (m.combined)
        used += (size_t)snprintf(out + used, size - used, "The %s is a whole console, so the game finds it (%s). ", megacd_titles[m.unit],
                                 m.japan ? "Japan" : "USA");
    if (m.dropped && used < size)
        used += (size_t)snprintf(out + used, size - used, "The %s has no expansion port, so the %s is off. ", console_titles[m.console],
                                 megacd_titles[c->megacd]);
    if (with_cpumate && c->cpumate && !m.cpumate_unit && used < size)
        used += (size_t)snprintf(out + used, size - used, "CPU MATE does nothing without a Mega CD unit the game knows (any but None and Unknown CD). ");
    if (used > 0 && used < size)
        out[used - 1] = 0;
}

static const char *key_of(int action) { return pc_input_key_name(pc_input_controls()->port_key[action]); }

static void menu_refresh(void)
{
    char label[64];
    int m, k, cheats = (int)pc_scum_cheats(), can_cheat = pc_scum_cheats_available(), gl = !strcmp(pc_window_renderer(), "opengl");
    PcControls *controls = pc_input_controls();
    pc_menu_begin();
    m = pc_menu_add_menu("Game", 1);
    pc_menu_add_item(m, "Pause", key_of(PC_PORT_PAUSE), PC_MENU_DO_ACTION, PC_ACTION_PAUSE, paused && !menu_paused_it ? PC_MENU_TICK : 0);
    pc_menu_add_item(m, "Soft Reset", "", PC_MENU_DO_SOFT_RESET, 0, 0);
    pc_menu_add_item(m, "Console Reset", "", PC_MENU_DO_CONSOLE_RESET, 0, pc_cart_holding() ? PC_MENU_GREY : 0);
    pc_menu_add_item(m, "Quit", key_of(PC_PORT_QUIT), PC_MENU_DO_ACTION, PC_ACTION_QUIT, PC_MENU_LINE);

    m = pc_menu_add_menu("Video", 1);
    for (k = 0; k < PC_LOOKS; k++) {
        snprintf(label, sizeof(label), "Filter: %s", pc_filter_title(k));
        pc_menu_add_item(m, label, "", PC_MENU_DO_FILTER, k, (pc_window_filter() == k ? PC_MENU_TICK : 0) | (pc_filter_needs_gl(k) && !gl ? PC_MENU_GREY : 0));
    }
    pc_menu_add_item(m, "Next filter", key_of(PC_PORT_FILTER), PC_MENU_DO_ACTION, PC_ACTION_FILTER, 0);
    pc_menu_add_item(m, "Full screen", key_of(PC_PORT_FULLSCREEN), PC_MENU_DO_ACTION, PC_ACTION_FULLSCREEN,
                     PC_MENU_LINE | (pc_window_is_fullscreen() ? PC_MENU_TICK : 0));
    /* Aspect Ratio: the Pocket core's Original and Corrected, then the program's own 4:3, the default, and square pixels (pc_window.h). The shape key
       goes through them in this order, as the filter key goes through the filters. */
    for (k = 0; k < PC_SHAPES; k++) {
        static const int order[PC_SHAPES] = {PC_SHAPE_ORIGINAL, PC_SHAPE_CORRECTED, PC_SHAPE_4_3, PC_SHAPE_SQUARE};
        snprintf(label, sizeof(label), "Aspect Ratio: %s", shape_titles[order[k]]);
        pc_menu_add_item(m, label, "", PC_MENU_DO_SHAPE, order[k], (k == 0 ? PC_MENU_LINE : 0) | (pc_window_shape() == order[k] ? PC_MENU_TICK : 0));
    }
    pc_menu_add_item(m, "Next aspect ratio", key_of(PC_PORT_SHAPE), PC_MENU_DO_ACTION, PC_ACTION_SHAPE, 0);
    pc_menu_add_item(m, "The cart's sprite limits", key_of(PC_PORT_LIMITS), PC_MENU_DO_ACTION, PC_ACTION_LIMITS,
                     render_options.limits == PR_LIMITS_CART ? PC_MENU_TICK : 0);
    pc_menu_add_item(m, "Menu bar in a window", "", PC_MENU_DO_MENU_BAR, 0, PC_MENU_LINE | (pc_window_menu_bar() ? PC_MENU_TICK : 0));
    menu_renderer_list = pc_menu_add_menu("Renderer", 0);
    pc_menu_add_item(m, "Renderer, from the next start", "", PC_MENU_DO_NOTHING, menu_renderer_list, PC_MENU_OPENS);
    pc_menu_add_item(m, "Vsync, from the next start", "", PC_MENU_DO_VSYNC, 0, settings->vsync ? PC_MENU_TICK : 0);
    for (k = 0; k < 5; k++)
        pc_menu_add_item(menu_renderer_list, renderer_titles[k], "", PC_MENU_DO_RENDERER, k, !strcmp(settings->renderer, renderer_names[k]) ? PC_MENU_TICK : 0);

    m = pc_menu_add_menu("Sound", 1);
    pc_menu_add_item(m, "Mute", key_of(PC_PORT_MUTE), PC_MENU_DO_ACTION, PC_ACTION_MUTE, pc_audio_muted() ? PC_MENU_TICK : 0);
    for (k = 100; k > 0; k -= 25) {
        snprintf(label, sizeof(label), "Volume %d %%", k);
        pc_menu_add_item(m, label, "", PC_MENU_DO_VOLUME, k, (k == 100 ? PC_MENU_LINE : 0) | (pc_audio_volume() == k ? PC_MENU_TICK : 0));
    }

    m = pc_menu_add_menu("Controls", 1);
    pc_menu_add_item(m, "Controls screen", key_of(PC_PORT_CONTROLS), PC_MENU_DO_ACTION, PC_ACTION_CONTROLS, 0);
    pc_menu_add_item(m, "Menu bar", key_of(PC_PORT_MENU), PC_MENU_DO_NOTHING, 0, 0);

    m = pc_menu_add_menu("Scum!", 1);
    /* Without the built game the cheats cannot act (pc_scum.h): they are grey then, and never ticked, even when the settings file has them on. */
    if (!can_cheat)
        cheats = 0;
    pc_menu_add_item(m, "Health: no damage", "", PC_MENU_DO_CHEAT, PC_CHEAT_HEALTH, (cheats & PC_CHEAT_HEALTH ? PC_MENU_TICK : 0) | (can_cheat ? 0 : PC_MENU_GREY));
    pc_menu_add_item(m, "Pills: a Blu pill is not spent", "", PC_MENU_DO_CHEAT, PC_CHEAT_PILLS, (cheats & PC_CHEAT_PILLS ? PC_MENU_TICK : 0) | (can_cheat ? 0 : PC_MENU_GREY));
    pc_menu_add_item(m, "Lives: a death is not spent", "", PC_MENU_DO_CHEAT, PC_CHEAT_LIVES, (cheats & PC_CHEAT_LIVES ? PC_MENU_TICK : 0) | (can_cheat ? 0 : PC_MENU_GREY));
    /* The Pocket core's entries, in its order: Arcade Mode, Arcade Coins, Attract Only, the stage, 6-Button, 3-Player Mode (pc_scum.h). */
    {
        PcArcade a;
        pc_scum_arcade(&a);
        menu_arcade_list = pc_menu_add_menu("Arcade Mode", 0);
        snprintf(label, sizeof(label), "Arcade Mode: %s", arcade_titles[a.mode]);
        pc_menu_add_item(m, label, "", PC_MENU_DO_NOTHING, menu_arcade_list, PC_MENU_LINE | PC_MENU_OPENS);
        for (k = 0; k < PC_ARCADE_MODES; k++)
            pc_menu_add_item(menu_arcade_list, arcade_titles[k], "", PC_MENU_DO_ARCADE_MODE, k, a.mode == k ? PC_MENU_TICK : 0);
        menu_coins_list = pc_menu_add_menu("Arcade Coins", 0);
        snprintf(label, sizeof(label), "Arcade Coins: %s", coins_titles[a.coins]);
        pc_menu_add_item(m, label, "", PC_MENU_DO_NOTHING, menu_coins_list, PC_MENU_OPENS);
        for (k = 0; k < PC_COINS_SETTINGS; k++)
            pc_menu_add_item(menu_coins_list, coins_titles[k], "", PC_MENU_DO_ARCADE_COINS, k, a.coins == k ? PC_MENU_TICK : 0);
        pc_menu_add_item(m, "Attract Only", "", PC_MENU_DO_ATTRACT, 0, a.attract ? PC_MENU_TICK : 0);
        menu_stage_list = pc_menu_add_menu("Arcade stage", 0);
        snprintf(label, sizeof(label), "Arcade stage: %s", a.stage ? pc_scum_stage_name(a.stage) : "off");
        pc_menu_add_item(m, label, "", PC_MENU_DO_NOTHING, menu_stage_list, PC_MENU_OPENS);
        pc_menu_add_item(menu_stage_list, "Off", "", PC_MENU_DO_STAGE, 0, a.stage == 0 ? PC_MENU_TICK : 0);
        for (k = 1; k <= PC_ARCADE_STAGES; k++)
            pc_menu_add_item(menu_stage_list, pc_scum_stage_name(k), "", PC_MENU_DO_STAGE, k, a.stage == k ? PC_MENU_TICK : 0);
        /* The stage list only picks the stage, as on the Pocket; Arcade Mode is its own entry. So while the arcade mode is on, the grey line says
           that the list's Off is the game's own first stage, not the arcade mode off; otherwise it says when the three entries above act. */
        if (a.mode == PC_ARCADE_ON || a.mode == PC_ARCADE_MULTIPLAYER)
            pc_menu_add_item(m, "Stage Off = first stage; arcade off: Arcade Mode", "", PC_MENU_DO_NOTHING, 0, PC_MENU_GREY);
        else
            pc_menu_add_item(m, "Coins, Attract Only, stage: with On or Multiplayer", "", PC_MENU_DO_NOTHING, 0, PC_MENU_GREY);
        for (k = 0; k < 3; k++) {
            snprintf(label, sizeof(label), "6-Button pad, player %d", k + 1);
            pc_menu_add_item(m, label, "", PC_MENU_DO_SIX_BUTTON, k, (k == 0 ? PC_MENU_LINE : 0) | (controls->player[k].three_button ? 0 : PC_MENU_TICK));
        }
        pc_menu_add_item(m, "3-Player Mode", "", PC_MENU_DO_THREE_PLAYERS, 0, a.three_players ? PC_MENU_TICK : 0);
        if (a.mode != PC_ARCADE_OFF && (a.three_players || a.mode == PC_ARCADE_MULTIPLAYER))
            pc_menu_add_item(m, "Player 3: in the stick's EXT socket", "", PC_MENU_DO_NOTHING, 0, PC_MENU_GREY);
        else if (a.three_players)
            pc_menu_add_item(m, "Player 3: in the EA 4-Way Play's socket C", "", PC_MENU_DO_NOTHING, 0, PC_MENU_GREY);
    }
    pc_menu_add_item(m, "Soft Reset, for these", "", PC_MENU_DO_SOFT_RESET, 0, PC_MENU_LINE);

    /* The Console menu: the Pocket core's two lists, each opened from its own line, then the notes of the hardware rules that change a choice
       (grey: they cannot be chosen), 32X and CPU MATE. */
    {
        PcMachine mach;
        pc_console_machine(&settings->console, &mach);
        m = pc_menu_add_menu("Console", 1);
        menu_console_list = pc_menu_add_menu("Consoles", 0);
        snprintf(label, sizeof(label), "Console: %s", console_titles[mach.console]);
        pc_menu_add_item(m, label, "", PC_MENU_DO_NOTHING, menu_console_list, PC_MENU_OPENS);
        for (k = 0; k < PC_CONSOLES; k++)
            pc_menu_add_item(menu_console_list, console_titles[k], "", PC_MENU_DO_CONSOLE, k, mach.console == k ? PC_MENU_TICK : 0);
        menu_megacd_list = pc_menu_add_menu("Mega CD units", 0);
        snprintf(label, sizeof(label), "Mega CD: %s", megacd_titles[settings->console.megacd]);
        pc_menu_add_item(m, label, "", PC_MENU_DO_NOTHING, menu_megacd_list, PC_MENU_OPENS);
        for (k = 0; k < PC_MEGACDS; k++)
            pc_menu_add_item(menu_megacd_list, megacd_titles[k], "", PC_MENU_DO_MEGACD, k, settings->console.megacd == k ? PC_MENU_TICK : 0);
        if (mach.combined) {
            snprintf(label, sizeof(label), "%s is the console (%s)", megacd_titles[mach.unit], mach.japan ? "Japan" : "USA");
            pc_menu_add_item(m, label, "", PC_MENU_DO_NOTHING, 0, PC_MENU_GREY);
        }
        if (mach.dropped) {
            snprintf(label, sizeof(label), "%s: no port, so no Mega CD", console_titles[mach.console]);
            pc_menu_add_item(m, label, "", PC_MENU_DO_NOTHING, 0, PC_MENU_GREY);
        }
        pc_menu_add_item(m, "32X", "", PC_MENU_DO_X32, 0, PC_MENU_LINE | (settings->console.x32 ? PC_MENU_TICK : 0));
        pc_menu_add_item(m, "CPU MATE, with the Mega CD", "", PC_MENU_DO_CPUMATE, 0, (settings->console.cpumate ? PC_MENU_TICK : 0) |
                         (mach.cpumate_unit ? 0 : PC_MENU_GREY));
        if (!mach.cpumate_unit)
            pc_menu_add_item(m, mach.unit == PC_MEGACD_UNKNOWN ? "CPU MATE: not with an Unknown CD" : "CPU MATE: needs a Mega CD unit", "",
                             PC_MENU_DO_NOTHING, 0, PC_MENU_GREY);
        pc_menu_add_item(m, "Soft Reset, for these", "", PC_MENU_DO_SOFT_RESET, 0, PC_MENU_LINE);
    }

    m = pc_menu_add_menu("Help", 1);
    pc_menu_add_item(m, "Keys", "", PC_MENU_DO_KEYS, 0, 0);
    pc_menu_add_item(m, "About", "", PC_MENU_DO_ABOUT, 0, 0);
    snprintf(label, sizeof(label), "Menu: %s", key_of(PC_PORT_MENU));
    pc_menu_set_hint(key_of(PC_PORT_MENU)[0] ? label : "");
}

/* pc_config_rewrite's change for the menu: the settings named in a SettingsChange set in their own lines. */
typedef struct SettingsChange { const char *const *names, *const *values; int n; } SettingsChange;

static int settings_change(const char *old_text, char *out, size_t out_size, void *arg)
{
    const SettingsChange *c = (const SettingsChange *)arg;
    return pc_config_merge_settings(old_text, c->names, c->values, c->n, out, out_size);
}

/* Why the last change setting_save could not write: the settings file's name and the reason, in a few words for the window. */
static char unsaved_why[200];
static int unsaved_any;             /* some menu change of this run is not in the settings file; pc_controls_unsaved tells the same of the controls screen */

/* The settings file's name without its folder, for the window's few words. */
static const char *config_name(void)
{
    const char *base = config_full, *slash;
    if ((slash = strrchr(base, '\\')) != NULL)
        base = slash + 1;
    if ((slash = strrchr(base, '/')) != NULL)
        base = slash + 1;
    return base;
}

/* Writes settings into the settings file, n of them, in one rewrite. Returns 1 when the file holds them now, or 0 when the file could not take them:
   then unsaved_why says why, and the console has the whole path. A Soft Reset, like the next start, reads the settings file again, so the menu's words
   must say when a change is not in it: tell_change does. */
static int settings_save(const char *const *names, const char *const *values, int n)
{
    SettingsChange c;
    int result;
    const char *why;
    if (!config_full[0])
        return 1;
    c.names = names;
    c.values = values;
    c.n = n;
    result = pc_config_rewrite(config_full, settings_change, &c);
    if (result == 0)
        return 1;
    why = result == PC_CONFIG_FILE_TOO_LARGE ? "is 32 KB or larger, more than the program rewrites" : result == PC_CONFIG_NOT_READ ? "could not be read" :
          result == PC_CONFIG_TOO_LARGE ? "would grow too large" : result == PC_CONFIG_NOT_WRITTEN ? "could not be written" : "could not be replaced";
    printf("Not saved: %s %s.\n", config_full, why);
    snprintf(unsaved_why, sizeof(unsaved_why), "%.100s %s", config_name(), why);
    unsaved_any = 1;
    return 0;
}

static int setting_save(const char *name, const char *value) { return settings_save(&name, &value, 1); }

static void tell_for(const char *text, int ms)
{
    printf("menu: %s\n", text);
    pc_window_show_text(text, ms);
}

static void tell(const char *text) { tell_for(text, 4000); }

/* Tells a menu change that setting_save wrote (saved 1) or could not write (saved 0). For a change that acts in this run, later is NULL and text says
   it; when it is not saved, the words add that it lasts only until a Soft Reset or the end of the program. For a change the game takes only when it
   starts (the console, the arcade stage, the renderer, vsync), later names it in a few words: when it is not saved, those words say so in place of text,
   since a Soft Reset would start without it. */
static void tell_change(const char *text, const char *later, int saved)
{
    char all[1600];
    if (saved) {
        tell(text);
        return;
    }
    if (later)
        snprintf(all, sizeof(all), "%s was not saved: %s, so a Soft Reset or the next start goes without it.", later, unsaved_why);
    else
        snprintf(all, sizeof(all), "%s%s Not saved: %s, so this lasts only until a Soft Reset or the end of the program.", text,
                 text[0] && text[strlen(text) - 1] == '.' ? "" : ".", unsaved_why);
    tell_for(all, 8000);
}

static void menu_page_keys(void)
{
    static const char *const actions[PC_CTL_N] = {"Up", "Down", "Left", "Right", "A", "B", "C", "X", "Y", "Z", "Start", "Mode"};
    static const char *const ports[PC_PORT_N] = {"Controls screen", "Pause", "Mute", "Sprite limits", "Picture shape", "Next filter", "Full screen", "Quit",
                                                 "Menu bar"};
    char text[2600];
    size_t used = 0;
    int k;
    PcControls *c = pc_input_controls();
    used += (size_t)snprintf(text + used, sizeof(text) - used, "The program's keys:\n");
    for (k = 0; k < PC_PORT_N; k++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "  %-14s %s\n", c->port_key[k] ? pc_input_key_name(c->port_key[k]) : "none", ports[k]);
    used += (size_t)snprintf(text + used, sizeof(text) - used, "\nPlayers 1, 2 and 3:\n");
    for (k = 0; k < PC_CTL_N; k++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "  %-6s %-14s %-14s %s\n", actions[k], c->player[0].key[k] ? pc_input_key_name(c->player[0].key[k]) : "none",
                                 c->player[1].key[k] ? pc_input_key_name(c->player[1].key[k]) : "none",
                                 c->player[2].key[k] ? pc_input_key_name(c->player[2].key[k]) : "none");
    snprintf(text + used, sizeof(text) - used, "\nThe controls screen (%s) sets every key and pad button.", key_of(PC_PORT_CONTROLS));
    pc_menu_show_page("Keys", text);
}

static void menu_page_about(void)
{
    pc_menu_show_page("About",
                      "Paprium on the PC: the game, translated from your own copy\n"
                      "of the game file on your own PC, with the cart's chip and\n"
                      "the console's chips as programs.\n"
                      "\n"
                      "Copyright (C) 2026 Marsupial Solutions LLC\n"
                      "GPL-3.0-or-later, except the game itself, which is not ours.\n"
                      "\n"
                      "Scum! and the console settings are the Pocket core's:\n"
                      "the cheats, Arcade Mode with the Grand Stick III, the\n"
                      "arcade stage (the stage word found by krikzz), 6-Button,\n"
                      "3-Player Mode with the EA 4-Way Play, 32X, Mega CD,\n"
                      "Console Reset and Aspect Ratio.\n"
                      "\n"
                      "SDL2 (zlib), Nuked-OPN2 (LGPL-2.1+), Nuked-PSG (GPL-2.0+),\n"
                      "chips z80.h (zlib), mega-ppm by krikzz (BSD-3-Clause),\n"
                      "WavPack (BSD), the CRT shaders (MIT, public domain,\n"
                      "GPL-2.0+) and SDL_GameControllerDB (zlib).\n"
                      "NOTICES.txt beside the program has every notice whole.");
}

static void brain_menu_words(const PcConsole *console, char *out, size_t size);

/* Whether this run started with the stick's ARCADE switch: Arcade Mode On or Multiplayer. */
static int arcade_started_on(void)
{
    PcArcade a;
    pc_scum_arcade_at_start(&a);
    return a.mode == PC_ARCADE_ON || a.mode == PC_ARCADE_MULTIPLAYER;
}

static void menu_command(int command, int arg)
{
    char text[2000], value[64], later[100];
    int saved;
    switch (command) {
    case PC_MENU_DO_SOFT_RESET:
        soft_reset_asked = 1;
        InterlockedExchange(&quit_asked, 1);
        if (unsaved_any || pc_controls_unsaved(NULL)) {
            snprintf(text, sizeof(text), "Soft Reset: the game starts again from power-on, with your save and the settings in %.100s; the changes that "
                     "could not be saved there are not among them.", config_name());
            tell(text);
        } else {
            tell("Soft Reset: the game starts again from power-on, with your save and these settings.");
        }
        break;
    case PC_MENU_DO_FILTER: {
        static const char *const names[] = PC_LOOK_NAMES;
        char note[900];
        if (arg < 0 || arg >= PC_LOOKS)
            break;
        pc_window_set_filter(names[arg], "", note, sizeof(note));
        if (note[0])
            printf("%s\n", note);
        if (pc_window_filter() == arg) {
            snprintf(settings->filter, sizeof(settings->filter), "%s", names[arg]);
            saved = setting_save("filter", names[arg]);
            snprintf(text, sizeof(text), "Filter: %s", pc_filter_title(arg));
            tell_change(text, NULL, saved);
        }
        pc_vsync_watch_again(&vsync_watch);     /* a look's first draw builds its shader, which can make that present slow */
        last_pace_point = 0;
        break;
    }
    case PC_MENU_DO_RENDERER:
        if (arg < 0 || arg >= 5)
            break;
        snprintf(settings->renderer, sizeof(settings->renderer), "%s", renderer_names[arg]);
        saved = setting_save("renderer", renderer_names[arg]);
        snprintf(text, sizeof(text), "Renderer: %s, from the next start. This run draws with %s.", renderer_titles[arg], pc_window_renderer());
        snprintf(later, sizeof(later), "Renderer: %s", renderer_titles[arg]);
        tell_change(text, later, saved);
        break;
    case PC_MENU_DO_VSYNC:
        settings->vsync = !settings->vsync;
        saved = setting_save("vsync", settings->vsync ? "on" : "off");
        tell_change(settings->vsync ? "Vsync on, from the next start." : "Vsync off, from the next start.", settings->vsync ? "Vsync on" : "Vsync off", saved);
        break;
    case PC_MENU_DO_MENU_BAR:
        settings->menu_bar = !pc_window_menu_bar();
        pc_window_set_menu_bar(settings->menu_bar);
        saved = setting_save("menu_bar", settings->menu_bar ? "on" : "off");
        snprintf(text, sizeof(text), "%s", settings->menu_bar ? "The menu bar has its own strip in the window." : "The menu bar is hidden in the window: the menu key or the mouse at the top brings it back.");
        tell_change(text, NULL, saved);
        break;
    case PC_MENU_DO_VOLUME:
        pc_audio_set_volume(arg);
        settings->volume = arg;
        snprintf(value, sizeof(value), "%d", arg);
        saved = setting_save("volume", value);
        snprintf(text, sizeof(text), "Volume %d %%", arg);
        tell_change(text, NULL, saved);
        break;
    case PC_MENU_DO_CHEAT:
        settings->cheats = pc_scum_cheats() ^ (unsigned)arg;
        pc_scum_set_cheats(settings->cheats);
        pc_config_cheats_text(settings->cheats, value, sizeof(value));
        saved = setting_save("cheats", value);
        snprintf(text, sizeof(text), "%s %s.", arg == PC_CHEAT_HEALTH ? "Health:" : arg == PC_CHEAT_PILLS ? "Pills:" : "Lives:",
                 settings->cheats & (unsigned)arg ? (arg == PC_CHEAT_HEALTH ? "on, players take no damage" : arg == PC_CHEAT_PILLS ? "on, a Blu pill is not spent" :
                 "on, a death is not spent") : "off");
        tell_change(text, NULL, saved);
        break;
    case PC_MENU_DO_STAGE: {
        /* The stage only picks the stage. Its line is written with the arcade_mode line, so a file that had only an arcade_stage line, which meant
           Arcade Mode On, keeps the Arcade Mode it had. */
        const char *names[2] = {"arcade_stage", "arcade_mode"}, *values[2];
        PcArcade a;
        if (arg < 0 || arg > PC_ARCADE_STAGES)
            break;
        settings->arcade_mode = pc_config_arcade_mode(settings);
        settings->arcade_stage = arg;
        pc_scum_arcade(&a);
        a.stage = arg;
        pc_scum_set_arcade(&a);
        if (arg)
            snprintf(value, sizeof(value), "%d", arg);
        else
            snprintf(value, sizeof(value), "off");
        values[0] = value;
        values[1] = arcade_names[settings->arcade_mode];
        saved = settings_save(names, values, 2);
        /* Without the translated game (the interpreter) there is no switch point, so the stage word waits for the next start (pc_scum.h). */
        if ((a.mode == PC_ARCADE_ON || a.mode == PC_ARCADE_MULTIPLAYER) && !pc_scum_cheats_available())
            snprintf(text, sizeof(text), "Arcade stage: %s, from the next Soft Reset (Game menu): this start plays through the program's interpreter.",
                     arg ? pc_scum_stage_name(arg) : "off, the game's own first stage");
        else if (a.mode == PC_ARCADE_ON || a.mode == PC_ARCADE_MULTIPLAYER)
            snprintf(text, sizeof(text), "Arcade stage: %s. The next arcade game starts there%s.", arg ? pc_scum_stage_name(arg) : "off, the game's own first stage",
                     arcade_started_on() ? "" : "; Arcade Mode takes a Soft Reset (Game menu)");
        else
            snprintf(text, sizeof(text), "Arcade stage: %s, for Arcade Mode On or Multiplayer (Scum! menu).", arg ? pc_scum_stage_name(arg) : "off");
        snprintf(later, sizeof(later), "Arcade stage: %s", arg ? pc_scum_stage_name(arg) : "off");
        tell_change(text, later, saved);
        break;
    }
    case PC_MENU_DO_ARCADE_MODE: {
        PcArcade a;
        const char *mode_key = pc_input_key_name(pc_input_controls()->player[0].key[PC_CTL_MODE]);
        if (arg < 0 || arg >= PC_ARCADE_MODES)
            break;
        settings->arcade_mode = arg;
        pc_scum_arcade(&a);
        a.mode = arg;
        pc_scum_set_arcade(&a);
        saved = setting_save("arcade_mode", arcade_names[arg]);
        if (arg == PC_ARCADE_ON || arg == PC_ARCADE_MULTIPLAYER)
            snprintf(text, sizeof(text), "Arcade Mode: %s, from the next Soft Reset (Game menu): the Grand Stick III in port 1, its ARCADE switch on%s. "
                     "Player 1's Mode (%s) puts in a coin.", arcade_titles[arg], arg == PC_ARCADE_MULTIPLAYER ? " and 3 PLAYERS, player 3 in its EXT socket" : "",
                     mode_key[0] ? mode_key : "no key");
        else if (arg == PC_ARCADE_STICK_ONLY)
            snprintf(text, sizeof(text), "Arcade Mode: Stick Only, from the next Soft Reset (Game menu): the Grand Stick III in port 1 with every switch off, the "
                     "normal game; its Mode button is player 1's Mode.");
        else
            snprintf(text, sizeof(text), "Arcade Mode: Off, from the next Soft Reset (Game menu).");
        snprintf(later, sizeof(later), "Arcade Mode: %s", arcade_titles[arg]);
        tell_change(text, later, saved);
        break;
    }
    case PC_MENU_DO_ARCADE_COINS: {
        PcArcade a;
        if (arg < 0 || arg >= PC_COINS_SETTINGS)
            break;
        settings->arcade_coins = arg;
        pc_scum_arcade(&a);
        a.coins = arg;
        pc_scum_set_arcade(&a);
        saved = setting_save("arcade_coins", coins_names[arg]);
        snprintf(text, sizeof(text), "Arcade Coins: %s, from the next Soft Reset (Game menu), with Arcade Mode On or Multiplayer.%s", coins_titles[arg],
                 arg == PC_COINS_INFINITE ? " With this switch the game stops on its red 0012 screen at start: that is the game's own answer to it." : "");
        snprintf(later, sizeof(later), "Arcade Coins: %s", coins_titles[arg]);
        tell_change(text, later, saved);
        break;
    }
    case PC_MENU_DO_ATTRACT: {
        PcArcade a;
        pc_scum_arcade(&a);
        a.attract = !a.attract;
        settings->attract_only = a.attract;
        pc_scum_set_arcade(&a);
        saved = setting_save("attract_only", a.attract ? "on" : "off");
        tell_change(a.attract ? "Attract Only on, from the next Soft Reset (Game menu), with Arcade Mode On or Multiplayer: the attract demo, and no game." :
                    "Attract Only off, from the next Soft Reset (Game menu).", a.attract ? "Attract Only on" : "Attract Only off", saved);
        break;
    }
    case PC_MENU_DO_THREE_PLAYERS: {
        PcArcade a;
        pc_scum_arcade(&a);
        a.three_players = !a.three_players;
        settings->three_players = a.three_players;
        pc_scum_set_arcade(&a);
        saved = setting_save("three_players", a.three_players ? "on" : "off");
        tell_change(a.three_players ? "3-Player Mode on, from the next Soft Reset (Game menu): player 3 plays through the EA 4-Way Play, or with Arcade Mode in "
                    "the stick's EXT socket. Player 3's pad is the third pad, or the one the controls screen gives them." :
                    "3-Player Mode off, from the next Soft Reset (Game menu).", a.three_players ? "3-Player Mode on" : "3-Player Mode off", saved);
        break;
    }
    case PC_MENU_DO_SHAPE:
        if (arg < 0 || arg >= PC_SHAPES)
            break;
        pc_window_set_shape(arg);
        settings->shape = arg;
        saved = setting_save("display", shape_names[arg]);
        snprintf(text, sizeof(text), "Aspect Ratio: %s", shape_titles[arg]);
        tell_change(text, NULL, saved);
        title_update();
        break;
    case PC_MENU_DO_CONSOLE_RESET:
        /* As on the Pocket core, a press while the cart holds the console, or while a Console Reset is still being served, is dropped. */
        if (pc_cart_holding() || console_reset_asked) {
            tell("Console Reset: not now, the console is held.");
            break;
        }
        console_reset_asked = 1;
        /* A cart chip that has stopped itself (pc_cart.c, fatal_halt) never acknowledges the reset, so the 68000 stays held, as the Pocket core holds
           it while it waits for that acknowledgement; only Soft Reset starts the game again. */
        if (pc_cart_fatal_count())
            tell("Console Reset: the console's reset button. The cart's chip has stopped, so it cannot answer the reset and the console stays held; "
                 "Soft Reset (Game menu) starts the game again.");
        else
            tell("Console Reset: the console's reset button. Its processor starts again from the cart's warm start, while the video chip, work RAM and "
                 "the cart run on; the settings that need a Soft Reset still do.");
        break;
    case PC_MENU_DO_SIX_BUTTON: {
        PcControls *c = pc_input_controls();
        static const char *const pad_lines[3] = {"pad1", "pad2", "pad3"};
        if (arg < 0 || arg > 2)
            break;
        c->player[arg].three_button = !c->player[arg].three_button;
        pc_io_set_three_button(arg, three_button_arg[arg] || c->player[arg].three_button);
        saved = setting_save(pad_lines[arg], c->player[arg].three_button ? "three" : "six");
        snprintf(text, sizeof(text), "Player %d: a %s-button pad.", arg + 1, c->player[arg].three_button ? "three" : "six");
        tell_change(text, NULL, saved);
        break;
    }
    case PC_MENU_DO_CONSOLE: {
        /* The console's name carries its region, so an earlier version's region line, which would pick between the first Mega Drive and the
           Genesis, goes back to auto with it. */
        const char *names[2] = {"console", "region"}, *values[2];
        char rules[400];
        if (arg < 0 || arg >= PC_CONSOLES)
            break;
        values[0] = console_names[arg];
        values[1] = "auto";
        saved = settings_save(names, values, settings->console.region != PC_REGION_AUTO ? 2 : 1);
        settings->console.console = arg;
        settings->console.region = PC_REGION_AUTO;
        console_rule_words(&settings->console, 1, rules, sizeof(rules));
        snprintf(text, sizeof(text), "Console: %s, from the next Soft Reset (Game menu).%s%s", console_titles[arg], rules[0] ? " " : "", rules);
        snprintf(later, sizeof(later), "Console: %s", console_titles[arg]);
        tell_change(text, later, saved);
        break;
    }
    case PC_MENU_DO_X32:
        settings->console.x32 = !settings->console.x32;
        saved = setting_save("x32", settings->console.x32 ? "on" : "off");
        tell_change(settings->console.x32 ? "32X on, from the next Soft Reset (Game menu)." : "32X off, from the next Soft Reset (Game menu).",
                    settings->console.x32 ? "32X on" : "32X off", saved);
        break;
    case PC_MENU_DO_MEGACD: {
        char rules[400];
        if (arg < 0 || arg >= PC_MEGACDS)
            break;
        settings->console.megacd = arg;
        saved = setting_save("megacd", megacd_names[arg]);
        console_rule_words(&settings->console, 1, rules, sizeof(rules));
        snprintf(text, sizeof(text), "Mega CD: %s, from the next Soft Reset (Game menu).%s%s", megacd_titles[arg], rules[0] ? " " : "", rules);
        snprintf(later, sizeof(later), "Mega CD: %s", megacd_titles[arg]);
        tell_change(text, later, saved);
        break;
    }
    case PC_MENU_DO_CPUMATE:
        settings->console.cpumate = !settings->console.cpumate;
        saved = setting_save("cpumate", settings->console.cpumate ? "on" : "off");
        if (settings->console.cpumate) {
            /* Where the brain will come from, as console_start will look for it, in the window's words (brain_menu_words): this program has none of
               its own (pc_brain.h). */
            char place[1700];
            brain_menu_words(&settings->console, place, sizeof(place));
            snprintf(text, sizeof(text), "CPU MATE on, with the Mega CD, from the next Soft Reset (Game menu). %s", place);
        } else
            snprintf(text, sizeof(text), "CPU MATE off, from the next Soft Reset (Game menu).");
        tell_change(text, settings->console.cpumate ? "CPU MATE on" : "CPU MATE off", saved);
        break;
    case PC_MENU_DO_KEYS:
        menu_page_keys();
        printf("menu: the Keys page\n");
        break;
    case PC_MENU_DO_ABOUT:
        menu_page_about();
        printf("menu: the About page\n");
        break;
    default:
        break;
    }
}

/* The program actions a pass of the window's events carried out, by the configuration's names for their keys, on the console: so a test can see that a
   menu item did what its key does. */
static void say_actions(uint32_t actions)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        {PC_ACTION_CONTROLS, "controls"}, {PC_ACTION_PAUSE, "pause"}, {PC_ACTION_MUTE, "mute"}, {PC_ACTION_LIMITS, "limits"}, {PC_ACTION_SHAPE, "shape"},
        {PC_ACTION_FILTER, "filter"}, {PC_ACTION_FULLSCREEN, "fullscreen"}, {PC_ACTION_QUIT, "quit"}, {PC_ACTION_MENU, "menu"}};
    char text[200];
    size_t used = 0, k;
    for (k = 0; k < sizeof(names) / sizeof(names[0]); k++)
        if (actions & names[k].bit)
            used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%s", used ? ", " : "", names[k].name);
    if (used)
        printf("actions: %s\n", text);
}

static int menu_dump_asked;

static void menu_dump(void)
{
    int m, i;
    for (m = 0; m < pc_menu_count(); m++) {
        for (i = 0; i < pc_menu_items(m); i++) {
            const char *key;
            int command, arg;
            unsigned look;
            if (pc_menu_item_info(m, i, &key, &command, &arg, &look) != 0)
                continue;
            printf("menu %s%s | %s | %s | %d %d |%s%s%s\n", pc_menu_title(m), pc_menu_on_bar(m) ? "" : " (list)", pc_menu_item_label(m, i), key, command, arg,
                   look & PC_MENU_TICK ? " tick" : "", look & PC_MENU_GREY ? " grey" : "", look & PC_MENU_OPENS ? " opens" : "");
        }
    }
}

/* A menu open pauses the game, as the controls screen does; when the last one closes the game goes on as it was. */
static void menu_follow(void)
{
    int open = pc_menu_is_open();
    if (open && !menu_was_open) {
        menu_paused_it = !paused;
        if (!paused) {
            paused = 1;
            pc_audio_paused(1);
        }
    } else if (!open && menu_was_open && menu_paused_it) {
        paused = 0;
        pc_audio_paused(0);
        if (paced)
            pace_reset();
    }
    menu_was_open = open;
}

static void window_events(void)
{
    uint32_t actions = pc_input_poll(input_pad);
    say_actions(actions);
    if (actions & PC_ACTION_MENU)
        pc_menu_open_first();
    menu_follow();
    if (actions & PC_ACTION_MENU_COMMAND) {
        int command, arg;
        while ((command = pc_input_take_menu_command(&arg)) != 0)
            menu_command(command, arg);
        menu_follow();      /* a page of the Help menu is open now */
    }
    if (actions & PC_ACTION_QUIT)
        InterlockedExchange(&quit_asked, 1);
    if (actions & PC_ACTION_CONTROLS)
        controls_open();
    if (actions & PC_ACTION_CONTROLS_CLOSED) {
        int named;
        const char *why = pc_controls_unsaved(&named);
        controls_closed();
        /* The screen showed the reason with the file's name; the console gets the whole path. */
        if (why)
            printf("Not saved: the controls screen's changes: %s%s%s. They hold until a Soft Reset or the end of the program.\n", named ? config_full : "",
                   named ? " " : "", why);
    }
    if (actions & PC_ACTION_LIMITS) {
        render_options.limits = render_options.limits == PR_LIMITS_CART ? PR_LIMITS_NONE : PR_LIMITS_CART;
        if (paused)
            pr_render(frame_rec, &render_options, picture);     /* so the flip shows on the paused frame; the walk's memory is left alone */
    }
    /* The controls screen keeps the game paused while it is open: a pause key never ends that pause (pc_input_poll also keeps the pause key out of the
       poll that opens the screen). Otherwise the game would run on behind a screen that is never shown and still takes the keys. */
    if ((actions & PC_ACTION_PAUSE) && !pc_controls_screen_open()) {
        paused = !paused;
        pc_audio_paused(paused);
        if (!paused && paced)
            pace_reset();
    }
    if (actions & PC_ACTION_FULLSCREEN) {
        pc_window_toggle_fullscreen();
        pc_vsync_watch_again(&vsync_watch);     /* the presents around a change of mode say nothing about the monitor */
        last_pace_point = 0;
    }
    if (actions & PC_ACTION_SHAPE)
        pc_window_toggle_shape();
    if (actions & PC_ACTION_FILTER) {
        char note[900];
        pc_window_next_filter(pc_input_key_name(pc_input_controls()->port_key[PC_PORT_FILTER]), note, sizeof(note));
        printf("%s\n", note);
        pc_vsync_watch_again(&vsync_watch);     /* a look's first draw builds its shader, which can make that present slow */
        last_pace_point = 0;                    /* while paused, the pause's own redraws show the new look */
    }
    if (actions & PC_ACTION_MUTE)
        pc_audio_toggle_mute();
    if (actions)
        title_update();
    menu_refresh();
}

static int window_shot_due(uint32_t frame)
{
    int k;
    for (k = 0; k < window_shots_n; k++)
        if (window_shots[k] == (int)frame)
            return 1;
    return 0;
}

/* A test's picture of the window during a pause or the controls screen, by the key script's clock. */
static int pause_shots[16], pause_shots_n;

static void pause_shot(int tick)
{
    static uint32_t *pixels;
    int k, w, h;
    char path[1200];
    for (k = 0; k < pause_shots_n; k++) {
        if (pause_shots[k] != tick || !run_dir)
            continue;
        if (!pixels)
            pixels = (uint32_t *)malloc(8192u * 4096u * 4u);
        if (pixels && pc_window_read_screen(pixels, 8192 * 4096, &w, &h) == 0) {
            snprintf(path, sizeof(path), "%s/pause_%06d.ppm", run_dir, tick);
            write_ppm(path, pixels, (unsigned)w, (unsigned)w, (unsigned)h);
        }
    }
}

static void window_frame(uint32_t frame)
{
    uint64_t t0;
    int k, shown = 1;
    if (paced) {
        pc_stall_part(PC_STALL_PACE);
        t0 = pc_fiber_host_time();
        pace_wait();
        stat_add(&stat_wait, pc_fiber_host_time() - t0);
        /* A frame a whole frame behind is not shown (pc_pace.h). A test's picture read is always shown, so a picture a test asks for is that frame's. */
        shown = window_shot_due(frame) || pc_pace_show(&pace, pc_fiber_host_time());
    }
    if (shown) {
        t0 = pc_fiber_host_time();     /* before the part's mark, so a test's planted sleep in the present counts as the present's own time */
        pc_stall_part(PC_STALL_PRESENT);
        if (test_monitor_hz > 0 && pc_window_vsync_on())
            test_monitor_wait();
        pc_window_present(picture);
        last_present_ticks = pc_fiber_host_time() - t0;
        {
            char note[800];
            if (pc_window_take_note(note, sizeof(note)))
                printf("%s\n", note);
        }
        stat_add(&stat_present, last_present_ticks);
    } else {
        last_present_ticks = 0;
    }
    last_shown = shown;
    for (k = 0; shown && k < window_shots_n; k++) {
        if (window_shots[k] == (int)frame) {
            static uint32_t *pixels;
            int w, h;
            char path[1200];
            if (!pixels)
                pixels = (uint32_t *)malloc(8192u * 4096u * 4u);
            if (pixels && pc_window_read_screen(pixels, 8192 * 4096, &w, &h) == 0) {
                snprintf(path, sizeof(path), "%s/win_%06u.ppm", run_dir, (unsigned)frame);
                write_ppm(path, pixels, (unsigned)w, (unsigned)w, (unsigned)h);
            }
            if (pixels && pc_window_read_large(pixels, 8192 * 4096, &w, &h) == 0) {
                snprintf(path, sizeof(path), "%s/large_%06u.ppm", run_dir, (unsigned)frame);
                write_ppm(path, pixels, (unsigned)w, (unsigned)w, (unsigned)h);
            }
        }
    }
    pc_stall_part(PC_STALL_EVENTS);
    key_script_due((int)frame + script_ticks);
    window_events();
    while (paused && !quit_asked) {
        /* Each pass of a pause counts on its own: a pause is no gap, and a present or an event poll that sticks during one is still noted. */
        pc_stall_pass_end(frame, pc_cart_host_ticks());
        pc_stall_part(PC_STALL_PACE);
        Sleep(15);
        pc_stall_part(PC_STALL_PRESENT);
        if (pc_controls_screen_open())
            pc_window_present(pc_controls_screen_picture());
        else
            pc_window_redraw();
        pause_shot((int)frame + script_ticks);
        script_ticks++;
        pc_stall_part(PC_STALL_EVENTS);
        key_script_due((int)frame + script_ticks);
        window_events();
    }
}

/* ---- the save: written when it changes, looked at about once a second, and at quit. Through a temporary name, so a kill leaves a whole file. The first write of a run also keeps the file as it was
   loaded, see pc_save.h. */
static const char *save_store_path;
static uint8_t save_last[PC_SAVE_BYTES];
static int save_failed;
#define TEST_SAVE_CHANGES 8
static int test_save_changes[TEST_SAVE_CHANGES], test_save_changes_n;

static void save_check(void)
{
    static uint8_t now[PC_SAVE_BYTES];
    if (!save_store_path)
        return;
    pc_cart_save_copy(now, sizeof(now));
    if (!memcmp(now, save_last, sizeof(now))) {
        /* The file already holds these bytes, as save_last is what last reached it. So a failure before this is over, and a temporary file it left is older than the save. */
        if (save_failed) {
            save_failed = 0;
            pc_save_forget_temp(save_store_path);
        }
        return;
    }
    if (pc_save_replace(save_store_path, now) != 0) {
        if (!save_failed)
            printf("could not write the save to %s\n", save_store_path);
        save_failed = 1;
    } else {
        memcpy(save_last, now, sizeof(now));
        save_failed = 0;
    }
}

/* The last write of a run. Another program can hold the file for a moment, so a failure gets a few short tries before it counts. */
#define SAVE_LAST_TRIES 5
#define SAVE_LAST_WAIT_MS 500

static void save_at_quit(void)
{
    int i;
    save_check();
    for (i = 0; i < SAVE_LAST_TRIES && save_failed; i++) {
        Sleep(SAVE_LAST_WAIT_MS);
        save_check();
    }
}

/* ---- the frame's two ends, called from the tick */
void pc_machine_frame_start(uint32_t frame)
{
    pc_vdp_frame_start(frame);
}

static PcCounts last_counts;

/* --mode-log: the 320 / 256 pixel mode (register 12's RS bits) and the 224 / 240 line mode (register 1's V30 bit). */
static FILE *f_mode;
static unsigned mode_last = 0xFFFFu, mode_frames[2][2];

static void mode_log_frame(uint32_t frame)
{
    PcVdpState s = frame_rec->start;
    unsigned at_start = (unsigned)(s.regs[12] & 0x81u) << 8 | (s.regs[1] & 0x08u), now = at_start, changes = 0;
    uint32_t i;
    for (i = 0; i < frame_rec->n_writes; i++) {
        const PcWrite *w = &frame_rec->writes[i];
        if (w->kind == PC_WRITE_REG && (w->addr == 1 || w->addr == 12)) {
            unsigned next;
            s.regs[w->addr] = (uint8_t)w->value;
            next = (unsigned)(s.regs[12] & 0x81u) << 8 | (s.regs[1] & 0x08u);
            if (next != now && w->vline < 224u)
                changes++;
            now = next;
        }
    }
    mode_frames[(at_start >> 8) != 0][(at_start & 0x08u) != 0]++;
    if (at_start != mode_last || changes)
        fprintf(f_mode, "%u start %s %s, changes in the picture %u\n", (unsigned)frame, (at_start >> 8) ? "320" : "256", (at_start & 0x08u) ? "240" : "224",
                changes);
    mode_last = at_start;
}

#define DELTA(name) ((unsigned long long)(pc_counts.name - last_counts.name))

void pc_machine_frame_end(uint32_t frame)
{
    uint64_t now = pc_clock_now(), pic = 0, host0;
    int want_shot = shot_next, k, drew = 0;
    pc_cart_run_to(now);
    pc_sound_frame_end(pc_tick_position());     /* the tick's time, the moment every source of sound has surely reached */
    host0 = pc_fiber_host_time();
    pc_stall_part(PC_STALL_RENDER);
    if (host_mark)
        stat_add(&stat_work, host0 - host_mark);
    for (k = 0; k < shots_n; k++)
        if (shots[k] == (int)frame)
            want_shot = 1;
    if (shot_every && frame % (uint32_t)shot_every == 0)
        want_shot = 1;
    if (flip_limits && frame && frame % (uint32_t)flip_limits == 0) {
        render_options.limits = render_options.limits == PR_LIMITS_CART ? PR_LIMITS_NONE : PR_LIMITS_CART;
        if (use_window)
            title_update();
    }
    if (render_all || want_shot || use_window) {
        pr_render_kept(frame_rec, &render_options, &render_memory, picture);
        drew = 1;
        stat_add(&stat_render, pc_fiber_host_time() - host0);
    }
    if (drew && f_frames) {
        uint32_t y;
        for (y = 0; y < picture->height; y++)
            pic = fnv(picture->rgb[y], picture->width * sizeof(uint32_t), y ? pic : FNV0);
    }
    if (want_shot)
        write_shot(frame);
    if (f_dump && (int)frame >= dump_first && (int)frame <= dump_last)
        pc_frame_write(f_dump, frame_rec);
    if (f_frames) {
        fprintf(f_frames, "%u ram %016llX pic %016llX now %llu lag %llu wr %u wh %016llX vr %d %d %u", (unsigned)frame, (unsigned long long)fnv(pt_ram, sizeof(pt_ram), FNV0),
                (unsigned long long)pic, (unsigned long long)now, (unsigned long long)pc_tick_max_lag(1), (unsigned)frame_rec->n_writes,
                (unsigned long long)fnv(frame_rec->writes, frame_rec->n_writes * sizeof(PcWrite), FNV0), pc_vread_first, pc_vread_last,
                (unsigned)DELTA(hv_reads));
        for (k = 0; k < watch_n; k++) {
            fprintf(f_frames, " w%d %d %u", k, watch_count[k] ? (int)watch_line[k] : -1, (unsigned)watch_count[k]);
            watch_count[k] = 0;
        }
        fprintf(f_frames, " st %llu dw %llu cw %llu dma %llu %llu mclk %llu stall %llu hi %llu vi %llu cr %llu cw2 %llu io %llu %llu z %llu psg %llu oth %llu %llu",
                DELTA(status_reads), DELTA(data_writes), DELTA(ctrl_writes), DELTA(dma_runs), DELTA(dma_words), DELTA(dma_mclk), DELTA(stall_mclk), DELTA(hints), DELTA(vints),
                DELTA(cart_reads), DELTA(cart_writes), DELTA(io_reads), DELTA(io_writes), DELTA(z80_writes), DELTA(psg_writes), DELTA(other_reads), DELTA(other_writes));
        if (drew)
            fprintf(f_frames, " over %u", (unsigned)picture->lines_over);
        {
            uint32_t i, vram_writes = 0;    /* VRAM words written during the picture: in play only the mid-frame sprite table swap does that */
            for (i = 0; i < frame_rec->n_writes; i++)
                vram_writes += frame_rec->writes[i].kind == PC_WRITE_VRAM;
            fprintf(f_frames, " vw %u", (unsigned)vram_writes);
        }
        for (k = 0; k < log_words_n; k++)
            fprintf(f_frames, " lw%d %u", k, (unsigned)pt_mem_r16(pt_ram, log_words[k] & 0xFFFEu));
        fputc('\n', f_frames);
    }
    if (f_cart)
        pc_cart_frame_line(f_cart, frame);
    if (f_mode)
        mode_log_frame(frame);
    last_counts = pc_counts;
    pc_vread_first = pc_vread_last = -1;
    frames_done = frame + 1u;
    if ((frame & 255u) == 255u) {
        if (f_frames) fflush(f_frames);
        if (f_cart) fflush(f_cart);
        if (route_log) fflush(route_log);
        if (run_dir) {
            printf("frame %u\n", (unsigned)frame);
            fflush(stdout);
        }
    }
    if (use_window)
        window_frame(frame);
    else if (paced) {
        uint64_t t0 = pc_fiber_host_time();
        pc_stall_part(PC_STALL_PACE);
        pace_wait();
        stat_add(&stat_wait, pc_fiber_host_time() - t0);
    }
    for (k = 0; k < test_save_changes_n; k++)
        if ((int)frame == test_save_changes[k])
            pc_cart_save_test_change();
    if ((frame & 63u) == 63u) {
        pc_stall_part(PC_STALL_SAVE);
        save_check();
    }
    pc_stall_part(PC_STALL_WORK);
    pads_for_frame((int)frame + 1);
    if (quit_asked) {
        run_done = 1;
        pc_tick_stop_request();
    }
    host_mark = pc_fiber_host_time();
    pc_stall_pass_end(frame + 1u, pc_cart_host_ticks());
    pc_audio_frame(frame + 1u);
    if ((int)frame == test_restart_frame) {
        test_restart_now = 1;
        pc_tick_stop_request();
    }
    for (k = 0; k < console_reset_frames_n; k++)
        if ((int)frame == console_reset_frames[k] && !pc_cart_holding())
            console_reset_asked = 1;
    if (console_reset_asked)
        pc_tick_stop_request();
    if ((int)frames_done >= frames_wanted) {
        run_done = 1;
        pc_tick_stop_request();
    }
}

/* ---- the console held by the cart: everything but the firmware off. Frames go on, blank. */
static void run_held(void)
{
    pc_vdp_reset();
    pc_io_reset();
    pc_addons_reset();
    pc_z80_reset();
    pc_tick_reset_handlers();
    memset(&render_memory, 0, sizeof(render_memory));
    while (pc_cart_holding() && !run_done) {
        pc_clock_stall(PC_MCLK_LINE);
        pc_tick_run_events();
        pc_cart_run_to(pc_clock_now());
    }
    pc_clock_stall(0);
}

/* The 68000 comes out of reset on what the cart RAM holds now. If that is not what was translated, the low page runs in the fallback. */
u32 pc_machine_restart(PtCtx *c)
{
    uint32_t bytes;
    const uint8_t *low = pc_cart_low(&bytes);
    memset(c, 0, sizeof(*c));
    c->imask = 7;
    pt_low_ram = low;
    pt_low_ram_end = bytes;
    pt_low_from_ram = memcmp(low, pt_rom, bytes) != 0;
    pc_tick_stop_clear();
    A(7) = IMG_R32(0);
    return IMG_R32(4);
}

/* ---- the console's reset button, the Game menu's Console Reset, as the Pocket core's 0.3.0 release presses it (its core_top.sv, the console_reset
   block; md_board's reset button into the bus arbiter). The 68000 alone is reset. Its reset and halt lines are held from the press until the button
   comes up and the cart's chip has acknowledged its console-reset interrupt. The core's button is 31 wraps of a counter that counts two a master clock
   and wraps at 32,768: 9.2 to 9.5 ms, by where the counter stands at the press; here the counter counts from power-on. The chip's firmware first writes
   the warm vector page into cart RAM (its core/boot/bootpage.c), then acknowledges. The bus arbiter's reset one-shot clears the Z80's reset register
   a moment after the press (0.64 to 0.88 ms on the Pocket core, by the phase of the arbiter's own divider; 0.64 ms here), so the Z80, and the FM chip
   on the same line, are held until the game lets them go. Everything else runs on as it was: the video chip and its sound generator, the I/O chip with
   what the ports hold and the stick's coins, work RAM, Z80 RAM, the add-ons and the cart. Then the 68000 takes its stack and its start from the warm
   page, as it does after the cart's hold. The game decides itself what that start shows; on the cart a reset in its boot's first moments locks it,
   and one later starts again at its terminal screen. A reset during the mini game of the game's first start locks it too, because the game clears
   its warm-start word when that mini game begins (console_locked below). */
#define CONSOLE_RESET_Z80_MCLK 34364u          /* 0.64 ms of master clocks; not a game address */
#define CONSOLE_RESET_COUNTER 32768u           /* the core's counter; not a game address */
/* How much of the program's C stack (64 MB, set where pc_build.py links the program) the 68000 may use after a Console Reset before the run takes it
   for the lock-up below: far more than the game's calls ever use, and far less than the whole. */
#define CONSOLE_RESET_STACK_ROOM (16u << 20)
/* The fatal halt the cart chip's watchdog makes when the game has sent it nothing for about 3.9 s (the firmware's core/wdog/wdog.c). */
#define CART_WATCHDOG_HALT 0x400000FAu

static u32 console_reset(PtCtx *c)
{
    uint64_t t0 = pc_clock_now(), z80_at = t0 + CONSOLE_RESET_Z80_MCLK;
    uint64_t up = ((t0 * 2u) / CONSOLE_RESET_COUNTER + 31u) * CONSOLE_RESET_COUNTER / 2u;
    int z80_done = 0;
    console_resets_done++;
    pc_tick_set_stack_floor(run_stack_top - CONSOLE_RESET_STACK_ROOM);
    printf("Console Reset at frame %u\n", (unsigned)frames_done);
    pc_cart_console_reset(t0);
    while (!run_done && (pc_clock_now() < up || pc_cart_console_reset_pending())) {
        uint64_t now = pc_clock_now(), step = PC_MCLK_LINE;
        if (!z80_done && z80_at > now && z80_at - now < step)
            step = z80_at - now;
        pc_clock_stall(step);
        pc_tick_run_events();
        pc_cart_run_to(pc_clock_now());
        if (!z80_done && pc_clock_now() >= z80_at) {
            pc_z80_console_reset();
            z80_done = 1;
        }
    }
    if (!z80_done)
        pc_z80_console_reset();
    if (console_reset_asked) {
        /* A press while this one was served, from the menu or a test: dropped, as the Pocket core drops it. */
        printf("Console Reset at frame %u: dropped, a Console Reset was still being served\n", (unsigned)frames_done);
        console_reset_asked = 0;
    }
    pc_clock_stall(0);
    pc_tick_reset_handlers();
    return pc_machine_restart(c);
}

/* After a Console Reset the 68000 starts on the vectors the cart's chip put back, and every one of them points into cart RAM at $206-$20E. On the
   cart, a reset while the game is zooming finds the zoom's stamp data there in place of the boot page's code (core/boot/bootpage.c), and the
   console locks up. Here the 68000 stops where it found no instruction it can run, and the rest runs on as it does on the cart: the picture stays as
   the video chip holds it, the Z80 and the FM chip are still held by the reset, the cart goes on. It stays so until the next Console Reset, a Soft
   Reset or the end.
   The other lock-up: a reset in the game's first seconds after power-on, or during the mini game of its first start (the game clears its warm-start
   word when that mini game begins), leaves the game in its own warm-start check, which calls the start again and again for ever, as on the cart, and
   the screen stays as it was. That loop makes no access to the video chip, the I/O chip, the Z80 or the sound
   chips and changes nothing in the cart; only its own stack in work RAM changes (measured over 1,700 frames of it). Its calls never return, and
   each keeps a C frame here, so the run stops the 68000 once it has used CONSOLE_RESET_STACK_ROOM (deep, from pc_tick_stack_deep) and holds the
   console as locked: from outside the 68000 the same as the loop going on. */
static u32 console_locked(PtCtx *c, int deep)
{
    console_locks++;
    if (deep)
        printf("the console locked up at frame %u, after a Console Reset: the 68000 calls on and on without returning, as the game's warm-start check "
               "does after a reset in its first seconds or in its first-start mini game\n", (unsigned)frames_done);
    else
        printf("the console locked up at frame %u, after a Console Reset: the 68000 found no instruction it can run\n", (unsigned)frames_done);
    while (!run_done && !console_reset_asked) {
        pc_clock_stall(PC_MCLK_LINE);
        pc_tick_run_events();
        pc_cart_run_to(pc_clock_now());
    }
    pc_clock_stall(0);
    if (run_done)
        return 0;
    console_reset_asked = 0;
    return console_reset(c);
}

/* ---- a fault in the machine or the firmware: say where, then stop. */
static LONG WINAPI on_fault(EXCEPTION_POINTERS *e)
{
    printf("RESULT: FAIL: fault %08lX at image offset %llX, reading or writing %p\n", (unsigned long)e->ExceptionRecord->ExceptionCode,
           (unsigned long long)((char *)e->ExceptionRecord->ExceptionAddress - (char *)GetModuleHandleA(NULL)),
           e->ExceptionRecord->NumberParameters > 1 ? (void *)e->ExceptionRecord->ExceptionInformation[1] : NULL);
    fflush(stdout);
    ExitProcess(5);
    return EXCEPTION_EXECUTE_HANDLER;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)n + 16);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static FILE *open_in_run(const char *name)
{
    char path[1200];
    if (!run_dir || no_logs)
        return NULL;
    snprintf(path, sizeof(path), "%s/%s", run_dir, name);
    return fopen(path, "w");
}

/* A name beside the program, unless it is already a full path. */
static void beside_program(char *out, size_t size, const char *name)
{
    char folder[1024];
    DWORD n;
    char *slash;
    if ((name[0] && name[1] == ':') || name[0] == '/' || name[0] == '\\') {
        snprintf(out, size, "%s", name);
        return;
    }
    n = GetModuleFileNameA(NULL, folder, sizeof(folder));
    if (!n || n >= sizeof(folder)) {
        snprintf(out, size, "%s", name);
        return;
    }
    slash = strrchr(folder, '\\');
    if (slash)
        *slash = 0;
    snprintf(out, size, "%s\\%s", folder, name);
}


/* A name from the settings file: a relative one starts from the settings file's own folder, and so does the save's default name. That folder is
 * the program's own unless --config names a file elsewhere, as the launcher does with the player's folder, so a launcher's player never has a
 * save written beside the programs. */
static void beside_settings(char *out, size_t size, const char *name)
{
    char folder[1100];
    char *slash, *other;
    if ((name[0] && name[1] == ':') || name[0] == '/' || name[0] == '\\' || !config_full[0]) {
        beside_program(out, size, name);
        return;
    }
    snprintf(folder, sizeof(folder), "%s", config_full);
    slash = strrchr(folder, '\\');
    other = strrchr(folder, '/');
    if (other && (!slash || other > slash))
        slash = other;
    if (!slash) {
        beside_program(out, size, name);
        return;
    }
    *slash = 0;
    snprintf(out, size, "%s\\%s", folder, name);
}

/* The Soft Reset, once this run has ended, saved and closed its window: the players' build tells its launcher, which starts the game again as it
   started it (exit code PC_EXIT_SOFT_RESET); the developers' build starts this program again itself, with the same command line but a test's key script
   (which belongs to the first run only), and ends. Returns the exit code. The code is one nothing else in the program ends with: 7 is a fault of the
   cart's chip program (pc_cart.c), which the launcher reports as a fault, and must never become a silent restart. */
#define PC_EXIT_SOFT_RESET 8

/* One argument added to a Windows command line, quoted so that CommandLineToArgvW (and the C runtime, which reads argv the same way) gives it back as it
   was: the backslashes just before a quote, or before the closing quote, are doubled, and a quote gets a backslash; other backslashes stay single. So
   a folder given as C:\games\ comes back whole and does not swallow the arguments after it. Returns 0, or -1 when the line has no room for it. */
static int command_line_add(wchar_t *line, size_t room, size_t *used, const wchar_t *arg)
{
    size_t n = *used, slashes = 0, k, times;
    const wchar_t *p;
    if (n + 3 > room)
        return -1;
    if (n)
        line[n++] = L' ';
    line[n++] = L'"';
    for (p = arg;; p++) {
        if (*p == L'\\') {
            slashes++;
            continue;
        }
        times = *p == L'"' || *p == 0 ? 2 * slashes : slashes;
        if (n + times + 3 > room)
            return -1;
        for (k = 0; k < times; k++)
            line[n++] = L'\\';
        slashes = 0;
        if (*p == 0)
            break;
        if (*p == L'"')
            line[n++] = L'\\';
        line[n++] = *p;
    }
    line[n++] = L'"';
    line[n] = 0;
    *used = n;
    return 0;
}

static int soft_reset(void)
{
    static wchar_t line[8192], exe[1100];
    const wchar_t *at = GetCommandLineW();
    size_t used = 0;
    int argc = 0, i;
    wchar_t **argv;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (started_by_launcher) {
        printf("Soft Reset: the launcher starts the game again\n");
        fflush(stdout);
        return PC_EXIT_SOFT_RESET;
    }
    argv = CommandLineToArgvW(at, &argc);
    if (!argv || !GetModuleFileNameW(NULL, exe, 1100))
        return 0;
    line[0] = 0;
    for (i = 0; i < argc; i++) {
        if (i && !wcscmp(argv[i], L"--key-script") && i + 1 < argc) {
            i++;
            continue;
        }
        if (command_line_add(line, sizeof(line) / sizeof(line[0]), &used, i ? argv[i] : exe) != 0) {
            LocalFree(argv);
            printf("Soft Reset: the command line is too long to start the program again; start it again by hand\n");
            fflush(stdout);
            return 0;
        }
    }
    LocalFree(argv);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("Soft Reset: the program could not start again (error %lu); start it again by hand\n", (unsigned long)GetLastError());
        fflush(stdout);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("Soft Reset: the program started again as process %lu\n", (unsigned long)pi.dwProcessId);
    fflush(stdout);
    return 0;
}

/* A player's error: to the console, and to a message box when the window is meant to be seen. */
static int player_error(const char *text, int code)
{
    printf("%s\n", text);
    fflush(stdout);
    if (!window_hidden && use_window)
        pc_window_message(text);
    return code;
}

/* The save could not be written at quit: say so where the player will see it, with the file's name and where the newest bytes are. The console closes with the program, so it gets the message box too. */
static void save_failed_message(void)
{
    char text[3000];
    size_t used = 0;
    if (pc_save_copy_failed())
        used = (size_t)snprintf(text, sizeof(text), "The save %s was not replaced, because a copy of %s%s could not be written to %s%s first. ", save_store_path, save_store_path, pc_save_copy_source(), save_store_path,
                                pc_save_copy_ending());
    else
        used = (size_t)snprintf(text, sizeof(text), "The save could not be written to %s. ", save_store_path);
    used += (size_t)snprintf(text + used, sizeof(text) - used, "That file still holds the last save that went through. ");
    if (pc_save_newest_in_temp())
        snprintf(text + used, sizeof(text) - used, "The newest save is in %s.tmp. With the game closed, copy that file over it to keep your progress. Another program may be holding the file, or it may be read only.", save_store_path);
    else
        snprintf(text + used, sizeof(text) - used, "The newest progress did not reach the disk. The disk may be full, or the folder read only.");
    player_error(text, 0);
}

/* ---- the freeze log: see pc_stall.h. It starts just before the first frame, so a run that is refused writes nothing. */
static const char *stall_log_path;
static int stall_log_off, stall_limit_ms;
static char sound_words[300] = "no device";

/* The start-up's parts, read from the PC's timer as they end, for one line at the top of the run's block: what can hold the first sound back. The sound device opens before the cart's boot
   and the window, so its silence until the first frame is all of those. Host time only: nothing here reaches the machine. */
static uint64_t start_program, start_sound_open, start_sound_done, start_boot_done, start_window_done, start_input_done;
static int sound_opened;

static void start_up_line(void)
{
    char line[600];
    double sdl_ms = 0, renderer_ms = 0;
    size_t used;
    uint64_t first = pc_fiber_host_time(), from = sound_opened ? start_sound_done : start_program;
    if (use_window)
        pc_window_open_times(&sdl_ms, &renderer_ms);
    if (sound_opened)
        used = (size_t)snprintf(line, sizeof(line), "start-up: the sound device opened %lld ms after the program started and took %lld ms; after it, ", ms_of_ticks((int64_t)(start_sound_open - start_program)),
                                ms_of_ticks((int64_t)(start_sound_done - start_sound_open)));
    else
        used = (size_t)snprintf(line, sizeof(line), "start-up: no sound device; ");
    snprintf(line + used, sizeof(line) - used, "the cart's boot took %lld ms, SDL's start for the window and the controllers %.0f ms, making the window and its renderer %.0f ms, the keys and controllers %lld ms, and the first frame "
             "began %lld ms after %s", ms_of_ticks((int64_t)(start_boot_done - start_sound_done)), sdl_ms, renderer_ms, ms_of_ticks((int64_t)(start_input_done > start_window_done ? start_input_done - start_window_done : 0)),
             ms_of_ticks((int64_t)(first - from)), sound_opened ? "the device opened" : "the program started");
    pc_stall_header(line);
    printf("%s\n", line);
}

static void stall_log_start(void)
{
    char path[1300], text[700], line[800];
    if (stall_log_off)
        return;
    if (stall_log_path)
        snprintf(path, sizeof(path), "%s", stall_log_path);
    else if (run_dir)
        snprintf(path, sizeof(path), "%s/stalls.txt", run_dir);
    else
        beside_program(path, sizeof(path), "stalls.txt");
    if (use_window) {
        int k;
        snprintf(line, sizeof(line), "renderer %s", pc_window_renderer());
        pc_stall_first_line(line);
        pc_window_displays(text, sizeof(text));
        snprintf(line, sizeof(line), "monitor: %s", text);
        pc_stall_header(line);
        pc_window_describe(text, sizeof(text));
        snprintf(line, sizeof(line), "renderer: %s", text);
        pc_stall_header(line);
        for (k = 0; k < pc_window_fallbacks(); k++)
            pc_stall_header(pc_window_fallback(k));
        pc_window_gpu(text, sizeof(text));
        snprintf(line, sizeof(line), "gpu: %s", text);
        pc_stall_header(line);
        pc_stall_adapters(text, sizeof(text));
        snprintf(line, sizeof(line), "adapters: %s", text);
        pc_stall_header(line);
    } else {
        pc_stall_header("monitor: no window");
    }
    snprintf(line, sizeof(line), "audio: %s", sound_words);
    pc_stall_header(line);
    start_up_line();
    pc_stall_state(use_window ? pc_window_vsync_on() : -1, 0);
    pc_stall_line_source(pc_audio_take_line);
    pc_stall_start(path, stall_limit_ms, &pc_cart_in_turn, use_window ? pc_window_native() : NULL, 0u);
}

/* Closing the console window, or Ctrl-C there: ask the run to stop, so the save gets written, and give it a moment. */
static BOOL WINAPI on_console_close(DWORD kind)
{
    (void)kind;
    InterlockedExchange(&quit_asked, 1);
    Sleep(1500);
    return TRUE;
}

/* The sound's way out. Whatever happens here, the game goes on: a device that will not open only means no sound. A headless run never starts SDL. */
static void sound_start(const char *device, int headless, int hidden, const PcConfig *config)
{
    char error[300];
    /* The volume the settings ask for, so the menu ticks it even when no device opens; a device that opens takes it again. */
    pc_audio_set_volume(config->volume);
    if (!device)
        device = headless ? "none" : hidden ? "dummy" : config->sound ? "sdl" : "none";
    if (!strcmp(device, "none")) {
        snprintf(sound_words, sizeof(sound_words), "%s", headless ? "no device, a run with no window" : "no device, the configuration says sound=off");
        return;
    }
    if (hidden && !strcmp(device, "sdl"))
        device = "dummy";       /* a hidden window's run never makes a sound on anyone's desktop */
    if (headless && (!strcmp(device, "sdl") || !strcmp(device, "dummy"))) {
        printf("a headless run has no SDL, so no sound device %s\n", device);
        snprintf(sound_words, sizeof(sound_words), "no device, a run with no window");
        return;
    }
    if (pc_audio_open(device, config->volume, error, sizeof(error)) != 0) {
        printf("no sound: %s\n", error);
        snprintf(sound_words, sizeof(sound_words), "no device: %s", error);
        return;
    }
    pc_audio_describe(sound_words, sizeof(sound_words));
    pc_mix_to_device(pc_audio_push);
    sound_opened = 1;
}

/* The console the game is to find, from the settings (pc_console.h), as the cart powers on. With CPU MATE on, the brain comes from Little-Man Firmware's file,
 * which the players' download carries: this program carries none of it (pc_brain.h). Without it the game still offers CPU MATE and the partner stands still, as the Pocket's
 * firmware built without the brain does, and the console says why, and so does the window when it opens. A Soft Reset from the menu bar starts the whole
 * program again (soft_reset), so this runs again with the settings as they then are; the brain is read only while none is ready, and the cart's link
 * starts afresh. */
static const char *test_brain_path, *cpumate_trace_path;

/* ---- Where CPU MATE's brain file is looked for.
 *
 * The team's decision: with no cpumate_brain line, the player's little_man_firmware.bin is looked for in two places, in this order:
 *   1. beside the program the player started, in the folder the player unzipped. In the players' build the launcher, Paprium.exe, starts this
 *      program, pc_port.exe, from the launcher's own folder (pl_launcher.c joins that folder and the package's game_exe, a bare file name), so
 *      this program's folder is Paprium.exe's. In the developers' build, where pc_port.exe is started by itself, it is pc_port.exe's folder.
 *   2. beside the settings file, paprium_pc.cfg. For the players' build that is the player's files folder: %LOCALAPPDATA%\Paprium PC, or my_copy
 *      beside Paprium.exe when that folder cannot be written (the launcher names the settings file with --config). In the developers' build the
 *      settings file is beside the program unless --config names another, so the two places are often one folder, and it is looked at once.
 * The first place where anything of that name is there is the one used, even when it turns out to be the wrong file, cut short or unreadable: the
 * window then says so and names that place, and the second place is not looked at while that file is there. So what happens never depends on a
 * file in a place the window did not name. A cpumate_brain line names one place instead of both; a relative path in it starts from the settings
 * file's folder. This program only reads the file: it never writes into its own folder. */
typedef struct BrainPlaces {
    int n;                      /* how many places are looked at: 1 or 2 */
    int named;                  /* the one place is a cpumate_brain line's */
    char path[2][1300];         /* each place's whole path, in the order they are looked at */
    char words[2][1300];        /* each place in a few words a player knows ("beside Paprium.exe"); for a cpumate_brain line, the line as written */
    char note[300];             /* for the console: a place that could not be looked at, and why */
} BrainPlaces;

/* The folder of this program's own file, and that file's name. Returns 0, or -1 when Windows does not give it. */
static int program_folder(char *folder, size_t size, char *name, size_t name_size)
{
    char raw[2100], *slash;
    DWORD n = GetModuleFileNameA(NULL, raw, sizeof(raw));
    if (!n || n >= sizeof(raw) || !(slash = strrchr(raw, '\\')))
        return -1;
    *slash = 0;
    if (strlen(raw) >= size)
        return -1;
    snprintf(name, name_size, "%s", slash + 1);
    snprintf(folder, size, "%s", raw);
    return 0;
}

/* A path in Windows' full form (back slashes, no "." or ".." parts), as the launcher names its folders, so two names of one folder compare equal. */
static void full_form(const char *path, char *out, size_t size)
{
    DWORD n = GetFullPathNameA(path, (DWORD)size, out, NULL);
    if (!n || n >= size)
        snprintf(out, size, "%s", path);
}

static void cut_name(char *path)
{
    char *slash = strrchr(path, '\\');
    if (slash)
        *slash = 0;
}

static void brain_places(const PcConsole *console, BrainPlaces *bp)
{
    char raw[1300], settings_folder[1300], program[1300], own_name[300], mine[1100], app[1300], copy[1300];
    DWORD n;
    memset(bp, 0, sizeof(*bp));
    if (console->brain[0]) {
        bp->n = 1;
        bp->named = 1;
        beside_settings(raw, sizeof(raw), console->brain);
        full_form(raw, bp->path[0], sizeof(bp->path[0]));
        snprintf(bp->words[0], sizeof(bp->words[0]), "%s", console->brain);
        return;
    }
    /* The second place: beside the settings file, named as the players' launcher keeps it, or as the folder of paprium_pc.cfg. */
    beside_settings(raw, sizeof(raw), PC_BRAIN_DEFAULT);
    full_form(raw, bp->path[1], sizeof(bp->path[1]));
    snprintf(settings_folder, sizeof(settings_folder), "%s", bp->path[1]);
    cut_name(settings_folder);
    snprintf(bp->words[1], sizeof(bp->words[1]), "beside paprium_pc.cfg");
    n = GetEnvironmentVariableA("LOCALAPPDATA", mine, sizeof(mine));
    if (n && n < sizeof(mine) && strlen(mine) + 12 < sizeof(mine)) {
        strcat(mine, "\\Paprium PC");
        full_form(mine, app, sizeof(app));
        if (!_stricmp(app, settings_folder))
            snprintf(bp->words[1], sizeof(bp->words[1]), "in %%LOCALAPPDATA%%\\Paprium PC");
    }
    beside_program(raw, sizeof(raw), "my_copy");
    full_form(raw, copy, sizeof(copy));
    if (!_stricmp(copy, settings_folder))
        snprintf(bp->words[1], sizeof(bp->words[1]), "in my_copy, beside Paprium.exe");
    /* The first place: beside the program the player started. */
    if (program_folder(program, sizeof(program), own_name, sizeof(own_name)) != 0) {
        snprintf(bp->note, sizeof(bp->note), "Windows did not give this program's own folder, so only the place beside the settings file is looked at");
        memmove(bp->path[0], bp->path[1], sizeof(bp->path[1]));
        memmove(bp->words[0], bp->words[1], sizeof(bp->words[1]));
        bp->n = 1;
        return;
    }
    snprintf(bp->path[0], sizeof(bp->path[0]), "%s\\%s", program, PC_BRAIN_DEFAULT);
    snprintf(bp->words[0], sizeof(bp->words[0]), "beside %s", started_by_launcher ? "Paprium.exe" : own_name);
    bp->n = _stricmp(program, settings_folder) ? 2 : 1;
}

/* The file of place i in a few words: "little_man_firmware.bin beside Paprium.exe", or a cpumate_brain line as written. */
static const char *brain_file_words(const BrainPlaces *bp, int i, char *out, size_t size)
{
    if (bp->named)
        snprintf(out, size, "%s", bp->words[0]);
    else
        snprintf(out, size, PC_BRAIN_DEFAULT " %s", bp->words[i]);
    return out;
}

/* Which place is used: the index of the first place where anything of that name is there (a file, or a folder, which pc_brain_load reads as the
   brain's three files), or -1 when no place has one. A place Windows could not even look at (a folder on the way this user may not open, a drive
   that is not there) ends the search too, since the file may well be there: -2 - its index, with Windows' error code in *error. */
static int brain_pick(const BrainPlaces *bp, DWORD *error)
{
    int i;
    for (i = 0; i < bp->n; i++) {
        if (GetFileAttributesA(bp->path[i]) != INVALID_FILE_ATTRIBUTES)
            return i;
        *error = GetLastError();
        if (*error == ERROR_SHARING_VIOLATION)
            return i;               /* there, but held by another program: pc_brain_load says it cannot be read */
        if (*error != ERROR_FILE_NOT_FOUND && *error != ERROR_PATH_NOT_FOUND)
            return -2 - i;
    }
    return -1;
}

static void system_words(DWORD code, char *out, size_t size)
{
    int n = (int)FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, out, (DWORD)size, NULL);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '.'))
        out[--n] = 0;
    if (n <= 0)
        snprintf(out, size, "Windows error %lu", (unsigned long)code);
}

/* The menu's words, when CPU MATE is switched on, for where its brain will be read at the next Soft Reset, as the file stands now. The file is not
   read here, so a file that is there is named by its place only: the next start says whether it is the right one. */
static void brain_menu_words(const PcConsole *console, char *out, size_t size)
{
    BrainPlaces bp;
    DWORD error = 0;
    char file[1400];
    int at;
    brain_places(console, &bp);
    at = brain_pick(&bp, &error);
    if (at >= 0)
        snprintf(out, size, "Its brain is read from %s.", brain_file_words(&bp, at, file, sizeof(file)));
    else if (at <= -2)
        snprintf(out, size, "Its brain is read from %s, which could not be reached just now.", brain_file_words(&bp, -2 - at, file, sizeof(file)));
    else if (bp.named)
        snprintf(out, size, "Its brain is read from %s, the place the cpumate_brain line names; no file is there now.", bp.words[0]);
    else if (bp.n == 2)
        snprintf(out, size, "Its brain comes from your little_man_firmware.bin (Little-Man Firmware 1.0): put it %s or %s first.", bp.words[0], bp.words[1]);
    else
        snprintf(out, size, "Its brain comes from your little_man_firmware.bin (Little-Man Firmware 1.0): put it %s first.", bp.words[0]);
}

static void console_start(const PcConsole *console)
{
    static int trace_open;
    PcMachine mach;
    pc_console_machine(console, &mach);
    if (cpumate_trace_path && !trace_open) {
        FILE *f = fopen(cpumate_trace_path, "w");
        trace_open = 1;
        if (f)
            pc_addons_trace(f);
    }
    if (console->cpumate && mach.cpumate_unit && pc_brain_ready()) {
        /* read already */
    } else if (console->cpumate && mach.cpumate_unit && test_brain_path) {
        char why[1500];
        if (pc_brain_load_test(test_brain_path, why, sizeof(why)) != 0)
            printf("CPU MATE: the test's brain: %s\n", why);
    } else if (console->cpumate && !mach.cpumate_unit) {
        pc_addons_set_brain_status(mach.dropped ? "CPU MATE needs a Mega CD, and this console has no port for one: pick another console or a Mega CD that is "
                                                  "a whole console, then Soft Reset" :
                                   mach.unit == PC_MEGACD_UNKNOWN ? "CPU MATE does nothing with an Unknown CD, which the game does not know: pick another "
                                                                    "Mega CD unit (Console menu), then Soft Reset" :
                                   "CPU MATE needs a Mega CD: Console menu, Mega CD, then Soft Reset (cpumate=on works with a megacd= unit the game knows)");
        printf("%s\n", pc_addons_brain_status());
    } else if (console->cpumate) {
        /* The console (and game_log.txt, from the launcher) gets the whole path and why that place; the window and the menu get the file and its
           place in a few words (brain_places), which READ_ME.txt names the same way. */
        BrainPlaces bp;
        DWORD error = 0;
        char why[1500], text[4400], brief[3000], file[1400], other[1400], words[400];
        const char *log = started_by_launcher ? "game_log.txt says why." : "The console says why.";
        int at, rc = -1, two;
        brain_places(console, &bp);
        if (bp.note[0])
            printf("CPU MATE: %s.\n", bp.note);
        at = brain_pick(&bp, &error);
        two = bp.n == 2;
        if (at >= 0) {
            rc = pc_brain_load(bp.path[at], why, sizeof(why));
            if (rc == -1) {
                /* pc_brain_load found nothing it could open. Say that the file went away only when Windows no longer sees it either. */
                DWORD gone = GetFileAttributesA(bp.path[at]) == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
                if (gone == ERROR_FILE_NOT_FOUND || gone == ERROR_PATH_NOT_FOUND) {
                    snprintf(why, sizeof(why), "%s was there, then went away before it was read", bp.path[at]);
                } else {
                    snprintf(why, sizeof(why), "%s is there but could not be opened", bp.path[at]);
                    rc = -3;
                }
            }
        }
        if (at >= 0 && rc == 0) {
            if (bp.named)
                snprintf(text, sizeof(text), "CPU MATE: the partner's brain from %s, the place the cpumate_brain line names", bp.path[0]);
            else if (two && at == 0)
                snprintf(text, sizeof(text), "CPU MATE: the partner's brain from %s, %s: the first of the two places looked at", bp.path[0], bp.words[0]);
            else if (two)
                snprintf(text, sizeof(text), "CPU MATE: the partner's brain from %s, %s: nothing of that name is at %s (%s), the first place looked at",
                         bp.path[1], bp.words[1], bp.path[0], bp.words[0]);
            else
                snprintf(text, sizeof(text), "CPU MATE: the partner's brain from %s, %s", bp.path[0], bp.words[0]);
            snprintf(brief, sizeof(brief), "CPU MATE: the partner's brain is read from %s.", brain_file_words(&bp, at, file, sizeof(file)));
        } else if (at >= 0) {
            /* Something is there: the place is used, and the other is not looked at. A file that cannot be read may well be the right one: another program
               holds it, or Windows denies it. rc -1 is left only for a file that Windows no longer sees: it went away between the look and the read. */
            snprintf(text, sizeof(text), "CPU MATE: %s, so a partner who joins stands still%s%s%s", why, two && at == 0 ? ". It is the first place looked at, so " : "",
                     two && at == 0 ? bp.path[1] : "", two && at == 0 ? " is not looked at while it is there" : "");
            snprintf(brief, sizeof(brief), "CPU MATE: %s %s, so a partner stands still. %s%s%s%s", brain_file_words(&bp, at, file, sizeof(file)),
                     rc == -3 ? "is there but could not be read" : rc == -1 ? "went away while it was read" : "is not the Little-Man Firmware 1.0 file with the brain",
                     log, two && at == 0 ? " While that file is there, " : "", two && at == 0 ? brain_file_words(&bp, 1, other, sizeof(other)) : "",
                     two && at == 0 ? " is not used." : "");
        } else if (at <= -2) {
            int i = -2 - at;
            system_words(error, words, sizeof(words));
            snprintf(text, sizeof(text), "CPU MATE: %s could not be reached (%s), so a partner who joins stands still%s%s%s", bp.path[i], words,
                     two && i == 0 ? ", and " : "", two && i == 0 ? bp.path[1] : "", two && i == 0 ? " is not looked at while the first place cannot be" : "");
            snprintf(brief, sizeof(brief), "CPU MATE: %s could not be reached, so a partner stands still. %s", brain_file_words(&bp, i, file, sizeof(file)), log);
        } else if (bp.named) {
            snprintf(text, sizeof(text), "CPU MATE: no brain at %s, the place the cpumate_brain line of %s names, so a partner who joins stands still. The brain "
                     "comes with Little-Man Firmware 1.0: copy your little_man_firmware.bin to that path, or take the line out", bp.path[0], config_name());
            snprintf(brief, sizeof(brief), "CPU MATE: no file at %s, the place the cpumate_brain line names, so a partner stands still. Copy your "
                     "little_man_firmware.bin (Little-Man Firmware 1.0) to that path, or take the line out, then Soft Reset.", bp.words[0]);
        } else if (two) {
            snprintf(text, sizeof(text), "CPU MATE: no brain: nothing of that name at %s (%s) or at %s (%s), so a partner who joins stands still. The brain comes "
                     "with Little-Man Firmware 1.0: put your little_man_firmware.bin in either place, or name it in paprium_pc.cfg with cpumate_brain=",
                     bp.path[0], bp.words[0], bp.path[1], bp.words[1]);
            snprintf(brief, sizeof(brief), "CPU MATE: no little_man_firmware.bin %s or %s, so a partner stands still. Put your little_man_firmware.bin "
                     "(Little-Man Firmware 1.0) in either place, then Soft Reset.", bp.words[0], bp.words[1]);
        } else {
            snprintf(text, sizeof(text), "CPU MATE: no brain at %s, so a partner who joins stands still. The brain comes with Little-Man Firmware 1.0: put your "
                     "little_man_firmware.bin there, or name it in paprium_pc.cfg with cpumate_brain=", bp.path[0]);
            snprintf(brief, sizeof(brief), "CPU MATE: no little_man_firmware.bin %s, so a partner stands still. Put your little_man_firmware.bin (Little-Man "
                     "Firmware 1.0) there, then Soft Reset.", bp.words[0]);
        }
        pc_addons_set_brain_status(brief);
        printf("%s\n", text);
    }
    pc_addons_configure(console);
    pc_addons_power_on();
    if (pc_addons_on || mach.teradrive || mach.model) {
        char rules[400];
        console_rule_words(console, 0, rules, sizeof(rules));
        printf("the console: %s, version register %02X%s%s%s%s%s\n", pc_console_name(console), pc_machine_version(&mach), mach.x32 ? ", a 32X" : "",
               mach.unit != PC_MEGACD_NONE && !mach.combined ? (mach.unit == PC_MEGACD_UNKNOWN ? ", an " : ", a ") : "",
               mach.unit != PC_MEGACD_NONE && !mach.combined ? megacd_titles[mach.unit] : "",
               rules[0] ? ". " : "", rules);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    static PcCartConfig cart;
    static uint8_t save[PC_SAVE_BYTES];
    static PcConfig config;
    static char rom_full[1100], save_full[1100];
    const char *rom_path = NULL, *pad_path = NULL, *route_path = NULL, *route_arg = NULL, *save_path = NULL, *save_raw = NULL, *save_out = NULL, *image_check = NULL, *dump_path = NULL;
    const char *config_path = NULL, *key_script_path = NULL, *sound_device = NULL, *test_renderer_fail = NULL, *cheats_arg = NULL, *stage_arg = NULL;
    const char *arcade_args[4] = {NULL, NULL, NULL, NULL};      /* --arcade-mode, --arcade-coins, --attract-only, --three-players */
    static const char *const arcade_lines[4] = {"arcade_mode", "arcade_coins", "attract_only", "three_players"};
    const char *console_args[16];
    int console_args_n = 0;
    int headless = 0, no_limit_arg = 0, test_window_w = 0, test_window_h = 0;
    PtCtx *c = &pc_ctx;
    size_t rom_size = 0;
    uint8_t *file;
    int i, stopped = 0;
#if defined(PT_GAME_FROM_DLL)
    const char *game_dll_path = NULL;
    int interpreter_only = 0, from_launcher = 0;
#endif
    u32 pc;
    uint64_t t0, t1;
    FILE *report;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom")) rom_path = argv[++i];
        else if (!strcmp(argv[i], "--run-dir")) run_dir = argv[++i];
        else if (!strcmp(argv[i], "--frames")) frames_wanted = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pad")) pad_path = argv[++i];
        else if (!strcmp(argv[i], "--route")) route_path = argv[++i];
        else if (!strcmp(argv[i], "--route-arg")) route_arg = argv[++i];
        else if (!strcmp(argv[i], "--save")) save_path = argv[++i];
        else if (!strcmp(argv[i], "--save-raw")) save_raw = argv[++i];
        else if (!strcmp(argv[i], "--save-out")) save_out = argv[++i];
        else if (!strcmp(argv[i], "--image-check")) image_check = argv[++i];
        else if (!strcmp(argv[i], "--fw-options")) cart.options = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--fw-step")) cart.step = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fw-no-bank")) cart.no_bank = 1;
        else if (!strcmp(argv[i], "--three-button")) { int port = atoi(argv[++i]); if (port >= 0 && port <= 2) three_button_arg[port] = 1; }
        else if (!strcmp(argv[i], "--dump-frames")) dump_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-range")) { if (sscanf(argv[++i], "%d-%d", &dump_first, &dump_last) != 2) return 2; }
        else if (!strcmp(argv[i], "--shot")) { if (shots_n < 256) shots[shots_n++] = atoi(argv[++i]); else i++; }
        else if (!strcmp(argv[i], "--render")) render_all = 1;
        else if (!strcmp(argv[i], "--fallback-log")) pt_fallback_log_path = argv[++i];
        else if (!strcmp(argv[i], "--test-restart")) test_restart_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--data-patch") && data_patches_n < 16) data_patches[data_patches_n++] = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--cheats")) cheats_arg = argv[++i];
        else if (!strcmp(argv[i], "--menu-dump")) menu_dump_asked = 1;
        else if (!strcmp(argv[i], "--arcade-stage")) stage_arg = argv[++i];
        else if (!strcmp(argv[i], "--arcade-mode")) arcade_args[0] = argv[++i];
        else if (!strcmp(argv[i], "--arcade-coins")) arcade_args[1] = argv[++i];
        else if (!strcmp(argv[i], "--attract-only")) arcade_args[2] = argv[++i];
        else if (!strcmp(argv[i], "--three-players")) arcade_args[3] = argv[++i];
        else if (!strcmp(argv[i], "--console-reset")) { if (console_reset_frames_n < 16) console_reset_frames[console_reset_frames_n++] = atoi(argv[++i]); else return 2; }
        else if (!strcmp(argv[i], "--mode-log")) {
            f_mode = fopen(argv[++i], "w");
            if (!f_mode)
                return 2;
        }
        else if (!strcmp(argv[i], "--io-log")) {
            FILE *f = fopen(argv[++i], "w");
            if (!f) {
                fprintf(stderr, "cannot write %s\n", argv[i]);
                return 2;
            }
            pc_io_log(f);
        }
#if defined(PT_GAME_FROM_DLL)
        else if (!strcmp(argv[i], "--game-dll")) game_dll_path = argv[++i];
        else if (!strcmp(argv[i], "--interpreter")) interpreter_only = 1;
        else if (!strcmp(argv[i], "--from-launcher")) from_launcher = started_by_launcher = 1;
#endif
        else if (!strcmp(argv[i], "--hidden-window")) { use_window = 1; window_hidden = 1; }
        else if (!strcmp(argv[i], "--config")) config_path = argv[++i];
        else if (!strcmp(argv[i], "--no-limit")) no_limit_arg = 1;
        else if (!strcmp(argv[i], "--flip-limits")) flip_limits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-every")) shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-free")) shot_free = 1;
        else if (!strcmp(argv[i], "--no-logs")) no_logs = 1;
        else if (!strcmp(argv[i], "--sound-file")) pc_sound_file_open(argv[++i]);
        else if (!strcmp(argv[i], "--sound-log")) pc_sound_log_open(argv[++i]);
        else if (!strcmp(argv[i], "--sound-device")) sound_device = argv[++i];
        else if (!strcmp(argv[i], "--sound-no-hold")) pc_mix_no_hold();
        else if (!strcmp(argv[i], "--paced")) paced = 1;
        else if (!strcmp(argv[i], "--key-script")) key_script_path = argv[++i];
        else if (!strcmp(argv[i], "--window-shot")) { if (window_shots_n < 64) window_shots[window_shots_n++] = atoi(argv[++i]); else i++; }
        else if (!strcmp(argv[i], "--pause-shot")) { if (pause_shots_n < 16) pause_shots[pause_shots_n++] = atoi(argv[++i]); else i++; }
        else if (!strcmp(argv[i], "--controls-db")) pc_input_set_controller_db(argv[++i]);
        else if (!strcmp(argv[i], "--log-word")) { if (log_words_n < LOG_WORDS_MAX) log_words[log_words_n++] = (uint32_t)strtoul(argv[++i], NULL, 16); else i++; }
        else if (!strcmp(argv[i], "--stall-log")) stall_log_path = argv[++i];
        else if (!strcmp(argv[i], "--no-stall-log")) stall_log_off = 1;
        else if (!strcmp(argv[i], "--stall-limit")) stall_limit_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--test-renderer-fail")) test_renderer_fail = argv[++i];
        else if (!strcmp(argv[i], "--console")) { if (console_args_n < 16) console_args[console_args_n++] = argv[++i]; else return 2; }
        else if (!strcmp(argv[i], "--test-brain")) test_brain_path = argv[++i];
        else if (!strcmp(argv[i], "--cpumate-trace")) cpumate_trace_path = argv[++i];
        else if (!strcmp(argv[i], "--test-monitor")) { test_monitor_hz = atof(argv[++i]); if (test_monitor_hz < 1.0 || test_monitor_hz > 1000.0) return 2; }
        else if (!strcmp(argv[i], "--test-window")) { if (sscanf(argv[++i], "%dx%d", &test_window_w, &test_window_h) != 2) return 2; }
        else if (!strcmp(argv[i], "--test-save-change")) { if (test_save_changes_n < TEST_SAVE_CHANGES) test_save_changes[test_save_changes_n++] = atoi(argv[++i]); else i++; }
        else if (!strcmp(argv[i], "--test-stall")) {
            int first, last, ms;
            char part[32];
            const char *arg = argv[++i];
            if (sscanf(arg, "%d-%d:%31[^:]:%d", &first, &last, part, &ms) != 4) {
                if (sscanf(arg, "%d:%31[^:]:%d", &first, part, &ms) != 3)
                    return 2;
                last = first;
            }
            if (pc_stall_test_plant(first, last, part, ms) < 0)
                return 2;
        }
        else if (argv[i][0] != '-' && !rom_path) rom_path = argv[i];    /* a game file dropped on the program, or named after it */
        else {
            fprintf(stderr, "bad argument %s\n", argv[i]);
            return 2;
        }
    }
    /* With a run folder, or asked for, this is a headless test run, unless --hidden-window said otherwise. With neither, a player's run with the window. */
    if (!use_window) {
        if (run_dir)
            headless = 1;
        if (!headless)
            use_window = 1;
    } else {
        headless = 0;
    }
    SetUnhandledExceptionFilter(on_fault);
    QueryPerformanceFrequency((void *)&host_freq);
    start_program = pc_fiber_host_time();
    pace_period = (uint64_t)((double)host_freq * 3420.0 * 262.0 / 53693175.0 + 0.5);

    pc_config_defaults(&config);
    if (!headless) {
        char error[1300];
        int result;
        beside_program(config_full, sizeof(config_full), config_path ? config_path : "paprium_pc.cfg");
        result = pc_config_load(config_full, &config, error, sizeof(error));
        if (result == -1 && config_path) {
            printf("no configuration file %s\n", config_full);
            return 2;
        }
        /* A settings file that is there but cannot be read (held by another program, a folder of that name) is told in Windows' words, as a line
           the game does not know is: the launcher leaves the game's refusals to the game. */
        if (result > 0 || result == -2)
            return player_error(error, 2);
        pc_controls_set_file(config_full);      /* the controls screen saves here, a file made if there is none */
        if (!rom_path && config.rom[0]) {
            beside_settings(rom_full, sizeof(rom_full), config.rom);
            rom_path = rom_full;
        }
        if (!rom_path)
#if defined(PT_GAME_FROM_DLL)
            return player_error("Start Paprium.exe, the launcher beside this program: it takes your own copy of the game file and builds the game from it.", 2);
#else
            return player_error("Name your own copy of the game: put a line rom=PATH in paprium_pc.cfg beside this program, or drop the file on the program.", 2);
#endif
    }
    if (!rom_path || (headless && !run_dir)) {
        fprintf(stderr, "need --rom and --run-dir\n");
        return 2;
    }
    for (i = 0; i < console_args_n; i++) {
        char name[32];
        const char *equals = strchr(console_args[i], '=');
        if (!equals || equals - console_args[i] >= (int)sizeof(name)) {
            fprintf(stderr, "bad --console %s\n", console_args[i]);
            return 2;
        }
        memcpy(name, console_args[i], (size_t)(equals - console_args[i]));
        name[equals - console_args[i]] = 0;
        if (pc_console_line(&config.console, name, equals + 1) != 1) {
            fprintf(stderr, "bad --console %s\n", console_args[i]);
            return 2;
        }
    }
    /* A test's command line in place of the configuration's lines, read the same way. */
    if (cheats_arg || stage_arg) {
        char text[200], error[300];
        snprintf(text, sizeof(text), "%s%s%s%s%s", cheats_arg ? "cheats=" : "", cheats_arg ? cheats_arg : "", cheats_arg ? "\n" : "", stage_arg ? "arcade_stage=" : "",
                 stage_arg ? stage_arg : "");
        if (pc_config_parse(text, &config, error, sizeof(error)) != 0) {
            fprintf(stderr, "bad --cheats or --arcade-stage: %s\n", error);
            return 2;
        }
    }
    for (i = 0; i < 4; i++) {
        char text[100], error[300];
        if (!arcade_args[i])
            continue;
        snprintf(text, sizeof(text), "%s=%.60s", arcade_lines[i], arcade_args[i]);
        if (pc_config_parse(text, &config, error, sizeof(error)) != 0) {
            fprintf(stderr, "bad --%s: %s\n", arcade_lines[i], error);
            return 2;
        }
    }
    if (no_limit_arg || config.no_limit)
        render_options.limits = PR_LIMITS_NONE;
    for (i = 0; i < 3; i++)
        pc_io_set_three_button(i, three_button_arg[i] || config.three_button[i]);

    if (pad_path) load_pad(pad_path);
    if (route_path) load_route(route_path, route_arg);
    if (key_script_path) {
        key_script = fopen(key_script_path, "r");
        if (!key_script) {
            fprintf(stderr, "no key script %s\n", key_script_path);
            return 2;
        }
        key_script_read();
    }
    if (use_window && !save_path && !save_raw) {
        /* A player's save: beside the settings file, or where the configuration says. */
        beside_settings(save_full, sizeof(save_full), config.save[0] ? config.save : "paprium.sav");
        save_store_path = save_full;
        save_path = save_full;
    }
    if (save_path) {
        long got = pc_save_load(save_path, save);
        /* Only a file that does not exist means a blank cart, as a new battery gives. Any other failure stops here with nothing written: this file may be the player's good save. */
        if (got == PC_SAVE_NO_FILE && save_store_path) {
            if (pc_save_backup_there(save_store_path))
                printf("no save %s, so the game starts as a new cart, and the first save of this run first keeps " PC_SAVE_BACKUP " as it is now as " PC_SAVE_BACKUP_KEPT "\n", save_store_path);
            save_path = NULL;
        } else if (got < 0) {
            char text[1400];
            if (got == PC_SAVE_NO_FILE)
                snprintf(text, sizeof(text), "No save file %s", save_path);
            else if (got == PC_SAVE_WRONG_SIZE)
                snprintf(text, sizeof(text), "The save file %s is not the size of a save. It may be cut short. Nothing was changed. To start with a blank cart, move that file somewhere else.", save_path);
            else
                snprintf(text, sizeof(text), "Cannot read the save file %s. Another program may be holding it, or its folder may be gone. Nothing was changed. Close that program, or bring the folder back, and start again.", save_path);
            return player_error(text, 2);
        } else if (save_store_path) {
            int check = pc_save_loaded_check(), backup = pc_save_backup_there(save_store_path);
            const char *why = pc_save_loaded_why();
            printf("the save %s, %ld bytes: %s%s%s%s%s\n", save_path, got,
                   check == PC_SAVE_GOOD ? "it checks good, so the first save of this run keeps it as the backup, " PC_SAVE_BACKUP
                   : check == PC_SAVE_BLANK ? "blank, as a new cart, so nothing is kept of it" : "it does not check as a save in the cart's format",
                   check == PC_SAVE_NOT_A_SAVE && why[0] ? "; " : "", check == PC_SAVE_NOT_A_SAVE ? why : "",
                   check == PC_SAVE_NOT_A_SAVE ? "; so the first save of this run keeps it as " PC_SAVE_KEPT_APART ", and " PC_SAVE_BACKUP " is left alone" : "",
                   check != PC_SAVE_GOOD && backup ? "; and first it keeps " PC_SAVE_BACKUP " as it is now as " PC_SAVE_BACKUP_KEPT : "");
        }
    }
    if (save_raw) {
        size_t n = 0;
        uint8_t *b = read_file(save_raw, &n);
        if (!b) {
            fprintf(stderr, "no save file %s\n", save_raw);
            return 2;
        }
        memset(save, 0x00, sizeof(save));
        memcpy(save, b, n < sizeof(save) ? n : sizeof(save));
    }

    file = read_file(rom_path, &rom_size);
    if (!file) {
        char text[1300];
        snprintf(text, sizeof(text), "Cannot read the game file %s", rom_path);
        return player_error(text, 2);
    }
    /* A player's run refuses any other file before it touches anything. A headless run says what it wants with --image-check. */
    if (!headless && pc_built_rom_sha256[0]) {
        char hash[65];
        pc_sha256_hex(file, rom_size, hash);
        if (strcmp(hash, pc_built_rom_sha256) != 0)
#if defined(PT_GAME_FROM_DLL)
            return player_error("This is not the game file this program was built for. Nothing was changed. Start Paprium.exe, the launcher beside this "
                                "program: it also takes the game file in another byte order or with a copier's header.", 3);
#else
            return player_error("This is not the game file this program was built for. Nothing was changed.", 3);
#endif
    }
    if (pt_load_rom(rom_path, 0x10000u) != 0) {
        fprintf(stderr, "no rom %s\n", rom_path);
        return 2;
    }

    frame_rec = (PcFrame *)malloc(sizeof(PcFrame));
    picture = (PcPicture *)malloc(sizeof(PcPicture));
    picture_free = (PcPicture *)malloc(sizeof(PcPicture));
    if (!frame_rec || !picture || !picture_free)
        return 2;
    memset(frame_rec, 0, sizeof(*frame_rec));
    memset(picture, 0, sizeof(*picture));
    picture->width = PC_PICTURE_W;
    picture->height = 224;
    pc_frame_rec = frame_rec;
    pc_vread_first = pc_vread_last = -1;

    pc_fiber_main_init();
    pc_clock_reset();
    pc_tick_init();
    pc_vdp_reset();
    console_start(&config.console);
    pc_io_reset();
    pc_z80_reset();

    cart.rom_bytes = file;
    cart.rom_size = (uint32_t)rom_size;
    cart.rom_words = pt_rom;
    if (save_path || save_raw) {
        cart.save = save;
        cart.save_size = sizeof(save);
    }
    start_sound_open = pc_fiber_host_time();
    sound_start(sound_device, headless, window_hidden, &config);
    start_sound_done = pc_fiber_host_time();
    {
        /* The cart's $1FE4 model bits for the console (pc_console.h): the Pocket core gives them to the cart's chip in its option word, and the
           68000 reads them there with the busy bit. 0 for the first Mega Drive and Genesis, so the option word is then unchanged. */
        PcMachine mach;
        pc_console_machine(&config.console, &mach);
        cart.options |= mach.model;
    }
    pc_cart_boot(&cart);
    pc_cart_copy_boot_page(pt_rom, 0x10000u);
    start_boot_done = start_window_done = start_input_done = pc_fiber_host_time();

    if (!image_check && !headless && pc_built_image_check[0])
        image_check = pc_built_image_check;
    if (image_check) {
        char text[32];
        snprintf(text, sizeof(text), "%016llX", (unsigned long long)fnv(pt_rom, pt_rom_size, FNV0));
        if (strcmp(text, image_check) != 0) {
            printf("RESULT: FAIL: the program was translated from image %s, and this file and firmware give %s\n", image_check, text);
            return 3;
        }
    }

#if defined(PT_GAME_FROM_DLL)
    /* The players' build: the translated game is the DLL the launcher built on this PC, checked against the same image hash as the game file. */
    if (interpreter_only) {
        pt_interp_only = 1;
        printf("no game DLL: the interpreter runs every instruction\n");
    } else {
        char dll_why[1600], dll_advice[400], dll_error[2100], dll_full[1100];
        beside_program(dll_full, sizeof(dll_full), game_dll_path ? game_dll_path : PC_GAME_DLL_DEFAULT);
        if (pc_game_dll_load(dll_full, image_check ? image_check : pc_built_image_check, dll_why, sizeof(dll_why), dll_advice, sizeof(dll_advice)) !=
            PC_GAME_DLL_OK) {
            /* Started by the launcher, which tells the player itself and then plays through the interpreter: why is printed on its own line, after
             * the words the launcher looks for, and the advice for a player who started this program by hand is left out. */
            if (from_launcher) {
                printf("%s%s\n", PC_GAME_DLL_SAID, dll_why);
                return 6;
            }
            snprintf(dll_error, sizeof(dll_error), "%s %s", dll_why, dll_advice);
            return player_error(dll_error, 6);
        }
    }
#endif

    /* The Scum! settings (pc_scum.h), before the 68000 first runs: what the ports hold (the stick, the adapter), the stage word, the cheats at the first
       switch point. Off, nothing changes. */
    {
        PcArcade arcade;
        PcIoPorts ports;
        arcade.mode = pc_config_arcade_mode(&config);
        arcade.coins = config.arcade_coins;
        arcade.attract = config.attract_only;
        arcade.three_players = config.three_players;
        arcade.stage = config.arcade_stage;
#if defined(PT_GAME_FROM_DLL)
        pc_scum_start(config.cheats, &arcade, interpreter_only);
#else
        pc_scum_start(config.cheats, &arcade, 0);
#endif
        pc_io_ports(&ports);
        if (config.cheats || config.arcade_stage || arcade.mode != PC_ARCADE_OFF || arcade.three_players) {
            char text[40];
            pc_config_cheats_text(config.cheats, text, sizeof(text));
            printf("Scum!: cheats %s%s, Arcade Mode %s, arcade stage %s, 3-Player Mode %s\n", text,
                   pc_scum_cheats_available() || !config.cheats ? "" : " (they cannot be used without the built game)", arcade_titles[arcade.mode],
                   config.arcade_stage ? pc_scum_stage_name(config.arcade_stage) : "off", arcade.three_players ? "on" : "off");
        }
        if (ports.stick)
            printf("Scum!: the Grand Stick III in port 1, its switches %02X (Arcade Coins %s, Attract Only %s), Mode %s, %s\n", ports.switches,
                   coins_titles[arcade.coins], arcade.attract ? "on" : "off", ports.stick_coin ? "puts in a coin" : "the stick's own button",
                   ports.stick_ext ? "player 3 in its EXT socket while they have a pad or a key" : "no player 3");
        else if (ports.tap)
            printf("Scum!: the EA 4-Way Play on ports 1 and 2, players 1, 2 and 3 in its sockets A, B and C\n");
    }

    for (i = 0; i < data_patches_n; i++) {
        const char *h = strchr(data_patches[i], ':');
        unsigned b;
        u32 a = (u32)strtoul(data_patches[i], NULL, 16);
        if (!h)
            return 2;
        h++;
        while (*h && sscanf(h, "%2x", &b) == 1) {
            pt_mem_w8(pt_rom, a++, b);
            h += 2;
        }
    }

    f_frames = open_in_run("frames.txt");
    f_cart = open_in_run("cart.txt");
    if (route_frame) route_log = open_in_run("route.log");
    if (dump_path) {
        f_dump = fopen(dump_path, "wb");
        if (dump_last < 0) dump_last = 0x7FFFFFFF;
    }
    if (run_dir && !no_logs && (!f_frames || !f_cart)) {
        fprintf(stderr, "cannot write in %s\n", run_dir);
        return 2;
    }

    if (frames_wanted <= 0 && use_window)
        frames_wanted = 0x7FFFFFFF;
    if (use_window) {
        PcWindowOptions options;
        char error[400], about[300];
        memset(&options, 0, sizeof(options));
        options.hidden = window_hidden;
        options.scale = config.scale;
        options.shape = config.shape;
        options.vsync = config.vsync;
        options.fullscreen = config.fullscreen;
        options.want_readback = window_shots_n > 0 || pause_shots_n > 0;
        options.renderer = config.renderer;
        options.test_fail = test_renderer_fail;
        options.test_w = test_window_w;
        options.test_h = test_window_h;
        options.menu_bar = config.menu_bar;
        pc_input_before_sdl();
        if (pc_window_open(&options, error, sizeof(error)) != 0)
            return player_error(error, 2);
        start_window_done = pc_fiber_host_time();
        vsync_watched = pc_window_vsync_on();
        pc_vsync_watch_start(&vsync_watch);
        vsync_refresh_update();
        for (i = 0; i < pc_window_fallbacks(); i++)
            printf("%s\n", pc_window_fallback(i));
        {
            /* Who comes first for pads depends on whether the game hears player 3 (pc_controls.h, WHICH PAD IS WHOSE): the ports as the Scum!
               settings above made them. */
            PcIoPorts ports;
            pc_io_ports(&ports);
            pc_input_set_player3_heard(ports.stick_ext || ports.tap);
        }
        if (pc_input_open(&config, error, sizeof(error)) != 0)
            return player_error(error, 2);
        start_input_done = pc_fiber_host_time();
        if (config.filter[0]) {
            /* A preset's relative path starts from the program's folder (the game file's and the save's start from the settings file's folder: see beside_settings), and
               librashader.dll is looked for only beside the program. A look that cannot be used leaves the picture as with off, and says why on the console and the window: the
               game still starts. */
            static char filter_full[1200], dll_full[1100];
            char note[1500];
            if (pc_config_is_preset(config.filter))
                beside_program(filter_full, sizeof(filter_full), config.filter);
            else
                snprintf(filter_full, sizeof(filter_full), "%s", config.filter);
            beside_program(dll_full, sizeof(dll_full), "librashader.dll");
            pc_window_set_filter(filter_full, dll_full, note, sizeof(note));
            if (note[0])
                printf("%s\n", note);
        }
        if (!window_hidden)
            paced = 1;
        pc_window_describe(about, sizeof(about));
        printf("%s\n", about);
        printf("controllers open: %d\n", pc_input_controllers());
        pc_input_print_keys(stdout);
        settings = &config;
        menu_refresh();
        /* A player sees the console only in a log, so for a few seconds the window says too what a setting cannot do in this start: the cheats, when
           the game plays through the interpreter, where they cannot act (pc_scum.h; the Scum! menu shows them grey), and with CPU MATE on, where its
           brain came from or why a partner will stand still. */
        {
            char note[1600] = "";
            if (config.cheats && !pc_scum_cheats_available())
                snprintf(note, sizeof(note), "Scum!: the cheats are off in this start. They need the game built on this PC, and this start plays through "
                         "the program's interpreter.");
            if (config.console.cpumate)
                snprintf(note + strlen(note), sizeof(note) - strlen(note), "%s%s", note[0] ? "\n" : "", pc_addons_brain_status());
            if (pc_addons_machine()->dropped) {
                char rules[400];
                console_rule_words(&config.console, 0, rules, sizeof(rules));
                snprintf(note + strlen(note), sizeof(note) - strlen(note), "%s%s", note[0] ? "\n" : "", rules);
            }
            if (note[0]) {
                pc_window_show_text(note, 10000);
                printf("on the window for 10 s: %s\n", note);
            }
        }
        title_update();
        SetConsoleCtrlHandler(on_console_close, TRUE);
        pc_input_poll(input_pad);
    }
    if (save_store_path)
        pc_cart_save_copy(save_last, sizeof(save_last));
    if (paced) {
        timeBeginPeriod(1);     /* so a short sleep in the pacing is short */
        pace_reset();
    }

    stall_log_start();
    pads_for_frame(0);
    pt_reset(c);
    pc = IMG_R32(4);
    t0 = pc_fiber_host_time();
    run_stack_top = (uintptr_t)__builtin_frame_address(0);
    for (;;) {
        pc = pt_run(c, pc, 0xFFFFFFFEu);
        if (run_done)
            break;
        if (pc_cart_holding()) {
            run_held();
            if (run_done)
                break;
            pc = pc_machine_restart(c);
            continue;
        }
        if (test_restart_now) {
            test_restart_now = 0;
            pc_tick_reset_handlers();
            pt_irq_level = 0;
            pc = pc_machine_restart(c);
            continue;
        }
        if (console_reset_asked) {
            console_reset_asked = 0;
            pc = console_reset(c);
            if (run_done)
                break;
            continue;
        }
        if (console_resets_done && c->stopped) {
            pc = console_locked(c, pc_tick_stack_deep());
            if (run_done)
                break;
            continue;
        }
        stopped = 1;
        printf("the translated program stopped, at frame %u\n", (unsigned)frames_done);
        break;
    }
    t1 = pc_fiber_host_time();

    /* The device first: once its thread has stopped, a gap still going on can be noted from here, and the totals count it. Closing it took about 300 ms on one PC, after the game's
       last frame: the freeze log is told, so it calls that the close and not a stall. */
    pc_stall_closing_sound(1);
    pc_audio_close();
    pc_stall_closing_sound(0);
    pc_audio_end_of_run();
    {
        char sound_totals[PC_STALL_FOOTER_CHARS];
        pc_audio_totals(sound_totals, sizeof(sound_totals));
        if (sound_totals[0])
            pc_stall_footer(sound_totals);
    }
    pc_stall_stop();
    save_at_quit();
    pc_mix_close();
    if (f_frames) fflush(f_frames);
    if (f_cart) fflush(f_cart);
    if (route_log) fflush(route_log);
    if (f_dump) fclose(f_dump);
    if (f_mode) {
        fprintf(f_mode, "frames: 320 x 224 %u, 320 x 240 %u, 256 x 224 %u, 256 x 240 %u\n", mode_frames[1][0], mode_frames[1][1], mode_frames[0][0],
                mode_frames[0][1]);
        fclose(f_mode);
    }
    if (run_dir && !no_logs)
        pt_fallback_write_summary_to(run_dir);
    if (save_failed)
        save_failed_message();
    if (use_window && menu_dump_asked)
        menu_dump();
    if (use_window) {
        /* The window closed while the controls screen was open: what it changed is kept, as when it closes. */
        char controls_error[300];
        if (pc_controls_save(controls_error, sizeof(controls_error)) != 0)
            printf("%s (%s)\n", controls_error, config_full);
        pc_input_close();
        pc_window_close();
    }

    printf("frames %u, clock %llu 68000 clocks, %llu master clocks, %lld ms, firmware %lld ms\n", (unsigned)frames_done, (unsigned long long)pt_clock,
           (unsigned long long)pc_clock_now(), (long long)((t1 - t0) * 1000 / (uint64_t)host_freq), (long long)(pc_cart_host_ticks() * 1000 / (uint64_t)host_freq));
    {
        FILETIME made, gone, kernel, user;
        if (GetProcessTimes(GetCurrentProcess(), &made, &gone, &kernel, &user)) {
            uint64_t k = (uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime, u = (uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime;
            printf("host time a frame, microseconds (the program used %llu ms of processor in all, the system %llu ms; frames late against the pace %llu, not shown %llu, stalls dropped %llu):\n",
                   (unsigned long long)(u / 10000u), (unsigned long long)(k / 10000u), (unsigned long long)pace.late, (unsigned long long)pace.unshown, (unsigned long long)pace.drops);
        }
        stat_print("work", &stat_work);
        stat_print("render", &stat_render);
        stat_print("present", &stat_present);
        stat_print("wait", &stat_wait);
    }
    printf("video chip: status reads %llu, V counter reads %llu, data writes %llu, control writes %llu, DMA %llu runs %llu words (%llu in the picture), %llu master clocks; queue stalls %llu\n",
           (unsigned long long)pc_counts.status_reads, (unsigned long long)pc_counts.hv_reads, (unsigned long long)pc_counts.data_writes, (unsigned long long)pc_counts.ctrl_writes,
           (unsigned long long)pc_counts.dma_runs, (unsigned long long)pc_counts.dma_words, (unsigned long long)pc_counts.dma_picture_words, (unsigned long long)pc_counts.dma_mclk,
           (unsigned long long)pc_counts.stall_mclk);
    printf("interrupts: H-int %llu (%llu merged by a mask), V-int %llu (%llu merged)\n", (unsigned long long)pc_counts.hints, (unsigned long long)pc_counts.hints_merged,
           (unsigned long long)pc_counts.vints, (unsigned long long)pc_counts.vints_merged);
    printf("not expected: fill %u, copy %u, interlace bits %u, misfiled writes %llu, frames over the log's size %llu, other reads %llu, other writes %llu, Z80 RAM reads %llu, handlers returning elsewhere %llu\n",
           (unsigned)pc_vdp_seen.fill, (unsigned)pc_vdp_seen.copy, (unsigned)pc_vdp_seen.interlace, (unsigned long long)pc_counts.misfiled, (unsigned long long)pc_counts.frame_full,
           (unsigned long long)pc_counts.other_reads, (unsigned long long)pc_counts.other_writes, (unsigned long long)pc_counts.z80_reads, (unsigned long long)pc_counts.handler_redirects);
    printf("fallback: %llu entries, low page from cart RAM %d, stopped %d\n", (unsigned long long)pt_fallback_uses, pt_low_from_ram, stopped);
    pc_cart_report();
    pc_addons_report();
    pc_mix_report();
    pc_audio_report();
    report = open_in_run("report.txt");
    if (report) {
        fprintf(report, "frames %u\nclock %llu\nmaster %llu\nfatal %u\nfallback %llu\n", (unsigned)frames_done, (unsigned long long)pt_clock, (unsigned long long)pc_clock_now(),
                (unsigned)pc_cart_fatal_count(), (unsigned long long)pt_fallback_uses);
        fclose(report);
    }
    if (save_out) {
        pc_cart_save_copy(save, sizeof(save));
        if (pc_save_store(save_out, save) != 0)
            printf("could not write the save to %s\n", save_out);
    }
    /* The cart's chip halts itself when its watchdog has heard nothing from the game for about 3.9 s (the firmware's core/wdog). After the game's own red
       0012 stop (Inf. Credits) and after a Console Reset that locked the console, that halt is the cart's own answer, as on the Pocket, and not a fault. */
    {
        PcArcade at_start;
        int game_stopped, fatal_expected = 0;
        pc_scum_arcade_at_start(&at_start);
        game_stopped = console_locks || ((at_start.mode == PC_ARCADE_ON || at_start.mode == PC_ARCADE_MULTIPLAYER) && at_start.coins == PC_COINS_INFINITE);
        if (pc_cart_fatal_count() == 1 && pc_cart_fatal_code() == CART_WATCHDOG_HALT && game_stopped) {
            printf("the cart's chip halted itself because the game had stopped (%s), as on the cart: not a fault\n",
                   console_locks ? "the console locked up after a Console Reset" : "Inf. Credits, the game's red 0012 stop");
            fatal_expected = 1;
        }
        if (stopped || (pc_cart_fatal_count() && !fatal_expected)) {
            printf("RESULT: FAIL\n");
            fflush(stdout);
            /* Soft Reset starts the game again even after a halt, as the Pocket's Soft Reset restarts the cart's chip. */
            if (soft_reset_asked)
                ExitProcess((UINT)soft_reset());
            ExitProcess(4);
        }
    }
    printf("RESULT: PASS\n");
    fflush(stdout);
    if (soft_reset_asked)
        ExitProcess((UINT)soft_reset());
    ExitProcess(0);
    return 0;
}
