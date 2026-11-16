/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The menu bar's model, keys, mouse and drawing (targets/pc/pc_menu.h), on made-up menus, with no SDL and no window. Built and run by
 * tests/test_pc_machine.py. Usage: pc_menu_test [a folder for its pictures]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_font.h"
#include "pc_menu.h"

static int checks, failed;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

/* SDL's scancodes for the keys used here (the USB keyboard usage numbers). */
enum { K_RETURN = 40, K_ESCAPE = 41, K_RIGHT = 79, K_LEFT = 80, K_DOWN = 81, K_UP = 82, K_F10 = 67, K_A = 4 };

static uint32_t pixels[4096 * 2048];
static int menu_game, menu_video, menu_scum, list_stage;

/* Menus like the program's: two on the bar with actions and commands, a grey item, ticks, a list opened from an item. */
static void build(int stage, int health)
{
    int i;
    char name[16];
    pc_menu_begin();
    menu_game = pc_menu_add_menu("Game", 1);
    pc_menu_add_item(menu_game, "Pause", "P", PC_MENU_DO_ACTION, 4, 0);
    pc_menu_add_item(menu_game, "Soft Reset", "", PC_MENU_DO_SOFT_RESET, 0, 0);
    pc_menu_add_item(menu_game, "Quit", "Escape", PC_MENU_DO_ACTION, 1, PC_MENU_LINE);
    menu_video = pc_menu_add_menu("Video", 1);
    pc_menu_add_item(menu_video, "Next filter", "F6", PC_MENU_DO_ACTION, 256, 0);
    pc_menu_add_item(menu_video, "CRT", "", PC_MENU_DO_FILTER, 3, PC_MENU_GREY);
    menu_scum = pc_menu_add_menu("Scum!", 1);
    pc_menu_add_item(menu_scum, "Health", "", PC_MENU_DO_CHEAT, 1, health ? PC_MENU_TICK : 0);
    list_stage = pc_menu_add_menu("Arcade stage", 0);
    pc_menu_add_item(menu_scum, "Arcade stage", "", PC_MENU_DO_NOTHING, list_stage, PC_MENU_OPENS | PC_MENU_LINE);
    pc_menu_add_item(list_stage, "Off", "", PC_MENU_DO_STAGE, 0, stage == 0 ? PC_MENU_TICK : 0);
    for (i = 1; i <= 25; i++) {
        snprintf(name, sizeof(name), "Stage %d", i);
        pc_menu_add_item(list_stage, name, "", PC_MENU_DO_STAGE, i, stage == i ? PC_MENU_TICK : 0);
    }
    pc_menu_set_hint("Menu: F10");
}

/* The program's own menu bar as pc_main.c fills it, its titles, items and keys, and the 26 arcade stages by their names. */
static int full_scum, full_stages;

