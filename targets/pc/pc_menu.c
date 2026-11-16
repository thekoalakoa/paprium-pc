/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_menu.h. Sizes are in the font's pixels (pc_font.h: cells of 6 by 8) times the bar's scale, a whole number that grows with the window, so
 * the letters stay sharp: 1 up to a window 671 pixels tall, 2 up to 1007, 3 up to 1343, then 4.
 */
#include <stdio.h>
#include <string.h>

#include "pc_font.h"
#include "pc_menu.h"

/* SDL's scancodes for the keys the menu uses (the USB keyboard usage numbers SDL keeps), so this file needs no SDL. */
enum { KEY_RETURN = 40, KEY_ESCAPE = 41, KEY_SPACE = 44, KEY_RIGHT = 79, KEY_LEFT = 80, KEY_DOWN = 81, KEY_UP = 82, KEY_KP_ENTER = 88 };

/* Colours, 0xAARRGGBB. */
#define C_BAR 0xFF22262Bu
#define C_BAR_LINE 0xFF3B4148u
#define C_TEXT 0xFFE6E8EBu
#define C_DIM 0xFF8A9099u
#define C_GREY 0xFF5E646Cu
#define C_KEY 0xFFB5BBC3u
#define C_LIT 0xFF2F6DB5u
#define C_LIT_TEXT 0xFFFFFFFFu
#define C_BOX 0xF71A1D21u
#define C_EDGE 0xFF59616Bu
#define C_TITLE 0xFFF2C14Eu

#define CELL_W 6
#define CELL_H 8
#define BAR_ROWS 14             /* the bar's height in font pixels */
#define ROW 12                  /* an item's row */
#define SEP 5                   /* a line between groups */
#define PAD 4
#define TICK_COL 10             /* the tick's column before the label */
#define KEY_GAP 16              /* at least this between a label and its key */

typedef struct Item {
    char label[56];
    char key[24];
    int command, arg;
    unsigned look;
} Item;

typedef struct Menu {
    char title[20];
    int on_bar, n;
    Item item[PC_MENU_ITEMS];
} Menu;

typedef struct Rect { int x, y, w, h; } Rect;

static Menu menus[PC_MENU_MENUS];
static int n_menus;
static char hint[48];
static int open_menu = -1, open_item, open_list = -1, list_item, hover_title = -1, peek;
static int page, page_part;
static char page_title[64], page_text[3000];
static int out_w = 640, out_h = 480, reserved, scale = 1;
static unsigned actions;
static int queue_cmd[16], queue_arg[16], queue_n;

/* ---- the model */

void pc_menu_begin(void)
{
    n_menus = 0;
    memset(menus, 0, sizeof(menus));
}

int pc_menu_add_menu(const char *title, int on_bar)
{
    Menu *m;
    if (n_menus >= PC_MENU_MENUS)
        return PC_MENU_MENUS - 1;
    m = &menus[n_menus];
    snprintf(m->title, sizeof(m->title), "%s", title);
    m->on_bar = on_bar;
    return n_menus++;
}

void pc_menu_add_item(int menu, const char *label, const char *key, int command, int arg, unsigned look)
{
    Menu *m;
    Item *it;
    if (menu < 0 || menu >= n_menus || menus[menu].n >= PC_MENU_ITEMS)
        return;
    m = &menus[menu];
    it = &m->item[m->n++];
    snprintf(it->label, sizeof(it->label), "%s", label);
    snprintf(it->key, sizeof(it->key), "%s", key ? key : "");
    it->command = command;
    it->arg = arg;
    it->look = look;
}

void pc_menu_set_hint(const char *text) { snprintf(hint, sizeof(hint), "%s", text ? text : ""); }
const char *pc_menu_item_label(int menu, int item) { return menu >= 0 && menu < n_menus && item >= 0 && item < menus[menu].n ? menus[menu].item[item].label : ""; }
int pc_menu_items(int menu) { return menu >= 0 && menu < n_menus ? menus[menu].n : 0; }
int pc_menu_count(void) { return n_menus; }
const char *pc_menu_title(int menu) { return menu >= 0 && menu < n_menus ? menus[menu].title : ""; }
int pc_menu_on_bar(int menu) { return menu >= 0 && menu < n_menus && menus[menu].on_bar; }

