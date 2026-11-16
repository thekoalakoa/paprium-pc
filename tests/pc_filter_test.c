/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The picture filters (targets/pc/pc_filter.h), with pc_window.c as the program uses it. A test never shows a window. Built by tests/test_pc_machine.py.
 *
 *   pc_filter_test logic        on SDL's dummy video driver, where only the software renderer starts: the configuration's filter line, the font of the words shown on the window,
 *                               the sharp look, the shader looks refused with a plain message, the filter key going through the looks this renderer can draw.
 *   pc_filter_test gl           in a hidden window with the OpenGL renderer, on the PC's own graphics card: every look builds and draws what it should into an offscreen picture,
 *                               and leaves OpenGL as SDL's renderer expects it; a missing preset or library is refused with a plain message, and a library that is
 *                               there but that Windows will not load (damaged, or a 32-bit DLL) with Windows' error, never as missing. When the environment names
 *                               PC_FILTER_TEST_LIBRASHADER (the library's full path) and PC_FILTER_TEST_PRESET (a .slangp file), a player's preset is drawn through it too.
 *                               Prints "skip" and passes when this PC's OpenGL renderer will not start.
 *   pc_filter_test sheet PICTURE.ppm FOLDER W H     each look's picture of a game frame at W x H, as FOLDER/<n>_<look>.ppm
 *   pc_filter_test bench PICTURE.ppm W H FRAMES [paced]   each look's time a frame on the GPU and in the program, through OpenGL's timer queries; paced draws
 *                                                          one each 17 ms
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_config.h"
#include "pc_filter.h"
#include "pc_font.h"
#include "pc_osd.h"
#include "pc_window.h"

static int checks, failed;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static PcPicture pic;
#define CAPACITY (2560 * 1600)
static uint32_t image[CAPACITY], image2[CAPACITY];

static uint32_t made_up_colour(unsigned x, unsigned y)
{
    return (x * 251u + y * 1021u) & 0xFFFFFFu;
}

static void fill_made_up(int width)
{
    unsigned x, y;
    memset(&pic, 0, sizeof(pic));
    pic.width = (uint16_t)width;
    pic.height = 224;
    for (y = 0; y < 224; y++)
        for (x = 0; x < (unsigned)width; x++)
            pic.rgb[y][x] = made_up_colour(x, y);
}

static void fill_flat(uint32_t colour)
{
    unsigned x, y;
    memset(&pic, 0, sizeof(pic));
    pic.width = 320;
    pic.height = 224;
    for (y = 0; y < 224; y++)
        for (x = 0; x < 320; x++)
            pic.rgb[y][x] = colour;
}

static int load_ppm(const char *path)
{
    FILE *f = fopen(path, "rb");
    int w, h, max, x, y;
    if (!f)
        return -1;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &max) != 3 || (w != 320 && w != 256) || h != 224 || max != 255) {
        fclose(f);
        return -1;
    }
    fgetc(f);
    memset(&pic, 0, sizeof(pic));
    pic.width = (uint16_t)w;
    pic.height = 224;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned char rgb[3];
            if (fread(rgb, 1, 3, f) != 3) {
                fclose(f);
                return -1;
            }
            pic.rgb[y][x] = (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
        }
    fclose(f);
    return 0;
}