static void build_full(void)
{
    static const char *const stages[25] = {
        "Block 888", "Intercom", "MTS", "Bion Mart", "Roof-Top", "Police HQ", "Ice Factory", "Reformatory", "Sanophix PU", "Central Park", "Green Cross",
        "The Docks", "Golden Of Seas", "Deep Storage", "Aquaworld", "Blue Bar", "Powder-Zone", "Bohpal B-2", "Juvanium", "Sky Spa", "Digniti KTV", "The Vault",
        "Sanophix Labs", "The Solarium", "Arena"};
    static const char *const looks[6] = {"off", "sharp", "scanlines", "crt, a home television", "pvm, a broadcast monitor", "curved"};
    char label[64];
    int m, i;
    pc_menu_begin();
    m = pc_menu_add_menu("Game", 1);
    pc_menu_add_item(m, "Pause", "P", PC_MENU_DO_ACTION, 4, 0);
    pc_menu_add_item(m, "Soft Reset", "", PC_MENU_DO_SOFT_RESET, 0, 0);
    pc_menu_add_item(m, "Quit", "Escape", PC_MENU_DO_ACTION, 1, PC_MENU_LINE);
    m = pc_menu_add_menu("Video", 1);
    for (i = 0; i < 6; i++) {
        snprintf(label, sizeof(label), "Filter: %s", looks[i]);
        pc_menu_add_item(m, label, "", PC_MENU_DO_FILTER, i, i == 0 ? PC_MENU_TICK : 0);
    }
    pc_menu_add_item(m, "Next filter", "F6", PC_MENU_DO_ACTION, 256, 0);
    pc_menu_add_item(m, "Full screen", "F11", PC_MENU_DO_ACTION, 32, PC_MENU_LINE);
    pc_menu_add_item(m, "Picture shape: 4:3", "F2", PC_MENU_DO_ACTION, 8, PC_MENU_TICK);
    pc_menu_add_item(m, "Picture shape: square pixels", "F2", PC_MENU_DO_ACTION, 8, 0);
    pc_menu_add_item(m, "The cart's sprite limits", "F1", PC_MENU_DO_ACTION, 2, PC_MENU_TICK);
    pc_menu_add_item(m, "Menu bar in a window", "", PC_MENU_DO_MENU_BAR, 0, PC_MENU_LINE | PC_MENU_TICK);
    pc_menu_add_item(m, "Renderer, from the next start", "", PC_MENU_DO_NOTHING, 0, 0);
    pc_menu_add_item(m, "Vsync, from the next start", "", PC_MENU_DO_VSYNC, 0, PC_MENU_TICK);
    m = pc_menu_add_menu("Sound", 1);
    pc_menu_add_item(m, "Mute", "F3", PC_MENU_DO_ACTION, 16, 0);
    for (i = 100; i > 0; i -= 25) {
        snprintf(label, sizeof(label), "Volume %d %%", i);
        pc_menu_add_item(m, label, "", PC_MENU_DO_VOLUME, i, i == 100 ? PC_MENU_LINE | PC_MENU_TICK : 0);
    }
    m = pc_menu_add_menu("Controls", 1);
    pc_menu_add_item(m, "Controls screen", "F4", PC_MENU_DO_ACTION, 64, 0);
    pc_menu_add_item(m, "Menu bar", "F10", PC_MENU_DO_NOTHING, 0, 0);
    full_scum = pc_menu_add_menu("Scum!", 1);
    pc_menu_add_item(full_scum, "Health: no damage", "", PC_MENU_DO_CHEAT, 1, 0);
    pc_menu_add_item(full_scum, "Pills: a Blu pill is not spent", "", PC_MENU_DO_CHEAT, 2, 0);
    pc_menu_add_item(full_scum, "Lives: a death is not spent", "", PC_MENU_DO_CHEAT, 4, 0);
    full_stages = pc_menu_add_menu("Arcade stage", 0);
    pc_menu_add_item(full_scum, "Arcade stage: off", "", PC_MENU_DO_NOTHING, full_stages, PC_MENU_LINE | PC_MENU_OPENS);
    pc_menu_add_item(full_stages, "Off", "", PC_MENU_DO_STAGE, 0, PC_MENU_TICK);
    for (i = 0; i < 25; i++)
        pc_menu_add_item(full_stages, stages[i], "", PC_MENU_DO_STAGE, i + 1, 0);
    pc_menu_add_item(full_scum, "6-Button pad, player 1", "", PC_MENU_DO_SIX_BUTTON, 0, PC_MENU_LINE | PC_MENU_TICK);
    pc_menu_add_item(full_scum, "6-Button pad, player 2", "", PC_MENU_DO_SIX_BUTTON, 1, PC_MENU_TICK);
    pc_menu_add_item(full_scum, "Soft Reset, for the stage", "", PC_MENU_DO_SOFT_RESET, 0, PC_MENU_LINE);
    m = pc_menu_add_menu("Console", 1);
    {
        static const char *const consoles[] = {"Mega Drive", "Genesis", "Mega Drive 2", "Genesis Model 2", "Genesis 3", "Nomad", "Mega Jet", "TeraDrive",
                                               "Unknown"};
        static const char *const units[] = {"None", "Mega-CD / Sega CD", "Mega-CD 2 / Sega CD 2", "Wondermega", "Wondermega M2 / X'Eye", "LaserActive",
                                            "Aiwa CSD-GM1", "CDX", "Unknown CD"};
        int list = pc_menu_add_menu("Consoles", 0);
        pc_menu_add_item(m, "Console: Mega Drive", "", PC_MENU_DO_NOTHING, list, PC_MENU_OPENS);
        for (i = 0; i < 9; i++)
            pc_menu_add_item(list, consoles[i], "", PC_MENU_DO_CONSOLE, i, i == 0 ? PC_MENU_TICK : 0);
        list = pc_menu_add_menu("Mega CD units", 0);
        pc_menu_add_item(m, "Mega CD: None", "", PC_MENU_DO_NOTHING, list, PC_MENU_OPENS);
        for (i = 0; i < 9; i++)
            pc_menu_add_item(list, units[i], "", PC_MENU_DO_MEGACD, i, i == 0 ? PC_MENU_TICK : 0);
    }
    pc_menu_add_item(m, "32X", "", PC_MENU_DO_X32, 0, PC_MENU_LINE);
    pc_menu_add_item(m, "CPU MATE, with the Mega CD", "", PC_MENU_DO_CPUMATE, 0, PC_MENU_GREY);
    pc_menu_add_item(m, "CPU MATE: needs a Mega CD unit", "", PC_MENU_DO_NOTHING, 0, PC_MENU_GREY);
    pc_menu_add_item(m, "Soft Reset, for these", "", PC_MENU_DO_SOFT_RESET, 0, PC_MENU_LINE);
    m = pc_menu_add_menu("Help", 1);
    pc_menu_add_item(m, "Keys", "", PC_MENU_DO_KEYS, 0, 0);
    pc_menu_add_item(m, "About", "", PC_MENU_DO_ABOUT, 0, 0);
    pc_menu_set_hint("Menu: F10");
}