int pc_menu_item_info(int menu, int item, const char **key, int *command, int *arg, unsigned *look)
{
    const Item *it;
    if (menu < 0 || menu >= n_menus || item < 0 || item >= menus[menu].n)
        return -1;
    it = &menus[menu].item[item];
    *key = it->key;
    *command = it->command;
    *arg = it->arg;
    *look = it->look;
    return 0;
}

/* ---- geometry */

static int text_w(const char *s) { return (int)strlen(s) * CELL_W * scale; }

int pc_menu_bar_height(int h)
{
    int s = h / 336;
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    return BAR_ROWS * s;
}

void pc_menu_layout(int w, int h, int keep_strip)
{
    out_w = w > 0 ? w : 1;
    out_h = h > 0 ? h : 1;
    reserved = keep_strip;
    scale = pc_menu_bar_height(out_h) / BAR_ROWS;
}

int pc_menu_bar_shown(void) { return reserved || peek || open_menu >= 0; }

/* The room each side of a title on the bar, in font pixels: 6, or less when the titles would not all fit across the window that way (the smallest
   window, at scale 1, is 298 pixels wide). */
static int title_gap(void)
{
    int i, gap, chars = 0, titles = 0;
    for (i = 0; i < n_menus; i++) {
        if (menus[i].on_bar) {
            chars += (int)strlen(menus[i].title);
            titles++;
        }
    }
    for (gap = 6; gap > 1; gap--)
        if ((PAD + chars * CELL_W + titles * 2 * gap) * scale <= out_w)
            break;
    return gap;
}

/* Where each title of the bar is. */
static Rect title_rect(int m)
{
    Rect r = {PAD * scale, 0, 0, BAR_ROWS * scale};
    int i, gap = title_gap();
    for (i = 0; i < n_menus; i++) {
        if (!menus[i].on_bar)
            continue;
        r.w = text_w(menus[i].title) + 2 * gap * scale;
        if (i == m)
            return r;
        r.x += r.w;
    }
    r.w = 0;
    return r;
}

static int item_h(const Item *it) { return (ROW + (it->look & PC_MENU_LINE ? SEP : 0)) * scale; }

/* Where each item of box m sits: its column, and the top of its row (a line above it included) from the box's top. The items run down a column and
   go on in the next one when the next item would pass the room there is, room pixels from the box's top: so a list longer than the window is tall,
   the 26 arcade stages in the smallest window, shows every item in two columns. Returns the number of columns, and the tallest column's height with
   the box's padding in *height. */
typedef struct Place { int col, y; } Place;

static int lay_items(int m, int room, Place *at, int *height)
{
    int i, col = 0, y = PAD * scale, tallest = 0;
    for (i = 0; i < menus[m].n; i++) {
        int h = item_h(&menus[m].item[i]);
        if (y > PAD * scale && y + h + PAD * scale > room) {
            col++;
            y = PAD * scale;
        }
        at[i].col = col;
        at[i].y = y;
        y += h;
        if (y > tallest)
            tallest = y;
    }
    *height = (tallest > PAD * scale ? tallest : PAD * scale) + PAD * scale;
    return col + 1;
}

/* One column's width in box m: the longest label, the tick before it and the longest key after it. */
static int column_w(int m)
{
    int i, longest = 0, keys = 0;
    for (i = 0; i < menus[m].n; i++) {
        int l = (int)strlen(menus[m].item[i].label), k = (int)strlen(menus[m].item[i].key) + (menus[m].item[i].look & PC_MENU_OPENS ? 2 : 0);
        if (l > longest) longest = l;
        if (k > keys) keys = k;
    }
    return (PAD + TICK_COL + 2 * PAD) * scale + longest * CELL_W * scale + (keys ? KEY_GAP * scale + keys * CELL_W * scale : 0);
}

