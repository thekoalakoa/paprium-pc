/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_controls.h. This file holds the controls' names and defaults, their lines in the configuration, and the controls screen. pc_input.c reads the
 * keys and pads with them, and feeds the screen while it is open.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_controls.h"
#include "pc_audio.h"
#include "pc_font.h"
#include "pc_io.h"
#include "pc_window.h"

/* ---- names and defaults */

static const char *const action_names[PC_CTL_N] = {"up", "down", "left", "right", "a", "b", "c", "x", "y", "z", "start", "mode"};
static const char *const action_labels[PC_CTL_N] = {"Up", "Down", "Left", "Right", "A", "B", "C", "X", "Y", "Z", "Start", "Mode"};
static const unsigned action_bits[PC_CTL_N] = {PC_PAD_UP, PC_PAD_DOWN, PC_PAD_LEFT, PC_PAD_RIGHT, PC_PAD_A, PC_PAD_B, PC_PAD_C,
                                               PC_PAD_X, PC_PAD_Y, PC_PAD_Z, PC_PAD_START, PC_PAD_MODE};
static const char *const port_names[PC_PORT_N] = {"controls", "pause", "mute", "limits", "shape", "filter", "fullscreen", "quit", "menu"};
static const char *const port_labels[PC_PORT_N] = {"Controls", "Pause", "Mute", "Sprite limits", "Picture shape", "Picture filter", "Full screen", "Quit",
                                                   "Menu bar"};
static const char *const port_about[PC_PORT_N] = {"opens and closes this screen", "stops the game until pressed again", "turns the sound off and on",
                                                  "the cart's sprite limits, or none", "the next Aspect Ratio shape", "the next look: sharp, scanlines, CRT, PVM ...",
                                                  "full screen, or a window", "saves and ends the program", "opens the menu bar, and closes it"};
static const unsigned port_bits[PC_PORT_N] = {PC_ACTION_CONTROLS, PC_ACTION_PAUSE, PC_ACTION_MUTE, PC_ACTION_LIMITS, PC_ACTION_SHAPE,
                                              PC_ACTION_FILTER, PC_ACTION_FULLSCREEN, PC_ACTION_QUIT, PC_ACTION_MENU};

/* The keys of earlier versions, unchanged: player 1 on the arrows and the letters below them, player 2 on I J K L and the number row. Player 3 has
   none: by default a third pad. */
static const int default_keys[PC_PLAYERS][PC_CTL_N] = {
    {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_A,
     SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_RETURN, SDL_SCANCODE_RSHIFT},
    {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
     SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_KP_0},
    {0},
};
/* No two of these are the same, and none is a player's key: F4 opens the controls screen, F6, a key nothing else uses, changes the picture filter, and
   F10, the key Windows programs open their menu bar with, opens the menu bar. */
static const int default_port_keys[PC_PORT_N] = {SDL_SCANCODE_F4, SDL_SCANCODE_P, SDL_SCANCODE_F3, SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F6,
                                                 SDL_SCANCODE_F11, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_F10};

/* A pad SDL knows, by place, as earlier versions had it: south B, east C, west A, north Y, left shoulder X, right shoulder Z, Start, Back as Mode. */
#define GCB(b) {PC_IN_BUTTON, SDL_CONTROLLER_BUTTON_##b, 0}
static const PcPadInput default_pad[PC_CTL_N] = {GCB(DPAD_UP), GCB(DPAD_DOWN), GCB(DPAD_LEFT), GCB(DPAD_RIGHT), GCB(X), GCB(A), GCB(B),
                                                 GCB(LEFTSHOULDER), GCB(Y), GCB(RIGHTSHOULDER), GCB(START), GCB(BACK)};
#undef GCB
/* A plain joystick: its first hat as the D-pad, buttons 0 to 5 as A B C X Y Z, and 8 and 9 as Mode and Start, where many plain pads have Select and Start.
   This is a guess that fits many such pads and not all: the screen sets the rest. */
#define JB(n) {PC_IN_JOY_BUTTON, n, 0}
#define JH(d) {PC_IN_JOY_HAT, 0, d}
static const PcPadInput default_joy[PC_CTL_N] = {JH(SDL_HAT_UP), JH(SDL_HAT_DOWN), JH(SDL_HAT_LEFT), JH(SDL_HAT_RIGHT), JB(0), JB(1), JB(2), JB(3), JB(4),
                                                 JB(5), JB(9), JB(8)};
#undef JB
#undef JH
#define DEFAULT_DEADZONE 50

const char *pc_controls_action_name(int a) { return a >= 0 && a < PC_CTL_N ? action_names[a] : ""; }
const char *pc_controls_port_name(int a) { return a >= 0 && a < PC_PORT_N ? port_names[a] : ""; }
unsigned pc_controls_pad_bit(int a) { return a >= 0 && a < PC_CTL_N ? action_bits[a] : 0; }
unsigned pc_controls_port_bit(int a) { return a >= 0 && a < PC_PORT_N ? port_bits[a] : 0; }

int pc_pad_input_equal(PcPadInput a, PcPadInput b)
{
    if (a.kind != b.kind)
        return 0;
    if (a.kind == PC_IN_NONE)
        return 1;
    return a.index == b.index && a.dir == b.dir;
}

static const char *hat_word(int dir)
{
    switch (dir) {
    case SDL_HAT_UP: return "up";
    case SDL_HAT_RIGHT: return "right";
    case SDL_HAT_DOWN: return "down";
    case SDL_HAT_LEFT: return "left";
    default: return "";
    }
}

void pc_pad_input_text(PcPadInput in, char *text, size_t size)
{
    const char *name;
    switch (in.kind) {
    case PC_IN_BUTTON:
        name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)in.index);
        snprintf(text, size, "%s", name ? name : "");
        return;
    case PC_IN_AXIS:
        name = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)in.index);
        snprintf(text, size, "%s%c", name ? name : "", in.dir < 0 ? '-' : '+');
        return;
    case PC_IN_JOY_BUTTON:
        snprintf(text, size, "button%u", (unsigned)in.index);
        return;
    case PC_IN_JOY_HAT:
        snprintf(text, size, "hat%u%s", (unsigned)in.index, hat_word(in.dir));
        return;
    case PC_IN_JOY_AXIS:
        snprintf(text, size, "axis%u%c", (unsigned)in.index, in.dir < 0 ? '-' : '+');
        return;
    default:
        if (size)
            text[0] = 0;
    }
}

/* A number of at most three digits, under 256, at the start of text; where it ends goes to *end. -1 when there is none. */
static int small_number(const char *text, const char **end)
{
    int n = 0, digits = 0;
    while (*text >= '0' && *text <= '9' && digits < 4) {
        n = n * 10 + (*text++ - '0');
        digits++;
    }
    *end = text;
    return digits && digits < 4 && n < 256 ? n : -1;
}

