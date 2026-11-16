/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Keys and pads, through SDL2. pc_controls.h says what a player can set and how it is saved. Keys are read by their place on the keyboard, not by the
 * letter printed on them, and from SDL's events, so a test can push a key press into the same queue. Pads are read as they are each frame.
 *
 * PADS. Every pad SDL sees is opened, up to eight, whether SDL knows it as a game controller or only as a plain joystick, and whether it is wired or on
 * Bluetooth. SDL reads both through the same calls, but some pads give SDL another product number or another name on Bluetooth than on a cable, so a
 * player's choice of one kind of pad (same_kind below) may not find that pad on the other connection. A pad may come and go while the game runs. Each
 * player has at most one pad (see pc_controls.h for which), so one pad never drives two players: on the cart a second pad joins a second player.
 *
 * THE DATABASE. gamecontrollerdb.txt beside the program is SDL's community controller database. Its lines for this system whose pads SDL has no mapping
 * for of its own are added when the program starts; SDL's own mappings, made for the pads it knows best, are never replaced by it.
 *
 * WHILE THE CONTROLS SCREEN IS OPEN its own keys and the pads' presses go to it (pc_controls.c), and the program's actions do not happen. The poll that
 * opens it does nothing else of the program's but quit, and after it closes each player's keys and pad reach the game again only once that player has let
 * go of all of them (pc_input_poll).
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>

#include "pc_controls.h"
#include "pc_menu.h"
#include "pc_io.h"
#include "pc_audio.h"
#include "pc_window.h"

#define DEVICES_MAX 8
#define AXES_MAX 64             /* the controls screen takes a plain joystick's axes 0 to 63 by "press it now"; a configuration line may name any up to 255 */
#define PRESS_ZONE 16384        /* how far an axis must move from its middle to count as a press on the controls screen: half its travel */

typedef struct Device {
    int used;
    SDL_JoystickID id;
    SDL_Joystick *joy;
    SDL_GameController *gc;     /* NULL for a plain joystick */
    int own_joy;                /* the joystick was opened on its own, so it is closed on its own */
    unsigned order;             /* the first pad plugged in has the lowest */
    char base[64];              /* the pad's name as SDL gives it, in the screen's letters */
    int number;                 /* 1 for the first pad open with that name, then 2, 3 ...: the screen adds it to the name from 2 on */
    int8_t zone[AXES_MAX];      /* each axis's side as last seen, for presses on the controls screen: -1, 0 or +1 */
    int8_t gc_zone[SDL_CONTROLLER_AXIS_MAX];
    PcPadDevice info;
} Device;

static Device devices[DEVICES_MAX];
static unsigned next_order, devices_changed;
static int assigned[PC_PLAYERS] = {-1, -1, -1};
static PcControls controls;
static uint8_t key_down[SDL_NUM_SCANCODES];
static int port_pad_was[PC_PORT_N];
static int held_back[PC_PLAYERS];      /* after the controls screen, a menu or a Help page closed: this player's input is kept from the game until all of it is let go */
static int menu_open_last_poll;        /* a menu or a Help page was open when the last poll ended */
static char db_path[1200];
static char db_report[400];

PcControls *pc_input_controls(void) { return &controls; }
int pc_input_three_button(int p) { return p >= 0 && p < PC_PLAYERS && controls.player[p].three_button; }
const char *pc_input_key_name(int scancode) { return scancode > 0 && scancode < SDL_NUM_SCANCODES ? SDL_GetScancodeName((SDL_Scancode)scancode) : ""; }
void pc_input_set_controller_db(const char *path) { snprintf(db_path, sizeof(db_path), "%s", path ? path : ""); }
const char *pc_input_controller_db_report(void) { return db_report; }