/* The room a box has, from its top: below the bar for a menu of the bar, the whole window for a list. */
static int box_room(int beside) { return beside < 0 ? out_h - BAR_ROWS * scale : out_h; }

/* A menu's box: below its title for a menu of the bar; beside the item that opens it for a list. */
static Rect box_rect(int m, int beside)
{
    Rect r;
    Place at[PC_MENU_ITEMS];
    int cols = lay_items(m, box_room(beside), at, &r.h);
    r.w = cols * column_w(m);
    if (beside < 0) {
        Rect t = title_rect(m);
        r.x = t.x;
        r.y = BAR_ROWS * scale;
    } else {
        Rect p = box_rect(open_menu, -1);
        Place pa[PC_MENU_ITEMS];
        int ph;
        lay_items(open_menu, box_room(-1), pa, &ph);
        r.x = p.x + (pa[beside].col + 1) * column_w(open_menu) - 2 * scale;
        r.y = p.y + pa[beside].y + (menus[open_menu].item[beside].look & PC_MENU_LINE ? SEP * scale : 0) - PAD * scale;
        if (r.x + r.w > out_w)
            r.x = p.x + pa[beside].col * column_w(open_menu) - r.w + 2 * scale;
    }
    if (r.x + r.w > out_w) r.x = out_w - r.w;
    if (r.x < 0) r.x = 0;
    if (r.y + r.h > out_h) r.y = out_h - r.h;
    if (r.y < 0) r.y = 0;
    return r;
}

/* The item of box m (laid out with beside, as box_rect does) at x, y inside it, or -1. */
static int item_at(int m, int beside, Rect r, int x, int y)
{
    Place at[PC_MENU_ITEMS];
    int i, height, w = column_w(m);
    lay_items(m, box_room(beside), at, &height);
    for (i = 0; i < menus[m].n; i++) {
        int top = r.y + at[i].y + (menus[m].item[i].look & PC_MENU_LINE ? SEP * scale : 0), left = r.x + at[i].col * w;
        if (x >= left && x < left + w && y >= top && y < r.y + at[i].y + item_h(&menus[m].item[i]))
            return i;
    }
    return -1;
}

/* A page's lines as drawn: the text's own lines, each broken at the last space that fits across the window (or inside a word too long for a line),
   so a page never runs off the side of a small window. A page is drawn in parts when its lines do not fit down the window: any key or a click shows
   the next part, and closes the page after the last. At the program's usual sizes every page is one part, its lines as written. */
#define PAGE_LINES 200

static int page_fit(void) { int n = (out_w / scale - 4 * PAD) / CELL_W; return n < 8 ? 8 : n; }            /* characters across */
static int page_rows(void) { int n = (out_h / scale - 4 * PAD) / (CELL_H + 2) - 3; return n < 1 ? 1 : n; }   /* lines down, less title and foot */

static int page_lines(int start[PAGE_LINES], int length[PAGE_LINES], int *longest)
{
    const char *t = page_text;
    int n = 0, at = 0, total = (int)strlen(t), fit = page_fit();
    *longest = 0;
    for (;;) {
        int end = at, from = at;
        while (end < total && t[end] != '\n')
            end++;
        do {    /* the text's line from at to end, in pieces that fit */
            int cut = end;
            if (end - from > fit) {
                int k = from + fit;
                while (k > from && t[k] != ' ')
                    k--;
                cut = k > from ? k : from + fit;
            }
            if (n < PAGE_LINES) {
                start[n] = from;
                length[n] = cut - from;
                if (length[n] > *longest)
                    *longest = length[n];
                n++;
            }
            from = cut;
            while (from < end && t[from] == ' ')
                from++;
        } while (from < end);
        if (end >= total)
            break;
        at = end + 1;
    }
    return n;
}

static int page_parts(void)
{
    int start[PAGE_LINES], length[PAGE_LINES], longest, n = page_lines(start, length, &longest);
    return (n + page_rows() - 1) / page_rows();
}