static int write_ppm(const char *path, const uint32_t *pixels, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        unsigned char rgb[3] = {(unsigned char)(pixels[i] >> 16), (unsigned char)(pixels[i] >> 8), (unsigned char)pixels[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

static int open_window(const char *renderer, int w, int h)
{
    PcWindowOptions o;
    char error[300];
    memset(&o, 0, sizeof(o));
    o.hidden = 1;
    o.scale = 3;
    o.want_readback = 1;
    o.renderer = renderer;
    o.test_w = w;
    o.test_h = h;
    if (pc_window_open(&o, error, sizeof(error)) != 0) {
        printf("could not open the window: %s\n", error);
        return -1;
    }
    return 0;
}

static int draw(int w, int h, uint32_t *out)
{
    double gm, gw, cm;
    return pc_window_draw_offscreen(&pic, w, h, 1, 0, out, CAPACITY, &gm, &gw, NULL, &cm);
}

static int brightness(uint32_t c)
{
    return (int)((c >> 16) & 255) + (int)((c >> 8) & 255) + (int)(c & 255);
}

/* The top half of the picture white and the bottom black must come out that way, offscreen and on the window (whose framebuffer keeps its top line last). */
static void upright(const char *what, int w, int h)
{
    unsigned x, y;
    int ww, wh, k;
    long top[2] = {0, 0}, bottom[2] = {0, 0};
    memset(&pic, 0, sizeof(pic));
    pic.width = 320;
    pic.height = 224;
    for (y = 0; y < 112; y++)
        for (x = 0; x < 320; x++)
            pic.rgb[y][x] = 0xFFFFFF;
    CHECK(draw(w, h, image) == 0);
    pc_window_present(&pic);
    CHECK(pc_window_read_screen(image2, CAPACITY, &ww, &wh) == 0 && ww == w && wh == h);
    for (k = 0; k < 2; k++) {
        const uint32_t *im = k ? image2 : image;
        for (x = (unsigned)w / 4; x < (unsigned)w * 3 / 4; x++) {
            top[k] += brightness(im[(h / 4) * w + (int)x]) + brightness(im[(h / 4 + 1) * w + (int)x]);
            bottom[k] += brightness(im[(h * 3 / 4) * w + (int)x]) + brightness(im[(h * 3 / 4 + 1) * w + (int)x]);
        }
    }
    if (!(top[0] > bottom[0] * 4 + 1000 && top[1] > bottom[1] * 4 + 1000))
        printf("%s: top %ld bottom %ld offscreen, top %ld bottom %ld on the window\n", what, top[0], bottom[0], top[1], bottom[1]);
    CHECK(top[0] > bottom[0] * 4 + 1000);
    CHECK(top[1] > bottom[1] * 4 + 1000);
}

/* ---- logic, on the dummy driver */

static void test_config(void)
{
    PcConfig c;
    char error[300];
    static const char *const names[] = PC_LOOK_NAMES;
    int i;
    for (i = 0; i < PC_LOOKS; i++) {
        pc_config_defaults(&c);
        CHECK(!strcmp(pc_filter_name(i), names[i]));
        CHECK(pc_filter_find(names[i]) == i);
        CHECK(pc_filter_title(i)[0] && pc_filter_about(i)[0]);
        CHECK(pc_filter_needs_gl(i) == (i >= PC_LOOK_SCANLINES));
        {
            char line[80];
            snprintf(line, sizeof(line), "filter=%s\n", names[i]);
            CHECK(pc_config_parse(line, &c, error, sizeof(error)) == 0 && !strcmp(c.filter, names[i]));
        }
    }
    pc_config_defaults(&c);
    CHECK(c.filter[0] == 0);
    CHECK(pc_config_parse("filter=presets\\my crt.slangp\n", &c, error, sizeof(error)) == 0 && !strcmp(c.filter, "presets\\my crt.slangp"));
    CHECK(pc_config_parse("filter = C:\\shaders\\crt-guest-advanced.SLANGP \n", &c, error, sizeof(error)) == 0 && !strcmp(c.filter, "C:\\shaders\\crt-guest-advanced.SLANGP"));
    /* bvm is another name for the pvm look (a Sony PVM or BVM monitor); a name no look has is refused, by its line. */
    CHECK(pc_config_parse("filter=bvm\n", &c, error, sizeof(error)) == 0 && !strcmp(c.filter, "pvm"));
    CHECK(pc_config_parse("filter=lcd\n", &c, error, sizeof(error)) == 1 && strstr(error, "filter=lcd"));
    CHECK(pc_config_parse("filter=CRT\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("filter=crt.glslp\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_parse("filter=.slangp\n", &c, error, sizeof(error)) == 1);
    CHECK(pc_config_is_preset("a.slangp") && pc_config_is_preset("A.SlangP") && !pc_config_is_preset("slangp") && !pc_config_is_preset("crt"));
    CHECK(pc_filter_find("bvm") == -1 && pc_filter_find("") == -1);
}

#define BOX_PIXELS (500 * 400)     /* room for the words' boxes the font test draws */

static void test_font(void)
{
    static const char every[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789 .,:;-+=_/\\()[]<>'\"?!%&*#|";
    static uint32_t box[BOX_PIXELS];
    int w, h, i, ink, x, y;
    for (i = 0; every[i]; i++)
        CHECK(pc_osd_has(every[i]));
    CHECK(pc_osd_has('@') && pc_osd_has('~') && !pc_osd_has('\t') && !pc_osd_has((char)0xE9));
    /* One font in the program: each character's dots over the window are the controls screen's shape for it, small letters as capitals. */
    {
        int c, bad = 0;
        for (c = 32; c <= 126; c++) {
            char one[2] = {(char)c, 0};
            const uint8_t *g = pc_font_glyph((unsigned char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c));
            int row, dot;
            CHECK(pc_osd_render(one, 40, 1, box, BOX_PIXELS, &w, &h) == 0 && w == 9 && h == 11);
            for (row = 0; row < 8; row++)
                for (dot = 0; dot < 5; dot++)
                    if ((box[(2 + row) * w + 2 + dot] == 0xFFFFFFFFu) != ((g[row] >> (4 - dot) & 1) != 0))
                        bad++;
        }
        CHECK(bad == 0);
    }
    /* One line: 2 dots of box around 5 x 7 letters with a dot between. */
    CHECK(pc_osd_render("CRT", 40, 1, box, BOX_PIXELS, &w, &h) == 0);
    CHECK(w == 3 * 6 - 1 + 4 && h == 9 + 2);
    ink = 0;
    for (i = 0; i < w * h; i++)
        ink += box[i] == 0xFFFFFFFFu;
    CHECK(ink == 13 + 18 + 11);     /* the dots of C, R and T in this font */
    CHECK(box[0] == 0xB0000000u);
    /* Scaled: every dot a block. */
    CHECK(pc_osd_render("I", 40, 3, box, BOX_PIXELS, &w, &h) == 0);
    CHECK(w == (6 - 1 + 4) * 3 && h == 11 * 3);
    CHECK(box[(2 * 3) * w + (2 + 1) * 3] == 0xFFFFFFFFu && box[(2 * 3 + 2) * w + (2 + 1) * 3 + 2] == 0xFFFFFFFFu);
    /* Broken at spaces into lines of at most 10 letters; a word too long for a line is cut. */
    CHECK(pc_osd_render("the filter crt could not", 10, 1, box, BOX_PIXELS, &w, &h) == 0);
    CHECK(h == 3 * 9 + 2 && w == 10 * 6 - 1 + 4);
    CHECK(pc_osd_render("abcdefghijklmnop", 10, 1, box, BOX_PIXELS, &w, &h) == 0 && h == 2 * 9 + 2);
    /* A box too large for the room given is refused, and nothing is written. */
    box[0] = 1;
    CHECK(pc_osd_render("CRT", 40, 50, box, 1000, &w, &h) == -1 && box[0] == 1);
    /* Lower case, and a byte the font has no shape for. */
    {
        static uint32_t a[4000], b[4000];
        CHECK(pc_osd_render("crt\x01", 40, 1, a, 4000, &w, &h) == 0 && pc_osd_render("CRT?", 40, 1, b, 4000, &w, &h) == 0 && !memcmp(a, b, (size_t)w * h * 4));
    }
    (void)x;
    (void)y;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* A preset file looked at before any library is loaded: what is wrong with it, in plain words, and a good one passes. The files are made here and removed after. */
static void test_preset_check(void)
{
    char why[1500];
    CreateDirectoryA("pc_filter_presets", NULL);
    CreateDirectoryA("pc_filter_presets\\shaders", NULL);
    put("pc_filter_presets\\shaders\\a.slang", "#version 450\n");
    put("pc_filter_presets\\shaders\\lut1.png", "not a picture, only there\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\none.slangp", why, sizeof(why)) == -1 && strstr(why, "there is no preset file"));
    put("pc_filter_presets\\empty.slangp", "# nothing\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\empty.slangp", why, sizeof(why)) == -1 && strstr(why, "no line shaders = N"));
    put("pc_filter_presets\\good.slangp", "shaders = 1\n\nshader0 = shaders/a.slang\nfilter_linear0 = false\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\good.slangp", why, sizeof(why)) == 0);
    put("pc_filter_presets\\quoted.slangp", "shaders = \"1\"\nshader0 = \"shaders\\a.slang\"\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\quoted.slangp", why, sizeof(why)) == 0);
    put("pc_filter_presets\\lost.slangp", "shaders = 2\nshader0 = shaders/a.slang\nshader1 = shaders/b.slang\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\lost.slangp", why, sizeof(why)) == -1 && strstr(why, "names shader1 = shaders/b.slang, and there is no such file"));
    put("pc_filter_presets\\short.slangp", "shaders = 2\nshader0 = shaders/a.slang\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\short.slangp", why, sizeof(why)) == -1 && strstr(why, "says shaders = 2 but names no shader1"));
    put("pc_filter_presets\\lut.slangp", "shaders = 1\nshader0 = shaders/a.slang\ntextures = \"LUT1;LUT2\"\nLUT1 = shaders/lut1.png\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\lut.slangp", why, sizeof(why)) == -1 && strstr(why, "lists the texture LUT2 but gives no file"));
    put("pc_filter_presets\\lut2.slangp", "shaders = 1\nshader0 = shaders/a.slang\ntextures = \"LUT1;LUT2\"\nLUT1 = shaders/lut1.png\nLUT2 = shaders/lut2.png\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\lut2.slangp", why, sizeof(why)) == -1 && strstr(why, "texture LUT2 = shaders/lut2.png, and there is no such file"));
    put("pc_filter_presets\\lut3.slangp", "shaders = 1\nshader0 = shaders/a.slang\ntextures = LUT1\nLUT1 = shaders/lut1.png\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\lut3.slangp", why, sizeof(why)) == 0);
    put("pc_filter_presets\\ref.slangp", "#reference \"good.slangp\"\nhardScan = -10.0\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\ref.slangp", why, sizeof(why)) == 0);
    put("pc_filter_presets\\ref2.slangp", "#reference \"base.slangp\"\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\ref2.slangp", why, sizeof(why)) == -1 && strstr(why, "builds on base.slangp, and there is no such file"));
    put("pc_filter_presets\\ref3.slangp", "#reference \"lost.slangp\"\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\ref3.slangp", why, sizeof(why)) == -1 && strstr(why, "lost.slangp names shader1"));
    put("pc_filter_presets\\loop.slangp", "#reference \"loop.slangp\"\n");
    CHECK(pc_filter_preset_check("pc_filter_presets\\loop.slangp", why, sizeof(why)) == -1 && strstr(why, "presets deep"));
    {
        static const char *const made[] = {"none", "empty", "good", "quoted", "lost", "short", "lut", "lut2", "lut3", "ref", "ref2", "ref3", "loop"};
        char path[200];
        size_t i;
        for (i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
            snprintf(path, sizeof(path), "pc_filter_presets\\%s.slangp", made[i]);
            remove(path);
        }
        remove("pc_filter_presets\\shaders\\a.slang");
        remove("pc_filter_presets\\shaders\\lut1.png");
        RemoveDirectoryA("pc_filter_presets\\shaders");
        RemoveDirectoryA("pc_filter_presets");
    }
}