void pc_input_before_sdl(void)
{
    /* The face buttons by their place on every pad, not by the letter printed on them: SDL's own default reads a Nintendo pad's by letter, which would put
       its south button, printed B, where other pads have their east one. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
}

/* ---- the database */

int pc_input_load_controller_db(const char *path, int *already_known)
{
    size_t size = 0, used = 0;
    char *file = (char *)SDL_LoadFile(path, &size), *keep, *line, *next;
    int added;
    *already_known = 0;
    if (!file)
        return -1;
    keep = (char *)SDL_malloc(size + 2);
    if (!keep) {
        SDL_free(file);
        return -1;
    }
    for (line = file; line && *line; line = next) {
        char guid_text[33], *platform;
        size_t length;
        next = strchr(line, '\n');
        length = next ? (size_t)(next - line) : strlen(line);
        if (next)
            *next++ = 0;
        if (length && line[length - 1] == '\r')
            line[--length] = 0;
        if (!length || line[0] == '#' || length < 34 || line[32] != ',')
            continue;
        /* A line for another system is left for SDL to skip; a line with no platform is for every system. */
        platform = strstr(line, "platform:");
        if (platform && strncmp(platform + 9, "Windows,", 8))
            continue;
        memcpy(guid_text, line, 32);
        guid_text[32] = 0;
        {
            char *known = SDL_GameControllerMappingForGUID(SDL_JoystickGetGUIDFromString(guid_text));
            if (known) {
                SDL_free(known);
                (*already_known)++;
                continue;
            }
        }
        memcpy(keep + used, line, length);
        used += length;
        keep[used++] = '\n';
    }
    added = used ? SDL_GameControllerAddMappingsFromRW(SDL_RWFromConstMem(keep, (int)used), 1) : 0;
    SDL_free(keep);
    SDL_free(file);
    return added < 0 ? -1 : added;
}

/* ---- pads */

static int find_device(SDL_JoystickID id)
{
    int i;
    for (i = 0; i < DEVICES_MAX; i++)
        if (devices[i].used && devices[i].id == id)
            return i;
    return -1;
}

static int8_t zone_of(int value) { return (int8_t)(value > PRESS_ZONE ? 1 : value < -PRESS_ZONE ? -1 : 0); }

static void describe(Device *d)
{
    const char *name = d->gc ? SDL_GameControllerName(d->gc) : SDL_JoystickName(d->joy);
    int i, j, n;
    if (!name || !name[0])
        name = d->gc ? "Game controller" : "Joystick";
    /* The screen's font has the plain ASCII letters only. */
    for (i = 0, j = 0; name[i] && j < (int)sizeof(d->base) - 4; i++)
        if ((unsigned char)name[i] >= 32 && (unsigned char)name[i] < 127)
            d->base[j++] = name[i];
    d->base[j] = 0;
    /* Pads open with exactly the same name are told apart by a number: the lowest no other one has, so a pad pulled out and plugged in again takes its
       number back. The first is shown with its name alone. */
    for (n = 1;; n++) {
        for (i = 0; i < DEVICES_MAX; i++)
            if (devices[i].used && &devices[i] != d && devices[i].number == n && !strcmp(devices[i].base, d->base))
                break;
        if (i == DEVICES_MAX)
            break;
    }
    d->number = n;
    memcpy(d->info.name, d->base, (size_t)j + 1);
    if (n > 1)
        snprintf(d->info.name + j, sizeof(d->info.name) - (size_t)j, " %d", n);
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(d->joy), d->info.guid, sizeof(d->info.guid));
    d->info.type = d->gc ? (int)SDL_GameControllerGetType(d->gc) : -1;
}

/* Opens the pad at SDL's device index, once. SDL also reports a pad that was there when its controller part started as just added, after this has
   opened it: it is found by its id and not opened again. A plain joystick that SDL later knows as a game controller (a mapping added for it) is opened
   as one then. */
static int open_device(int index)
{
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    int slot = id < 0 ? -1 : find_device(id), i;
    Device *d;
    if (id < 0)
        return -1;
    if (slot >= 0) {
        d = &devices[slot];
        if (!d->gc && SDL_IsGameController(index) && (d->gc = SDL_GameControllerOpen(index)) != NULL) {
            devices_changed++;
            describe(d);
            for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
                d->gc_zone[i] = zone_of(SDL_GameControllerGetAxis(d->gc, (SDL_GameControllerAxis)i));
        }
        return slot;
    }
    for (slot = 0; slot < DEVICES_MAX && devices[slot].used; slot++)
        ;
    if (slot == DEVICES_MAX)
        return -1;
    d = &devices[slot];
    memset(d, 0, sizeof(*d));
    if (SDL_IsGameController(index))
        d->gc = SDL_GameControllerOpen(index);
    if (d->gc) {
        d->joy = SDL_GameControllerGetJoystick(d->gc);
    } else {
        d->joy = SDL_JoystickOpen(index);
        d->own_joy = 1;
    }
    if (!d->joy) {
        if (d->gc)
            SDL_GameControllerClose(d->gc);
        memset(d, 0, sizeof(*d));
        return -1;
    }
    d->used = 1;
    d->id = id;
    d->order = ++next_order;
    devices_changed++;
    describe(d);
    for (i = 0; i < AXES_MAX && i < SDL_JoystickNumAxes(d->joy); i++) {
        Sint16 v = 0;
        if (!SDL_JoystickGetAxisInitialState(d->joy, i, &v))
            v = SDL_JoystickGetAxis(d->joy, i);
        d->zone[i] = zone_of(v);
    }
    for (i = 0; d->gc && i < SDL_CONTROLLER_AXIS_MAX; i++)
        d->gc_zone[i] = zone_of(SDL_GameControllerGetAxis(d->gc, (SDL_GameControllerAxis)i));
    return slot;
}

/* The pad a player had and lost for a while: it was pulled out (a cable, a pad that went to sleep), or, while the game does not hear player 3, player 1
   or 2 took player 3's pad for want of their own. When that pad is free again it goes back to that player (resolve), so a short pull never leaves two
   players with each other's pads. SDL gives a pad that comes back a new number, so it is known again by SDL's id for its model and, when SDL has one, its
   serial number: two pads of one model without a serial number cannot be told apart. So for a pad pulled out, only a pad opened after the pull can be it
   coming back: another pad of that model already plugged in at the pull (a spare the player takes meanwhile) never counts as theirs. */