/* The words at the foot of a page: what any key or a click does. */
static void page_foot(char *out, size_t size)
{
    int parts = page_parts();
    if (parts <= 1)
        snprintf(out, size, "Any key or a click closes this.");
    else if (page_part + 1 < parts)
        snprintf(out, size, "Any key or a click: the next part (%d of %d).", page_part + 1, parts);
    else
        snprintf(out, size, "Any key or a click closes this (%d of %d).", page_part + 1, parts);
}

static Rect page_rect(void)
{
    Rect r;
    int start[PAGE_LINES], length[PAGE_LINES], longest, n = page_lines(start, length, &longest), rows = page_rows();
    char foot[80];
    if ((int)strlen(page_title) > longest)
        longest = (int)strlen(page_title);
    if (n > rows) {
        page_foot(foot, sizeof(foot));
        if ((int)strlen(foot) > longest)
            longest = (int)strlen(foot);
        n = rows;
    }
    r.w = (2 * PAD * 2 + longest * CELL_W) * scale;
    r.h = (2 * PAD * 2 + (n + 3) * (CELL_H + 2)) * scale;
    r.x = (out_w - r.w) / 2;
    r.y = (out_h - r.h) / 2;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    return r;
}

/* ---- state */

int pc_menu_is_open(void) { return open_menu >= 0 || page; }
int pc_menu_page_open(void) { return page; }

void pc_menu_close(void)
{
    open_menu = open_list = -1;
    hover_title = -1;
    page = 0;
}

static int first_on_bar(void)
{
    int i;
    for (i = 0; i < n_menus; i++)
        if (menus[i].on_bar)
            return i;
    return -1;
}

static void open(int m)
{
    open_menu = m;
    open_item = 0;
    open_list = -1;
}

void pc_menu_open_first(void)
{
    /* On a page shown in parts, the menu input turns to the next part, as any key or a click does (pc_menu_key, pc_menu_mouse), and closes the page
       after the last. A pad's Menu bar input comes here (pc_input.c), and a player with only a pad has no other way to see the other parts. */
    if (page && page_part + 1 < page_parts())
        page_part++;
    else if (pc_menu_is_open())
        pc_menu_close();
    else
        open(first_on_bar());
}

void pc_menu_show_page(const char *title, const char *text)
{
    open_menu = open_list = -1;
    snprintf(page_title, sizeof(page_title), "%s", title);
    snprintf(page_text, sizeof(page_text), "%s", text);
    page = 1;
    page_part = 0;
}

static void choose(int m, int i)
{
    const Item *it;
    if (m < 0 || i < 0 || i >= menus[m].n)
        return;
    it = &menus[m].item[i];
    if (it->look & PC_MENU_GREY)
        return;
    if (it->look & PC_MENU_OPENS) {
        if (it->arg > 0 && it->arg < n_menus) {
            open_list = it->arg;
            list_item = 0;
            for (i = 0; i < menus[open_list].n; i++)
                if (menus[open_list].item[i].look & PC_MENU_TICK)
                    list_item = i;
        }
        return;
    }
    if (it->command == PC_MENU_DO_ACTION)
        actions |= (unsigned)it->arg;
    else if (it->command != PC_MENU_DO_NOTHING && queue_n < 16) {
        queue_cmd[queue_n] = it->command;
        queue_arg[queue_n] = it->arg;
        queue_n++;
    }
    open_menu = open_list = -1;
}

/* The next menu of the bar after m, one way or the other. */
static int bar_step(int m, int way)
{
    int i, k = m;
    for (i = 0; i < n_menus; i++) {
        k = (k + way + n_menus) % n_menus;
        if (menus[k].on_bar)
            return k;
    }
    return m;
}