static void test_logic(void)
{
    char note[1000];
    int w, h, x, y, bad, look;
    test_config();
    test_font();
    test_preset_check();
    if (open_window(NULL, 1280, 960) != 0) {
        failed++;
        return;
    }
    CHECK(!strcmp(pc_window_renderer(), "software"));
    CHECK(pc_window_filter() == PC_LOOK_OFF && !strcmp(pc_window_filter_label(), "off"));
    /* sharp, on any renderer: the whole height, each game pixel's middle its exact colour. */
    CHECK(pc_window_set_filter("sharp", "nowhere\\librashader.dll", note, sizeof(note)) == 0 && strstr(note, "filter: sharp"));
    fill_made_up(320);
    pc_window_present(&pic);
    CHECK(pc_window_read_screen(image, CAPACITY, &w, &h) == 0 && w == 1280 && h == 960);
    bad = 0;
    for (y = 0; y < 224; y++)
        for (x = 0; x < 320; x++)
            if (image[(y * 960 / 224 + 2) * 1280 + x * 4 + 2] != made_up_colour((unsigned)x, (unsigned)y))
                bad++;
    CHECK(bad == 0);
    CHECK(image[1279] != 0 && image[959 * 1280] != 0);     /* no black bars: 4:3 fills a 4:3 window to its corners */
    /* The same through the offscreen drawing the timing tool uses. */
    CHECK(draw(1280, 960, image2) == 0);
    CHECK(!memcmp(image, image2, (size_t)1280 * 960 * 4));
    /* With square pixels the 320 line keeps its shape, as tall as the window. */
    pc_window_toggle_shape();
    CHECK(draw(1280, 960, image2) == 0);
    {
        /* 320 by 224 is wider than 4:3: the whole width, 896 lines, black above and below. */
        CHECK(image2[31 * 1280 + 640] == 0 && image2[33 * 1280 + 640] != 0 && image2[926 * 1280 + 640] != 0 && image2[928 * 1280 + 640] == 0);
    }
    /* The Pocket core's Original: a 320 pixel line at 64:49, as tall as the window (960 lines, 1,254 wide); Corrected: square pixels for it, as square
       pixels give it. */
    pc_window_set_shape(PC_SHAPE_ORIGINAL);
    CHECK(draw(1280, 960, image2) == 0);
    CHECK(image2[480 * 1280 + 11] == 0 && image2[480 * 1280 + 14] != 0 && image2[480 * 1280 + 1265] != 0 && image2[480 * 1280 + 1268] == 0);
    pc_window_set_shape(PC_SHAPE_CORRECTED);
    CHECK(draw(1280, 960, image2) == 0);
    CHECK(image2[31 * 1280 + 640] == 0 && image2[33 * 1280 + 640] != 0 && image2[926 * 1280 + 640] != 0 && image2[928 * 1280 + 640] == 0);
    pc_window_set_shape(PC_SHAPE_4_3);
    /* The shader looks need OpenGL: refused here, in words, with the picture as with off, and the refusal on the window. */
    for (look = PC_LOOK_SCANLINES; look < PC_LOOKS; look++) {
        CHECK(pc_window_set_filter(pc_filter_name(look), "nowhere\\librashader.dll", note, sizeof(note)) == -1);
        CHECK(strstr(note, "could not be used") && strstr(note, "OpenGL renderer") && strstr(note, "drawn as with off"));
        CHECK(pc_window_filter() == PC_LOOK_OFF);
    }
    pc_window_present(&pic);
    CHECK(pc_window_read_screen(image, CAPACITY, &w, &h) == 0);
    /* The words are white dots on a dark box at the top left; the picture is there under them. */
    {
        int white = 0;
        for (y = 0; y < 60; y++)
            for (x = 0; x < 600; x++)
                white += image[y * 1280 + x] == 0xFFFFFF;
        CHECK(white > 100);
    }
    CHECK(pc_window_set_filter("my.slangp", "nowhere\\librashader.dll", note, sizeof(note)) == -1 && strstr(note, "preset") && strstr(note, "OpenGL"));
    CHECK(pc_window_set_filter("nonesuch", "nowhere\\librashader.dll", note, sizeof(note)) == -1 && strstr(note, "no filter called nonesuch"));
    CHECK(pc_window_set_filter("off", "", note, sizeof(note)) == 0 && note[0] == 0 && pc_window_filter() == PC_LOOK_OFF);
    CHECK(pc_window_set_filter("", "", note, sizeof(note)) == 0 && pc_window_filter() == PC_LOOK_OFF);
    /* The filter key here: off, sharp, and back to off, saying the others need OpenGL when it passes them. The look's name on the window says which
       key changes it, as the program passes it in; with no key, only the name. */
    CHECK(pc_window_next_filter("F6", note, sizeof(note)) == PC_LOOK_SHARP && strstr(note, "filter: sharp"));
    CHECK(!strcmp(pc_window_shown_text(), "Filter: Sharp pixels. F6: the next filter"));
    CHECK(pc_window_next_filter("F6", note, sizeof(note)) == PC_LOOK_OFF && strstr(note, "filter: off") && strstr(note, "need the OpenGL renderer"));
    CHECK(!strcmp(pc_window_shown_text(), "Filter: Off (the CRT looks need the OpenGL renderer). F6: the next filter"));
    CHECK(pc_window_next_filter(NULL, note, sizeof(note)) == PC_LOOK_SHARP && !strcmp(pc_window_shown_text(), "Filter: Sharp pixels"));
    CHECK(pc_window_take_note(note, sizeof(note)) == 0);
    /* sharp is the right way up, offscreen and on the window. */
    CHECK(pc_window_set_filter("sharp", "", note, sizeof(note)) == 0);
    upright("sharp", 1280, 960);
    pc_window_close();
}