typedef struct HadPad {
    int used;
    unsigned since;             /* a pad pulled out: only a pad whose order is above this one (next_order at the pull) can be it; 0 for a pad still in */
    char guid[33];
    char serial[64];
} HadPad;
static HadPad had[PC_PLAYERS];
static int player3_heard;      /* the ports route player 3 to the game: the stick's EXT socket or the EA 4-Way Play (pc_input_set_player3_heard) */

static const char *serial_of(int slot)
{
    const char *s = devices[slot].joy ? SDL_JoystickGetSerial(devices[slot].joy) : NULL;
    return s ? s : "";
}

/* Remembers the pad in slot as the one player p had, unless p already waits for one: then the pad in slot was only standing in for it. pulled: the pad
   is being closed, pulled out, so only a pad opened from now on can be it when it comes back; otherwise it is still plugged in (player 3's pad that
   player 1 or 2 took), and it is known by its model and serial number alone. */
static void remember(int p, int slot, int pulled)
{
    if (had[p].used)
        return;
    memset(&had[p], 0, sizeof(had[p]));
    had[p].used = 1;
    had[p].since = pulled ? next_order : 0;
    memcpy(had[p].guid, devices[slot].info.guid, sizeof(had[p].guid));
    snprintf(had[p].serial, sizeof(had[p].serial), "%s", serial_of(slot));
}

static int is_had(int p, int slot)
{
    const char *serial;
    if (!had[p].used || !devices[slot].used || devices[slot].order <= had[p].since || strcmp(devices[slot].info.guid, had[p].guid))
        return 0;
    serial = serial_of(slot);
    return !had[p].serial[0] || !serial[0] || !strcmp(serial, had[p].serial);
}

static void close_device(int slot)
{
    Device *d = &devices[slot];
    int p;
    if (!d->used)
        return;
    for (p = 0; p < PC_PLAYERS; p++)
        if (assigned[p] == slot)
            remember(p, slot, 1);
    if (d->gc)
        SDL_GameControllerClose(d->gc);
    if (d->own_joy)
        SDL_JoystickClose(d->joy);
    memset(d, 0, sizeof(*d));
    devices_changed++;
    for (p = 0; p < PC_PLAYERS; p++)
        if (assigned[p] == slot)
            assigned[p] = -1;
}

/* Whether a pad is of the kind a player asked for: the same maker and product, as SDL's id gives them, so the same pad on a cable or on Bluetooth matches
   when SDL reports the same two numbers both ways. An id without them is matched whole, but for the check digits SDL keeps of the pad's name. */
static int same_kind(const char *guid, const char *want)
{
    if (strncmp(want + 8, "0000", 4))
        return !strncasecmp(guid + 8, want + 8, 4) && !strncasecmp(guid + 16, want + 16, 4);
    return !strncasecmp(guid, want, 4) && !strncasecmp(guid + 8, want + 8, 24);
}

static int wants_kind(int p)
{
    const char *want = controls.player[p].controller;
    return strcmp(want, PC_CONTROLLER_ANY) && strcmp(want, PC_CONTROLLER_NONE);
}

/* Whether another player than p has the pad in slot. */
static int taken_by_other(int p, int slot)
{
    int q;
    for (q = 0; q < PC_PLAYERS; q++)
        if (q != p && assigned[q] == slot)
            return 1;
    return 0;
}

static int set_aside = -1;     /* a pad first_free and had_free pass over: see first_free_but and resolve */

/* The free pad that came first: one no other player has, and, when only_kind, of the kind this player asked for. Otherwise a game controller
   comes before a plain joystick, whatever the order they came in: SDL also lists devices that are not pads as plain joysticks (a pedal, a throttle), and
   one of those as a player's pad could hold a direction for ever. */
static int first_free(int p, int only_kind)
{
    int i, best = -1;
    for (i = 0; i < DEVICES_MAX; i++) {
        int this_gc, best_gc;
        if (!devices[i].used || taken_by_other(p, i) || i == set_aside)
            continue;
        if (only_kind && !same_kind(devices[i].info.guid, controls.player[p].controller))
            continue;
        if (best < 0) {
            best = i;
            continue;
        }
        this_gc = !only_kind && devices[i].gc != NULL;
        best_gc = !only_kind && devices[best].gc != NULL;
        if (this_gc > best_gc || (this_gc == best_gc && devices[i].order < devices[best].order))
            best = i;
    }
    return best;
}

/* first_free, but the pad aside (player 3's, while players 1 and 2 come first) only when no other pad will do, so a spare pad is taken before it. */
static int first_free_but(int p, int only_kind, int aside)
{
    int mine;
    set_aside = aside;
    mine = first_free(p, only_kind);
    set_aside = -1;
    return mine >= 0 ? mine : first_free(p, only_kind);
}