int pc_menu_key(int key, int menu_key)
{
    if (page) {
        if (page_part + 1 < page_parts())
            page_part++;
        else
            page = 0;
        return 1;
    }
    if (open_menu < 0)
        return 0;
    if (key == menu_key) {
        pc_menu_close();
        return 1;
    }
    if (open_list >= 0) {
        int n = menus[open_list].n;
        if (key == KEY_UP)
            list_item = (list_item + n - 1) % n;
        else if (key == KEY_DOWN)
            list_item = (list_item + 1) % n;
        else if (key == KEY_LEFT || key == KEY_ESCAPE)
            open_list = -1;
        else if (key == KEY_RETURN || key == KEY_KP_ENTER || key == KEY_SPACE)
            choose(open_list, list_item);
        return 1;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        if (key == KEY_RIGHT && menus[open_menu].n && (menus[open_menu].item[open_item].look & PC_MENU_OPENS))
            choose(open_menu, open_item);
        else
            open(bar_step(open_menu, key == KEY_LEFT ? -1 : 1));
    } else if (key == KEY_UP && menus[open_menu].n) {
        open_item = (open_item + menus[open_menu].n - 1) % menus[open_menu].n;
    } else if (key == KEY_DOWN && menus[open_menu].n) {
        open_item = (open_item + 1) % menus[open_menu].n;
    } else if (key == KEY_RETURN || key == KEY_KP_ENTER || key == KEY_SPACE) {
        choose(open_menu, open_item);
    } else if (key == KEY_ESCAPE) {
        pc_menu_close();
    }
    return 1;
}