/* ---- OpenGL, on this PC's own graphics card, in a hidden window */

static void look_draws(int look, int w, int h)
{
    int x, y, fill_w = h * 4 / 3, left = (w - fill_w) / 2, nonblack = 0, outside = 0;
    char note[1000];
    CHECK(pc_window_set_filter(pc_filter_name(look), "nowhere\\librashader.dll", note, sizeof(note)) == 0);
    if (pc_window_filter() != look) {
        printf("look %s: %s\n", pc_filter_name(look), note);
        return;
    }
    fill_made_up(320);
    CHECK(draw(w, h, image) == 0);
    for (y = 0; y < h; y += 7)
        for (x = 0; x < w; x += 3) {
            if (x < left - 1 || x > left + fill_w)
                outside += image[y * w + x] != 0;
            else
                nonblack += image[y * w + x] != 0;
        }
    CHECK(nonblack > (h / 7) * (fill_w / 3) / 2);   /* the picture is there */
    CHECK(outside == 0);                            /* and nothing outside its place */
    CHECK(pc_window_take_note(note, sizeof(note)) == 0);
}

static void test_gl(void)
{
    char note[1000];
    int look, w, h, x, y;
    if (open_window("opengl", 1280, 960) != 0 || strcmp(pc_window_renderer(), "opengl")) {
        printf("skip: the OpenGL renderer will not start on this PC\n");
        pc_window_close();
        return;
    }
    {
        char gpu[300];
        pc_window_gpu(gpu, sizeof(gpu));
        printf("OpenGL: %s\n", gpu);
    }
    /* What off and sharp draw is the same on OpenGL as on the software renderer: each game pixel's middle keeps its colour. */
    CHECK(pc_window_set_filter("sharp", "nowhere\\librashader.dll", note, sizeof(note)) == 0);
    fill_made_up(320);
    CHECK(draw(1280, 960, image) == 0);
    {
        int bad = 0;
        for (y = 0; y < 224; y++)
            for (x = 0; x < 320; x++)
                if (image[(y * 960 / 224 + 2) * 1280 + x * 4 + 2] != made_up_colour((unsigned)x, (unsigned)y))
                    bad++;
        CHECK(bad == 0);
    }
    for (look = PC_LOOK_SCANLINES; look < PC_LOOKS; look++) {
        look_draws(look, 1280, 960);
        look_draws(look, 1920, 1080);
        look_draws(look, 2560, 1600);
    }
    /* scanlines: on a flat grey picture each game line is brighter in its middle than where it meets the next, and grey stays grey (no mask). */
    CHECK(pc_window_set_filter("scanlines", "", note, sizeof(note)) == 0);
    fill_flat(0xA0A0A0);
    CHECK(draw(1600, 1120, image) == 0);    /* 5 screen lines a game line, the place 1493 wide in the middle */
    {
        int middle = brightness(image[(100 * 5 + 2) * 1600 + 800]), edge = brightness(image[(100 * 5) * 1600 + 800]);
        uint32_t c = image[(100 * 5 + 2) * 1600 + 800];
        CHECK(middle > edge + 30);
        CHECK(((c >> 16) & 255) == (c & 255) && ((c >> 8) & 255) == (c & 255));
    }
    /* pvm: an aperture grille, so on flat white the colours change across a line, and the same columns repeat down a line's middle on every game line (no slot breaks). */
    CHECK(pc_window_set_filter("pvm", "", note, sizeof(note)) == 0);
    fill_flat(0xFFFFFF);
    CHECK(draw(1600, 1120, image) == 0);
    {
        int changes = 0, same = 1;
        for (x = 400; x < 420; x++) {
            changes += image[(100 * 5 + 2) * 1600 + x] != image[(100 * 5 + 2) * 1600 + x + 1];
            same &= image[(100 * 5 + 2) * 1600 + x] == image[(101 * 5 + 2) * 1600 + x];
        }
        CHECK(changes >= 10);
        CHECK(same);
    }
    /* crt: a colour mask, so on flat white the strongest of red, green and blue moves from one screen pixel to the next along a line. */
    CHECK(pc_window_set_filter("crt", "", note, sizeof(note)) == 0);
    CHECK(draw(1600, 1120, image) == 0);
    {
        int moves = 0;
        for (x = 400; x < 412; x++) {
            uint32_t a = image[(100 * 5 + 2) * 1600 + x], b = image[(100 * 5 + 2) * 1600 + x + 1];
            int ka = (a >> 16 & 255) >= (a >> 8 & 255) && (a >> 16 & 255) >= (a & 255) ? 0 : (a >> 8 & 255) >= (a & 255) ? 1 : 2;
            int kb = (b >> 16 & 255) >= (b >> 8 & 255) && (b >> 16 & 255) >= (b & 255) ? 0 : (b >> 8 & 255) >= (b & 255) ? 1 : 2;
            moves += ka != kb;
        }
        CHECK(moves >= 6);
    }
    /* curved: the glass bends the picture in, so the place's corners are black and its middle is not. */
    CHECK(pc_window_set_filter("curved", "", note, sizeof(note)) == 0);
    CHECK(draw(1280, 960, image) == 0);
    CHECK(image[0] == 0 && image[959 * 1280 + 1279] == 0 && image[20 * 1280 + 640] != 0 && image[480 * 1280 + 640] != 0);
    /* Every look the right way up, offscreen and on the window. */
    for (look = PC_LOOK_SHARP; look < PC_LOOKS; look++) {
        CHECK(pc_window_set_filter(pc_filter_name(look), "", note, sizeof(note)) == 0);
        upright(pc_filter_name(look), 1280, 960);
    }
    /* After every look, OpenGL is as SDL's renderer left it: off drawn after the CRT looks is the same, pixel for pixel, as off drawn first. */
    fill_made_up(320);
    CHECK(pc_window_set_filter("off", "", note, sizeof(note)) == 0);
    CHECK(draw(1280, 960, image2) == 0);
    for (look = PC_LOOK_SCANLINES; look < PC_LOOKS; look++) {
        CHECK(pc_window_set_filter(pc_filter_name(look), "", note, sizeof(note)) == 0);
        CHECK(draw(1280, 960, image) == 0);
    }
    CHECK(pc_window_set_filter("off", "", note, sizeof(note)) == 0);
    CHECK(draw(1280, 960, image) == 0);
    CHECK(!memcmp(image, image2, (size_t)1280 * 960 * 4));
    /* The window's own path: a shader look presented, then the words over it, read back from the window. */
    CHECK(pc_window_set_filter("pvm", "", note, sizeof(note)) == 0);
    pc_window_show_text("Filter: PVM / BVM monitor", 5000);
    pc_window_present(&pic);
    CHECK(pc_window_read_screen(image, CAPACITY, &w, &h) == 0 && w == 1280 && h == 960);
    {
        int white = 0, picture = 0;
        for (y = 0; y < 60; y++)
            for (x = 0; x < 900; x++)
                white += image[y * 1280 + x] == 0xFFFFFF;
        for (x = 0; x < 1280; x++)
            picture += image[600 * 1280 + x] != 0;
        CHECK(white > 100 && picture > 1000);
    }
    /* The filter key on OpenGL goes through every look in order, then back to off. */
    CHECK(pc_window_set_filter("off", "", note, sizeof(note)) == 0);
    for (look = PC_LOOK_SHARP; look <= PC_LOOKS; look++) {
        int got = pc_window_next_filter("F6", note, sizeof(note));
        CHECK(got == (look == PC_LOOKS ? PC_LOOK_OFF : look));
        CHECK(strstr(pc_window_shown_text(), pc_filter_title(got)) && strstr(pc_window_shown_text(), ". F6: the next filter"));
        CHECK(!strstr(note, "need the OpenGL"));
    }
    /* A player's preset that is not there, or a library that is not: refused in plain words, drawn as with off. */
    CHECK(pc_window_set_filter("C:\\no such folder\\crt.slangp", "C:\\no such folder\\librashader.dll", note, sizeof(note)) == -1);
    CHECK(strstr(note, "there is no preset file") && strstr(note, "drawn as with off") && pc_window_filter() == PC_LOOK_OFF);
    {
        /* A preset that passes the file check, and no library: refused for the library. A broken one: refused for what is wrong with it, before any library is looked for. */
        put("pc_filter_test_one.slang", "#version 450\n");
        put("pc_filter_test_good.slangp", "shaders = 1\nshader0 = pc_filter_test_one.slang\n");
        CHECK(pc_window_set_filter("pc_filter_test_good.slangp", "C:\\no such folder\\librashader.dll", note, sizeof(note)) == -1);
        CHECK(strstr(note, "needs librashader.dll beside the program") && strstr(note, "there is none at") && pc_window_filter() == PC_LOOK_OFF);
        {
            /* A library that is there but that Windows will not load: a damaged file, and a 32-bit Windows DLL (the wrong download for this
               64-bit program; Windows' own 32-bit version.dll stands in for it). Each is told as there, with Windows' error, never as missing. */
            static const unsigned char rest[4094];
            char here[MAX_PATH], damaged[MAX_PATH + 40], wow[MAX_PATH], dll32[MAX_PATH + 20];
            FILE *f;
            GetFullPathNameA(".", MAX_PATH, here, NULL);
            snprintf(damaged, sizeof(damaged), "%s\\pc_filter_test_damaged.dll", here);
            f = fopen(damaged, "wb");
            if (f) {
                fputs("MZ", f);
                fwrite(rest, 1, sizeof(rest), f);
                fclose(f);
            }
            CHECK(pc_window_set_filter("pc_filter_test_good.slangp", damaged, note, sizeof(note)) == -1);
            CHECK(strstr(note, "librashader.dll is there, at ") && strstr(note, "but Windows would not load it (error 193: ") &&
                  strstr(note, "It is damaged, or it is not a 64-bit Windows build") && !strstr(note, "there is none") && strstr(note, "drawn as with off") &&
                  pc_window_filter() == PC_LOOK_OFF);
            printf("a damaged library: %s\n", note);
            remove(damaged);
            if (GetSystemWow64DirectoryA(wow, MAX_PATH) > 0) {
                snprintf(dll32, sizeof(dll32), "%s\\version.dll", wow);
                CHECK(pc_window_set_filter("pc_filter_test_good.slangp", dll32, note, sizeof(note)) == -1);
                CHECK(strstr(note, "is there, at ") && strstr(note, "Windows would not load it (error 193: ") && !strstr(note, "there is none") &&
                      pc_window_filter() == PC_LOOK_OFF);
                printf("a 32-bit library: %s\n", note);
            } else {
                printf("this Windows has no 32-bit system folder: the 32-bit library is not tried\n");
            }
        }
        put("pc_filter_test_bad.slangp", "shaders = 1\nshader0 = no_such_shader.slang\n");
        CHECK(pc_window_set_filter("pc_filter_test_bad.slangp", "C:\\no such folder\\librashader.dll", note, sizeof(note)) == -1);
        CHECK(strstr(note, "no_such_shader.slang, and there is no such file") && strstr(note, "drawn as with off") && pc_window_filter() == PC_LOOK_OFF);
        remove("pc_filter_test_one.slang");
        remove("pc_filter_test_good.slangp");
        remove("pc_filter_test_bad.slangp");
    }
    {
        const char *dll = getenv("PC_FILTER_TEST_LIBRASHADER"), *preset = getenv("PC_FILTER_TEST_PRESET");
        if (dll && preset) {
            FILE *f = fopen("pc_filter_test_broken.slangp", "w");
            if (f) {
                fprintf(f, "shaders = 1\nshader0 = no_such_shader.slang\n");
                fclose(f);
            }
            CHECK(pc_window_set_filter("pc_filter_test_broken.slangp", dll, note, sizeof(note)) == -1);
            CHECK(strstr(note, "librashader could not") && pc_window_filter() == PC_LOOK_OFF);
            printf("a broken preset: %s\n", note);
            remove("pc_filter_test_broken.slangp");
            CHECK(pc_window_set_filter(preset, dll, note, sizeof(note)) == 0 && pc_window_filter() == PC_LOOK_PRESET);
            printf("%s\n", note);
            if (pc_window_filter() == PC_LOOK_PRESET) {
                fill_made_up(320);
                CHECK(draw(1280, 960, image) == 0);
                {
                    int lit = 0;
                    for (x = 0; x < 1280 * 960; x += 97)
                        lit += image[x] != 0;
                    CHECK(lit > 1280 * 960 / 97 / 2);
                }
                upright("your preset", 1280, 960);
                CHECK(pc_window_take_note(note, sizeof(note)) == 0);
                CHECK(pc_window_set_filter("off", "", note, sizeof(note)) == 0);
                CHECK(draw(1280, 960, image) == 0);
                CHECK(!memcmp(image, image2, (size_t)1280 * 960 * 4));      /* OpenGL as SDL left it, after the library's frame too */
            }
        } else {
            printf("no librashader named in the environment: the preset's own drawing is not tried\n");
        }
    }
    pc_window_close();
}