/* The free pad that is the one player p had (see HadPad), or -1: not the pad p holds now, which only stands in for it, and not the pad set aside, which
   is player 3's (another pad of the same model, with no serial number to tell them apart). Not when p has a pad of the kind they asked for and that one
   is not of it. */
static int had_free(int p)
{
    int i;
    for (i = 0; i < DEVICES_MAX; i++) {
        if (!is_had(p, i) || taken_by_other(p, i) || assigned[p] == i || i == set_aside)
            continue;
        if (wants_kind(p) && assigned[p] >= 0 && same_kind(devices[assigned[p]].info.guid, controls.player[p].controller) &&
            !same_kind(devices[i].info.guid, controls.player[p].controller))
            return -1;
        return i;
    }
    return -1;
}

/* Who has which pad, after a pad came or went or a player's choice changed. "none" has no pad. A pad stays with its player while it is there, and a pad
   that comes back after a pull goes back to the player who had it (the pad that player held meanwhile is then free again, and goes back to whoever had
   it in turn). Then a player who asked for a kind of pad gets one of that kind first, and a player with no pad the first free pad, as "any" does.
   Which players come first depends on whether the game hears player 3 (pc_input_set_player3_heard):
   - while it does (3-Player Mode, or Arcade Mode Multiplayer), the three players are equal: no player takes another's pad, so a player whose pad is
     pulled out takes a free one, or with none their port is quiet until it comes back; a pad of the kind player 3 asked for stays theirs;
   - while it does not, players 1 and 2 come first: player 3's pad counts as free for them, so they get exactly the pads they would get with no player 3,
     and player 3, whom the game cannot hear then, has what is left over. A pad of player 3's that player 1 or 2 took goes back to player 3 once it is
     free again. */
static void resolve(void)
{
    int p, last = PC_PLAYERS - 1, kept = -1, served = player3_heard ? PC_PLAYERS : last, changed;
    for (p = 0; p < PC_PLAYERS; p++) {
        if (assigned[p] >= 0 && !devices[assigned[p]].used)
            assigned[p] = -1;
        if (!strcmp(controls.player[p].controller, PC_CONTROLLER_NONE)) {
            assigned[p] = -1;
            had[p].used = 0;
        }
    }
    if (!player3_heard && assigned[last] >= 0) {
        kept = assigned[last];
        assigned[last] = -1;
    }
    set_aside = kept;
    do {
        changed = 0;
        for (p = 0; p < served; p++) {
            int mine = had[p].used ? had_free(p) : -1;
            if (mine >= 0) {
                assigned[p] = mine;
                had[p].used = 0;
                changed = 1;
            }
        }
    } while (changed);
    set_aside = -1;
    for (p = 0; p < served; p++) {
        if (wants_kind(p) && (assigned[p] < 0 || !same_kind(devices[assigned[p]].info.guid, controls.player[p].controller))) {
            int mine = first_free_but(p, 1, kept);
            if (mine >= 0) {
                assigned[p] = mine;
                had[p].used = 0;
            }
        }
    }
    for (p = 0; p < served; p++)
        if (assigned[p] < 0 && strcmp(controls.player[p].controller, PC_CONTROLLER_NONE))
            assigned[p] = first_free_but(p, 0, kept);
    if (player3_heard)
        return;
    /* Player 3, after players 1 and 2: the pad they had, then the pad they held (when players 1 and 2 left it), then their kind, then any free pad. */
    if (kept >= 0 && taken_by_other(last, kept))
        remember(last, kept, 0);
    if (!strcmp(controls.player[last].controller, PC_CONTROLLER_NONE))
        return;
    if (had[last].used && (assigned[last] = had_free(last)) >= 0)
        had[last].used = 0;
    else if (kept >= 0 && !taken_by_other(last, kept))
        assigned[last] = kept;
    if (wants_kind(last) && (assigned[last] < 0 || !same_kind(devices[assigned[last]].info.guid, controls.player[last].controller))) {
        int mine = first_free(last, 1);
        if (mine >= 0) {
            assigned[last] = mine;
            had[last].used = 0;
        }
    }
    if (assigned[last] < 0)
        assigned[last] = first_free(last, 0);
}

void pc_input_controls_changed(void) { resolve(); }

void pc_input_set_player3_heard(int heard)
{
    player3_heard = heard != 0;
    if (pc_input_controllers())
        resolve();
}

int pc_input_device_slots(int *slots, int max)
{
    int n = 0, i, j;
    for (i = 0; i < DEVICES_MAX; i++)
        if (devices[i].used && n < max)
            slots[n++] = i;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && devices[slots[j]].order < devices[slots[j - 1]].order; j--) {
            int t = slots[j];
            slots[j] = slots[j - 1];
            slots[j - 1] = t;
        }
    return n;
}

const PcPadDevice *pc_input_device(int slot) { return slot >= 0 && slot < DEVICES_MAX && devices[slot].used ? &devices[slot].info : NULL; }
int pc_input_player_device(int p) { return p >= 0 && p < PC_PLAYERS ? assigned[p] : -1; }