int pc_pad_input_parse(const char *text, PcPadInput *in)
{
    const char *end;
    int n;
    size_t length = strlen(text);
    memset(in, 0, sizeof(*in));
    if (!length)
        return 0;
    if (!strncmp(text, "button", 6) && (n = small_number(text + 6, &end)) >= 0 && !*end) {
        in->kind = PC_IN_JOY_BUTTON;
        in->index = (uint8_t)n;
        return 0;
    }
    if (!strncmp(text, "hat", 3) && (n = small_number(text + 3, &end)) >= 0) {
        static const int dirs[4] = {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT};
        int i;
        for (i = 0; i < 4; i++) {
            if (!strcmp(end, hat_word(dirs[i]))) {
                in->kind = PC_IN_JOY_HAT;
                in->index = (uint8_t)n;
                in->dir = (int8_t)dirs[i];
                return 0;
            }
        }
        return -1;
    }
    if (!strncmp(text, "axis", 4) && (n = small_number(text + 4, &end)) >= 0 && (!strcmp(end, "+") || !strcmp(end, "-"))) {
        in->kind = PC_IN_JOY_AXIS;
        in->index = (uint8_t)n;
        in->dir = *end == '-' ? -1 : 1;
        return 0;
    }
    if (length > 1 && length < 40 && (text[length - 1] == '+' || text[length - 1] == '-')) {
        char name[40];
        SDL_GameControllerAxis axis;
        memcpy(name, text, length - 1);
        name[length - 1] = 0;
        axis = SDL_GameControllerGetAxisFromString(name);
        if (axis == SDL_CONTROLLER_AXIS_INVALID)
            return -1;
        in->kind = PC_IN_AXIS;
        in->index = (uint8_t)axis;
        in->dir = text[length - 1] == '-' ? -1 : 1;
        return 0;
    }
    {
        SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(text);
        if (button == SDL_CONTROLLER_BUTTON_INVALID)
            return -1;
        in->kind = PC_IN_BUTTON;
        in->index = (uint8_t)button;
    }
    return 0;
}

/* ---- the screen's names for a pad's inputs, by the kind of pad */

enum { FAMILY_PLACES, FAMILY_XBOX, FAMILY_PLAYSTATION, FAMILY_NINTENDO };

static int family_of(int type)
{
    switch (type) {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE:
        return FAMILY_XBOX;
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
    case SDL_CONTROLLER_TYPE_PS5:
        return FAMILY_PLAYSTATION;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return FAMILY_NINTENDO;
    default:
        return FAMILY_PLACES;
    }
}

