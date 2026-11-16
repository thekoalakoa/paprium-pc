/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The window and the keys, on SDL's dummy video driver or a hidden window, with a made-up picture. A test never shows a window. Built by tests/test_pc_machine.py when the local
 * configuration names SDL's development files.
 *
 * The picture's pixels all differ, so a wrong stretch or a swapped colour shows. What is checked: the shapes' sizes, that every game pixel keeps its exact colour in a whole-number
 * enlargement, that colours pass through unchanged, that keys reach the right pad bit in the default list and through a configuration's key, and that the actions fire once a press.
 *
 * Game controllers: SDL's virtual joystick stands in for a player's pad, since a test PC may have none. One pad attached before the program starts must be on port 1 only, whatever
 * SDL reports afterwards. A second one attached later takes port 2, and when the first goes away the second stays where it is. SDL's drivers for real pads are turned off first,
 * so a pad plugged into the test PC is neither read nor counted here.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_controls.h"
#include "pc_io.h"
#include "pc_window.h"

static int checks, failed;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static PcPicture pic;
static uint32_t pixels[4096 * 2048];

static uint32_t made_up_colour(unsigned x, unsigned y)
{
    return (x * 251u + y * 1021u) & 0xFFFFFFu;
}

static void fill(int width)
{
    unsigned x, y;
    memset(&pic, 0, sizeof(pic));
    pic.width = (uint16_t)width;
    pic.height = 224;
    for (y = 0; y < 224; y++)
        for (x = 0; x < (unsigned)width; x++)
            pic.rgb[y][x] = made_up_colour(x, y);
}

/* Each game pixel covers a whole block of the image, with its exact colour. */
static int blocks_are_exact(const uint32_t *image, int w, int h, int width, int kw, int kh)
{
    int x, y;
    if (w != width * kw || h != 224 * kh)
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (image[y * w + x] != made_up_colour((unsigned)(x / kw), (unsigned)(y / kh)))
                return 0;
    return 1;
}

static void key_gives(const char *name, int port, unsigned bit)
{
    uint32_t pad[3];
    CHECK(pc_input_push_key(name, 1) == 0);
    pc_input_poll(pad);
    CHECK(pad[port] == bit && pad[1 - port] == 0);
    CHECK(pc_input_push_key(name, 0) == 0);
    pc_input_poll(pad);
    CHECK(pad[0] == 0 && pad[1] == 0);
}

static void key_action(const char *name, unsigned action)
{
    uint32_t pad[3];
    CHECK(pc_input_push_key(name, 1) == 0);
    CHECK(pc_input_poll(pad) == action);
    CHECK(pc_input_poll(pad) == 0);     /* once a press, however long it is held */
    CHECK(pc_input_push_key(name, 0) == 0);
    CHECK(pc_input_poll(pad) == 0);
}

/* ---- a virtual pad, in place of a player's */
#define PAD_BUTTONS 15

/* SDL's drivers for real pads off, before SDL starts each time: SDL_Quit (pc_window_close) forgets every hint set this way. */
static void no_real_pads(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
}

static int attach_pad(void)
{
    /* All 15 buttons of SDL's controller layout, with their numbers as SDL's virtual pad maps them, and the two sticks and triggers. */
    return SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, PAD_BUTTONS, 0);
}

static void settle(uint32_t pad[3])
{
    pc_input_poll(pad);
    pc_input_poll(pad);
}

static void button_gives(SDL_Joystick *stick, int button, int port, unsigned bit)
{
    uint32_t pad[3];
    CHECK(SDL_JoystickSetVirtualButton(stick, button, 1) == 0);
    settle(pad);
    CHECK(pad[port] == bit && pad[1 - port] == 0);
    CHECK(SDL_JoystickSetVirtualButton(stick, button, 0) == 0);
    settle(pad);
    CHECK(pad[0] == 0 && pad[1] == 0);
}