int pc_input_player_present(int p)
{
    int a;
    if (p < 2)
        return p >= 0;
    if (p >= PC_PLAYERS)
        return 0;
    if (assigned[p] >= 0)
        return 1;
    for (a = 0; a < PC_CTL_N; a++)
        if (controls.player[p].key[a])
            return 1;
    return 0;
}
int pc_input_controllers(void)
{
    int i, n = 0;
    for (i = 0; i < DEVICES_MAX; i++)
        n += devices[i].used;
    return n;
}

void pc_input_choose(int p, int slot)
{
    PcPlayerControls *pl = &controls.player[p];
    memset(pl->controller, 0, sizeof(pl->controller));
    had[p].used = 0;            /* a choice of the player's own: no pad to give back to them */
    if (slot == -1) {
        snprintf(pl->controller, sizeof(pl->controller), "%s", PC_CONTROLLER_ANY);
    } else if (slot == -2) {
        snprintf(pl->controller, sizeof(pl->controller), "%s", PC_CONTROLLER_NONE);
        assigned[p] = -1;
    } else if (pc_input_device(slot)) {
        /* That pad, and its kind from now on. If another player had it, they get this player's old one. */
        int old = assigned[p], q;
        snprintf(pl->controller, sizeof(pl->controller), "%s %s", devices[slot].info.guid, devices[slot].info.name);
        for (q = 0; q < PC_PLAYERS; q++)
            if (q != p && assigned[q] == slot)
                had[q].used = 0;        /* a swap the player chose: nothing to give back to the other player either */
        for (q = 0; q < PC_PLAYERS; q++)
            if (q != p && assigned[q] == slot)
                assigned[q] = old;
        assigned[p] = slot;
    }
    resolve();
}

void pc_input_assignment(int slots[PC_PLAYERS])
{
    int p;
    for (p = 0; p < PC_PLAYERS; p++)
        slots[p] = assigned[p];
}

void pc_input_set_assignment(const int slots[PC_PLAYERS])
{
    int p;
    for (p = 0; p < PC_PLAYERS; p++)
        assigned[p] = slots[p] >= 0 && slots[p] < DEVICES_MAX && devices[slots[p]].used ? slots[p] : -1;
    for (p = 1; p < PC_PLAYERS; p++) {
        int q;
        for (q = 0; q < p; q++)
            if (assigned[p] >= 0 && assigned[p] == assigned[q])
                assigned[p] = -1;      /* one pad, one player: the first of them keeps it */
    }
    resolve();
}

unsigned pc_input_devices_changed(void) { return devices_changed; }

/* ---- reading */

static int threshold(int deadzone) { return deadzone * 32767 / 100; }

static int held(const Device *d, PcPadInput in, int t)
{
    int v;
    switch (in.kind) {
    case PC_IN_BUTTON:
        return d->gc && SDL_GameControllerGetButton(d->gc, (SDL_GameControllerButton)in.index);
    case PC_IN_AXIS:
        if (!d->gc)
            return 0;
        v = SDL_GameControllerGetAxis(d->gc, (SDL_GameControllerAxis)in.index);
        return in.dir < 0 ? v < -t : v > t;
    case PC_IN_JOY_BUTTON:
        return in.index < SDL_JoystickNumButtons(d->joy) && SDL_JoystickGetButton(d->joy, in.index);
    case PC_IN_JOY_HAT:
        return in.index < SDL_JoystickNumHats(d->joy) && (SDL_JoystickGetHat(d->joy, in.index) & in.dir) != 0;
    case PC_IN_JOY_AXIS:
        if (in.index >= SDL_JoystickNumAxes(d->joy))
            return 0;
        v = SDL_JoystickGetAxis(d->joy, in.index);
        return in.dir < 0 ? v < -t : v > t;
    default:
        return 0;
    }
}

static unsigned stick_bits(const Device *d, int right, int t)
{
    int x, y;
    unsigned bits = 0;
    if (d->gc) {
        x = SDL_GameControllerGetAxis(d->gc, right ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX);
        y = SDL_GameControllerGetAxis(d->gc, right ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY);
    } else {
        int first = right ? 2 : 0;
        if (SDL_JoystickNumAxes(d->joy) < first + 2)
            return 0;
        x = SDL_JoystickGetAxis(d->joy, first);
        y = SDL_JoystickGetAxis(d->joy, first + 1);
    }
    if (x < -t) bits |= PC_PAD_LEFT;
    if (x > t) bits |= PC_PAD_RIGHT;
    if (y < -t) bits |= PC_PAD_UP;
    if (y > t) bits |= PC_PAD_DOWN;
    return bits;
}

static unsigned key_bits(int p)
{
    const PcPlayerControls *pl = &controls.player[p];
    unsigned bits = 0;
    int a;
    for (a = 0; a < PC_CTL_N; a++)
        if (pl->key[a] > 0 && pl->key[a] < SDL_NUM_SCANCODES && key_down[pl->key[a]])
            bits |= pc_controls_pad_bit(a);
    return bits;
}