/* ---- a game frame through each look: pictures and times */

static int sheet(const char *picture, const char *folder, int w, int h)
{
    char note[1000], path[1200];
    int look, n = 1;
    const char *dll = getenv("PC_FILTER_TEST_LIBRASHADER"), *preset = getenv("PC_FILTER_TEST_PRESET");
    if (load_ppm(picture) != 0 || open_window("opengl", 640, 480) != 0) {
        printf("no picture %s, or no OpenGL\n", picture);
        return 1;
    }
    for (look = 0; look <= PC_LOOKS; look++) {
        if (look == PC_LOOKS) {
            if (!dll || !preset)
                break;
            if (pc_window_set_filter(preset, dll, note, sizeof(note)) != 0) {
                printf("%s\n", note);
                break;
            }
        } else if (pc_window_set_filter(pc_filter_name(look), "", note, sizeof(note)) != 0) {
            printf("%s\n", note);
            failed++;
            continue;
        }
        if (draw(w, h, image) != 0) {
            printf("look %s did not draw\n", pc_window_filter_label());
            failed++;
            continue;
        }
        snprintf(path, sizeof(path), "%s\\%d_%s.ppm", folder, n++, look == PC_LOOKS ? "preset" : pc_filter_name(look));
        write_ppm(path, image, w, h);
        printf("%s\n", path);
    }
    pc_window_close();
    return failed ? 1 : 0;
}