static void axis_gives(SDL_Joystick *stick, int axis, int value, int port, unsigned bit)
{
    uint32_t pad[3];
    CHECK(SDL_JoystickSetVirtualAxis(stick, axis, (Sint16)value) == 0);
    settle(pad);
    CHECK(pad[port] == bit && pad[1 - port] == 0);
    CHECK(SDL_JoystickSetVirtualAxis(stick, axis, 0) == 0);
    settle(pad);
    CHECK(pad[0] == 0 && pad[1] == 0);
}

/* Every control of a pad on the port it belongs to, and nothing on the other port. */
static void every_control(SDL_Joystick *stick, int port)
{
    button_gives(stick, SDL_CONTROLLER_BUTTON_A, port, PC_PAD_B);
    button_gives(stick, SDL_CONTROLLER_BUTTON_B, port, PC_PAD_C);
    button_gives(stick, SDL_CONTROLLER_BUTTON_X, port, PC_PAD_A);
    button_gives(stick, SDL_CONTROLLER_BUTTON_Y, port, PC_PAD_Y);
    button_gives(stick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, port, PC_PAD_X);
    button_gives(stick, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, port, PC_PAD_Z);
    button_gives(stick, SDL_CONTROLLER_BUTTON_START, port, PC_PAD_START);
    button_gives(stick, SDL_CONTROLLER_BUTTON_BACK, port, PC_PAD_MODE);
    button_gives(stick, SDL_CONTROLLER_BUTTON_DPAD_UP, port, PC_PAD_UP);
    button_gives(stick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, port, PC_PAD_DOWN);
    button_gives(stick, SDL_CONTROLLER_BUTTON_DPAD_LEFT, port, PC_PAD_LEFT);
    button_gives(stick, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, port, PC_PAD_RIGHT);
    axis_gives(stick, SDL_CONTROLLER_AXIS_LEFTX, -30000, port, PC_PAD_LEFT);
    axis_gives(stick, SDL_CONTROLLER_AXIS_LEFTX, 30000, port, PC_PAD_RIGHT);
    axis_gives(stick, SDL_CONTROLLER_AXIS_LEFTY, -30000, port, PC_PAD_UP);
    axis_gives(stick, SDL_CONTROLLER_AXIS_LEFTY, 30000, port, PC_PAD_DOWN);
}

/* The order the renderers are tried in. first: the one asked for, and order: what must follow it, with the one that starts last of all. */
static void order_is(const char *first, const char *test_fail, const char *const *order, int n)
{
    PcWindowOptions o;
    char error[300], word[80];
    int i;
    memset(&o, 0, sizeof(o));
    o.hidden = 1;
    o.scale = 1;
    o.renderer = first;
    o.test_fail = test_fail;
    CHECK(pc_window_open(&o, error, sizeof(error)) == 0);
    CHECK(!strcmp(pc_window_renderer(), "software"));
    CHECK(pc_window_vsync_on() == 0);
    CHECK(pc_window_fallbacks() == n - 1);
    for (i = 0; i < n - 1; i++) {
        snprintf(word, sizeof(word), "renderer %s could not start (", order[i]);
        CHECK(strstr(pc_window_fallback(i), word) == pc_window_fallback(i));
        snprintf(word, sizeof(word), ", so next is %s", order[i + 1]);
        CHECK(strstr(pc_window_fallback(i), word) != NULL);
    }
    CHECK(!strcmp(pc_window_fallback(n), ""));
    pc_window_close();
}