unsigned pc_input_player_bits(int p, int with_keys)
{
    const PcPlayerControls *pl = &controls.player[p];
    unsigned bits = with_keys ? key_bits(p) : 0;
    int a;
    if (assigned[p] >= 0) {
        const Device *d = &devices[assigned[p]];
        const PcPadInput *set = d->gc ? pl->pad : pl->joy;
        int t = threshold(pl->deadzone);
        for (a = 0; a < PC_CTL_N; a++)
            if (held(d, set[a], t))
                bits |= pc_controls_pad_bit(a);
        if (pl->stick & PC_STICK_LEFT)
            bits |= stick_bits(d, 0, t);
        if (pl->stick & PC_STICK_RIGHT)
            bits |= stick_bits(d, 1, t);
    }
    return bits;
}

/* Whether a pad SDL knows has this joystick button, hat or axis in its layout: if not, the controls screen takes it by number. */
static int in_layout(const Device *d, int bind_type, int index)
{
    int i;
    for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        SDL_GameControllerButtonBind b = SDL_GameControllerGetBindForButton(d->gc, (SDL_GameControllerButton)i);
        if (b.bindType == bind_type && (bind_type == SDL_CONTROLLER_BINDTYPE_BUTTON ? b.value.button : bind_type == SDL_CONTROLLER_BINDTYPE_AXIS ? b.value.axis : b.value.hat.hat) == index)
            return 1;
    }
    for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++) {
        SDL_GameControllerButtonBind b = SDL_GameControllerGetBindForAxis(d->gc, (SDL_GameControllerAxis)i);
        if (b.bindType == bind_type && (bind_type == SDL_CONTROLLER_BINDTYPE_BUTTON ? b.value.button : bind_type == SDL_CONTROLLER_BINDTYPE_AXIS ? b.value.axis : b.value.hat.hat) == index)
            return 1;
    }
    return 0;
}

static void screen_pad(int slot, int kind, int index, int dir)
{
    PcPadInput in;
    in.kind = (uint8_t)kind;
    in.index = (uint8_t)index;
    in.dir = (int8_t)dir;
    pc_controls_screen_pad(slot, in);
}

/* A pad's event: the axes' sides are kept up to date always, and while the screen is open a new press goes to it. */
static void pad_event(const SDL_Event *e, int screen)
{
    int slot;
    Device *d;
    switch (e->type) {
    case SDL_CONTROLLERBUTTONDOWN:
        if ((slot = find_device(e->cbutton.which)) >= 0 && screen && devices[slot].gc)
            screen_pad(slot, PC_IN_BUTTON, e->cbutton.button, 0);
        break;
    case SDL_CONTROLLERAXISMOTION:
        if ((slot = find_device(e->caxis.which)) >= 0 && e->caxis.axis < SDL_CONTROLLER_AXIS_MAX) {
            int8_t z = zone_of(e->caxis.value);
            d = &devices[slot];
            if (z != d->gc_zone[e->caxis.axis] && z && screen && d->gc)
                screen_pad(slot, PC_IN_AXIS, e->caxis.axis, z);
            d->gc_zone[e->caxis.axis] = z;
        }
        break;
    case SDL_JOYBUTTONDOWN:
        if ((slot = find_device(e->jbutton.which)) >= 0 && screen && (!devices[slot].gc || !in_layout(&devices[slot], SDL_CONTROLLER_BINDTYPE_BUTTON, e->jbutton.button)))
            screen_pad(slot, PC_IN_JOY_BUTTON, e->jbutton.button, 0);
        break;
    case SDL_JOYHATMOTION: {
        int v = e->jhat.value;
        if ((slot = find_device(e->jhat.which)) >= 0 && screen && (v == SDL_HAT_UP || v == SDL_HAT_RIGHT || v == SDL_HAT_DOWN || v == SDL_HAT_LEFT) &&
            (!devices[slot].gc || !in_layout(&devices[slot], SDL_CONTROLLER_BINDTYPE_HAT, e->jhat.hat)))
            screen_pad(slot, PC_IN_JOY_HAT, e->jhat.hat, v);
        break;
    }
    case SDL_JOYAXISMOTION:
        if ((slot = find_device(e->jaxis.which)) >= 0 && e->jaxis.axis < AXES_MAX) {
            int8_t z = zone_of(e->jaxis.value);
            d = &devices[slot];
            if (z != d->zone[e->jaxis.axis] && z && screen && (!d->gc || !in_layout(d, SDL_CONTROLLER_BINDTYPE_AXIS, e->jaxis.axis)))
                screen_pad(slot, PC_IN_JOY_AXIS, e->jaxis.axis, z);
            d->zone[e->jaxis.axis] = z;
        }
        break;
    default:
        break;
    }
}