/* paced: one frame each 17 ms, near the game's 60 a second, so the GPU's clock is what it is in play rather than flat out. */
static int by_size(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static int bench(const char *picture, int w, int h, int frames, int paced)
{
    static double each[4096];
    char note[1000], gpu[300];
    int look;
    const char *dll = getenv("PC_FILTER_TEST_LIBRASHADER"), *preset = getenv("PC_FILTER_TEST_PRESET");
    if (load_ppm(picture) != 0 || open_window("opengl", 640, 480) != 0) {
        printf("no picture %s, or no OpenGL\n", picture);
        return 1;
    }
    pc_window_gpu(gpu, sizeof(gpu));
    printf("%dx%d, %d frames a look, %s, %s\n", w, h, frames, paced ? "one each 17 ms" : "back to back", gpu);
    printf("%-10s %8s %8s %8s %8s %8s %8s   (GPU ms a frame; cpu: the program's ms a frame)\n", "look", "mean", "middle", "95%", "99%", "worst", "cpu");
    for (look = 0; look <= PC_LOOKS; look++) {
        double gm, gw, cm;
        if (look == PC_LOOKS) {
            if (!dll || !preset || pc_window_set_filter(preset, dll, note, sizeof(note)) != 0)
                break;
        } else if (pc_window_set_filter(pc_filter_name(look), "", note, sizeof(note)) != 0) {
            printf("%s\n", note);
            failed++;
            continue;
        }
        pc_window_draw_offscreen(&pic, w, h, 20, 0, image, CAPACITY, &gm, &gw, NULL, &cm);  /* warm: builds, first uploads */
        if (frames > 4096 || pc_window_draw_offscreen(&pic, w, h, frames, paced ? 17 : 0, image, CAPACITY, &gm, &gw, each, &cm) != 0) {
            failed++;
            continue;
        }
        qsort(each, (size_t)frames, sizeof(each[0]), by_size);
        printf("%-10s %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f\n", pc_window_filter_label(), gm, each[frames / 2], each[frames * 95 / 100], each[frames * 99 / 100], gw, cm);
    }
    pc_window_close();
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "logic"))
        test_logic();
    else if (argc >= 2 && !strcmp(argv[1], "gl"))
        test_gl();
    else if (argc >= 6 && !strcmp(argv[1], "sheet"))
        return sheet(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
    else if (argc >= 6 && !strcmp(argv[1], "bench"))
        return bench(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), argc >= 7 && !strcmp(argv[6], "paced"));
    else {
        printf("usage: pc_filter_test logic | gl | sheet PICTURE.ppm FOLDER W H | bench PICTURE.ppm W H FRAMES [paced]\n");
        return 2;
    }
    printf("%d checks, %d failed\n", checks, failed);
    printf("RESULT: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
