/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pr_render: turns a PcFrame into a PcPicture. It has no clock, no window and no machine state: the same frame always gives the same picture.
 * It copies the frame's start state, walks the lines in the chip's order, applies each write when its time says the chip's fetches would see it, and draws.
 *
 * THE ORDER OF A LINE ON THE CHIP
 * The V counter steps during the horizontal blank, so the time of V counter value L runs like this, in master clocks since the step, in the 320 pixel mode. These times
 * were measured on the exact chip model, with made-up scenes and writes at chosen times.
 *       30-740   the chip's copy of the sprite table is walked for line L + 1, about a dot an entry
 *        0-300   the last sprite tiles for line L
 *       before 362  the V scroll words of line L, when there is one word for the whole plane
 *      362       an access slot
 *      594       H scroll for line L
 *      683-3307  the planes' map and tiles for line L, one 2-cell column each 128 clocks, starting with the column hidden at the left
 *      891-3323  X and the tile of the sprites for line L + 1: the last n of 20 reads, one each 128 clocks
 *      981 on    the colour output of line L: a colour landing at 981 + 8x shows from pixel x
 *     3403 on    the sprite tiles for line L + 1
 * So the sprites of a line come from the time before that line's own V counter value, and line 0's sprites come from before the frame starts. A frame starts from its start
 * state with line 0's sprites read from it, and it goes on line by line: what lands during V counter value L is sorted out in one place, in pr_render below.
 *
 * WHEN A WRITE COUNTS
 * The frame says when the program made a write. What the chip's fetches see depends on when the write lands: a data port write waits for the next access slot, so during
 * the picture it lands up to a few hundred clocks later and the writes behind it queue up. That is worked out here, because it is drawing knowledge.
 *
 * WHAT IS LEFT OUT, BY NAME
 * None of these changed a pixel on any recorded frame. A register write takes effect from the next map fetch of a whole line, where the chip acts on some registers from a
 * column or a pixel partway across the line. A V scroll word in the per-column mode is read as it stands at the line's first fetch. A VRAM write that lands during a line
 * reaches that line's planes only from the next line, where the chip's remaining columns would see it.
 *
 * The chip's sprite walk remembers things from frame to frame, see PrMemory. pr_render starts each frame with that memory clear, and pr_render_kept carries it over.
 */
#include "pr_local.h"

#include <stddef.h>
#include <stdlib.h>

#ifdef PR_TEST_PLANTS
int pr_plant = 0;
#endif

typedef struct PrContext {
    PcVdpState      state;          /* the chip's state, moved forward as the frame goes on */
    const PcWrite  *writes;         /* the frame's writes */
    const uint32_t *order;          /* their order in time, or NULL when the list is in order already */
    uint32_t        n_writes;
    uint32_t        next_write;     /* the first one not wholly landed */
    uint32_t        queue_free;     /* the write queue is empty from this time */
    uint32_t        land_first;     /* when the next write lands: its low byte first if it is a VRAM word */
    uint32_t        land_second;
    uint8_t         landing_known;
    uint8_t         low_byte_done;
    uint32_t        palette[256];   /* colours from the current colour table: bits 7-6 of the index say normal, shadow or highlight */
} PrContext;

/* ---- colour */

/* The chip's colour output, by the three bits of a colour table entry. The levels are not evenly spaced: they include what the voltage divider on the
   console's board does, as the exact chip model has them, and the Pocket core passes them straight through. */
static const uint8_t level_normal[8]    = { 0, 49, 84, 114, 142, 172, 206, 255 };
static const uint8_t level_shadow[8]    = { 0, 27, 49, 67, 84, 100, 114, 128 };
static const uint8_t level_highlight[8] = { 128, 142, 156, 172, 188, 206, 228, 255 };

enum { SHADE_NORMAL = 0, SHADE_SHADOW = 1, SHADE_HIGHLIGHT = 2 };

/* The palette entries for one colour. Without register 0's bit 2 only the low bit of each of the three channels reaches the output. */
static void palette_set(uint32_t *palette, const PcVdpState *s, unsigned colour)
{
    unsigned entry = s->cram[colour];
    unsigned mask = (s->regs[0] & 4u) ? 7u : 1u;
    unsigned r = (entry >> 1) & mask, g = (entry >> 5) & mask, b = (entry >> 9) & mask;
    unsigned normal_r = PR_PLANT(20) && r == 3u ? 115u : level_normal[r];
    palette[SHADE_NORMAL << 6 | colour] = (uint32_t)normal_r << 16 | (uint32_t)level_normal[g] << 8 | level_normal[b];
    palette[SHADE_SHADOW << 6 | colour] = (uint32_t)level_shadow[r] << 16 | (uint32_t)level_shadow[g] << 8 | level_shadow[b];
    palette[SHADE_HIGHLIGHT << 6 | colour] = (uint32_t)level_highlight[r] << 16 | (uint32_t)level_highlight[g] << 8 | level_highlight[b];
}

