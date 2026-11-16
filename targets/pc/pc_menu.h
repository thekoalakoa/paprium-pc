/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The menu bar at the top of the window: every option of the program's keys, the Scum! cheats and arcade stage, and the console settings, each
 * item with the key that does the same, so a player learns the keys from it.
 *
 * WHY IT IS DRAWN BY THE PROGRAM. Windows' own menu bar runs its own message loop while a menu is open, on the thread that also runs the game and feeds
 * the sound, so game and sound would stop while a player looks through a menu. This one is drawn with the program's own small font (pc_font.h), the
 * same in a window and full screen and on every renderer, and an automated test can draw it into a picture without showing a window.
 *
 * WHERE IT IS. In a window the bar has a strip of its own above the picture, so it never covers the game (menu_bar=off in the configuration gives the
 * window back to the picture alone). In full screen it is hidden; it comes back over the top of the picture while the mouse is at the top edge, or when
 * the menu key (F10 by default, on the controls screen's Program keys page) is pressed.
 *
 * HOW IT WORKS. A click on a title, or the menu key, opens a menu; the game pauses while one is open, and goes on as it was when the last one closes.
 * The arrows move (Left and Right go from menu to menu, Right also opens a list such as the arcade stages), Enter or a click chooses, Esc closes one
 * level. An item that does what a key does gives the program exactly that key's action. Items that would do nothing now are drawn grey.
 *
 * THE MODEL. pc_main.c fills the menus each time something they show changes (pc_menu_begin, pc_menu_add_menu, pc_menu_add_item), keeping the place of
 * whatever is open. pc_window.c asks for the pixels (pc_menu_layout, pc_menu_render_bar, pc_menu_render_box) and pc_input.c passes the keys and the
 * mouse while a menu is open. Nothing here needs SDL, so a test can drive and draw it.
 */
#ifndef PC_MENU_H
#define PC_MENU_H

#include <stdint.h>

/* What an item does when chosen. PC_MENU_DO_ACTION gives arg as PC_ACTION_* bits, as the item's key does (pc_menu_take_actions). Every other command is the
   program's own and waits in a queue for pc_menu_take_command. */
enum {
    PC_MENU_DO_NOTHING,
    PC_MENU_DO_ACTION,
    PC_MENU_DO_SOFT_RESET,
    PC_MENU_DO_FILTER,          /* arg: the look, a PC_LOOK_ number */
    PC_MENU_DO_RENDERER,        /* arg: the renderer's place in pc_menu_renderers */
    PC_MENU_DO_VSYNC,
    PC_MENU_DO_MENU_BAR,
    PC_MENU_DO_VOLUME,          /* arg: 0 to 100 */
    PC_MENU_DO_CHEAT,           /* arg: the cheat's bit, flipped */
    PC_MENU_DO_STAGE,           /* arg: 0 to 25 */
    PC_MENU_DO_SIX_BUTTON,      /* arg: the player, 0 to 2, flipped */
    PC_MENU_DO_CONSOLE,         /* arg: PC_CONSOLE_* */
    PC_MENU_DO_X32,
    PC_MENU_DO_MEGACD,          /* arg: PC_MEGACD_* */
    PC_MENU_DO_CPUMATE,
    PC_MENU_DO_KEYS,            /* the Help pages */
    PC_MENU_DO_ABOUT,
    PC_MENU_DO_CONSOLE_RESET,
    PC_MENU_DO_SHAPE,           /* arg: PC_SHAPE_* (pc_config.h) */
    PC_MENU_DO_ARCADE_MODE,     /* arg: PC_ARCADE_* (pc_scum.h) */
    PC_MENU_DO_ARCADE_COINS,    /* arg: PC_COINS_* (pc_scum.h) */
    PC_MENU_DO_ATTRACT,         /* flipped */
    PC_MENU_DO_THREE_PLAYERS    /* flipped */
};

/* An item's look. */
enum {
    PC_MENU_TICK = 1,           /* a tick before it: on, or the one chosen */
    PC_MENU_GREY = 2,           /* drawn grey, and cannot be chosen */
    PC_MENU_LINE = 4,           /* a line above it, between groups */
    PC_MENU_OPENS = 8           /* it opens the list whose menu number is arg (a list is a menu with no title on the bar) */
};

#define PC_MENU_MENUS 16
#define PC_MENU_ITEMS 32

/* ---- the model, filled by pc_main.c */
void pc_menu_begin(void);
int pc_menu_add_menu(const char *title, int on_bar);           /* returns its number */
void pc_menu_add_item(int menu, const char *label, const char *key, int command, int arg, unsigned look);
void pc_menu_set_hint(const char *text);                         /* drawn at the right end of the bar */

/* ---- state */
int pc_menu_is_open(void);              /* a menu or a page is open: the game is paused */
void pc_menu_open_first(void);          /* the menu key: opens the first menu, or closes what is open; on a page in parts, its next part first */
void pc_menu_close(void);
/* A page of text over the paused game (the Help menu's Keys and About), closed by any key or click. title and text are copied. In a window too small
   for it (the smallest, at scale 1), its lines are broken to fit across and it shows in parts: a key or a click shows the next part, and closes the
   page after the last. A menu or a list taller than the window goes on in a second column. */
void pc_menu_show_page(const char *title, const char *text);
int pc_menu_page_open(void);

/* Keys and the mouse while it is involved. key is an SDL scancode; returns 1 when it used the key. menu_key is the scancode of the program's menu key. */
int pc_menu_key(int key, int menu_key);
/* The mouse, in the window's pixels: moved (button 0), pressed (1) or let go (2). Returns 1 when the event was the menu's (on the bar, on a box, or
   closing a menu). */
int pc_menu_mouse(int x, int y, int event);

/* What a chosen item asked for: the PC_ACTION_* bits of items that do what a key does, and the program's own commands, one at a time. */
unsigned pc_menu_take_actions(void);
int pc_menu_take_command(int *arg);

/* ---- drawing, for pc_window.c. Every pixel is 0xAARRGGBB. */
/* The bar's height in a window out_h pixels tall: the font's cells at a whole scale that grows with the window. */
int pc_menu_bar_height(int out_h);
/* The window's size now. reserved: the window keeps a strip for the bar (a window, not full screen, with the bar on). */
void pc_menu_layout(int out_w, int out_h, int reserved);
int pc_menu_bar_shown(void);            /* the bar is drawn now: reserved, or brought back by the mouse or a menu open */
unsigned pc_menu_version(void);         /* changes whenever anything drawn changes */
int pc_menu_render_bar(uint32_t *out, int capacity, int *w, int *h);
/* The open boxes, from 0: a menu, a list beside it, a page. Returns 0 and the box's place and size, or -1 when there is no such box (or out is too small). */
int pc_menu_render_box(int which, uint32_t *out, int capacity, int *x, int *y, int *w, int *h);

/* ---- for tests */
const char *pc_menu_item_label(int menu, int item);
int pc_menu_items(int menu);
int pc_menu_count(void);                        /* the menus, the lists among them */
const char *pc_menu_title(int menu);            /* "" for no such menu */
int pc_menu_on_bar(int menu);
/* An item's key, command, argument and look. Returns 0, or -1 for no such item. */
int pc_menu_item_info(int menu, int item, const char **key, int *command, int *arg, unsigned *look);
int pc_menu_highlight(int *menu, int *item);    /* the open menu (or list) and its highlighted item; 0 when none is open */

#endif /* PC_MENU_H */