static void test_renderers(void)
{
    static const char *const by_default[] = {"opengl", "direct3d", "direct3d11", "direct3d12", "software"};
    static const char *const d3d12_first[] = {"direct3d12", "opengl", "direct3d", "direct3d11", "software"};
    static const char *const d3d11_first[] = {"direct3d11", "opengl", "direct3d", "direct3d12", "software"};
    static const char *const auto_first[] = {"auto", "opengl", "direct3d", "direct3d11", "direct3d12", "software"};
    PcWindowOptions o;
    char error[300];
    order_is(NULL, NULL, by_default, 5);
    order_is("", NULL, by_default, 5);
    order_is("opengl", NULL, by_default, 5);
    order_is("direct3d12", NULL, d3d12_first, 5);
    order_is("direct3d11", NULL, d3d11_first, 5);
    order_is("auto", NULL, auto_first, 6);
    /* Software starts on the dummy driver unless a test says no. Then the program has nothing left, and says what the last one said. */
    memset(&o, 0, sizeof(o));
    o.hidden = 1;
    o.scale = 1;
    o.test_fail = "direct3d,software";
    no_real_pads();
    CHECK(pc_window_open(&o, error, sizeof(error)) == -1);
    CHECK(strstr(error, "No renderer would start") && strstr(error, "a test asked for this"));
    CHECK(strstr(pc_window_fallback(1), "a test asked for this") != NULL);
    CHECK(strstr(pc_window_fallback(2), "a test asked for this") == NULL);     /* direct3d is not direct3d11 */
    CHECK(strstr(pc_window_fallback(4), "and there is no other") != NULL);
    pc_window_close();
    /* A test's size for the window: the picture is placed in it as it would be in a full screen of that size. */
    memset(&o, 0, sizeof(o));
    o.hidden = 1;
    o.scale = 1;
    o.want_readback = 1;
    o.test_w = 2560;
    o.test_h = 1600;
    no_real_pads();
    CHECK(pc_window_open(&o, error, sizeof(error)) == 0);
    fill(320);
    pc_window_present(&pic);
    {
        int w, h;
        CHECK(pc_window_read_screen(pixels, 4096 * 2048, &w, &h) == 0);
        CHECK(w == 2560 && h == 1600);
        /* 7 x 224 lines, as tall as fits, 16 lines of black above and below, and the first line of the picture repeated 7 times. */
        CHECK(pixels[15 * 2560 + 1280] == 0);
        CHECK(pixels[16 * 2560 + 1280] == pixels[22 * 2560 + 1280]);
        CHECK(pixels[23 * 2560 + 1280] != pixels[22 * 2560 + 1280]);
        CHECK(pixels[1583 * 2560 + 1280] != 0 && pixels[1584 * 2560 + 1280] == 0);
    }
    pc_window_close();
}

/* The 4:3 window at each scale: the picture fills the window's height in whole multiples. At scales 2, 4 and 5 the window is a fraction of a
   pixel narrower than 4:3 (597, 1194 and 1493 pixels wide, where 4:3 is 597.3, 1194.7 and 1493.3), which once cost the picture one multiple. */
static void test_scales(void)
{
    static const int widths[] = {0, 298, 597, 896, 1194, 1493};
    PcWindowOptions o;
    char error[300];
    int scale;
    for (scale = 1; scale <= 5; scale++) {
        int w, h, x, black_top = 0, black_bottom = 0;
        memset(&o, 0, sizeof(o));
        o.hidden = 1;
        o.scale = scale;
        o.want_readback = 1;
        no_real_pads();
        CHECK(pc_window_open(&o, error, sizeof(error)) == 0);
        fill(320);
        pc_window_present(&pic);
        CHECK(pc_window_read_screen(pixels, 4096 * 2048, &w, &h) == 0);
        CHECK(w == widths[scale] && h == 224 * scale);
        for (x = 0; x < w; x++) {
            black_top += pixels[x] == 0;
            black_bottom += pixels[(h - 1) * w + x] == 0;
        }
        if (black_top >= 10 || black_bottom >= 10)
            printf("scale %d: %d and %d black pixels in the window's first and last rows\n", scale, black_top, black_bottom);
        CHECK(black_top < 10 && black_bottom < 10);     /* the picture reaches the top and the bottom: no black bars */
        /* None of these widths is a whole multiple of 320, so the picture is first enlarged by whole multiples, scale x scale, then fitted. */
        CHECK(pc_window_read_large(pixels, 4096 * 2048, &w, &h) == 0 && blocks_are_exact(pixels, w, h, 320, scale, scale));
        pc_window_close();
    }
}