static void palette_all(PrContext *c)
{
    unsigned i;
    for (i = 0; i < PC_CRAM_WORDS; i++)
        palette_set(c->palette, &c->state, i);
}

/* ---- when writes land */

/* A write through the data port waits for one of the chip's access slots. These are the slots of a line, in master clocks since the V counter stepped, as the exact chip
   model shows them. A VRAM word takes two slots, low byte first. The colour table and the V scroll memory take one. */
static const uint16_t slots_320[18] = { 362, 829, 957, 1085, 1341, 1469, 1597, 1853, 1981, 2109, 2365, 2493, 2621, 2877, 3005, 3133, 3373, 3389 };
static const uint16_t slots_256[16] = { 262, 522, 822, 982, 1142, 1462, 1622, 1782, 2102, 2262, 2422, 2742, 2902, 3062, 3362, 3382 };

/* A slot is out of reach unless the write's bus cycle started this long before it. A register write takes effect this long after its bus cycle starts. */
enum { PR_SLOT_LEAD = 56, PR_REGISTER_LATENCY = 48 };

static uint32_t next_slot(uint32_t t, unsigned h40)
{
    const uint16_t *slots = h40 ? slots_320 : slots_256;
    const unsigned count = h40 ? 18u : 16u;
    const unsigned line = t / PC_LINE_CLOCKS, h = t % PC_LINE_CLOCKS;
    unsigned i;
    for (i = 0; i < count; i++)
        if (slots[i] >= h)
            return pr_time(line, slots[i]);
    return pr_time(line + 1u, slots[0]);
}

/* The next thing to land before limit, if any: a register, a colour, a V scroll word, or one byte of a VRAM word. */
static int next_landing(PrContext *c, uint32_t limit, PrLanding *out)
{
    const PcWrite *w;
    if (c->next_write >= c->n_writes)
        return 0;
    w = &c->writes[c->order ? c->order[c->next_write] : c->next_write];
    if (!c->landing_known) {
        const unsigned h40 = c->state.regs[12] & 1u, display = c->state.regs[1] & 0x40u;
        uint32_t t = pr_time(w->vline, w->hclock);
        if (PR_PLANT(22))
            t += PC_LINE_CLOCKS;
        if (t >= limit)
            return 0;
        if (w->kind == PC_WRITE_REG) {
            c->land_first = c->land_second = t + PR_REGISTER_LATENCY;
        } else {
            t += PR_SLOT_LEAD;
            if (t < c->queue_free)
                t = c->queue_free;
            /* With the display off nearly every slot is an access slot */
            c->land_first = display && !PR_PLANT(23) ? next_slot(t, h40) : t;
            c->land_second = w->kind != PC_WRITE_VRAM ? c->land_first : display && !PR_PLANT(23) ? next_slot(c->land_first + 1u, h40) : c->land_first + 16u;
            c->queue_free = c->land_second + 1u;
        }
        c->landing_known = 1;
        c->low_byte_done = 0;
    }
    if (w->kind == PC_WRITE_VRAM && !c->low_byte_done) {
        if (c->land_first >= limit)
            return 0;
        out->time = c->land_first;
        out->addr = (uint16_t)((w->addr & 0xFFFEu) | (PR_PLANT(27) ? 0u : 1u));
        out->value = PR_PLANT(27) ? w->value >> 8 : w->value & 0xFFu;
        out->kind = PC_WRITE_VRAM;
        c->low_byte_done = 1;
        return 1;
    }
    if (c->land_second >= limit)
        return 0;
    out->time = c->land_second;
    if (w->kind == PC_WRITE_VRAM) {
        out->addr = (uint16_t)((w->addr & 0xFFFEu) | (PR_PLANT(27) ? 1u : 0u));
        out->value = PR_PLANT(27) ? w->value & 0xFFu : w->value >> 8;
    } else {
        out->addr = w->addr;
        out->value = w->value;
    }
    out->kind = w->kind;
    c->next_write++;
    c->landing_known = 0;
    return 1;
}