int pc_input_open(const PcConfig *config, char *error, size_t error_size)
{
    char path[1300];
    int added, known = 0, i;
    if (pc_controls_from_config(&controls, config, error, error_size) != 0)
        return -1;
    if (db_path[0]) {
        snprintf(path, sizeof(path), "%s", db_path);
    } else {
        char *base = SDL_GetBasePath();
        snprintf(path, sizeof(path), "%sgamecontrollerdb.txt", base ? base : "");
        SDL_free(base);
    }
    added = pc_input_load_controller_db(path, &known);
    if (added < 0)
        snprintf(db_report, sizeof(db_report), "no controller database: %.250s cannot be read, so only the pads SDL knows itself are game controllers", path);
    else
        snprintf(db_report, sizeof(db_report), "controller database %.250s: %d pads added for this system, %d left to SDL's own mappings", path, added, known);
    memset(had, 0, sizeof(had));
    for (i = 0; i < SDL_NumJoysticks(); i++)
        open_device(i);
    resolve();
    memset(port_pad_was, 0, sizeof(port_pad_was));
    memset(held_back, 0, sizeof(held_back));
    menu_open_last_poll = 0;
    return 0;
}

void pc_input_close(void)
{
    int i;
    for (i = 0; i < DEVICES_MAX; i++)
        close_device(i);
    for (i = 0; i < PC_PLAYERS; i++)
        assigned[i] = -1;
    memset(had, 0, sizeof(had));
    memset(key_down, 0, sizeof(key_down));
    memset(held_back, 0, sizeof(held_back));
}

/* A command of the program's own that a menu item asked for, kept for pc_input_take_menu_command. */
static int menu_command_waiting, menu_command_arg;

unsigned pc_input_poll(uint32_t pad[PC_PLAYERS])
{
    SDL_Event e;
    unsigned actions = 0;
    int i, p, screen = pc_controls_screen_open(), waiting = pc_controls_screen_waiting(), menu = pc_menu_is_open();
    while (SDL_PollEvent(&e)) {
        int stop = 0;
        switch (e.type) {
        case SDL_QUIT:
            actions |= PC_ACTION_QUIT;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int k = e.key.keysym.scancode;
            if (k <= 0 || k >= SDL_NUM_SCANCODES)
                break;
            if (!e.key.repeat)
                key_down[k] = e.type == SDL_KEYDOWN;
            if (e.type != SDL_KEYDOWN)
                break;
            if (screen) {
                /* When the screen closes, the events after this one are the game's. */
                stop = pc_controls_screen_key(k, e.key.repeat);
            } else if (menu) {
                /* An open menu takes every key (pc_menu.h); when it closes, the events after this one are the game's. */
                pc_menu_key(k, controls.port_key[PC_PORT_MENU]);
                stop = !pc_menu_is_open();
            } else if (!e.key.repeat) {
                for (i = 0; i < PC_PORT_N; i++)
                    if (controls.port_key[i] == k)
                        actions |= pc_controls_port_bit(i);
                /* The events after the key that opens the screen, or a menu, are theirs. */
                stop = (actions & (PC_ACTION_CONTROLS | PC_ACTION_MENU)) != 0;
            }
            break;
        }
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
            /* The mouse is only the menu bar's: the game has no use for it. Not while the controls screen is open. */
            if (!screen && (e.type == SDL_MOUSEMOTION || e.button.button == SDL_BUTTON_LEFT)) {
                int x, y, was = pc_menu_is_open();
                pc_window_point(e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y, &x, &y);
                pc_menu_mouse(x, y, e.type == SDL_MOUSEMOTION ? 0 : 1);
                stop = was != pc_menu_is_open();
            }
            break;
        case SDL_WINDOWEVENT:
            /* Losing the window must not leave a key held for ever. */
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                memset(key_down, 0, sizeof(key_down));
            break;
        case SDL_JOYDEVICEADDED:
            open_device(e.jdevice.which);
            resolve();
            break;
        case SDL_CONTROLLERDEVICEADDED:
            open_device(e.cdevice.which);
            resolve();
            break;
        case SDL_JOYDEVICEREMOVED:
            if ((i = find_device(e.jdevice.which)) >= 0) {
                close_device(i);
                resolve();
            }
            break;
        default:
            pad_event(&e, screen);
            break;
        }
        if (stop)
            break;
    }
    /* The program's own actions on a pad: once a press, from any player's pad. */
    for (i = 0; i < PC_PORT_N; i++) {
        int now = 0;
        for (p = 0; p < PC_PLAYERS; p++)
            if (assigned[p] >= 0 && held(&devices[assigned[p]], controls.port_pad[i], threshold(controls.player[p].deadzone)))
                now = 1;
        /* A press the screen was waiting for in this poll is the one it took as an input, not a press to close it with. */
        if (now && !port_pad_was[i]) {
            if (!screen && !pc_menu_is_open())
                actions |= pc_controls_port_bit(i);
            /* While a menu or a Help page is open, a pad's Menu bar input closes it, as the menu key does (the caller's pc_menu_open_first): the
               controls screen says that input opens the menu bar and closes it, and a player with only a pad has no other way to close it. */
            else if (!screen && i == PC_PORT_MENU)
                actions |= pc_controls_port_bit(i);
            else if (i == PC_PORT_CONTROLS && !waiting)
                pc_controls_screen_ask_close();
        }
        port_pad_was[i] = now;
    }
    if (screen) {
        actions |= pc_controls_screen_tick();
        if (!pc_controls_screen_open())
            for (p = 0; p < PC_PLAYERS; p++)
                held_back[p] = 1;
    }
    /* The same when a menu or a Help page closes, in this poll or since the last one. The key that closed it can be a player's key: Enter, which
       chooses an item, is player 1's Start by default, and any key closes a page. Without this, that key, still held, would be a new press for the
       game the moment it goes on: a Start press that skips a story screen or opens the game's own pause. */
    if ((menu || menu_open_last_poll) && !pc_menu_is_open())
        for (p = 0; p < PC_PLAYERS; p++)
            held_back[p] = 1;
    menu_open_last_poll = pc_menu_is_open();
    /* The poll that opens the controls screen does nothing else of the program's but quit. A pause key, or a pad's pause input, pressed in the same poll
       (within about a frame of the controls key) would otherwise end the pause the screen starts: the game would run on behind a screen never shown,
       which still took the keys and saved them. The other program keys would act on the game behind the screen. */
    if (actions & PC_ACTION_CONTROLS)
        actions &= PC_ACTION_CONTROLS | PC_ACTION_QUIT;
    else if (actions & PC_ACTION_MENU)
        actions &= PC_ACTION_MENU | PC_ACTION_QUIT;
    /* What the menu's chosen items asked for: an item that does what a key does gives that key's action, here, as the key would. */
    actions |= pc_menu_take_actions();
    {
        int peek_arg;
        int c = pc_menu_take_command(&peek_arg);
        if (c) {
            menu_command_waiting = c;
            menu_command_arg = peek_arg;
            actions |= PC_ACTION_MENU_COMMAND;
        }
    }
    /* After the screen, a menu or a page closes, a player's keys and pad reach the game again only once that player has let go of every one of them, so
       the button or key that closed it (a pad's Start, say) is not also a new press for the game. */
    for (p = 0; p < PC_PLAYERS; p++) {
        unsigned now = key_bits(p) | pc_input_player_bits(p, 0);
        if (held_back[p] && !now)
            held_back[p] = 0;
        pad[p] = held_back[p] ? 0 : now;
    }
    return actions;
}