static void test_pads(void)
{
    PcWindowOptions o;
    PcConfig config;
    char error[300];
    uint32_t pad[3];
    int first, second, i;
    SDL_Joystick *first_stick, *second_stick;

    /* The player's pad is there before the program starts, so SDL knows it before its controller part starts. Without focus SDL would ignore the buttons of a window that is never shown. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    no_real_pads();
    CHECK(SDL_Init(SDL_INIT_JOYSTICK) == 0);
    first = attach_pad();
    CHECK(first >= 0);
    if (first < 0)
        return;
    memset(&o, 0, sizeof(o));
    o.hidden = 1;
    o.scale = 1;
    pc_config_defaults(&config);
    if (pc_window_open(&o, error, sizeof(error)) != 0) {
        printf("FAIL: %s\n", error);
        failed++;
        return;
    }
    CHECK(SDL_IsGameController(first));
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    CHECK(pc_input_controllers() == 1);
    /* What SDL says when its controller part starts comes in the first polls: it must not put this pad on port 2 as well. */
    for (i = 0; i < 5; i++) {
        pc_input_poll(pad);
        CHECK(pc_input_controllers() == 1);
    }
    /* Asking again for the same list, as a second open does, changes nothing. */
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    CHECK(pc_input_controllers() == 1);
    first_stick = SDL_JoystickOpen(first);
    CHECK(first_stick != NULL);
    if (!first_stick)
        return;
    every_control(first_stick, 0);

    /* A second pad plugged in while the game runs: player 2. */
    second = attach_pad();
    CHECK(second >= 0);
    if (second < 0)
        return;
    settle(pad);
    CHECK(pc_input_controllers() == 2);
    second_stick = SDL_JoystickOpen(second);
    CHECK(second_stick != NULL);
    if (!second_stick)
        return;
    every_control(second_stick, 1);
    /* Both held together: each on its own port. */
    CHECK(SDL_JoystickSetVirtualButton(first_stick, SDL_CONTROLLER_BUTTON_A, 1) == 0);
    CHECK(SDL_JoystickSetVirtualButton(second_stick, SDL_CONTROLLER_BUTTON_START, 1) == 0);
    settle(pad);
    CHECK(pad[0] == PC_PAD_B && pad[1] == PC_PAD_START);
    CHECK(SDL_JoystickSetVirtualButton(first_stick, SDL_CONTROLLER_BUTTON_A, 0) == 0);
    CHECK(SDL_JoystickSetVirtualButton(second_stick, SDL_CONTROLLER_BUTTON_START, 0) == 0);
    settle(pad);
    CHECK(pad[0] == 0 && pad[1] == 0);

    /* The first pad pulled out: port 1 goes quiet, and the second pad stays on port 2. */
    SDL_JoystickClose(first_stick);
    CHECK(SDL_JoystickDetachVirtual(first) == 0);
    settle(pad);
    CHECK(pc_input_controllers() == 1);
    every_control(second_stick, 1);

    /* Plugged back in, it takes the free port, and only that one. */
    first = attach_pad();
    CHECK(first >= 0);
    if (first < 0)
        return;
    settle(pad);
    CHECK(pc_input_controllers() == 2);
    first_stick = SDL_JoystickOpen(first);
    CHECK(first_stick != NULL);
    if (first_stick)
        every_control(first_stick, 0);

    pc_input_close();
    CHECK(pc_input_controllers() == 0);
    if (first_stick)
        SDL_JoystickClose(first_stick);
    SDL_JoystickClose(second_stick);
    pc_window_close();
}