static int inside(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

int pc_menu_mouse(int x, int y, int event)
{
    int m, on_bar;
    if (page) {
        if (event == 1) {
            if (page_part + 1 < page_parts())
                page_part++;
            else
                page = 0;
        }
        return 1;
    }
    /* Full screen: the bar comes back while the mouse is at the top edge, and stays while a menu is open. */
    if (!reserved)
        peek = y >= 0 && y < BAR_ROWS * scale;
    hover_title = -1;
    /* The open boxes first, as they are drawn over the bar: in the smallest window a long list can reach up over it. */
    if (open_menu >= 0) {
        if (open_list >= 0) {
            Rect r = box_rect(open_list, open_item);
            if (inside(r, x, y)) {
                int i = item_at(open_list, open_item, r, x, y);
                if (i >= 0) {
                    list_item = i;
                    if (event == 1)
                        choose(open_list, i);
                }
                return 1;
            }
        }
        {
            Rect r = box_rect(open_menu, -1);
            if (inside(r, x, y)) {
                int i = item_at(open_menu, -1, r, x, y);
                if (i >= 0 && i != open_item) {
                    open_item = i;
                    open_list = -1;
                }
                if (i >= 0 && (event == 1 || (menus[open_menu].item[i].look & PC_MENU_OPENS)))
                    choose(open_menu, i);
                return 1;
            }
        }
    }
    on_bar = pc_menu_bar_shown() && y >= 0 && y < BAR_ROWS * scale;
    if (on_bar) {
        for (m = 0; m < n_menus; m++) {
            Rect t = title_rect(m);
            if (menus[m].on_bar && x >= t.x && x < t.x + t.w) {
                hover_title = m;
                if (event == 1) {
                    if (open_menu == m)
                        pc_menu_close();
                    else
                        open(m);
                } else if (open_menu >= 0 && open_menu != m) {
                    open(m);
                }
                return 1;
            }
        }
        if (event == 1 && open_menu >= 0)
            pc_menu_close();
        return 1;
    }
    if (open_menu < 0)
        return 0;
    if (event == 1)
        pc_menu_close();
    return 1;
}

unsigned pc_menu_take_actions(void)
{
    unsigned a = actions;
    actions = 0;
    return a;
}

int pc_menu_take_command(int *arg)
{
    int c;
    if (!queue_n)
        return 0;
    c = queue_cmd[0];
    *arg = queue_arg[0];
    queue_n--;
    memmove(queue_cmd, queue_cmd + 1, sizeof(int) * (size_t)queue_n);
    memmove(queue_arg, queue_arg + 1, sizeof(int) * (size_t)queue_n);
    return c;
}

int pc_menu_highlight(int *menu, int *item)
{
    if (open_list >= 0) {
        *menu = open_list;
        *item = list_item;
        return 1;
    }
    if (open_menu >= 0) {
        *menu = open_menu;
        *item = open_item;
        return 1;
    }
    return 0;
}

/* ---- drawing */

typedef struct Canvas { uint32_t *p; int w, h; } Canvas;

static void fill(Canvas *c, int x, int y, int w, int h, uint32_t colour)
{
    int i, j;
    for (j = y < 0 ? 0 : y; j < y + h && j < c->h; j++)
        for (i = x < 0 ? 0 : x; i < x + w && i < c->w; i++)
            c->p[j * c->w + i] = colour;
}

/* Text at x, y (the cell's top left), every font pixel scale x scale. */
static void text(Canvas *c, int x, int y, const char *s, uint32_t colour)
{
    for (; *s; s++, x += CELL_W * scale) {
        const uint8_t *g = pc_font_glyph((unsigned char)*s);
        int r, b;
        for (r = 0; r < 8; r++)
            for (b = 0; b < 5; b++)
                if (g[r] & (0x10u >> b))
                    fill(c, x + b * scale, y + r * scale, scale, scale, colour);
    }
}

/* The tick: our own small shape, in the font's grid. */
static void tick(Canvas *c, int x, int y, uint32_t colour)
{
    static const uint8_t rows[8] = {0x00, 0x01, 0x03, 0x16, 0x1C, 0x08, 0x00, 0x00};
    int r, b;
    for (r = 0; r < 8; r++)
        for (b = 0; b < 5; b++)
            if (rows[r] & (0x10u >> b))
                fill(c, x + b * scale, y + r * scale, scale, scale, colour);
}

int pc_menu_render_bar(uint32_t *out, int capacity, int *w, int *h)
{
    Canvas c;
    int m;
    *w = out_w;
    *h = BAR_ROWS * scale;
    if (!out)
        return 0;
    if (*w * *h > capacity)
        return -1;
    c.p = out;
    c.w = *w;
    c.h = *h;
    fill(&c, 0, 0, c.w, c.h, C_BAR);
    fill(&c, 0, c.h - scale, c.w, scale, C_BAR_LINE);
    for (m = 0; m < n_menus; m++) {
        Rect t;
        int lit = m == open_menu || (open_menu < 0 && m == hover_title);
        if (!menus[m].on_bar)
            continue;
        t = title_rect(m);
        if (lit)
            fill(&c, t.x, 0, t.w, c.h - scale, C_LIT);
        text(&c, t.x + title_gap() * scale, 3 * scale, menus[m].title, lit ? C_LIT_TEXT : C_TEXT);
    }
    if (hint[0]) {
        int x = c.w - text_w(hint) - PAD * 2 * scale;
        Rect last = title_rect(-1);
        if (x > last.x + PAD * scale)
            text(&c, x, 3 * scale, hint, C_DIM);
    }
    return 0;
}

/* Box m, laid out with beside as box_rect does: each item in its column (lay_items), the lit one highlighted across its column. */
static void draw_box(Canvas *c, int m, int beside, int lit)
{
    Place at[PC_MENU_ITEMS];
    int i, height, w = column_w(m);
    lay_items(m, box_room(beside), at, &height);
    fill(c, 0, 0, c->w, c->h, C_BOX);
    fill(c, 0, 0, c->w, scale, C_EDGE);
    fill(c, 0, c->h - scale, c->w, scale, C_EDGE);
    fill(c, 0, 0, scale, c->h, C_EDGE);
    fill(c, c->w - scale, 0, scale, c->h, C_EDGE);
    for (i = 0; i < menus[m].n; i++) {
        const Item *it = &menus[m].item[i];
        uint32_t ink = it->look & PC_MENU_GREY ? C_GREY : C_TEXT;
        int x0 = at[i].col * w, row_y = at[i].y;
        if (it->look & PC_MENU_LINE) {
            fill(c, x0 + PAD * scale, row_y + 2 * scale, w - 2 * PAD * scale, scale, C_EDGE);
            row_y += SEP * scale;
        }
        if (i == lit) {
            fill(c, x0 + scale, row_y, w - 2 * scale, ROW * scale, C_LIT);
            if (!(it->look & PC_MENU_GREY))
                ink = C_LIT_TEXT;
        }
        if (it->look & PC_MENU_TICK)
            tick(c, x0 + PAD * scale, row_y + 2 * scale, ink);
        text(c, x0 + (PAD + TICK_COL) * scale, row_y + 2 * scale, it->label, ink);
        if (it->key[0] || (it->look & PC_MENU_OPENS)) {
            char right[32];
            snprintf(right, sizeof(right), "%s%s", it->key, it->look & PC_MENU_OPENS ? (it->key[0] ? " >" : ">") : "");
            text(c, x0 + w - (PAD + 1) * scale - (int)strlen(right) * CELL_W * scale, row_y + 2 * scale, right,
                 i == lit && !(it->look & PC_MENU_GREY) ? C_LIT_TEXT : it->look & PC_MENU_GREY ? C_GREY : C_KEY);
        }
    }
}

static void draw_page(Canvas *c)
{
    int start[PAGE_LINES], length[PAGE_LINES], longest, n = page_lines(start, length, &longest), rows = page_rows(), i;
    int y = 2 * PAD * scale;
    char foot[80];
    fill(c, 0, 0, c->w, c->h, C_BOX);
    fill(c, 0, 0, c->w, scale, C_EDGE);
    fill(c, 0, c->h - scale, c->w, scale, C_EDGE);
    fill(c, 0, 0, scale, c->h, C_EDGE);
    fill(c, c->w - scale, 0, scale, c->h, C_EDGE);
    text(c, 2 * PAD * scale, y, page_title, C_TITLE);
    y += 2 * (CELL_H + 2) * scale;
    for (i = page_part * rows; i < n && i < (page_part + 1) * rows; i++) {
        char row[200];
        int k = length[i] < (int)sizeof(row) - 1 ? length[i] : (int)sizeof(row) - 1;
        memcpy(row, page_text + start[i], (size_t)k);
        row[k] = 0;
        text(c, 2 * PAD * scale, y, row, C_TEXT);
        y += (CELL_H + 2) * scale;
    }
    page_foot(foot, sizeof(foot));
    text(c, 2 * PAD * scale, c->h - (2 * PAD + CELL_H) * scale, foot, C_DIM);
}

int pc_menu_render_box(int which, uint32_t *out, int capacity, int *x, int *y, int *w, int *h)
{
    Canvas c;
    Rect r;
    int m, lit;
    if (page) {
        if (which != 0)
            return -1;
        r = page_rect();
        m = -1;
        lit = -1;
    } else if (which == 0 && open_menu >= 0) {
        r = box_rect(open_menu, -1);
        m = open_menu;
        lit = open_item;
    } else if (which == 1 && open_menu >= 0 && open_list >= 0) {
        r = box_rect(open_list, open_item);
        m = open_list;
        lit = list_item;
    } else {
        return -1;
    }
    *x = r.x;
    *y = r.y;
    *w = r.w;
    *h = r.h;
    if (!out)
        return 0;
    if (r.w * r.h > capacity)
        return -1;
    c.p = out;
    c.w = r.w;
    c.h = r.h;
    if (m < 0)
        draw_page(&c);
    else
        draw_box(&c, m, which == 1 ? open_item : -1, lit);
    return 0;
}

/* A hash of everything that is drawn: the window's pixels need drawing again only when it changes. */
unsigned pc_menu_version(void)
{
    uint64_t h = 0xCBF29CE484222325ull;
    const unsigned char *b;
    size_t i;
    int state[13];
    state[0] = open_menu; state[1] = open_item; state[2] = open_list; state[3] = list_item; state[4] = hover_title; state[5] = peek;
    state[6] = page; state[7] = out_w; state[8] = out_h; state[9] = reserved; state[10] = scale; state[11] = n_menus; state[12] = page_part;
#define MIX(p, n) for (b = (const unsigned char *)(p), i = 0; i < (size_t)(n); i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    MIX(state, sizeof(state));
    MIX(menus, sizeof(menus[0]) * (size_t)n_menus);
    MIX(hint, sizeof(hint));
    if (page) {
        MIX(page_title, strlen(page_title));
        MIX(page_text, strlen(page_text));
    }
#undef MIX
    return (unsigned)(h ^ (h >> 32));
}