int pc_input_take_menu_command(int *arg)
{
    int c = menu_command_waiting;
    *arg = menu_command_arg;
    menu_command_waiting = 0;
    if (!c)
        c = pc_menu_take_command(arg);
    return c;
}

int pc_input_push_mouse(int x, int y, int press)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    if (press) {
        e.type = SDL_MOUSEBUTTONDOWN;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.state = SDL_PRESSED;
        e.button.x = x;
        e.button.y = y;
    } else {
        e.type = SDL_MOUSEMOTION;
        e.motion.x = x;
        e.motion.y = y;
    }
    return SDL_PushEvent(&e) == 1 ? 0 : -1;
}

int pc_input_push_key(const char *name, int down)
{
    SDL_Scancode key = SDL_GetScancodeFromName(name);
    SDL_Event e;
    if (key == SDL_SCANCODE_UNKNOWN)
        return -1;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = key;
    return SDL_PushEvent(&e) == 1 ? 0 : -1;
}

void pc_input_print_keys(FILE *to)
{
    int i, p;
    for (p = 0; p < PC_PLAYERS; p++) {
        fprintf(to, "Player %d:", p + 1);
        for (i = 0; i < PC_CTL_N; i++)
            fprintf(to, " %s = %s,", pc_controls_action_name(i), controls.player[p].key[i] ? pc_input_key_name(controls.player[p].key[i]) : "none");
        fprintf(to, "\n");
    }
    for (i = 0; i < PC_PORT_N; i++)
        if (controls.port_key[i])
            fprintf(to, "%-12s %s\n", pc_input_key_name(controls.port_key[i]), pc_controls_port_name(i));
    fprintf(to, "%s\n", db_report);
    for (p = 0; p < PC_PLAYERS; p++) {
        const PcPadDevice *d = pc_input_device(assigned[p]);
        fprintf(to, "Player %d's pad: %s%s%s\n", p + 1, d ? d->name : "none now", d ? (d->type < 0 ? ", a plain joystick" : ", a game controller") : "",
                strcmp(controls.player[p].controller, PC_CONTROLLER_ANY) ? "" : " (any pad)");
    }
    if (controls.port_key[PC_PORT_CONTROLS])
        fprintf(to, "%s opens the controls screen, to set every key and pad button and to choose each player's pad.\n",
                pc_input_key_name(controls.port_key[PC_PORT_CONTROLS]));
    else if (controls.port_pad[PC_PORT_CONTROLS].kind != PC_IN_NONE)
        fprintf(to, "No key opens the controls screen, only the pad input pad_controls: key_controls in the configuration file can name one.\n");
    else
        fprintf(to, "Nothing opens the controls screen: key_controls in the configuration file is empty. Name a key there, F4 for example.\n");
    fflush(to);
}