/* SDL reads the face buttons by place, so on a Switch pad the south button, printed B, is SDL's "a". */
static const char *button_label(int index, int type)
{
    static const char *const places[SDL_CONTROLLER_BUTTON_MAX] = {"South", "East", "West", "North", "Back", "Guide", "Start", "L stick press",
        "R stick press", "L shoulder", "R shoulder", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Misc", "Paddle 1", "Paddle 2", "Paddle 3",
        "Paddle 4", "Touchpad"};
    static const char *const xbox[SDL_CONTROLLER_BUTTON_MAX] = {"A", "B", "X", "Y", "View", "Xbox", "Menu", "L stick press", "R stick press", "LB", "RB",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Share", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};
    static const char *const ps[SDL_CONTROLLER_BUTTON_MAX] = {"Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Mute", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};
    static const char *const nintendo[SDL_CONTROLLER_BUTTON_MAX] = {"B", "A", "Y", "X", "Minus", "Home", "Plus", "L stick press", "R stick press", "L", "R",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Capture", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};
    if (index < 0 || index >= SDL_CONTROLLER_BUTTON_MAX)
        return "?";
    if (type == SDL_CONTROLLER_TYPE_XBOX360 && (index == SDL_CONTROLLER_BUTTON_BACK || index == SDL_CONTROLLER_BUTTON_START))
        return index == SDL_CONTROLLER_BUTTON_BACK ? "Back" : "Start";
    if (type == SDL_CONTROLLER_TYPE_PS3 && (index == SDL_CONTROLLER_BUTTON_BACK || index == SDL_CONTROLLER_BUTTON_START))
        return index == SDL_CONTROLLER_BUTTON_BACK ? "Select" : "Start";
    if (type == SDL_CONTROLLER_TYPE_PS5 && index == SDL_CONTROLLER_BUTTON_BACK)
        return "Create";
    switch (family_of(type)) {
    case FAMILY_XBOX: return xbox[index];
    case FAMILY_PLAYSTATION: return ps[index];
    case FAMILY_NINTENDO: return nintendo[index];
    default: return places[index];
    }
}

static void axis_label(int index, int dir, int type, char *text, size_t size)
{
    static const char *const triggers[4][2] = {{"L trigger", "R trigger"}, {"LT", "RT"}, {"L2", "R2"}, {"ZL", "ZR"}};
    int family = family_of(type);
    switch (index) {
    case SDL_CONTROLLER_AXIS_LEFTX: snprintf(text, size, "L stick %s", dir < 0 ? "left" : "right"); break;
    case SDL_CONTROLLER_AXIS_LEFTY: snprintf(text, size, "L stick %s", dir < 0 ? "up" : "down"); break;
    case SDL_CONTROLLER_AXIS_RIGHTX: snprintf(text, size, "R stick %s", dir < 0 ? "left" : "right"); break;
    case SDL_CONTROLLER_AXIS_RIGHTY: snprintf(text, size, "R stick %s", dir < 0 ? "up" : "down"); break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        snprintf(text, size, "%s%s", triggers[family][index - SDL_CONTROLLER_AXIS_TRIGGERLEFT], dir < 0 ? " (let go)" : "");
        break;
    default: snprintf(text, size, "axis %d %c", index, dir < 0 ? '-' : '+'); break;
    }
}

void pc_pad_input_label(PcPadInput in, int type, char *text, size_t size)
{
    switch (in.kind) {
    case PC_IN_BUTTON: snprintf(text, size, "%s", button_label(in.index, type)); break;
    case PC_IN_AXIS: axis_label(in.index, in.dir, type, text, size); break;
    case PC_IN_JOY_BUTTON: snprintf(text, size, "Button %u", (unsigned)in.index); break;
    case PC_IN_JOY_HAT: snprintf(text, size, "Hat %u %s", (unsigned)in.index, hat_word(in.dir)); break;
    case PC_IN_JOY_AXIS: snprintf(text, size, "Axis %u %c", (unsigned)in.index, in.dir < 0 ? '-' : '+'); break;
    default: snprintf(text, size, "-"); break;
    }
}

/* ---- the model */

static void player_defaults(PcControls *c, int p)
{
    PcPlayerControls *pl = &c->player[p];
    memset(pl, 0, sizeof(*pl));
    memcpy(pl->key, default_keys[p], sizeof(pl->key));
    memcpy(pl->pad, default_pad, sizeof(pl->pad));
    memcpy(pl->joy, default_joy, sizeof(pl->joy));
    snprintf(pl->controller, sizeof(pl->controller), "%s", PC_CONTROLLER_ANY);
    pl->three_button = 0;
    pl->stick = PC_STICK_LEFT;
    pl->deadzone = DEFAULT_DEADZONE;
}

static void port_defaults(PcControls *c)
{
    memcpy(c->port_key, default_port_keys, sizeof(c->port_key));
    memset(c->port_pad, 0, sizeof(c->port_pad));
}

void pc_controls_defaults(PcControls *c)
{
    int p;
    memset(c, 0, sizeof(*c));
    for (p = 0; p < PC_PLAYERS; p++)
        player_defaults(c, p);
    port_defaults(c);
}

/* An action's place by the configuration's name: "p1_up" gives player 0 and PC_CTL_UP, "pause" gives player -1 and PC_PORT_PAUSE. -1 for no such name. */
static int action_by_name(const char *name, int *player, int *action)
{
    int i;
    for (i = 0; i < PC_PORT_N; i++) {
        if (!strcmp(name, port_names[i])) {
            *player = -1;
            *action = i;
            return 0;
        }
    }
    if (name[0] == 'p' && name[1] >= '1' && name[1] < '1' + PC_PLAYERS && name[2] == '_') {
        for (i = 0; i < PC_CTL_N; i++) {
            if (!strcmp(name + 3, action_names[i])) {
                *player = name[1] - '1';
                *action = i;
                return 0;
            }
        }
    }
    return -1;
}

static int is_guid(const char *text)
{
    int i;
    for (i = 0; i < 32; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') || (text[i] >= 'A' && text[i] <= 'F')))
            return 0;
    return text[32] == 0 || text[32] == ' ';
}

int pc_controls_from_config(PcControls *c, const PcConfig *config, char *error, size_t error_size)
{
    int i, p, a;
    pc_controls_defaults(c);
    for (i = 0; i < config->n_keys; i++) {
        const char *value = config->keys[i].key;
        int key = value[0] ? (int)SDL_GetScancodeFromName(value) : 0;
        int found = action_by_name(config->keys[i].action, &p, &a) == 0;
        if (!found || (value[0] && key == SDL_SCANCODE_UNKNOWN)) {
            snprintf(error, error_size, "the configuration's key_%s=%s: %s", config->keys[i].action, value, found ? "no key has that name" : "no action has that name");
            return -1;
        }
        if (p < 0)
            c->port_key[a] = key;
        else
            c->player[p].key[a] = key;
    }
    for (i = 0; i < config->n_pads + config->n_joys; i++) {
        int joy = i >= config->n_pads;
        const PcConfigKey *line = joy ? &config->joys[i - config->n_pads] : &config->pads[i];
        PcPadInput in;
        int found = action_by_name(line->action, &p, &a) == 0, named = pc_pad_input_parse(line->key, &in) == 0;
        if (named && joy && (in.kind == PC_IN_BUTTON || in.kind == PC_IN_AXIS))
            named = 0;      /* a plain joystick has no game controller names */
        if (!found || !named || (joy && p < 0)) {
            snprintf(error, error_size, "the configuration's %s_%s=%s: %s", joy ? "joy" : "pad", line->action, line->key,
                     !found || (joy && p < 0) ? "no action has that name" : joy ? "a plain joystick has no input of that name" : "no pad input has that name");
            return -1;
        }
        if (p < 0)
            c->port_pad[a] = in;
        else if (joy)
            c->player[p].joy[a] = in;
        else
            c->player[p].pad[a] = in;
    }
    for (p = 0; p < PC_PLAYERS; p++) {
        PcPlayerControls *pl = &c->player[p];
        const char *want = config->controller[p];
        if (want[0]) {
            if (strcmp(want, PC_CONTROLLER_ANY) && strcmp(want, PC_CONTROLLER_NONE) && !is_guid(want)) {
                snprintf(error, error_size, "the configuration's controller%d=%s: it must be any, none, or a pad's 32 digit id and its name", p + 1, want);
                return -1;
            }
            memset(pl->controller, 0, sizeof(pl->controller));
            snprintf(pl->controller, sizeof(pl->controller), "%s", want);
        }
        pl->three_button = config->three_button[p];
        if (config->stick[p])
            pl->stick = config->stick[p] - 1;
        if (config->deadzone[p])
            pl->deadzone = config->deadzone[p];
    }
    return 0;
}

int pc_controls_lines(const PcControls *c, char *text, size_t size)
{
    static const char *const sticks[4] = {"off", "left", "right", "both"};
    size_t used = 0;
    int p, a, set;
    char name[48];
#define LINE(...) do { int n_ = snprintf(text + used, used < size ? size - used : 0, __VA_ARGS__); if (n_ < 0 || used + (size_t)n_ >= size) return -1; used += (size_t)n_; } while (0)
    LINE("# ---- Controls. The game's controls screen writes the lines from here to the end of the file each time it closes, and keeps every line above.\n");
    LINE("# Keys, by SDL's name for each key. An empty value means no key.\n");
    for (p = 0; p < PC_PLAYERS; p++)
        for (a = 0; a < PC_CTL_N; a++)
            LINE("key_p%d_%s=%s\n", p + 1, action_names[a], pc_input_key_name(c->player[p].key[a]));
    for (a = 0; a < PC_PORT_N; a++)
        LINE("key_%s=%s\n", port_names[a], pc_input_key_name(c->port_key[a]));
    LINE("# Each player's pad: controllerN is any (the first pad free), none (keyboard only), or SDL's id for a kind of pad and its name.\n");
    LINE("# padN is six or three buttons, stickN left, right, both or off, deadzoneN the stick's dead zone in percent.\n");
    for (p = 0; p < PC_PLAYERS; p++) {
        const PcPlayerControls *pl = &c->player[p];
        LINE("controller%d=%s\n", p + 1, pl->controller);
        LINE("pad%d=%s\n", p + 1, pl->three_button ? "three" : "six");
        LINE("stick%d=%s\n", p + 1, sticks[pl->stick & 3]);
        LINE("deadzone%d=%d\n", p + 1, pl->deadzone);
    }
    LINE("# Inputs on a pad SDL knows (pad_), then on a plain joystick (joy_). An empty value means none.\n");
    for (set = 0; set < 2; set++) {
        for (p = 0; p < PC_PLAYERS; p++) {
            for (a = 0; a < PC_CTL_N; a++) {
                pc_pad_input_text(set ? c->player[p].joy[a] : c->player[p].pad[a], name, sizeof(name));
                LINE("%s_p%d_%s=%s\n", set ? "joy" : "pad", p + 1, action_names[a], name);
            }
        }
        if (!set) {
            for (a = 0; a < PC_PORT_N; a++) {
                pc_pad_input_text(c->port_pad[a], name, sizeof(name));
                LINE("pad_%s=%s\n", port_names[a], name);
            }
        }
    }
#undef LINE
    return (int)used;
}

/* ---- the screen */

#define CAPTURE_MS 5000

enum { CAPTURE_NONE, CAPTURE_KEY, CAPTURE_PAD, CAPTURE_DEVICE };
enum { PAGE_P1, PAGE_P2, PAGE_P3, PAGE_PORT, PAGES };
/* The rows a player's page has, top to bottom, and the program keys' page's. */
enum { ROW_TABS, ROW_DEVICE, ROW_BUTTONS, ROW_STICK, ROW_DEADZONE, ROW_ACTION, ROW_RESET = ROW_ACTION + PC_CTL_N, PLAYER_ROWS };
enum { PORT_ROW_ACTION = 1, PORT_ROW_RESET = PORT_ROW_ACTION + PC_PORT_N, PORT_ROWS };

/* Colours as red, green and blue. */
#define COLOUR(r, g, b) ((uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
#define C_TEXT COLOUR(216, 220, 232)
#define C_DIM COLOUR(168, 176, 200)
#define C_GOLD COLOUR(240, 192, 64)
#define C_HELD COLOUR(96, 224, 112)
#define C_BAD COLOUR(240, 112, 112)
#define C_GOOD COLOUR(112, 208, 240)
#define C_CURSOR COLOUR(46, 78, 156)
#define C_CAPTURE COLOUR(156, 46, 46)
#define C_DARK COLOUR(16, 20, 24)
#define C_WHITE COLOUR(255, 255, 255)

static struct {
    int open;
    int page, row[PAGES], column;
    int capture, capture_action;
    uint64_t capture_until;
    char status[120];
    uint32_t status_colour;
    unsigned nav_last;
    int nav_wait_release;
    int dirty, save_failed, close_asked;
    char file[1200];
    const char *unsaved;        /* why the last save failed, until one succeeds (pc_controls_unsaved) */
    int unsaved_named;
    uint64_t test_ms;
    /* The pad row: whose row the last Left or Right was on (-1 for none since the cursor last moved, and from each opening of the screen on), and where
       the pads stood when it began. */
    int browse_player;
    int browse_slots[PC_PLAYERS];
    char browse_controller[PC_CONFIG_VALUE];
    unsigned browse_devices;
    PcPicture base, out;
} s;        /* all zero at the start, so it takes no room in the program file: the screen sets what it needs when it opens */

static uint64_t now_ms(void) { return s.test_ms ? s.test_ms : SDL_GetTicks64(); }
void pc_controls_test_clock(uint64_t ms) { s.test_ms = ms; }
int pc_controls_screen_open(void) { return s.open; }
int pc_controls_screen_waiting(void) { return s.open && s.capture != CAPTURE_NONE; }
void pc_controls_set_file(const char *path) { snprintf(s.file, sizeof(s.file), "%s", path ? path : ""); }

static void say(uint32_t colour, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.status, sizeof(s.status), fmt, ap);
    va_end(ap);
    s.status_colour = colour;
}

static int rows_of(int page) { return page == PAGE_PORT ? PORT_ROWS : PLAYER_ROWS; }

void pc_controls_screen_show(const PcPicture *game)
{
    int x, y, w = game ? game->width : 0, h = game ? game->height : 0, left;
    memset(&s.base, 0, sizeof(s.base));
    s.base.width = PC_PICTURE_W;
    s.base.height = 224;
    if (w > PC_PICTURE_W) w = PC_PICTURE_W;
    if (h > 224) h = 224;
    left = (PC_PICTURE_W - w) / 2;
    /* The game's picture stays behind the text, dimmed to a quarter with a little blue, so the player sees where they paused and the text stands out. */
    for (y = 0; y < 224; y++) {
        for (x = 0; x < PC_PICTURE_W; x++) {
            uint32_t c = x >= left && x < left + w && y < h ? game->rgb[y][x - left] : 0;
            s.base.rgb[y][x] = COLOUR((c >> 16 & 255u) / 4u, (c >> 8 & 255u) / 4u, (c & 255u) / 4u + 20u);
        }
    }
    s.open = 1;
    s.capture = CAPTURE_NONE;
    s.close_asked = 0;
    s.save_failed = 0;
    s.nav_wait_release = 1;        /* the press that opened it, if it came from a pad, is let go first */
    s.nav_last = 0;
    s.browse_player = -1;
    say(C_DIM, "");
}

/* The player whose page is shown, or -1 on the program keys' page. */
static int page_player(void) { return s.page == PAGE_PORT ? -1 : s.page; }

/* Which of a player's two sets of pad inputs fits the pad they have now: the plain joystick's only when their pad is one. */
static int uses_joy(int p)
{
    int slot = pc_input_player_device(p);
    const PcPadDevice *d = slot >= 0 ? pc_input_device(slot) : NULL;
    return d && d->type < 0;
}

static int pad_type_of(int p)
{
    int slot = p >= 0 ? pc_input_player_device(p) : -1;
    const PcPadDevice *d = slot >= 0 ? pc_input_device(slot) : NULL;
    return d ? d->type : SDL_CONTROLLER_TYPE_UNKNOWN;
}

static PcPadInput *pad_cell(PcControls *c, int p, int a) { return p < 0 ? &c->port_pad[a] : uses_joy(p) ? &c->player[p].joy[a] : &c->player[p].pad[a]; }
static int *key_cell(PcControls *c, int p, int a) { return p < 0 ? &c->port_key[a] : &c->player[p].key[a]; }
static const char *label_of(int p, int a) { return p < 0 ? port_labels[a] : action_labels[a]; }

static void who(int p, int a, char *text, size_t size)
{
    if (p < 0)
        snprintf(text, size, "the %s key", port_labels[a]);
    else
        snprintf(text, size, "player %d's %s", p + 1, action_labels[a]);
}

/* Whether this key is the only way left to open the controls screen: the Controls action's key, with no pad input beside it. Nothing else opens the
   screen, so that key stays where it is until Controls has another way in. */
static int last_way_in(int key)
{
    PcControls *c = pc_input_controls();
    return key && c->port_key[PC_PORT_CONTROLS] == key && c->port_pad[PC_PORT_CONTROLS].kind == PC_IN_NONE;
}

/* A key on one action only: any other action with the same key loses it, and the status line says so. */
static void set_key(int p, int a, int key)
{
    PcControls *c = pc_input_controls();
    char was[64] = "", mine[64];
    int q, b;
    if (!(p < 0 && a == PC_PORT_CONTROLS) && last_way_in(key)) {
        say(C_BAD, "%s stays: nothing else opens this screen", pc_input_key_name(key));
        return;
    }
    for (q = -1; q < PC_PLAYERS; q++) {
        for (b = 0; b < (q < 0 ? PC_PORT_N : PC_CTL_N); b++) {
            int *k = key_cell(c, q, b);
            if ((q != p || b != a) && *k == key) {
                *k = 0;
                who(q, b, was, sizeof(was));
            }
        }
    }
    *key_cell(c, p, a) = key;
    s.dirty = 1;
    who(p, a, mine, sizeof(mine));
    if (was[0])
        say(C_GOOD, "%s is %s now; %s has no key", pc_input_key_name(key), mine, was);
    else
        say(C_GOOD, "%s is %s now", pc_input_key_name(key), mine);
}

static void set_pad(int p, int a, PcPadInput in, int type)
{
    PcControls *c = pc_input_controls();
    char label[48], also[64] = "";
    int b, q;
    if (p >= 0) {
        /* On one player's own pad an input does one thing: another action with it loses it. */
        int joy = uses_joy(p);
        for (b = 0; b < PC_CTL_N; b++) {
            PcPadInput *other = joy ? &c->player[p].joy[b] : &c->player[p].pad[b];
            if (b != a && pc_pad_input_equal(*other, in)) {
                memset(other, 0, sizeof(*other));
                snprintf(also, sizeof(also), "; %s has none", action_labels[b]);
            }
        }
    } else {
        /* A program key on a pad may share a button with a player's action: then both happen, and the line says so. */
        for (q = 0; q < PC_PLAYERS && !also[0]; q++)
            for (b = 0; b < PC_CTL_N && !also[0]; b++)
                if (pc_pad_input_equal(c->player[q].pad[b], in) || pc_pad_input_equal(c->player[q].joy[b], in))
                    snprintf(also, sizeof(also), "; it is player %d's %s too", q + 1, action_labels[b]);
    }
    *pad_cell(c, p, a) = in;
    s.dirty = 1;
    pc_pad_input_label(in, type, label, sizeof(label));
    say(C_GOOD, "%s: %s%s", label_of(p, a), label, also);
}

static void start_capture(int what)
{
    int p = page_player(), a = s.page == PAGE_PORT ? s.row[s.page] - PORT_ROW_ACTION : s.row[s.page] - ROW_ACTION;
    if (what == CAPTURE_PAD && p >= 0 && pc_input_player_device(p) < 0) {
        say(C_BAD, "Player %d has no pad: plug one in, or pick one above", p + 1);
        return;
    }
    if (what == CAPTURE_PAD && p < 0 && pc_input_player_device(0) < 0 && pc_input_player_device(1) < 0 && pc_input_player_device(2) < 0) {
        say(C_BAD, "No player has a pad now: plug one in first");
        return;
    }
    s.capture = what;
    s.capture_action = a;
    s.capture_until = now_ms() + CAPTURE_MS;
    s.nav_wait_release = 1;
    s.browse_player = -1;
}

static void end_capture(void)
{
    s.capture = CAPTURE_NONE;
    s.nav_wait_release = 1;     /* the press that was taken does not also move the cursor */
}

/* A page back to its defaults. A key does one thing here too: a default key that another page has given to one of its actions comes back to this page,
   and that action has no key, except the key that is the only way left to open this screen, which stays the Controls key. */
static void reset_page(void)
{
    PcControls *c = pc_input_controls();
    PcControls d;
    int p = page_player(), a, q, b, moved = 0, kept = 0;
    pc_controls_defaults(&d);
    if (p < 0) {
        memcpy(c->port_key, d.port_key, sizeof(c->port_key));
        memcpy(c->port_pad, d.port_pad, sizeof(c->port_pad));
    } else {
        c->player[p] = d.player[p];
        pc_input_controls_changed();
    }
    for (a = 0; a < (p < 0 ? PC_PORT_N : PC_CTL_N); a++) {
        int *mine = key_cell(c, p, a);
        for (q = -1; q < PC_PLAYERS && *mine; q++) {
            if (q == p)
                continue;
            for (b = 0; b < (q < 0 ? PC_PORT_N : PC_CTL_N) && *mine; b++) {
                int *other = key_cell(c, q, b);
                if (*other != *mine)
                    continue;
                if (q < 0 && b == PC_PORT_CONTROLS && last_way_in(*other)) {
                    kept = *other;
                    *mine = 0;
                } else {
                    *other = 0;
                    moved = 1;
                }
            }
        }
    }
    if (kept)
        say(C_GOOD, "Player %d back to defaults, but %s stays Controls", p + 1, pc_input_key_name(kept));
    else if (p < 0)
        say(C_GOOD, moved ? "Program keys back to defaults: taken from players" : "The program keys are back to the defaults");
    else
        say(C_GOOD, moved ? "Player %d back to defaults: keys taken from others" : "Player %d is back to the defaults", p + 1);
    s.browse_player = -1;
    s.dirty = 1;
}

/* The pad choices of the pad row, in order: any, each pad plugged in, none. Returns how many, and where the player's choice is now. */
static int device_choices(int p, int *slots, int *now)
{
    PcControls *c = pc_input_controls();
    int n = pc_input_device_slots(slots + 1, 8), i, mine = pc_input_player_device(p);
    slots[0] = -1;
    slots[n + 1] = -2;
    *now = 0;
    if (!strcmp(c->player[p].controller, PC_CONTROLLER_NONE))
        *now = n + 1;
    else if (strcmp(c->player[p].controller, PC_CONTROLLER_ANY))
        for (i = 1; i <= n; i++)
            if (slots[i] == mine)
                *now = i;
    return n + 2;
}

static void change(int step)
{
    PcControls *c = pc_input_controls();
    int p = page_player(), row = s.row[s.page];
    if (row == ROW_TABS) {
        s.page = (s.page + PAGES + step) % PAGES;
        s.row[s.page] = ROW_TABS;       /* the cursor stays on the tabs */
        s.browse_player = -1;
        say(C_DIM, "");
        return;
    }
    if (p < 0) {
        if (row >= PORT_ROW_ACTION && row < PORT_ROW_RESET)
            s.column = step > 0;
        return;
    }
    switch (row) {
    case ROW_DEVICE: {
        int slots[12], now, n = device_choices(p, slots, &now);
        /* Each step starts from where the pads stood when this row's first step came (or after a pad came or went since): a choice that took the other
           player's pad is undone by the next step, so going along the row and back leaves both players as they were. */
        if (s.browse_player != p || s.browse_devices != pc_input_devices_changed()) {
            s.browse_player = p;
            s.browse_devices = pc_input_devices_changed();
            pc_input_assignment(s.browse_slots);
            memcpy(s.browse_controller, c->player[p].controller, sizeof(s.browse_controller));
        } else {
            memcpy(c->player[p].controller, s.browse_controller, sizeof(c->player[p].controller));
            pc_input_set_assignment(s.browse_slots);
        }
        now = (now + n + step) % n;
        pc_input_choose(p, slots[now]);
        s.dirty = 1;
        say(C_DIM, "");
        break;
    }
    case ROW_BUTTONS:
        c->player[p].three_button = !c->player[p].three_button;
        s.dirty = 1;
        break;
    case ROW_STICK:
        c->player[p].stick = (c->player[p].stick + 4 + step) & 3;
        s.dirty = 1;
        break;
    case ROW_DEADZONE: {
        int z = c->player[p].deadzone + 5 * step;
        c->player[p].deadzone = z < 5 ? 5 : z > 95 ? 95 : z;
        s.dirty = 1;
        break;
    }
    default:
        if (row >= ROW_ACTION && row < ROW_RESET)
            s.column = step > 0;
        break;
    }
}

static void choose(void)
{
    int p = page_player(), row = s.row[s.page];
    if (row == ROW_TABS) {
        change(1);
    } else if (p < 0) {
        if (row == PORT_ROW_RESET)
            reset_page();
        else
            start_capture(s.column ? CAPTURE_PAD : CAPTURE_KEY);
    } else if (row == ROW_DEVICE) {
        start_capture(CAPTURE_DEVICE);
    } else if (row == ROW_RESET) {
        reset_page();
    } else if (row >= ROW_ACTION) {
        start_capture(s.column ? CAPTURE_PAD : CAPTURE_KEY);
    } else {
        change(1);
    }
}

static void clear_cell(void)
{
    PcControls *c = pc_input_controls();
    int p = page_player(), row = s.row[s.page];
    int a = p < 0 ? row - PORT_ROW_ACTION : row - ROW_ACTION, n = p < 0 ? PC_PORT_N : PC_CTL_N;
    if (p >= 0 && row == ROW_DEVICE) {
        pc_input_choose(p, -2);
        s.browse_player = -1;
        s.dirty = 1;
        say(C_GOOD, "Player %d plays on the keyboard only", p + 1);
        return;
    }
    if (a < 0 || a >= n)
        return;
    /* The Controls action keeps a key or a pad input: nothing else opens this screen. */
    if (p < 0 && a == PC_PORT_CONTROLS && (s.column ? !c->port_key[a] && c->port_pad[a].kind != PC_IN_NONE : last_way_in(c->port_key[a]))) {
        say(C_BAD, "Controls keeps it: nothing else opens this screen");
        return;
    }
    if (s.column)
        memset(pad_cell(c, p, a), 0, sizeof(PcPadInput));
    else
        *key_cell(c, p, a) = 0;
    s.dirty = 1;
    say(C_GOOD, "%s has no %s now", label_of(p, a), s.column ? "pad input" : "key");
}

static void move(int step)
{
    int n = rows_of(s.page), row = s.row[s.page] + step;
    s.row[s.page] = row < 0 ? 0 : row >= n ? n - 1 : row;
    s.browse_player = -1;
    say(C_DIM, "");
}

static void ask_close(void) { s.close_asked = 1; }
void pc_controls_screen_ask_close(void) { if (s.open && s.capture == CAPTURE_NONE) ask_close(); }

static void screen_key(int key, int repeat);

int pc_controls_screen_key(int key, int repeat)
{
    if (!s.open)
        return 0;
    screen_key(key, repeat);
    return s.close_asked;
}

static void screen_key(int key, int repeat)
{
    if (s.capture == CAPTURE_KEY) {
        int p = page_player();
        if (repeat)
            return;
        if (key == SDL_SCANCODE_ESCAPE) {
            say(C_DIM, "Not changed");
            end_capture();
        } else if (!pc_input_key_name(key)[0] || SDL_GetScancodeFromName(pc_input_key_name(key)) != (SDL_Scancode)key) {
            say(C_BAD, "That key has no name the file can keep: try another");
        } else {
            set_key(p, s.capture_action, key);
            end_capture();
        }
        return;
    }
    if (s.capture != CAPTURE_NONE) {
        if (key == SDL_SCANCODE_ESCAPE && !repeat) {
            say(C_DIM, "Not changed");
            end_capture();
        }
        return;
    }
    switch (key) {
    case SDL_SCANCODE_UP: move(-1); break;
    case SDL_SCANCODE_DOWN: move(1); break;
    case SDL_SCANCODE_LEFT: change(-1); break;
    case SDL_SCANCODE_RIGHT: change(1); break;
    default:
        if (repeat)
            break;
        if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER || key == SDL_SCANCODE_SPACE)
            choose();
        else if (key == SDL_SCANCODE_DELETE || key == SDL_SCANCODE_BACKSPACE)
            clear_cell();
        else if (key == SDL_SCANCODE_TAB || key == SDL_SCANCODE_PAGEDOWN || key == SDL_SCANCODE_PAGEUP) {
            s.page = (s.page + (key == SDL_SCANCODE_PAGEUP ? PAGES - 1 : 1)) % PAGES;
            s.browse_player = -1;
        } else if (key == SDL_SCANCODE_ESCAPE || key == pc_input_controls()->port_key[PC_PORT_CONTROLS])
            ask_close();
        break;
    }
}

void pc_controls_screen_pad(int slot, PcPadInput in)
{
    const PcPadDevice *d = pc_input_device(slot);
    int p = page_player();
    if (!s.open || !d || s.capture == CAPTURE_NONE || s.capture == CAPTURE_KEY)
        return;
    if (s.capture == CAPTURE_DEVICE) {
        pc_input_choose(p, slot);
        s.dirty = 1;
        say(C_GOOD, "Player %d's pad: %s", p + 1, d->name);
        end_capture();
        return;
    }
    if (p >= 0 && slot != pc_input_player_device(p))
        return;         /* another player's pad */
    if (p < 0 && slot != pc_input_player_device(0) && slot != pc_input_player_device(1) && slot != pc_input_player_device(2))
        return;
    if (p >= 0 && d->type < 0 && (in.kind == PC_IN_BUTTON || in.kind == PC_IN_AXIS))
        return;
    set_pad(p, s.capture_action, in, d->type);
    end_capture();
}

/* The pad's own moves, from every player's pad: the D-pad moves, B chooses, Start closes. The other buttons do nothing here, so a player can press them
   to see them light up. A press is taken once, and not while the press that opened the screen or set an input is still held. */
static void pad_moves(void)
{
    unsigned held = pc_input_player_bits(0, 0) | pc_input_player_bits(1, 0) | pc_input_player_bits(2, 0), now = held & ~s.nav_last;
    s.nav_last = held;
    if (s.nav_wait_release) {
        if (held)
            return;
        s.nav_wait_release = 0;
        return;
    }
    if (s.capture != CAPTURE_NONE)
        return;
    if (now & PC_PAD_UP) move(-1);
    if (now & PC_PAD_DOWN) move(1);
    if (now & PC_PAD_LEFT) change(-1);
    if (now & PC_PAD_RIGHT) change(1);
    if (now & PC_PAD_B) choose();
    else if (now & PC_PAD_START) ask_close();
}

unsigned pc_controls_screen_tick(void)
{
    if (!s.open)
        return 0;
    if (s.capture != CAPTURE_NONE && (int64_t)(now_ms() - s.capture_until) >= 0) {
        say(C_DIM, "Nothing was pressed: not changed");
        end_capture();
    }
    if (s.capture == CAPTURE_PAD && page_player() >= 0 && pc_input_player_device(page_player()) < 0) {
        say(C_BAD, "The pad went away: not changed");
        end_capture();
    }
    pad_moves();
    if (s.close_asked) {
        char error[300];
        s.close_asked = 0;
        if (pc_controls_save(error, sizeof(error)) != 0 && !s.save_failed) {
            /* Shown once; the next close goes, and the controls hold for this run only. */
            s.save_failed = 1;
            say(C_BAD, "%.110s", error);
            return 0;
        }
        s.open = 0;
        s.capture = CAPTURE_NONE;
        return PC_ACTION_CONTROLS_CLOSED;
    }
    return 0;
}

/* The settings file's new text: the controls' block in place of their old lines (pc_config_rewrite's change). */
static int merge_block(const char *old_text, char *out, size_t out_size, void *block)
{
    return pc_config_merge_controls(old_text, (const char *)block, out, out_size);
}

/* The settings file's name without its folder: the screen's one line holds about 52 characters, too few for a whole path. */
static const char *file_name(void)
{
    const char *name = strrchr(s.file, '\\'), *name2 = strrchr(s.file, '/');
    if (name2 > name)
        name = name2;
    return name ? name + 1 : s.file;
}

const char *pc_controls_unsaved(int *named)
{
    if (named)
        *named = s.unsaved_named;
    return s.unsaved;
}

int pc_controls_save(char *error, size_t error_size)
{
    static char block[16384];
    int result;
    const char *why = NULL;
    if (!s.dirty)
        return 0;
    s.unsaved_named = 0;
    if (!s.file[0]) {
        why = "the program has no settings file";
    } else if (pc_controls_lines(pc_input_controls(), block, sizeof(block)) < 0) {
        why = "the controls' lines are too long";
    } else {
        /* The program's one writer of its settings file (pc_config.h), which the menu bar uses too. */
        result = pc_config_rewrite(s.file, merge_block, block);
        s.unsaved_named = 1;
        why = result == 0 ? NULL : result == PC_CONFIG_NOT_READ ? "cannot be read" : result == PC_CONFIG_FILE_TOO_LARGE ? "is 32 KB or larger" :
              result == PC_CONFIG_TOO_LARGE ? "would grow too large" : result == PC_CONFIG_NOT_WRITTEN ? "cannot be written" : "cannot be replaced";
    }
    s.unsaved = why;
    if (why) {
        if (s.unsaved_named)
            snprintf(error, error_size, "Not saved: %.30s %s", file_name(), why);
        else
            snprintf(error, error_size, "Not saved: %s", why);
        return -1;
    }
    s.dirty = 0;
    return 0;
}

/* ---- drawing */

static void bar(int x, int y, int w, int h, uint32_t colour)
{
    int i, j;
    for (j = y; j < y + h && j < 224; j++)
        for (i = x; i < x + w && i < PC_PICTURE_W; i++)
            if (i >= 0 && j >= 0)
                s.out.rgb[j][i] = colour;
}

/* Text cut to at most n characters, with ".." at the end when cut. */
static void text_in(int x, int y, const char *text, int n, uint32_t colour)
{
    char cut[64];
    if ((int)strlen(text) <= n) {
        pc_font_draw(&s.out, x, y, text, colour);
        return;
    }
    if (n > (int)sizeof(cut) - 1)
        n = (int)sizeof(cut) - 1;
    memcpy(cut, text, (size_t)n);
    cut[n - 2] = cut[n - 1] = '.';
    cut[n] = 0;
    pc_font_draw(&s.out, x, y, cut, colour);
}

/* A cell: under the cursor it has a bar behind it, red while it waits for a press. */
static void cell(int x, int y, int chars, const char *text, uint32_t colour, int under_cursor)
{
    if (under_cursor)
        bar(x - 2, y - 1, chars * PC_FONT_CELL_W + 3, PC_FONT_CELL_H + 1, s.capture != CAPTURE_NONE ? C_CAPTURE : C_CURSOR);
    text_in(x, y, text, chars, under_cursor ? C_WHITE : colour);
}

static int seconds_left(void)
{
    int64_t left = (int64_t)(s.capture_until - now_ms());
    return left <= 0 ? 0 : (int)((left + 999) / 1000);
}

static void device_text(int p, char *text, size_t size)
{
    PcControls *c = pc_input_controls();
    int slot = pc_input_player_device(p);
    const PcPadDevice *d = slot >= 0 ? pc_input_device(slot) : NULL;
    const char *want = c->player[p].controller;
    if (!strcmp(want, PC_CONTROLLER_NONE))
        snprintf(text, size, "none: keyboard only");
    else if (!strcmp(want, PC_CONTROLLER_ANY) && d)
        snprintf(text, size, "%s", d->name);
    else if (!strcmp(want, PC_CONTROLLER_ANY))
        snprintf(text, size, pc_input_controllers() ? "any pad (none free now)" : "any pad (none plugged in)");
    else if (d && !strncmp(d->guid, want, 32))
        snprintf(text, size, "%s", d->name);
    else if (d)
        snprintf(text, size, "%s (asked: %s)", d->name, strlen(want) > 33 ? want + 33 : "another pad");
    else
        snprintf(text, size, "%s (not plugged in)", strlen(want) > 33 ? want + 33 : "a pad");
}

static void draw_tabs(void)
{
    static const char *const names[PAGES] = {"Player 1", "Player 2", "Player 3", "Program keys"};
    static const int xs[PAGES] = {8, 70, 132, 194};
    int i;
    for (i = 0; i < PAGES; i++) {
        int w = (int)strlen(names[i]) * PC_FONT_CELL_W + 5;
        if (i == s.page) {
            bar(xs[i] - 3, 14, w, 11, s.row[s.page] == ROW_TABS ? C_WHITE : C_GOLD);
            pc_font_draw(&s.out, xs[i], 16, names[i], C_DARK);
        } else {
            pc_font_draw(&s.out, xs[i], 16, names[i], C_DIM);
        }
    }
}

static void draw_player(int p)
{
    PcControls *c = pc_input_controls();
    PcPlayerControls *pl = &c->player[p];
    static const char *const sticks[4] = {"off", "the left stick moves as the D-pad too", "the right stick moves as the D-pad too",
                                          "both sticks move as the D-pad too"};
    char text[96], label[48];
    int row = s.row[s.page], a, joy = uses_joy(p), type = pad_type_of(p);
    unsigned held = pc_input_player_bits(p, 1);
    device_text(p, text, sizeof(text));
    pc_font_draw(&s.out, 8, 30, "Pad", C_DIM);
    cell(74, 30, 40, s.capture == CAPTURE_DEVICE ? "press a button on the pad for this player" : text, C_TEXT, row == ROW_DEVICE);
    pc_font_draw(&s.out, 8, 38, "Buttons", C_DIM);
    cell(74, 38, 40, pl->three_button ? "3-button pad" : "6-button pad", C_TEXT, row == ROW_BUTTONS);
    pc_font_draw(&s.out, 8, 46, "Stick", C_DIM);
    cell(74, 46, 40, sticks[pl->stick & 3], C_TEXT, row == ROW_STICK);
    pc_font_draw(&s.out, 8, 54, "Dead zone", C_DIM);
    snprintf(text, sizeof(text), "%d%% of a stick's or trigger's travel", pl->deadzone);
    cell(74, 54, 40, text, C_TEXT, row == ROW_DEADZONE);
    pc_font_draw(&s.out, 8, 66, "ACTION", C_GOLD);
    pc_font_draw(&s.out, 74, 66, "KEYBOARD", C_GOLD);
    pc_font_draw(&s.out, 170, 66, joy ? "JOYSTICK (by number)" : "PAD", C_GOLD);
    for (a = 0; a < PC_CTL_N; a++) {
        int y = 76 + a * 8, here = row == ROW_ACTION + a, off = pl->three_button && (action_bits[a] & (PC_PAD_X | PC_PAD_Y | PC_PAD_Z | PC_PAD_MODE));
        PcPadInput in = joy ? pl->joy[a] : pl->pad[a];
        pc_font_draw(&s.out, 8, y, action_labels[a], off ? C_DIM : (held & action_bits[a]) ? C_HELD : C_TEXT);
        if (here && s.capture == CAPTURE_KEY) {
            snprintf(text, sizeof(text), "press a key %d", seconds_left());
            cell(74, y, 15, text, C_TEXT, 1);
        } else {
            cell(74, y, 15, pl->key[a] ? pc_input_key_name(pl->key[a]) : "-", off ? C_DIM : C_TEXT, here && s.column == 0);
        }
        if (here && s.capture == CAPTURE_PAD) {
            snprintf(text, sizeof(text), "press it on the pad %d", seconds_left());
            cell(170, y, 24, text, C_TEXT, 1);
        } else {
            pc_pad_input_label(in, joy ? -1 : type, label, sizeof(label));
            if (off)
                snprintf(label, sizeof(label), "6-button pad only");
            cell(170, y, 24, label, off ? C_DIM : C_TEXT, here && s.column == 1);
        }
    }
    snprintf(text, sizeof(text), "Reset player %d to the defaults", p + 1);
    cell(8, 176, 40, text, C_TEXT, row == ROW_RESET);
}

static void draw_port(void)
{
    PcControls *c = pc_input_controls();
    char text[64], label[48];
    int row = s.row[s.page], a, type = SDL_CONTROLLER_TYPE_UNKNOWN, slot = pc_input_player_device(0);
    if (slot < 0)
        slot = pc_input_player_device(1);
    if (slot < 0)
        slot = pc_input_player_device(2);
    if (slot >= 0 && pc_input_device(slot))
        type = pc_input_device(slot)->type;
    pc_font_draw(&s.out, 8, 30, "ACTION", C_GOLD);
    pc_font_draw(&s.out, 104, 30, "KEYBOARD", C_GOLD);
    pc_font_draw(&s.out, 200, 30, "PAD", C_GOLD);
    for (a = 0; a < PC_PORT_N; a++) {
        int y = 42 + a * 16, here = row == PORT_ROW_ACTION + a;
        pc_font_draw(&s.out, 8, y, port_labels[a], C_TEXT);
        pc_font_draw(&s.out, 14, y + 8, port_about[a], C_DIM);
        if (here && s.capture == CAPTURE_KEY) {
            snprintf(text, sizeof(text), "press a key %d", seconds_left());
            cell(104, y, 15, text, C_TEXT, 1);
        } else {
            cell(104, y, 15, c->port_key[a] ? pc_input_key_name(c->port_key[a]) : "-", C_TEXT, here && s.column == 0);
        }
        if (here && s.capture == CAPTURE_PAD) {
            snprintf(text, sizeof(text), "press it on a pad %d", seconds_left());
            cell(200, y, 19, text, C_TEXT, 1);
        } else {
            pc_pad_input_label(c->port_pad[a], c->port_pad[a].kind >= PC_IN_JOY_BUTTON ? -1 : type, label, sizeof(label));
            cell(200, y, 19, label, C_TEXT, here && s.column == 1);
        }
    }
    cell(8, 44 + PC_PORT_N * 16, 40, "Reset the program keys to the defaults", C_TEXT, row == PORT_ROW_RESET);
}

static void draw_help(void)
{
    int p = page_player(), row = s.row[s.page], slots[8], n = pc_input_device_slots(slots, 8);
    char text[128], file[64], pads[64];
    const char *help, *name = strrchr(s.file, '\\'), *name2 = strrchr(s.file, '/');
    if (name2 > name)
        name = name2;
    name = name ? name + 1 : s.file;
    snprintf(pads, sizeof(pads), "%d pad%s in. Left/Right: which. Enter: press one", n, n == 1 ? "" : "s");
    if (s.capture == CAPTURE_KEY)
        help = "Press the key now. Esc: leave it as it was";
    else if (s.capture != CAPTURE_NONE)
        help = "Press it on the pad now. Esc: leave it as it was";
    else if (row == ROW_TABS)
        help = "Left/Right or Tab: the page. Down: into it";
    else if (p >= 0 && row == ROW_DEVICE)
        help = pads;
    else if (p >= 0 && row > ROW_TABS && row < ROW_ACTION)
        help = "Left/Right: change it";
    else if ((p >= 0 && row == ROW_RESET) || (p < 0 && row == PORT_ROW_RESET))
        help = "Enter: back to the defaults, for this page only";
    else
        help = "Enter: set it. Del: clear it. Left/Right: column";
    pc_font_draw(&s.out, 8, 190, s.status, s.status_colour);
    pc_font_draw(&s.out, 8, 200, help, C_TEXT);
    {
        /* The pad's own way through the screen, by the names on a pad a player has: the button the game's B is on chooses, Start's closes. */
        PcControls *c = pc_input_controls();
        int q = pc_input_player_device(0) >= 0 ? 0 : 1, slot = pc_input_player_device(q);
        const PcPadDevice *d = slot >= 0 ? pc_input_device(slot) : NULL;
        char b[24] = "B", start[24] = "Start";
        if (d) {
            const PcPadInput *set = d->type < 0 ? c->player[q].joy : c->player[q].pad;
            if (set[PC_CTL_B].kind != PC_IN_NONE)
                pc_pad_input_label(set[PC_CTL_B], d->type, b, sizeof(b));
            if (set[PC_CTL_START].kind != PC_IN_NONE)
                pc_pad_input_label(set[PC_CTL_START], d->type, start, sizeof(start));
        }
        snprintf(text, sizeof(text), "Pad: D-pad moves, %s chooses, %s closes", b, start);
    }
    pc_font_draw(&s.out, 8, 208, text, C_DIM);
    snprintf(file, sizeof(file), "%.40s", name[0] ? name : "no file");
    snprintf(text, sizeof(text), "Saved in %s on closing", file);
    /* After a save that failed, the line above it says why in red, and this one how long the changes last: the next close goes without saving. */
    if (s.save_failed)
        pc_font_draw(&s.out, 8, 216, "Kept until a Soft Reset or the program ends", C_BAD);
    else
        pc_font_draw(&s.out, 8, 216, text, C_DIM);
}

const PcPicture *pc_controls_screen_picture(void)
{
    char text[64];
    const char *key = pc_input_key_name(pc_input_controls()->port_key[PC_PORT_CONTROLS]);
    memcpy(&s.out, &s.base, sizeof(s.out));
    pc_font_draw(&s.out, 8, 4, "CONTROLS", C_GOLD);
    snprintf(text, sizeof(text), "%s%s%s: close", key[0] ? key : "", key[0] ? " or " : "", "Esc");
    pc_font_draw(&s.out, PC_PICTURE_W - 8 - (int)strlen(text) * PC_FONT_CELL_W, 4, text, C_DIM);
    draw_tabs();
    if (s.page == PAGE_PORT)
        draw_port();
    else
        draw_player(s.page);
    draw_help();
    return &s.out;
}