/* A Keys page as the program writes it: 26 lines, its last one the longest. */
static const char keys_page[] =
    "The program's keys:\n  F4             Controls screen\n  P              Pause\n  F3             Mute\n  F1             Sprite limits\n"
    "  F2             Picture shape\n  F6             Next filter\n  F11            Full screen\n  Escape         Quit\n  F10            Menu bar\n\n"
    "Player 1 and player 2:\n  Up     Up             I\n  Down   Down           K\n  Left   Left           J\n  Right  Right          L\n"
    "  A      Z              1\n  B      X              2\n  C      C              3\n  X      A              4\n  Y      S              5\n"
    "  Z      D              6\n  Start  Return         Keypad Enter\n  Mode   Right Shift    Keypad 0\n\n"
    "The controls screen (F4) sets every key and pad button.";

/* The colour at x, y of a picture w wide. */
static uint32_t at(int w, int x, int y) { return pixels[y * w + x]; }

/* How many pixels of a picture have this colour. */
static int count(int n, uint32_t colour)
{
    int i, k = 0;
    for (i = 0; i < n; i++)
        k += pixels[i] == colour;
    return k;
}

static void write_ppm(const char *folder, const char *name, int w, int h)
{
    char path[1024];
    FILE *f;
    int i;
    if (!folder)
        return;
    snprintf(path, sizeof(path), "%s/%s.ppm", folder, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        unsigned char rgb[3] = {(unsigned char)(pixels[i] >> 16), (unsigned char)(pixels[i] >> 8), (unsigned char)pixels[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    int w, h, x, y, m, it, arg;
    unsigned v;

    /* ---- sizes: the bar grows with the window by whole steps */
    CHECK(pc_menu_bar_height(462) == 14 && pc_menu_bar_height(700) == 28 && pc_menu_bar_height(924) == 28 && pc_menu_bar_height(1162) == 42);
    CHECK(pc_menu_bar_height(1400) == 56 && pc_menu_bar_height(1600) == 56 && pc_menu_bar_height(100) == 14);
    /* a window of the configuration's scale: the picture's lines times the scale, and the bar's strip, keep the same scale for the bar */
    for (x = 1; x <= 7; x++)
        CHECK(pc_menu_bar_height(224 * x + pc_menu_bar_height(224 * x)) == pc_menu_bar_height(224 * x));

    /* ---- a window: the bar keeps its strip and is always drawn */
    build(0, 0);
    pc_menu_layout(896, 700, 1);
    CHECK(pc_menu_bar_shown() && !pc_menu_is_open());
    CHECK(pc_menu_render_bar(NULL, 0, &w, &h) == 0 && w == 896 && h == 28);
    CHECK(pc_menu_render_bar(pixels, 10, &w, &h) == -1);
    CHECK(pc_menu_render_bar(pixels, (int)(sizeof(pixels) / 4), &w, &h) == 0);
    CHECK(count(w * h, 0xFFE6E8EBu) > 300);         /* the titles' letters */
    CHECK(count(w * h, 0xFF8A9099u) > 50);          /* the hint at the right */
    CHECK(at(w, 2, 2) == 0xFF22262Bu && at(w, 2, h - 1) == 0xFF3B4148u);
    write_ppm(folder, "bar", w, h);
    CHECK(pc_menu_render_box(0, NULL, 0, &x, &y, &w, &h) == -1);

    /* ---- the menu key opens the first menu; arrows move; Enter chooses; the item gives its key's action */
    v = pc_menu_version();
    pc_menu_open_first();
    CHECK(pc_menu_is_open() && pc_menu_highlight(&m, &it) && m == menu_game && it == 0);
    CHECK(pc_menu_version() != v);
    CHECK(pc_menu_render_box(0, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0 && y == 28 && x >= 0 && w > 100 && h > 30);
    CHECK(count(w * h, 0xFF2F6DB5u) > 100);         /* the highlighted row */
    write_ppm(folder, "menu_game", w, h);
    CHECK(pc_menu_key(K_RETURN, K_F10) == 1);
    CHECK(!pc_menu_is_open() && pc_menu_take_actions() == 4 && pc_menu_take_actions() == 0);

    /* Down twice reaches Quit; Up from the top goes round to the bottom */
    pc_menu_open_first();
    pc_menu_key(K_UP, K_F10);
    CHECK(pc_menu_highlight(&m, &it) && it == 2);
    pc_menu_key(K_RETURN, K_F10);
    CHECK(pc_menu_take_actions() == 1);
    /* a command of the program's own waits in the queue */
    pc_menu_open_first();
    pc_menu_key(K_DOWN, K_F10);
    pc_menu_key(K_RETURN, K_F10);
    CHECK(pc_menu_take_actions() == 0 && pc_menu_take_command(&arg) == PC_MENU_DO_SOFT_RESET && pc_menu_take_command(&arg) == 0);

    /* Right goes to the next menu, Left back, round the bar; Esc closes; the menu key closes too; a key while closed is not the menu's */
    pc_menu_open_first();
    pc_menu_key(K_RIGHT, K_F10);
    CHECK(pc_menu_highlight(&m, &it) && m == menu_video);
    pc_menu_key(K_LEFT, K_F10);
    pc_menu_key(K_LEFT, K_F10);
    CHECK(pc_menu_highlight(&m, &it) && m == menu_scum);
    CHECK(pc_menu_key(K_ESCAPE, K_F10) == 1 && !pc_menu_is_open());
    CHECK(pc_menu_key(K_A, K_F10) == 0);
    pc_menu_open_first();
    CHECK(pc_menu_key(K_F10, K_F10) == 1 && !pc_menu_is_open());

    /* a grey item cannot be chosen */
    pc_menu_open_first();
    pc_menu_key(K_RIGHT, K_F10);
    pc_menu_key(K_DOWN, K_F10);
    pc_menu_key(K_RETURN, K_F10);
    CHECK(pc_menu_is_open() && pc_menu_take_command(&arg) == 0);
    pc_menu_close();

    /* ---- the list: Right on its item opens it on the ticked one; Down and Enter choose a stage */
    build(3, 1);
    pc_menu_open_first();
    pc_menu_key(K_LEFT, K_F10);
    pc_menu_key(K_DOWN, K_F10);
    pc_menu_key(K_RIGHT, K_F10);
    CHECK(pc_menu_highlight(&m, &it) && m == list_stage && it == 3);
    CHECK(pc_menu_render_box(1, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0 && h >= 26 * 12 && x > 0);
    write_ppm(folder, "list_stage", w, h);
    pc_menu_key(K_DOWN, K_F10);
    pc_menu_key(K_RETURN, K_F10);
    CHECK(!pc_menu_is_open() && pc_menu_take_command(&arg) == PC_MENU_DO_STAGE && arg == 4);
    /* Left closes the list, not the menu */
    pc_menu_open_first();
    pc_menu_key(K_LEFT, K_F10);
    pc_menu_key(K_DOWN, K_F10);
    pc_menu_key(K_RIGHT, K_F10);
    pc_menu_key(K_LEFT, K_F10);
    CHECK(pc_menu_highlight(&m, &it) && m == menu_scum && it == 1);
    /* the ticked Health draws its tick */
    pc_menu_key(K_UP, K_F10);
    CHECK(pc_menu_render_box(0, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0);
    write_ppm(folder, "menu_scum", w, h);
    pc_menu_close();

    /* ---- the mouse: a click on a title opens it, on the open title closes it, on an item chooses it, outside closes */
    build(0, 0);
    pc_menu_layout(896, 700, 1);
    CHECK(pc_menu_mouse(10, 10, 1) == 1 && pc_menu_highlight(&m, &it) && m == menu_game);
    CHECK(pc_menu_render_box(0, NULL, 0, &x, &y, &w, &h) == 0);
    CHECK(pc_menu_mouse(x + 20, y + 8 + 24 + 12, 0) == 1 && pc_menu_highlight(&m, &it) && it == 1);
    CHECK(pc_menu_mouse(x + 20, y + 8 + 24 + 12, 1) == 1 && !pc_menu_is_open() && pc_menu_take_command(&arg) == PC_MENU_DO_SOFT_RESET);
    CHECK(pc_menu_mouse(10, 10, 1) == 1 && pc_menu_is_open());
    CHECK(pc_menu_mouse(10, 10, 1) == 1 && !pc_menu_is_open());
    CHECK(pc_menu_mouse(10, 10, 1) == 1 && pc_menu_mouse(600, 600, 1) == 1 && !pc_menu_is_open());
    CHECK(pc_menu_mouse(600, 600, 1) == 0);     /* a click on the picture with nothing open is not the menu's */
    /* moving over another title while one is open opens that one */
    pc_menu_mouse(10, 10, 1);
    pc_menu_mouse(120, 10, 0);
    CHECK(pc_menu_highlight(&m, &it) && m == menu_video);
    pc_menu_close();

    /* ---- full screen: no strip; the bar shows while the mouse is at the top edge, or while a menu is open */
    pc_menu_layout(2560, 1600, 0);
    CHECK(!pc_menu_bar_shown());
    pc_menu_mouse(1000, 20, 0);
    CHECK(pc_menu_bar_shown());
    pc_menu_mouse(1000, 900, 0);
    CHECK(!pc_menu_bar_shown());
    pc_menu_open_first();
    pc_menu_mouse(1000, 900, 0);
    CHECK(pc_menu_bar_shown());
    CHECK(pc_menu_render_bar(pixels, (int)(sizeof(pixels) / 4), &w, &h) == 0 && w == 2560 && h == 56);
    write_ppm(folder, "bar_fullscreen", w, h);
    pc_menu_close();
    pc_menu_mouse(1000, 900, 0);
    CHECK(!pc_menu_bar_shown());

    /* ---- a page: drawn in the middle, closed by any key or a click */
    pc_menu_layout(896, 700, 1);
    pc_menu_show_page("Keys", "F1  sprite limits\nF2  picture shape");
    CHECK(pc_menu_is_open() && pc_menu_page_open());
    CHECK(pc_menu_render_box(0, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0 && x > 0 && y > 0 && count(w * h, 0xFFF2C14Eu) > 20);
    write_ppm(folder, "page", w, h);
    CHECK(pc_menu_key(K_A, K_F10) == 1 && !pc_menu_is_open());
    pc_menu_show_page("About", "x");
    CHECK(pc_menu_mouse(5, 5, 1) == 1 && !pc_menu_is_open());

    /* ---- the program's own menus in a window of scale 3 (896 x 700): the stage list is one column, the Keys page one part, as they always were */
    build_full();
    pc_menu_layout(896, 700, 1);
    pc_menu_mouse(10, 10, 1);
    for (it = 0; it < 4; it++)
        pc_menu_key(K_RIGHT, K_F10);
    pc_menu_key(K_DOWN, K_F10);
    pc_menu_key(K_DOWN, K_F10);
    pc_menu_key(K_DOWN, K_F10);
    pc_menu_key(K_RIGHT, K_F10);
    CHECK(pc_menu_highlight(&m, &it) && m == full_stages && it == 0);
    CHECK(pc_menu_render_box(1, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0 && h == (26 * 12 + 8) * 2);
    pc_menu_close();
    pc_menu_show_page("Keys", keys_page);
    CHECK(pc_menu_render_box(0, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0 && h == (16 + 29 * 10) * 2 && w == (16 + 55 * 6) * 2);
    CHECK(pc_menu_key(K_UP, K_F10) == 1 && !pc_menu_is_open());

    /* ---- the smallest window, scale 1: 298 x 238 with the bar's strip. Every title can be clicked inside the window, every arcade stage can be
       reached (the list in two columns), and the Keys page fits across, in two parts down: a key shows the second part, the next key closes it. */
    pc_menu_layout(298, 238, 1);
    {
        int seen[PC_MENU_MENUS] = {0}, titles = 0, opened = 0, k, items[32] = {0}, reached = 0, arena_x = -1, arena_y = -1;
        for (k = 0; k < PC_MENU_MENUS; k++)
            titles += pc_menu_on_bar(k);
        for (x = 0; x < 298; x++) {
            pc_menu_close();
            if (pc_menu_mouse(x, 6, 1) == 1 && pc_menu_highlight(&m, &it) && !seen[m]) {
                seen[m] = 1;
                opened++;
            }
        }
        pc_menu_close();
        CHECK(titles == 7 && opened == 7);
        CHECK(pc_menu_render_bar(pixels, (int)(sizeof(pixels) / 4), &w, &h) == 0 && w == 298 && h == 14);
        write_ppm(folder, "bar_scale1", w, h);
        /* every title whole: as many dots of the titles' ink as in a window wide enough for the usual room around them */
        k = count(w * h, 0xFFE6E8EBu);
        pc_menu_layout(400, 238, 1);
        CHECK(pc_menu_render_bar(pixels, (int)(sizeof(pixels) / 4), &w, &h) == 0 && w == 400 && k > 300 && count(w * h, 0xFFE6E8EBu) == k);
        pc_menu_layout(298, 238, 1);
        /* every menu's box inside the window */
        pc_menu_open_first();
        for (k = 0; k < 7; k++) {
            CHECK(pc_menu_render_box(0, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0 && x >= 0 && y >= 14 && x + w <= 298 && y + h <= 238);
            pc_menu_key(K_RIGHT, K_F10);
        }
        pc_menu_close();
        /* the stage list: in the window, and each of its 26 items under the mouse somewhere inside it */
        pc_menu_open_first();
        for (k = 0; k < 4; k++)
            pc_menu_key(K_RIGHT, K_F10);
        for (k = 0; k < 3; k++)
            pc_menu_key(K_DOWN, K_F10);
        pc_menu_key(K_RIGHT, K_F10);
        CHECK(pc_menu_highlight(&m, &it) && m == full_stages);
        CHECK(pc_menu_render_box(1, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0 && x >= 0 && y >= 0 && x + w <= 298 && y + h <= 238);
        write_ppm(folder, "list_stage_scale1", w, h);
        {
            int bx = x, by = y, bw = w, bh = h, px, py;
            for (py = by; py < by + bh; py += 2)
                for (px = bx; px < bx + bw; px += 3) {
                    pc_menu_mouse(px, py, 0);
                    if (pc_menu_highlight(&m, &it) && m == full_stages && it >= 0 && it < 32 && !items[it]) {
                        items[it] = 1;
                        reached++;
                        if (it == 25) {
                            arena_x = px;
                            arena_y = py;
                        }
                    }
                }
        }
        CHECK(reached == 26);
        /* a click on the last stage, the Arena, chooses it */
        CHECK(arena_x >= 0 && pc_menu_mouse(arena_x, arena_y, 1) == 1 && !pc_menu_is_open());
        CHECK(pc_menu_take_command(&arg) == PC_MENU_DO_STAGE && arg == 25);
        pc_menu_close();
        /* the Keys page */
        pc_menu_show_page("Keys", keys_page);
        CHECK(pc_menu_render_box(0, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0 && x >= 0 && y >= 0 && x + w <= 298 && y + h <= 238);
        write_ppm(folder, "page_keys_scale1_part1", w, h);
        CHECK(pc_menu_key(K_A, K_F10) == 1 && pc_menu_page_open());
        CHECK(pc_menu_render_box(0, pixels, (int)(sizeof(pixels) / 4), &x, &y, &w, &h) == 0 && x >= 0 && y >= 0 && x + w <= 298 && y + h <= 238);
        write_ppm(folder, "page_keys_scale1_part2", w, h);
        CHECK(pc_menu_key(K_A, K_F10) == 1 && !pc_menu_is_open());
        /* a click goes through the parts the same way */
        pc_menu_show_page("Keys", keys_page);
        CHECK(pc_menu_mouse(5, 5, 1) == 1 && pc_menu_page_open() && pc_menu_mouse(5, 5, 1) == 1 && !pc_menu_is_open());
        /* and so does the menu input, a pad's Menu bar button among them (the program calls pc_menu_open_first for it): the next part, then closed */
        pc_menu_show_page("Keys", keys_page);
        pc_menu_open_first();
        CHECK(pc_menu_page_open());
        pc_menu_open_first();
        CHECK(!pc_menu_is_open());
    }

    /* ---- the version stays while nothing drawn changes */
    build(0, 0);
    v = pc_menu_version();
    build(0, 0);
    CHECK(pc_menu_version() == v);
    build(0, 1);
    CHECK(pc_menu_version() != v);

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