int main(void)
{
    PcWindowOptions o;
    PcConfig config;
    char error[300], about[300];
    int w, h, x, y, bad;
    uint32_t pad[3];

    no_real_pads();
    memset(&o, 0, sizeof(o));
    o.hidden = 1;
    o.scale = 3;
    o.want_readback = 1;
    pc_config_defaults(&config);
    if (pc_window_open(&o, error, sizeof(error)) != 0) {
        printf("FAIL: %s\n", error);
        return 1;
    }
    CHECK(pc_window_available());
    pc_window_describe(about, sizeof(about));
    /* What the freeze log's first lines say. The dummy driver has a display but no GPU to name and no system window. */
    {
        char displays[300], gpu[300];
        pc_window_displays(displays, sizeof(displays));
        CHECK(strstr(displays, " Hz, the window is on this one") != NULL);
        pc_window_gpu(gpu, sizeof(gpu));
        CHECK(!strcmp(gpu, "this renderer does not say"));
        CHECK(pc_window_native() == NULL);
        CHECK(pc_window_vsync_on() == 0 || pc_window_vsync_on() == 1);
        CHECK(pc_window_refresh_hz() >= 0 && pc_window_refresh_hz() < 1000);
    }
    printf("%s\n", about);
    CHECK(strstr(about, "hidden") != NULL);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);

    /* 4:3, the default: 896 by 672 at scale 3. */
    fill(320);
    pc_window_present(&pic);
    CHECK(pc_window_read_screen(pixels, 4096 * 2048, &w, &h) == 0);
    CHECK(w == 896 && h == 672);
    CHECK(pc_window_read_large(pixels, 4096 * 2048, &w, &h) == 0);
    CHECK(blocks_are_exact(pixels, w, h, 320, 3, 3));
    /* The 256 pixel mode fills the same width: a larger whole number across. */
    fill(256);
    pc_window_present(&pic);
    CHECK(pc_window_read_screen(pixels, 4096 * 2048, &w, &h) == 0);
    CHECK(w == 896 && h == 672);
    bad = 0;
    for (y = 0; y < 672; y++)
        if (pixels[y * 896 + 0] != made_up_colour(0, (unsigned)(y / 3)))
            bad++;
    CHECK(bad == 0);    /* no black bar at the left: the picture reaches the edge */
    CHECK(pc_window_read_large(pixels, 4096 * 2048, &w, &h) == 0);
    CHECK(blocks_are_exact(pixels, w, h, 256, 4, 3));

    /* Square pixels: whole multiples and exact colours on the screen itself. The window is 4:3 here, so the 320 pixel picture gets 2 x 2 and black above and below. */
    pc_window_toggle_shape();
    CHECK(pc_window_is_square());
    fill(320);
    pc_window_present(&pic);
    CHECK(pc_window_read_screen(pixels, 4096 * 2048, &w, &h) == 0);
    CHECK(w == 896 && h == 672);
    {
        int left = (896 - 640) / 2, top = (672 - 448) / 2;
        bad = 0;
        for (y = 0; y < 448; y++)
            for (x = 0; x < 640; x++)
                if (pixels[(y + top) * 896 + x + left] != made_up_colour((unsigned)(x / 2), (unsigned)(y / 2)))
                    bad++;
        CHECK(bad == 0);
        CHECK(pixels[(top - 1) * 896 + left] == 0 && pixels[top * 896 + left - 1] == 0);
    }
    /* The Pocket core's two shapes, then round to 4:3 again. Original: 64:49, so three lines of the picture to a game line, 877 pixels wide, a 256 pixel
       line as wide; Corrected: square pixels for a 320 pixel line (two whole multiples here), a 256 pixel line at 64:49 in the middle, 585 wide. */
    pc_window_toggle_shape();
    CHECK(pc_window_shape() == PC_SHAPE_ORIGINAL);
    {
        int k, widths[4], sizes[2] = {320, 256};
        for (k = 0; k < 4; k++) {
            int n = 0;
            if (k == 2)
                pc_window_toggle_shape();
            fill(sizes[k & 1]);
            pc_window_present(&pic);
            CHECK(pc_window_read_screen(pixels, 4096 * 2048, &w, &h) == 0 && w == 896 && h == 672);
            for (x = 0; x < 896; x++)
                n += pixels[336 * 896 + x] != 0;
            widths[k] = n;
        }
        CHECK(pc_window_shape() == PC_SHAPE_CORRECTED);
        CHECK(widths[0] == 877 && widths[1] == 877 && widths[2] == 640 && widths[3] == 585);
        printf("Original 320 / 256 and Corrected 320 / 256, on a 896 x 672 window: %d %d %d %d pixels wide\n", widths[0], widths[1], widths[2], widths[3]);
    }
    pc_window_toggle_shape();
    CHECK(pc_window_shape() == PC_SHAPE_4_3);

    /* Every default key of player 1 and player 2. */
    key_gives("Up", 0, PC_PAD_UP); key_gives("Down", 0, PC_PAD_DOWN); key_gives("Left", 0, PC_PAD_LEFT); key_gives("Right", 0, PC_PAD_RIGHT);
    key_gives("Z", 0, PC_PAD_A); key_gives("X", 0, PC_PAD_B); key_gives("C", 0, PC_PAD_C);
    key_gives("A", 0, PC_PAD_X); key_gives("S", 0, PC_PAD_Y); key_gives("D", 0, PC_PAD_Z);
    key_gives("Return", 0, PC_PAD_START); key_gives("Right Shift", 0, PC_PAD_MODE);
    key_gives("I", 1, PC_PAD_UP); key_gives("K", 1, PC_PAD_DOWN); key_gives("J", 1, PC_PAD_LEFT); key_gives("L", 1, PC_PAD_RIGHT);
    key_gives("1", 1, PC_PAD_A); key_gives("2", 1, PC_PAD_B); key_gives("3", 1, PC_PAD_C);
    key_gives("4", 1, PC_PAD_X); key_gives("5", 1, PC_PAD_Y); key_gives("6", 1, PC_PAD_Z);
    key_gives("Keypad Enter", 1, PC_PAD_START); key_gives("Keypad 0", 1, PC_PAD_MODE);
    key_action("F1", PC_ACTION_LIMITS); key_action("F2", PC_ACTION_SHAPE); key_action("P", PC_ACTION_PAUSE); key_action("F11", PC_ACTION_FULLSCREEN); key_action("Escape", PC_ACTION_QUIT);
    key_action("F6", PC_ACTION_FILTER); key_action("F4", PC_ACTION_CONTROLS);

    /* Two keys held together, and one let go. */
    CHECK(pc_input_push_key("Left", 1) == 0);
    CHECK(pc_input_push_key("Z", 1) == 0);
    pc_input_poll(pad);
    CHECK(pad[0] == (PC_PAD_LEFT | PC_PAD_A));
    CHECK(pc_input_push_key("Left", 0) == 0);
    pc_input_poll(pad);
    CHECK(pad[0] == PC_PAD_A);
    CHECK(pc_input_push_key("Z", 0) == 0);
    pc_input_poll(pad);

    /* A key a configuration moves, and two the configuration gets wrong. */
    CHECK(pc_config_parse("key_p1_a=Space\n", &config, error, sizeof(error)) == 0);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    key_gives("Space", 0, PC_PAD_A);
    CHECK(pc_config_parse("key_p1_b=No Such Key\n", &config, error, sizeof(error)) == 0);
    CHECK(pc_input_open(&config, error, sizeof(error)) == -1 && strstr(error, "No Such Key"));
    pc_config_defaults(&config);
    CHECK(pc_config_parse("key_p9_a=Space\n", &config, error, sizeof(error)) == 0);
    CHECK(pc_input_open(&config, error, sizeof(error)) == -1 && strstr(error, "p9_a"));
    CHECK(pc_input_push_key("No Such Key", 1) == -1);

    pc_input_print_keys(stdout);
    pc_input_close();
    pc_window_close();

    test_renderers();
    test_scales();
    test_pads();

    printf("%d checks, %d failed\n", checks, failed);
    printf("RESULT: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