/* One landing into the state. All writes go through pc_state_apply, so the renderer's copy follows the one rule the machine's memories follow. */
static void apply_landing(PrContext *c, const PrLanding *e)
{
    PcWrite w;
    w.vline = 0;
    w.hclock = 0;
    w.kind = e->kind;
    w.flags = 0;
    w.addr = e->addr;
    w.value = e->value;
    if (e->kind == PC_WRITE_VRAM) {
        unsigned a = e->addr & 0xFFFEu;
        w.addr = (uint16_t)a;
        w.value = (e->addr & 1u) ? (uint16_t)((unsigned)c->state.vram[a] << 8 | e->value) : (uint16_t)(e->value << 8 | c->state.vram[a + 1u]);
    }
    pc_state_apply(&c->state, &w);
    if (e->kind == PC_WRITE_CRAM)
        palette_set(c->palette, &c->state, e->addr & 63u);
    else if (e->kind == PC_WRITE_REG && e->addr == 0)
        palette_all(c);
}

/* The order of the writes in time, stable, or NULL when they are in order already, which is nearly always. The list is nearly sorted when it is not, so
   an insertion sort does little work. If the memory cannot be had the list order stands. */
static uint32_t *sort_writes(const PcFrame *f)
{
    uint32_t i, *order;
    if (PR_PLANT(21))
        return NULL;
    for (i = 1; i < f->n_writes; i++)
        if (pr_time(f->writes[i].vline, f->writes[i].hclock) < pr_time(f->writes[i - 1].vline, f->writes[i - 1].hclock))
            break;
    if (i >= f->n_writes)
        return NULL;
    order = (uint32_t *)malloc(f->n_writes * sizeof(uint32_t));
    if (!order)
        return NULL;
    for (i = 0; i < f->n_writes; i++) {
        uint32_t t = pr_time(f->writes[i].vline, f->writes[i].hclock), j = i;
        while (j > 0 && pr_time(f->writes[order[j - 1]].vline, f->writes[order[j - 1]].hclock) > t) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    return order;
}

/* ---- composing a line */

enum { SHOW_BACKDROP, SHOW_SPRITE, SHOW_A, SHOW_B };

/* What a line buffer byte says for priority: bit 1 the priority bit, bit 0 opaque. */
static inline unsigned pixel_class(unsigned v)
{
    return ((v >> 5) & 2u) | ((v & 15u) != 0);
}

/* Which layer shows, for every mix of the three pixels' classes: a layer with the priority bit is above any without it, and among equals the order is sprite, A, B. A
   transparent pixel gives way to whatever is next. */
static void build_priority(uint8_t *table)
{
    unsigned key;
    for (key = 0; key < 64u; key++) {
        const unsigned s_high = key & 0x20u, s_opaque = key & 0x10u, a_high = key & 8u, a_opaque = key & 4u, b_high = key & 2u, b_opaque = key & 1u;
        unsigned show;
        if (PR_PLANT(1) && a_opaque && a_high)
            show = SHOW_A;
        else if (s_opaque && s_high)
            show = SHOW_SPRITE;
        else if (PR_PLANT(2) && b_opaque && b_high)
            show = SHOW_B;
        else if (a_opaque && a_high)
            show = SHOW_A;
        else if (b_opaque && b_high)
            show = SHOW_B;
        else if (s_opaque)
            show = SHOW_SPRITE;
        else if (PR_PLANT(2) && b_opaque)
            show = SHOW_B;
        else if (a_opaque)
            show = SHOW_A;
        else if (b_opaque)
            show = SHOW_B;
        else
            show = SHOW_BACKDROP;
        table[key] = (uint8_t)show;
    }
}

/* Picks the colour of each pixel. out holds a palette index: shade in bits 7-6, colour in bits 5-0. */
static void compose_plain(const uint8_t *priority, const uint8_t *sprite, const uint8_t *a, const uint8_t *b, unsigned backdrop, unsigned width, uint8_t *out)
{
    unsigned x;
    for (x = 0; x < width; x++) {
        const uint8_t layer[4] = { (uint8_t)backdrop, sprite[x], a[x], b[x] };
        out[x] = (uint8_t)(layer[priority[pixel_class(sprite[x]) << 4 | pixel_class(a[x]) << 2 | pixel_class(b[x])]] & 0x3Fu);
    }
}

/* The same with shadow and highlight on. Colours 14 and 15 of a palette 3 sprite pixel are operators: they do not draw, they change the shade of what is
   under them when the sprite would otherwise have been on top. These are the chip's rules:
     - Both planes without priority: the pixel is in shadow, unless a sprite with priority is what shows.
     - A sprite pixel of colour 14 in any other palette stays at normal brightness even so.
     - Operator 15 puts the pixel in shadow.
     - Operator 14 raises it one step: from shadow to normal, from normal to highlight. */
static void compose_shaded(const uint8_t *priority, const uint8_t *sprite, const uint8_t *a, const uint8_t *b, unsigned backdrop, unsigned width, uint8_t *out)
{
    unsigned x;
    for (x = 0; x < width; x++) {
        const unsigned s = sprite[x], pa = a[x], pb = b[x];
        const unsigned colour = s & 15u, s_high = (s >> 6) & 1u;
        const unsigned both_low = !((pa | pb) & 0x40u);
        const unsigned operator_pixel = (s & 0x3Fu) >= 0x3Eu;
        const unsigned key = pixel_class(s) << 4 | pixel_class(pa) << 2 | pixel_class(pb);
        const unsigned on_top = priority[key] == SHOW_SPRITE;
        /* An operator shows what is under it, as a transparent pixel does */
        const uint8_t layer[4] = { (uint8_t)backdrop, (uint8_t)s, (uint8_t)pa, (uint8_t)pb };
        const unsigned shown = layer[operator_pixel ? priority[key & ~0x10u] : priority[key]];
        unsigned shade = SHADE_NORMAL;

        if (on_top && operator_pixel) {
            if (colour == 15u)
                shade = PR_PLANT(16) ? SHADE_NORMAL : SHADE_SHADOW;
            else if (!both_low)
                shade = PR_PLANT(17) ? SHADE_NORMAL : SHADE_HIGHLIGHT;
        } else if (both_low && !PR_PLANT(18)) {
            if (on_top && s_high)
                shade = SHADE_NORMAL;
            else if (colour == 14u && !PR_PLANT(19))
                shade = SHADE_NORMAL;
            else
                shade = SHADE_SHADOW;
        }
        out[x] = (uint8_t)(shade << 6 | (shown & 0x3Fu));
    }
}

/* ---- the frame */

/* Times in a line, in master clocks since the V counter stepped. Between the last access slot of a line and the first of the next the chip reads the whole plane V scroll
   words, so any time there does. No access slot lies between the H scroll fetch and the first map fetch, so one time serves both. */
enum {
    PR_TIME_V_SCROLL = 100,
    PR_TIME_FETCH_320 = 811,
    PR_TIME_FETCH_256 = 799,
    PR_TIME_PIXEL_320 = 981,
    PR_TIME_PIXEL_256 = 1012
};

/* More than can land in a line, in practice: with the picture on there are at most 18 slots. If more land they wait for the next line. */
#define PR_MOST_LANDINGS 512u

/* Turns palette indexes into colours from pixel x0 up to x1. */
static inline void paint(const PrContext *c, const uint8_t *line_index, uint32_t *rgb, unsigned x0, unsigned x1)
{
    unsigned x;
    for (x = x0; x < x1; x++)
        rgb[x] = c->palette[line_index[x]];
}

void pr_render_kept(const PcFrame *frame, const PrOptions *options, PrMemory *memory, PcPicture *out)
{
    /* The working state is large for a stack, and it is there only for the length of this call */
    PrContext c;
    PrWalk walk;
    uint32_t *order = sort_writes(frame);
    uint8_t buffer_1[PR_BUFFER_W], buffer_2[PR_BUFFER_W], plane_a[PR_BUFFER_W], plane_b[PR_BUFFER_W], line_index[PC_PICTURE_W];
    uint8_t *sprite_now = buffer_1, *sprite_next = buffer_2;
    PrLanding landings[PR_MOST_LANDINGS];
    uint8_t priority[64];
    const PrLimits limits = options ? options->limits : PR_LIMITS_CART;
    const int want_layer = options && options->want_layer;
    int line;

    memcpy(&c.state, &frame->start, sizeof(c.state));
    c.writes = frame->writes;
    c.order = order;
    c.n_writes = frame->n_writes;
    c.next_write = 0;
    c.queue_free = 0;
    c.landing_known = 0;
    c.low_byte_done = 0;
    memset(&walk, 0, offsetof(PrWalk, table_copy));
    walk.memory = memory;
    if (PR_PLANT(31))
        memset(memory, 0, sizeof(*memory));
    /* The chip's walk does not draw in the no-limit mode, so when the limits come back there is nothing to remember */
    if (limits == PR_LIMITS_NONE && (c.state.regs[12] & 1u))
        memset(memory, 0, sizeof(*memory));
    palette_all(&c);
    build_priority(priority);

    out->lines_over = 0;
    out->width = (c.state.regs[12] & 1u) ? 320 : 256;
    out->height = 224;
    if (want_layer)
        memset(out->sprite_layer, 0, sizeof(out->sprite_layer));

    /* Line 0's sprites were read before the frame began */
    memset(sprite_now, 0, PR_BUFFER_W);
    if (c.state.regs[1] & 0x40u) {
        pr_sprites_fetch(&c.state, &walk, 0, limits, NULL, 0);
        if (pr_sprites_draw(&c.state, &walk, 0, limits, sprite_now))
            out->lines_over++;
    }

    /* What lands from the V scroll read of a line up to that of the next is handled as one list. There is nothing in it on most lines. The last pass reads the sprites of
       line 224, which never shows, because the chip does that too and it changes what the walk remembers. */
    for (line = 0; line < 224; line++) {
        const uint32_t start = pr_time((unsigned)line, 0);
        const uint32_t end = pr_time((unsigned)line + 1u, PR_TIME_V_SCROLL);
        uint16_t scroll_a[PR_COLUMNS], scroll_b[PR_COLUMNS];
        uint32_t *rgb = out->rgb[line];
        unsigned h40, width, n = 0, i = 0, x = 0;
        uint32_t fetch;
        uint8_t *swap;

        pr_scroll_columns(&c.state, 0, scroll_a);
        pr_scroll_columns(&c.state, 1, scroll_b);
        while (n < PR_MOST_LANDINGS && next_landing(&c, end, &landings[n]))
            n++;

        /* The walk of the next line's sprites begins now, from the table as it stood before this list */
        if (c.state.regs[1] & 0x40u)
            pr_sprites_fetch(&c.state, &walk, line + 1, limits, landings, n);

        /* What lands before the planes are fetched reaches them */
        fetch = start + ((c.state.regs[12] & 1u) ? PR_TIME_FETCH_320 : PR_TIME_FETCH_256);
        for (; i < n && landings[i].time < fetch; i++)
            apply_landing(&c, &landings[i]);
        h40 = c.state.regs[12] & 1u;
        width = h40 ? 320u : 256u;

        if (c.state.regs[1] & 0x40u) {
            /* A word per column is read when the column is fetched */
            if ((c.state.regs[11] & 4u) || PR_PLANT(24)) {
                pr_scroll_columns(&c.state, 0, scroll_a);
                pr_scroll_columns(&c.state, 1, scroll_b);
            }
            pr_plane_a_line(&c.state, line, pr_h_scroll(&c.state, line, 0), scroll_a, plane_a);
            pr_plane_b_line(&c.state, line, pr_h_scroll(&c.state, line, 1), scroll_b, plane_b);
            if (c.state.regs[12] & 8u)
                compose_shaded(priority, sprite_now, plane_a + PR_MARGIN, plane_b + PR_MARGIN, c.state.regs[7] & 0x3Fu, width, line_index);
            else
                compose_plain(priority, sprite_now, plane_a + PR_MARGIN, plane_b + PR_MARGIN, c.state.regs[7] & 0x3Fu, width, line_index);
        } else {
            memset(line_index, c.state.regs[7] & 0x3Fu, width);
        }
        if (want_layer)
            memcpy(out->sprite_layer[line], sprite_now, width);

        /* The rest of the list: a colour reaches the output at its pixel */
        for (; i < n; i++) {
            if (landings[i].kind == PC_WRITE_CRAM) {
                const uint32_t first = start + (h40 ? PR_TIME_PIXEL_320 : PR_TIME_PIXEL_256);
                uint32_t at = landings[i].time <= first || PR_PLANT(25) ? 0 : (landings[i].time - first) / (h40 ? 8u : 10u);
                if (at > width)
                    at = width;
                if (at > x) {
                    paint(&c, line_index, rgb, x, at);
                    x = at;
                }
            }
            apply_landing(&c, &landings[i]);
        }
        paint(&c, line_index, rgb, x, width);
        if (width < PC_PICTURE_W)
            memset(rgb + width, 0, (PC_PICTURE_W - width) * sizeof(uint32_t));

        /* The sprite tiles of the next line are read after all of that */
        swap = sprite_now;
        sprite_now = sprite_next;
        sprite_next = swap;
        memset(sprite_now, 0, PR_BUFFER_W);
        if ((c.state.regs[1] & 0x40u) && pr_sprites_draw(&c.state, &walk, line + 1, limits, sprite_now) && line < 223)
            out->lines_over++;
    }
    memset(out->rgb[224], 0, sizeof(out->rgb) - 224u * sizeof(out->rgb[0]));
    free(order);
}

void pr_render(const PcFrame *frame, const PrOptions *options, PcPicture *out)
{
    PrMemory memory;
    memset(&memory, 0, sizeof(memory));
    pr_render_kept(frame, options, &memory, out);
}
