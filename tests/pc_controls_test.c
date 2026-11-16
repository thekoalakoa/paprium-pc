/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The controls (targets/pc/pc_controls.h): their names and defaults, their lines in the configuration, pads SDL knows and plain joysticks, pads coming
 * and going, which pad is whose, the controller database, and the controls screen driven by made-up keys and pads, its saved file and its picture.
 * Built by tests/test_pc_machine.py when the local configuration names SDL's development files.
 *
 * SDL's virtual joysticks stand in for players' pads, with any layout of buttons, hats and axes, plugged in and pulled out at will. SDL's drivers for real
 * pads are turned off first, so a pad plugged into the test PC changes nothing here; how a real USB or Bluetooth pad reaches SDL is not tested. Nothing is
 * ever shown. Usage: pc_controls_test [the real gamecontrollerdb.txt]. Run in a scratch folder: it writes its files there.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_controls.h"
#include "pc_font.h"
#include "pc_io.h"
#include "pc_menu.h"
#include "pc_window.h"

static int checks, failed;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static uint32_t pad_now[3];
static unsigned actions_seen;

static void pass(void) { actions_seen |= pc_input_poll(pad_now); }
static void settle(void) { pass(); pass(); }

static void key(const char *name)
{
    CHECK(pc_input_push_key(name, 1) == 0);
    pass();
    CHECK(pc_input_push_key(name, 0) == 0);
    pass();
}

/* ---- made-up pads */

/* A pad SDL knows: its own virtual game controller, with all 15 buttons of its layout (their numbers as SDL's virtual pad maps them), two sticks and two
   triggers. */
static int attach_known(void) { return SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0); }

/* A pad SDL does not know, as a plain USB pad would be: any number of buttons, hats and axes. */
static int attach_plain(const char *name, int vendor, int product, int buttons, int hats, int axes)
{
    SDL_VirtualJoystickDesc d;
    SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    d.naxes = (Uint16)axes;
    d.nbuttons = (Uint16)buttons;
    d.nhats = (Uint16)hats;
    d.vendor_id = (Uint16)vendor;
    d.product_id = (Uint16)product;
    d.name = name;
    return SDL_JoystickAttachVirtualEx(&d);
}

/* The virtual pad's own handle, to set its buttons. The index is SDL's for the pad just attached. */
static SDL_Joystick *handle(int index) { return index >= 0 ? SDL_JoystickOpen(index) : NULL; }

static void unplug(SDL_Joystick **j)
{
    SDL_JoystickID id = SDL_JoystickInstanceID(*j);
    int i, index = -1;
    for (i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id)
            index = i;
    SDL_JoystickClose(*j);
    *j = NULL;
    CHECK(index >= 0 && SDL_JoystickDetachVirtual(index) == 0);
    settle();
}

static int slot_of(SDL_Joystick *j)
{
    int slots[8], n = pc_input_device_slots(slots, 8), i;
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof(guid));
    for (i = 0; i < n; i++)
        if (!strcmp(pc_input_device(slots[i])->guid, guid) && !strncmp(pc_input_device(slots[i])->name, SDL_JoystickName(j), strlen(SDL_JoystickName(j))))
            return slots[i];
    return -1;
}

static void button(SDL_Joystick *j, int b, int down) { CHECK(SDL_JoystickSetVirtualButton(j, b, (Uint8)down) == 0); settle(); }
static void axis(SDL_Joystick *j, int a, int v) { CHECK(SDL_JoystickSetVirtualAxis(j, a, (Sint16)v) == 0); settle(); }
static void hat(SDL_Joystick *j, int h, int v) { CHECK(SDL_JoystickSetVirtualHat(j, h, (Uint8)v) == 0); settle(); }

/* One input gives exactly this bit on this player's port, and nothing on the other. */
#define GIVES(set, unset, port, bit) do { set; CHECK(pad_now[port] == (bit) && pad_now[1 - (port)] == 0); unset; CHECK(pad_now[0] == 0 && pad_now[1] == 0); } while (0)

static void known_defaults(SDL_Joystick *j, int port)
{
    static const struct { int button; unsigned bit; } map[] = {
        {SDL_CONTROLLER_BUTTON_A, PC_PAD_B}, {SDL_CONTROLLER_BUTTON_B, PC_PAD_C}, {SDL_CONTROLLER_BUTTON_X, PC_PAD_A}, {SDL_CONTROLLER_BUTTON_Y, PC_PAD_Y},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PC_PAD_X}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PC_PAD_Z}, {SDL_CONTROLLER_BUTTON_START, PC_PAD_START},
        {SDL_CONTROLLER_BUTTON_BACK, PC_PAD_MODE}, {SDL_CONTROLLER_BUTTON_DPAD_UP, PC_PAD_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PC_PAD_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PC_PAD_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PC_PAD_RIGHT}};
    size_t i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        GIVES(button(j, map[i].button, 1), button(j, map[i].button, 0), port, map[i].bit);
    GIVES(axis(j, SDL_CONTROLLER_AXIS_LEFTX, -30000), axis(j, SDL_CONTROLLER_AXIS_LEFTX, 0), port, PC_PAD_LEFT);
    GIVES(axis(j, SDL_CONTROLLER_AXIS_LEFTX, 30000), axis(j, SDL_CONTROLLER_AXIS_LEFTX, 0), port, PC_PAD_RIGHT);
    GIVES(axis(j, SDL_CONTROLLER_AXIS_LEFTY, -30000), axis(j, SDL_CONTROLLER_AXIS_LEFTY, 0), port, PC_PAD_UP);
    GIVES(axis(j, SDL_CONTROLLER_AXIS_LEFTY, 30000), axis(j, SDL_CONTROLLER_AXIS_LEFTY, 0), port, PC_PAD_DOWN);
    /* The other buttons and the right stick do nothing by default. */
    GIVES(button(j, SDL_CONTROLLER_BUTTON_GUIDE, 1), button(j, SDL_CONTROLLER_BUTTON_GUIDE, 0), port, 0);
    GIVES(axis(j, SDL_CONTROLLER_AXIS_RIGHTX, 30000), axis(j, SDL_CONTROLLER_AXIS_RIGHTX, 0), port, 0);
}

static void plain_defaults(SDL_Joystick *j, int port)
{
    static const unsigned buttons[10] = {PC_PAD_A, PC_PAD_B, PC_PAD_C, PC_PAD_X, PC_PAD_Y, PC_PAD_Z, 0, 0, PC_PAD_MODE, PC_PAD_START};
    int i;
    for (i = 0; i < 10; i++)
        GIVES(button(j, i, 1), button(j, i, 0), port, buttons[i]);
    GIVES(hat(j, 0, SDL_HAT_UP), hat(j, 0, SDL_HAT_CENTERED), port, PC_PAD_UP);
    GIVES(hat(j, 0, SDL_HAT_RIGHTDOWN), hat(j, 0, SDL_HAT_CENTERED), port, PC_PAD_RIGHT | PC_PAD_DOWN);
    GIVES(hat(j, 0, SDL_HAT_LEFT), hat(j, 0, SDL_HAT_CENTERED), port, PC_PAD_LEFT);
    GIVES(axis(j, 0, -30000), axis(j, 0, 0), port, PC_PAD_LEFT);
    GIVES(axis(j, 1, 30000), axis(j, 1, 0), port, PC_PAD_DOWN);
    GIVES(axis(j, 2, 30000), axis(j, 2, 0), port, 0);
}

/* ---- names and lines */

static void test_names(void)
{
    PcPadInput in, back;
    char text[48];
    int i, d, n, bad = 0, keys = 0;
    for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        in.kind = PC_IN_BUTTON;
        in.index = (uint8_t)i;
        in.dir = 0;
        pc_pad_input_text(in, text, sizeof(text));
        CHECK(!strcmp(text, SDL_GameControllerGetStringForButton((SDL_GameControllerButton)i)));
        CHECK(pc_pad_input_parse(text, &back) == 0 && pc_pad_input_equal(in, back));
    }
    for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
        for (d = -1; d <= 1; d += 2) {
            in.kind = PC_IN_AXIS;
            in.index = (uint8_t)i;
            in.dir = (int8_t)d;
            pc_pad_input_text(in, text, sizeof(text));
            CHECK(pc_pad_input_parse(text, &back) == 0 && pc_pad_input_equal(in, back));
        }
    for (i = 0; i < 256; i++) {
        in.kind = PC_IN_JOY_BUTTON;
        in.index = (uint8_t)i;
        in.dir = 0;
        pc_pad_input_text(in, text, sizeof(text));
        bad += pc_pad_input_parse(text, &back) != 0 || !pc_pad_input_equal(in, back);
        in.kind = PC_IN_JOY_AXIS;
        in.dir = i & 1 ? 1 : -1;
        pc_pad_input_text(in, text, sizeof(text));
        bad += pc_pad_input_parse(text, &back) != 0 || !pc_pad_input_equal(in, back);
        in.kind = PC_IN_JOY_HAT;
        in.dir = (int8_t)(1 << (i & 3));
        pc_pad_input_text(in, text, sizeof(text));
        bad += pc_pad_input_parse(text, &back) != 0 || !pc_pad_input_equal(in, back);
    }
    CHECK(bad == 0);
    CHECK(pc_pad_input_parse("", &back) == 0 && back.kind == PC_IN_NONE);
    CHECK(pc_pad_input_parse("a", &back) == 0 && back.kind == PC_IN_BUTTON && back.index == SDL_CONTROLLER_BUTTON_A);
    CHECK(pc_pad_input_parse("lefttrigger+", &back) == 0 && back.kind == PC_IN_AXIS && back.index == SDL_CONTROLLER_AXIS_TRIGGERLEFT && back.dir == 1);
    CHECK(pc_pad_input_parse("hat1left", &back) == 0 && back.kind == PC_IN_JOY_HAT && back.index == 1 && back.dir == SDL_HAT_LEFT);
    {
        static const char *const refused[] = {"button256", "button", "buttonx", "hat0north", "hat0", "axis3", "axis3*", "leftx", "nonsense", "dpup+x", "button1 "};
        for (i = 0; i < (int)(sizeof(refused) / sizeof(refused[0])); i++)
            CHECK(pc_pad_input_parse(refused[i], &back) == -1);
    }
    /* The screen's names, by the kind of pad: by place on a pad of no known family, by its own letters elsewhere. A Switch pad's south button is
       printed B. */
    in.kind = PC_IN_BUTTON;
    in.index = SDL_CONTROLLER_BUTTON_A;
    pc_pad_input_label(in, SDL_CONTROLLER_TYPE_XBOXONE, text, sizeof(text));
    CHECK(!strcmp(text, "A"));
    pc_pad_input_label(in, SDL_CONTROLLER_TYPE_PS5, text, sizeof(text));
    CHECK(!strcmp(text, "Cross"));
    pc_pad_input_label(in, SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO, text, sizeof(text));
    CHECK(!strcmp(text, "B"));
    pc_pad_input_label(in, SDL_CONTROLLER_TYPE_UNKNOWN, text, sizeof(text));
    CHECK(!strcmp(text, "South"));
    in.kind = PC_IN_AXIS;
    in.index = SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
    in.dir = 1;
    pc_pad_input_label(in, SDL_CONTROLLER_TYPE_PS4, text, sizeof(text));
    CHECK(!strcmp(text, "R2"));
    /* Every key with a name: the name brings the same key back, or the screen refuses that key. The defaults all come back. */
    for (i = 1, n = 0; i < SDL_NUM_SCANCODES; i++) {
        const char *name = pc_input_key_name(i);
        if (!name[0])
            continue;
        keys++;
        if (SDL_GetScancodeFromName(name) == (SDL_Scancode)i)
            n++;
        else
            printf("key %d, named '%s': the name gives key %d, so the screen will not take this key\n", i, name, (int)SDL_GetScancodeFromName(name));
    }
    printf("keys with a name: %d, of which %d come back from their name\n", keys, n);
    CHECK(keys > 200 && n >= keys - 2);
    for (i = 0; i < PC_PLAYERS * PC_CTL_N + PC_PORT_N; i++) {
        PcControls d;
        int k;
        pc_controls_defaults(&d);
        k = i < PC_PLAYERS * PC_CTL_N ? d.player[i / PC_CTL_N].key[i % PC_CTL_N] : d.port_key[i - PC_PLAYERS * PC_CTL_N];
        CHECK(SDL_GetScancodeFromName(pc_input_key_name(k)) == (SDL_Scancode)k);
    }
}

/* No two default keys are the same, over players 1 and 2 and the program's own keys, so each does one thing as the program comes: F4 opens the controls
   screen and F6 changes the picture filter. Player 3 has no keys by default: a third pad. Each program action has a bit of its own, which no other
   action and not the screen's closing uses. */
static void test_default_keys(void)
{
    PcControls d;
    int all[PC_PLAYERS * PC_CTL_N + PC_PORT_N], n = 0, i, j, same = 0;
    pc_controls_defaults(&d);
    for (i = 0; i < 2 * PC_CTL_N; i++)
        all[n++] = d.player[i / PC_CTL_N].key[i % PC_CTL_N];
    for (i = 0; i < PC_CTL_N; i++)
        CHECK(d.player[2].key[i] == 0);
    CHECK(PC_PLAYERS == 3 && !strcmp(d.player[2].controller, "any") && d.player[2].three_button == 0);
    for (i = 0; i < PC_PORT_N; i++)
        all[n++] = d.port_key[i];
    for (i = 0; i < n; i++) {
        CHECK(all[i] != 0);
        for (j = i + 1; j < n; j++) {
            if (all[i] == all[j]) {
                same++;
                printf("default keys %d and %d are both %s\n", i, j, pc_input_key_name(all[i]));
            }
        }
    }
    CHECK(same == 0);
    printf("default keys: %d, all different: %s\n", n, same ? "no" : "yes");
    CHECK(d.port_key[PC_PORT_CONTROLS] == SDL_SCANCODE_F4 && d.port_key[PC_PORT_FILTER] == SDL_SCANCODE_F6);
    CHECK(!strcmp(pc_controls_port_name(PC_PORT_FILTER), "filter") && pc_controls_port_bit(PC_PORT_FILTER) == PC_ACTION_FILTER);
    for (i = 0; i < PC_PORT_N; i++) {
        unsigned bit = pc_controls_port_bit(i);
        CHECK(bit != 0 && !(bit & (bit - 1)) && !(bit & PC_ACTION_CONTROLS_CLOSED));
        for (j = i + 1; j < PC_PORT_N; j++)
            CHECK(!(bit & pc_controls_port_bit(j)));
    }
}

static void same_controls(const PcControls *a, const PcControls *b, int line)
{
    if (memcmp(a, b, sizeof(*a))) {
        failed++;
        printf("FAIL line %d: the controls differ\n", line);
    }
    checks++;
}

static void test_lines(void)
{
    static char text[16384];
    PcControls c, back;
    PcConfig config;
    char error[300];
    int n;
    /* The defaults, as lines and back. */
    pc_controls_defaults(&c);
    n = pc_controls_lines(&c, text, sizeof(text));
    CHECK(n > 0 && strstr(text, "key_p1_up=Up\n") && strstr(text, "key_p2_start=Keypad Enter\n") && strstr(text, "key_controls=F4\n"));
    CHECK(strstr(text, "key_filter=F6\n") != NULL);
    CHECK(strstr(text, "pad_p1_a=x\n") && strstr(text, "pad_p1_b=a\n") && strstr(text, "joy_p1_up=hat0up\n") && strstr(text, "joy_p2_start=button9\n"));
    CHECK(strstr(text, "controller1=any\n") && strstr(text, "pad2=six\n") && strstr(text, "stick1=left\n") && strstr(text, "deadzone2=50\n"));
    CHECK(pc_controls_lines(&c, text, 100) == -1);
    pc_config_defaults(&config);
    n = pc_controls_lines(&c, text, sizeof(text));
    CHECK(pc_config_parse(text, &config, error, sizeof(error)) == 0);
    CHECK(pc_controls_from_config(&back, &config, error, sizeof(error)) == 0);
    same_controls(&c, &back, __LINE__);
    /* Every kind of change, as lines and back. */
    c.player[0].key[PC_CTL_A] = SDL_SCANCODE_SPACE;
    c.player[1].key[PC_CTL_MODE] = 0;
    c.port_key[PC_PORT_PAUSE] = SDL_SCANCODE_PAUSE;
    c.player[0].pad[PC_CTL_C].kind = PC_IN_AXIS;
    c.player[0].pad[PC_CTL_C].index = SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
    c.player[0].pad[PC_CTL_C].dir = 1;
    c.player[1].pad[PC_CTL_Z].kind = PC_IN_JOY_BUTTON;
    c.player[1].pad[PC_CTL_Z].index = 17;
    memset(&c.player[1].joy[PC_CTL_UP], 0, sizeof(PcPadInput));
    c.player[1].joy[PC_CTL_Y].kind = PC_IN_JOY_AXIS;
    c.player[1].joy[PC_CTL_Y].index = 5;
    c.player[1].joy[PC_CTL_Y].dir = -1;
    c.port_pad[PC_PORT_CONTROLS].kind = PC_IN_BUTTON;
    c.port_pad[PC_PORT_CONTROLS].index = SDL_CONTROLLER_BUTTON_GUIDE;
    snprintf(c.player[1].controller, sizeof(c.player[1].controller), "ff0058b8341200007856000000007600 Made-up USB pad");
    snprintf(c.player[0].controller, sizeof(c.player[0].controller), "none");
    c.player[0].three_button = 1;
    c.player[0].stick = PC_STICK_BOTH;
    c.player[1].stick = PC_STICK_OFF;
    c.player[1].deadzone = 85;
    pc_config_defaults(&config);
    n = pc_controls_lines(&c, text, sizeof(text));
    CHECK(strstr(text, "key_p2_mode=\n") && strstr(text, "pad_p1_c=righttrigger+\n") && strstr(text, "pad_p2_z=button17\n") && strstr(text, "joy_p2_up=\n"));
    CHECK(strstr(text, "pad_controls=guide\n") && strstr(text, "controller1=none\n") && strstr(text, "pad1=three\n") && strstr(text, "stick1=both\n"));
    CHECK(pc_config_parse(text, &config, error, sizeof(error)) == 0);
    CHECK(pc_controls_from_config(&back, &config, error, sizeof(error)) == 0);
    same_controls(&c, &back, __LINE__);
    /* A file of an earlier version, with only key_ lines: the rest keep their defaults, and an old key still works. */
    pc_config_defaults(&config);
    CHECK(pc_config_parse("key_p1_a=Space\nkey_mute=M\npad2=three\n", &config, error, sizeof(error)) == 0);
    CHECK(pc_controls_from_config(&back, &config, error, sizeof(error)) == 0);
    pc_controls_defaults(&c);
    c.player[0].key[PC_CTL_A] = SDL_SCANCODE_SPACE;
    c.port_key[PC_PORT_MUTE] = SDL_SCANCODE_M;
    c.player[1].three_button = 1;
    same_controls(&c, &back, __LINE__);
    /* Each name it does not know is refused, and named. */
    {
        static const char *const bad[] = {"key_p1_a=No Such Key\n", "key_p9_a=Space\n", "pad_p1_a=nonsense\n", "pad_p4_a=a\n", "joy_p1_a=a\n", "joy_pause=button1\n",
                                          "controller1=my pad\n", "controller2=0123\n", "pad_shoot=a\n"};
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            pc_config_defaults(&config);
            CHECK(pc_config_parse(bad[i], &config, error, sizeof(error)) == 0);
            error[0] = 0;
            CHECK(pc_controls_from_config(&back, &config, error, sizeof(error)) == -1 && strstr(error, "the configuration's "));
        }
        pc_config_defaults(&config);
        CHECK(pc_config_parse("stick1=up\n", &config, error, sizeof(error)) == 1);
        CHECK(pc_config_parse("deadzone2=100\n", &config, error, sizeof(error)) == 1);
        CHECK(pc_config_parse("deadzone2=4\n", &config, error, sizeof(error)) == 1);
    }
}

/* ---- pads */

static void test_pads(void)
{
    PcConfig config;
    PcControls *c;
    char error[300];
    SDL_Joystick *k1, *j1, *k2;
    int s;

    /* A pad SDL knows, there before the program starts: player 1's, and only player 1's. */
    k1 = handle(attach_known());
    pc_config_defaults(&config);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    c = pc_input_controls();
    settle();
    CHECK(pc_input_controllers() == 1 && pc_input_player_device(0) == slot_of(k1) && pc_input_player_device(1) == -1);
    known_defaults(k1, 0);

    /* A plain joystick plugged in while the game runs: player 2, read by number. */
    j1 = handle(attach_plain("Made-up USB pad", 0x1234, 0x5678, 12, 1, 4));
    settle();
    s = slot_of(j1);
    CHECK(s >= 0 && pc_input_player_device(1) == s && pc_input_device(s)->type == -1);
    plain_defaults(j1, 1);
    known_defaults(k1, 0);
    /* Both at once, each on its own port. */
    button(k1, SDL_CONTROLLER_BUTTON_START, 1);
    button(j1, 0, 1);
    CHECK(pad_now[0] == PC_PAD_START && pad_now[1] == PC_PAD_A);
    button(k1, SDL_CONTROLLER_BUTTON_START, 0);
    button(j1, 0, 0);

    /* The dead zone: at 80 %, a stick at 70 % of its travel is not a press, at 85 % it is. */
    c->player[1].deadzone = 80;
    GIVES(axis(j1, 0, 22937), axis(j1, 0, 0), 1, 0);
    GIVES(axis(j1, 0, 27852), axis(j1, 0, 0), 1, PC_PAD_RIGHT);
    c->player[1].deadzone = 50;
    /* Which stick moves as the D-pad: the right is axes 2 and 3 on a plain joystick. */
    c->player[1].stick = PC_STICK_RIGHT;
    GIVES(axis(j1, 2, 30000), axis(j1, 2, 0), 1, PC_PAD_RIGHT);
    GIVES(axis(j1, 0, 30000), axis(j1, 0, 0), 1, 0);
    c->player[1].stick = PC_STICK_OFF;
    GIVES(axis(j1, 2, 30000), axis(j1, 2, 0), 1, 0);
    c->player[1].stick = PC_STICK_BOTH;
    GIVES(axis(j1, 3, -30000), axis(j1, 3, 0), 1, PC_PAD_UP);
    GIVES(axis(j1, 1, -30000), axis(j1, 1, 0), 1, PC_PAD_UP);
    c->player[1].stick = PC_STICK_LEFT;
    c->player[0].stick = PC_STICK_RIGHT;
    GIVES(axis(k1, SDL_CONTROLLER_AXIS_RIGHTY, 30000), axis(k1, SDL_CONTROLLER_AXIS_RIGHTY, 0), 0, PC_PAD_DOWN);
    c->player[0].stick = PC_STICK_LEFT;
    /* A program key on a pad: once a press, as a key is. */
    c->port_pad[PC_PORT_PAUSE].kind = PC_IN_BUTTON;
    c->port_pad[PC_PORT_PAUSE].index = SDL_CONTROLLER_BUTTON_GUIDE;
    actions_seen = 0;
    button(k1, SDL_CONTROLLER_BUTTON_GUIDE, 1);
    CHECK(actions_seen == PC_ACTION_PAUSE);
    actions_seen = 0;
    settle();
    CHECK(actions_seen == 0);
    button(k1, SDL_CONTROLLER_BUTTON_GUIDE, 0);
    memset(&c->port_pad[PC_PORT_PAUSE], 0, sizeof(PcPadInput));
    /* The menu bar's input on a pad: one press opens the menu bar, the next closes it, as the controls screen says, so a player with only a pad can
       close it. The program does both with pc_menu_open_first when it sees the action. While a menu or a Help page is open, the pad's other program
       inputs do nothing. */
    pc_menu_begin();
    pc_menu_add_item(pc_menu_add_menu("Game", 1), "Pause", "", PC_MENU_DO_NOTHING, 0, 0);
    c->port_pad[PC_PORT_MENU].kind = PC_IN_BUTTON;
    c->port_pad[PC_PORT_MENU].index = SDL_CONTROLLER_BUTTON_GUIDE;
    c->port_pad[PC_PORT_PAUSE].kind = PC_IN_BUTTON;
    c->port_pad[PC_PORT_PAUSE].index = SDL_CONTROLLER_BUTTON_LEFTSTICK;
    actions_seen = 0;
    button(k1, SDL_CONTROLLER_BUTTON_GUIDE, 1);
    CHECK(actions_seen == PC_ACTION_MENU);
    if (actions_seen == PC_ACTION_MENU)
        pc_menu_open_first();
    button(k1, SDL_CONTROLLER_BUTTON_GUIDE, 0);
    CHECK(pc_menu_is_open());
    actions_seen = 0;
    button(k1, SDL_CONTROLLER_BUTTON_LEFTSTICK, 1);
    button(k1, SDL_CONTROLLER_BUTTON_LEFTSTICK, 0);
    CHECK(actions_seen == 0 && pc_menu_is_open());
    button(k1, SDL_CONTROLLER_BUTTON_GUIDE, 1);
    CHECK(actions_seen == PC_ACTION_MENU);
    if (actions_seen == PC_ACTION_MENU)
        pc_menu_open_first();
    button(k1, SDL_CONTROLLER_BUTTON_GUIDE, 0);
    CHECK(!pc_menu_is_open());
    pc_menu_show_page("Keys", "A page of the Help menu.");
    actions_seen = 0;
    button(k1, SDL_CONTROLLER_BUTTON_GUIDE, 1);
    CHECK(actions_seen == PC_ACTION_MENU);
    if (actions_seen == PC_ACTION_MENU)
        pc_menu_open_first();
    button(k1, SDL_CONTROLLER_BUTTON_GUIDE, 0);
    CHECK(!pc_menu_is_open() && pad_now[0] == 0);
    memset(&c->port_pad[PC_PORT_MENU], 0, sizeof(PcPadInput));
    memset(&c->port_pad[PC_PORT_PAUSE], 0, sizeof(PcPadInput));
    pc_menu_begin();

    /* A third pad, plugged in while players 1 and 2 have one, is player 3's: it plugs player 3 in, and players 1 and 2 do not hear it. Player 1's pad
       pulled out: players 1 and 2 come before player 3, so player 1 takes the third pad, as with no player 3, and player 3 has none; the plain
       joystick stays player 2's. */
    CHECK(!pc_input_player_present(2) && pc_input_player_present(0) && pc_input_player_present(1));
    k2 = handle(attach_known());
    settle();
    CHECK(pc_input_controllers() == 3 && pc_input_player_device(0) == slot_of(k1) && pc_input_player_device(1) == slot_of(j1));
    /* k2 is of k1's kind with k1's name, so slot_of cannot tell the two apart: player 3's pad is the third one, neither k1's nor j1's. */
    CHECK(pc_input_player_device(2) >= 0 && pc_input_player_device(2) != slot_of(k1) && pc_input_player_device(2) != slot_of(j1) &&
          pc_input_player_present(2));
    button(k2, SDL_CONTROLLER_BUTTON_A, 1);
    CHECK(pad_now[0] == 0 && pad_now[1] == 0 && pad_now[2] == PC_PAD_B);
    button(k2, SDL_CONTROLLER_BUTTON_A, 0);
    CHECK(pad_now[2] == 0);
    unplug(&k1);
    CHECK(pc_input_player_device(0) == slot_of(k2) && pc_input_player_device(1) == slot_of(j1) && pc_input_player_device(2) == -1 &&
          !pc_input_player_present(2));
    known_defaults(k2, 0);
    unplug(&k2);
    CHECK(pc_input_player_device(0) == -1 && pc_input_player_device(1) == slot_of(j1) && pc_input_player_device(2) == -1 && !pc_input_player_present(2));
    /* A key of player 3's plugs player 3 in without a pad. */
    c->player[2].key[PC_CTL_A] = SDL_SCANCODE_KP_1;
    CHECK(pc_input_player_present(2));
    c->player[2].key[PC_CTL_A] = 0;
    plain_defaults(j1, 1);
    k1 = handle(attach_known());
    settle();
    CHECK(pc_input_player_device(0) == slot_of(k1));
    known_defaults(k1, 0);

    /* Choosing: player 1 takes the plain joystick, so player 2 gets player 1's pad, and each player's set of inputs follows the pad they have. */
    pc_input_choose(0, slot_of(j1));
    CHECK(pc_input_player_device(0) == slot_of(j1) && pc_input_player_device(1) == slot_of(k1));
    CHECK(!strncmp(c->player[0].controller, "ff0058b8341200007856000000007600 Made-up USB pad", 64));
    plain_defaults(j1, 0);
    known_defaults(k1, 1);
    /* None: keyboard only for player 1. The pad that frees goes to player 2 only if player 2 has none; player 3 has none, so it is player 3's, and
       players 1 and 2 do not hear it. */
    pc_input_choose(0, -2);
    CHECK(pc_input_player_device(0) == -1 && pc_input_player_device(1) == slot_of(k1) && pc_input_player_device(2) == slot_of(j1));
    GIVES(button(j1, 0, 1), button(j1, 0, 0), 0, 0);
    /* Any again: player 1 comes before player 3, so player 1 takes that pad back, as with no player 3, and player 3 has none. */
    pc_input_choose(0, -1);
    CHECK(pc_input_player_device(0) == slot_of(j1) && pc_input_player_device(1) == slot_of(k1) && pc_input_player_device(2) == -1);
    /* Started again with player 3 asking for the plain joystick's kind. While the game does not hear player 3, players 1 and 2 come first, so they
       have the two pads and player 3 none; while it does, a pad of the kind player 3 asked for stays theirs, so with two pads player 2 has none. */
    pc_input_close();
    pc_config_defaults(&config);
    CHECK(pc_config_parse("controller3=ff0058b8341200007856000000007600 Made-up USB pad\n", &config, error, sizeof(error)) == 0);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    settle();
    CHECK(pc_input_player_device(0) == slot_of(k1) && pc_input_player_device(1) == slot_of(j1) && pc_input_player_device(2) == -1);
    pc_input_close();
    pc_input_set_player3_heard(1);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    settle();
    CHECK(pc_input_player_device(2) == slot_of(j1) && pc_input_player_device(0) == slot_of(k1) && pc_input_player_device(1) == -1);
    pc_input_set_player3_heard(0);
    /* Started again with player 2 asking for the plain joystick's kind and player 1 for any: player 2 gets it, though player 1 asks first. */
    pc_input_close();
    pc_config_defaults(&config);
    CHECK(pc_config_parse("controller2=ff0058b8341200007856000000007600 Made-up USB pad\n", &config, error, sizeof(error)) == 0);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    settle();
    CHECK(pc_input_player_device(1) == slot_of(j1) && pc_input_player_device(0) == slot_of(k1));
    /* The kind asked for is not there: the first free pad, as any. When it comes back, it is taken again. */
    unplug(&j1);
    unplug(&k1);
    k1 = handle(attach_known());
    settle();
    CHECK(pc_input_player_device(0) == slot_of(k1) && pc_input_player_device(1) == -1);
    j1 = handle(attach_plain("Made-up USB pad", 0x1234, 0x5678, 12, 1, 4));
    settle();
    CHECK(pc_input_player_device(1) == slot_of(j1));
    /* A pad asked for by kind is the same kind on another way of connecting, when SDL gives the same maker and product: the bus and the name's check
       digits at the id's start do not count. */
    pc_input_close();
    pc_config_defaults(&config);
    CHECK(pc_config_parse("controller1=05001111341200007856000000007600 The same pad on Bluetooth\n", &config, error, sizeof(error)) == 0);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    settle();
    CHECK(pc_input_player_device(0) == slot_of(j1) && pc_input_player_device(1) == slot_of(k1));
    pc_input_close();
    unplug(&j1);
    unplug(&k1);
    CHECK(pc_input_controllers() == 0);

    /* "any" takes a game controller before a plain joystick, whatever came first: SDL lists pedals and the like as plain joysticks too. */
    j1 = handle(attach_plain("Made-up pedal", 0x4567, 0x1111, 2, 0, 1));
    k1 = handle(attach_known());
    pc_config_defaults(&config);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    settle();
    CHECK(pc_input_player_device(0) == slot_of(k1) && pc_input_player_device(1) == slot_of(j1));
    pc_input_close();
    unplug(&k1);
    unplug(&j1);

    /* Names: pads open with the same name get a number, the lowest free, so a pad plugged in again takes its number back; a name that only begins
       like another one is its own. */
    pc_config_defaults(&config);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    {
        SDL_Joystick *t1 = handle(attach_plain("Twin pad", 0x5678, 0x2222, 12, 1, 2)), *t2, *pro, *plain;
        int slots[8], n;
        settle();
        t2 = handle(attach_plain("Twin pad", 0x5678, 0x2222, 12, 1, 2));
        settle();
        n = pc_input_device_slots(slots, 8);
        CHECK(n == 2 && !strcmp(pc_input_device(slots[0])->name, "Twin pad") && !strcmp(pc_input_device(slots[1])->name, "Twin pad 2"));
        unplug(&t1);
        t1 = handle(attach_plain("Twin pad", 0x5678, 0x2222, 12, 1, 2));
        settle();
        n = pc_input_device_slots(slots, 8);
        CHECK(n == 2 && !strcmp(pc_input_device(slots[0])->name, "Twin pad 2") && !strcmp(pc_input_device(slots[1])->name, "Twin pad"));
        pro = handle(attach_plain("Pad Pro", 0x6789, 0x4444, 12, 1, 2));
        settle();
        plain = handle(attach_plain("Pad", 0x6789, 0x5555, 12, 1, 2));
        settle();
        n = pc_input_device_slots(slots, 8);
        CHECK(n == 4 && !strcmp(pc_input_device(slots[2])->name, "Pad Pro") && !strcmp(pc_input_device(slots[3])->name, "Pad"));
        pc_input_close();
        unplug(&t1);
        unplug(&t2);
        unplug(&pro);
        unplug(&plain);
    }
    CHECK(pc_input_controllers() == 0);
}

/* ---- pads pulled out for a moment, with a third player */

/* Which pad each player has, as letters of the pads given (a pad not among them '?', none '-'). */
static void whose(SDL_Joystick *const *pads, const char *letters, char out[4])
{
    int p, i;
    for (p = 0; p < 3; p++) {
        int s = pc_input_player_device(p);
        out[p] = s < 0 ? '-' : '?';
        for (i = 0; letters[i]; i++)
            if (pads[i] && s >= 0 && s == slot_of(pads[i]))
                out[p] = letters[i];
    }
    out[3] = 0;
}
#define WHOSE(want) do { char got_[4]; whose(pads, "ABCD", got_); CHECK(!strcmp(got_, want)); \
                         if (strcmp(got_, want)) printf("  line %d: whose %s, wanted %s\n", __LINE__, got_, want); } while (0)

static SDL_Joystick *plug_pad(int k)
{
    static const char *names[4] = {"Pad A", "Pad B", "Pad C", "Pad D"};
    return handle(attach_plain(names[k], 0x1100 * (k + 1), 0x0042, 12, 1, 2));
}

/* A press of button 0 (A on a plain joystick) on this pad reaches exactly this player; -1: no one. */
static void reaches(SDL_Joystick *j, int player)
{
    button(j, 0, 1);
    CHECK(pad_now[0] == (player == 0 ? (uint32_t)PC_PAD_A : 0u) && pad_now[1] == (player == 1 ? (uint32_t)PC_PAD_A : 0u) &&
          pad_now[2] == (player == 2 ? (uint32_t)PC_PAD_A : 0u));
    button(j, 0, 0);
}

/* The pad plugged in last, as its slot. */
static int newest_slot(void)
{
    int slots[8], n = pc_input_device_slots(slots, 8);
    return n ? slots[n - 1] : -1;
}

/* A press of X (the game's A by default) on a pad SDL knows reaches exactly this player; -1: no one. */
static void reaches_x(SDL_Joystick *j, int player)
{
    button(j, SDL_CONTROLLER_BUTTON_X, 1);
    CHECK(pad_now[0] == (player == 0 ? (uint32_t)PC_PAD_A : 0u) && pad_now[1] == (player == 1 ? (uint32_t)PC_PAD_A : 0u) &&
          pad_now[2] == (player == 2 ? (uint32_t)PC_PAD_A : 0u));
    button(j, SDL_CONTROLLER_BUTTON_X, 0);
}

/* The rule: a pad that comes back after a pull goes back to the player who had it, so no two players end up with each other's pads; while the game does
   not hear player 3, players 1 and 2 come first (player 3's pad is theirs to use until their own is back); while it does, no player takes another's. */
static void test_pads_pulled(void)
{
    PcConfig config;
    char error[300];
    SDL_Joystick *pads[4] = {NULL, NULL, NULL, NULL};
    int heard, k, s3;

    for (heard = 0; heard < 2; heard++) {
        pc_input_set_player3_heard(heard);
        pc_config_defaults(&config);
        for (k = 0; k < 3; k++)
            pads[k] = plug_pad(k);
        CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
        settle();
        WHOSE("ABC");
        /* Player 1's pad pulled out for a moment. Not heard: player 1 takes player 3's pad meanwhile. Heard: player 1's port is quiet. */
        unplug(&pads[0]);
        WHOSE(heard ? "-BC" : "CB-");
        pads[0] = plug_pad(0);
        settle();
        WHOSE("ABC");
        reaches(pads[0], 0);
        reaches(pads[2], 2);
        /* The same with player 2's pad. */
        unplug(&pads[1]);
        WHOSE(heard ? "A-C" : "AC-");
        pads[1] = plug_pad(1);
        settle();
        WHOSE("ABC");
        reaches(pads[1], 1);
        /* Player 3's pad pulled out and back: player 3's again. */
        unplug(&pads[2]);
        WHOSE("AB-");
        pads[2] = plug_pad(2);
        settle();
        WHOSE("ABC");
        /* Both of players 1 and 2's pads out, back in either order. */
        unplug(&pads[0]);
        unplug(&pads[1]);
        WHOSE(heard ? "--C" : "C--");
        pads[1] = plug_pad(1);
        settle();
        WHOSE(heard ? "-BC" : "CB-");
        pads[0] = plug_pad(0);
        settle();
        WHOSE("ABC");
        unplug(&pads[1]);
        unplug(&pads[0]);
        pads[0] = plug_pad(0);
        settle();
        WHOSE(heard ? "A-C" : "AC-");
        pads[1] = plug_pad(1);
        settle();
        WHOSE("ABC");
        /* A spare fourth pad: player 1 takes it while their own is out (not player 3's), and it is free again once their own is back. */
        pads[3] = plug_pad(3);
        settle();
        WHOSE("ABC");
        unplug(&pads[0]);
        WHOSE("DBC");
        pads[0] = plug_pad(0);
        settle();
        WHOSE("ABC");
        reaches(pads[3], -1);
        unplug(&pads[3]);
        /* Not heard: player 1 took player 3's pad, then a spare came and player 3 took it; when player 1's own pad is back, player 3 has their own
           pad back too and the spare is free. */
        if (!heard) {
            unplug(&pads[0]);
            WHOSE("CB-");
            pads[3] = plug_pad(3);
            settle();
            WHOSE("CBD");
            pads[0] = plug_pad(0);
            settle();
            WHOSE("ABC");
            unplug(&pads[3]);
        }
        /* A choice of the player's own ends the memory: player 1's pad out, player 1 chooses none, and the pad that comes back is no longer theirs. */
        unplug(&pads[0]);
        pc_input_choose(0, -2);
        pads[0] = plug_pad(0);
        settle();
        CHECK(pc_input_player_device(0) == -1);
        pc_input_choose(0, -1);
        WHOSE("ABC");
        pc_input_close();
        for (k = 0; k < 4; k++)
            if (pads[k])
                unplug(&pads[k]);
    }

    /* Three pads of one model (the same id, no serial number): after a pull of player 1's, the pad that comes back is player 1's, and player 3 has the
       pad they held. */
    for (heard = 0; heard < 2; heard++) {
        SDL_Joystick *k1, *k2, *k3;
        int s1;
        pc_input_set_player3_heard(heard);
        pc_config_defaults(&config);
        k1 = handle(attach_known());
        k2 = handle(attach_known());
        k3 = handle(attach_known());
        CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
        settle();
        s1 = pc_input_player_device(0);
        s3 = pc_input_player_device(2);
        CHECK(s1 >= 0 && s3 >= 0 && pc_input_player_device(1) >= 0);
        unplug(&k1);
        CHECK(pc_input_player_device(0) == (heard ? -1 : s3) && pc_input_player_device(2) == (heard ? s3 : -1));
        k1 = handle(attach_known());
        settle();
        CHECK(pc_input_player_device(0) >= 0 && pc_input_player_device(0) != s3 && pc_input_player_device(2) == s3);
        if (pc_input_player_device(0) < 0 || pc_input_player_device(0) == s3 || pc_input_player_device(2) != s3)
            printf("  heard %d: player 1 had %d, player 3 %d; now %d %d %d\n", heard, s1, s3, pc_input_player_device(0), pc_input_player_device(1),
                   pc_input_player_device(2));
        pc_input_close();
        unplug(&k1);
        unplug(&k2);
        unplug(&k3);
    }

    /* Four pads of one model (the same id, no serial number), the fourth a spare. Player 1's pad pulled: player 1 takes the spare meanwhile. The pad
       that comes back is player 1's again and the spare is free: a pad already plugged in at the pull is never taken for the pad that comes back. Then
       player 2's pad pulled: player 2 takes the spare, never the pad player 1 has, and gets their own back too. The four have one name, so a slot is
       known by the order the pads came in (pc_input_device_slots), the pad that came back last. */
    for (heard = 0; heard < 2; heard++) {
        SDL_Joystick *k4[4];
        int s4[4], i, back1, back2;
        pc_input_set_player3_heard(heard);
        pc_config_defaults(&config);
        for (i = 0; i < 4; i++)
            k4[i] = handle(attach_known());
        CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
        settle();
        CHECK(pc_input_device_slots(s4, 4) == 4);
        CHECK(pc_input_player_device(0) == s4[0] && pc_input_player_device(1) == s4[1] && pc_input_player_device(2) == s4[2]);
        unplug(&k4[0]);
        CHECK(pc_input_player_device(0) == s4[3] && pc_input_player_device(1) == s4[1] && pc_input_player_device(2) == s4[2]);
        k4[0] = handle(attach_known());
        settle();
        back1 = newest_slot();
        CHECK(pc_input_player_device(0) == back1 && pc_input_player_device(1) == s4[1] && pc_input_player_device(2) == s4[2]);
        if (pc_input_player_device(0) != back1)
            printf("  heard %d: the pad that came back is in slot %d, player 1 has %d (the spare is %d)\n", heard, back1, pc_input_player_device(0), s4[3]);
        reaches_x(k4[0], 0);
        reaches_x(k4[3], -1);
        unplug(&k4[1]);
        CHECK(pc_input_player_device(1) == s4[3] && pc_input_player_device(0) == back1 && pc_input_player_device(2) == s4[2]);
        k4[1] = handle(attach_known());
        settle();
        back2 = newest_slot();
        CHECK(pc_input_player_device(1) == back2 && pc_input_player_device(0) == back1 && pc_input_player_device(2) == s4[2]);
        reaches_x(k4[1], 1);
        reaches_x(k4[3], -1);
        pc_input_close();
        for (i = 0; i < 4; i++)
            unplug(&k4[i]);
    }

    /* Two pads of one model and player 3 asking for that model (what the Player 3 page writes), with the game not hearing player 3: player 2 keeps a
       pad. */
    pc_input_set_player3_heard(0);
    pc_config_defaults(&config);
    CHECK(pc_config_parse("controller3=ff0058b8341200007856000000007600 Made-up USB pad\n", &config, error, sizeof(error)) == 0);
    pads[0] = handle(attach_plain("Made-up USB pad", 0x1234, 0x5678, 12, 1, 4));
    pads[1] = handle(attach_plain("Made-up USB pad", 0x1234, 0x5678, 12, 1, 4));
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    settle();
    CHECK(pc_input_player_device(0) >= 0 && pc_input_player_device(1) >= 0 && pc_input_player_device(2) == -1);
    pc_input_close();
    unplug(&pads[0]);
    unplug(&pads[1]);
    CHECK(pc_input_controllers() == 0);
}

/* ---- the poll that opens the controls screen, and the poll that closes it */

static void test_open_close(void)
{
    PcConfig config;
    PcControls *c;
    char error[300];
    SDL_Joystick *k;
    SDL_Event quit;

    k = handle(attach_known());
    pc_config_defaults(&config);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    c = pc_input_controls();
    settle();
    /* P, then F4, in one poll, as two keys pressed within a frame come: only the screen opens. A pause there would end the pause the screen starts. */
    actions_seen = 0;
    CHECK(pc_input_push_key("P", 1) == 0 && pc_input_push_key("F4", 1) == 0);
    pass();
    CHECK(actions_seen == PC_ACTION_CONTROLS);
    CHECK(pc_input_push_key("P", 0) == 0 && pc_input_push_key("F4", 0) == 0);
    pass();
    /* The same for the other program keys before it. Quitting still quits. */
    actions_seen = 0;
    CHECK(pc_input_push_key("F3", 1) == 0 && pc_input_push_key("F1", 1) == 0 && pc_input_push_key("F2", 1) == 0 && pc_input_push_key("F11", 1) == 0);
    CHECK(pc_input_push_key("F6", 1) == 0 && pc_input_push_key("F4", 1) == 0);
    pass();
    CHECK(actions_seen == PC_ACTION_CONTROLS);
    CHECK(pc_input_push_key("F3", 0) == 0 && pc_input_push_key("F1", 0) == 0 && pc_input_push_key("F2", 0) == 0 && pc_input_push_key("F11", 0) == 0);
    CHECK(pc_input_push_key("F6", 0) == 0 && pc_input_push_key("F4", 0) == 0);
    pass();
    actions_seen = 0;
    SDL_zero(quit);
    quit.type = SDL_QUIT;
    CHECK(SDL_PushEvent(&quit) == 1 && pc_input_push_key("F4", 1) == 0);
    pass();
    CHECK(actions_seen == (PC_ACTION_CONTROLS | PC_ACTION_QUIT));
    CHECK(pc_input_push_key("F4", 0) == 0);
    pass();
    /* A pad's pause input held in the poll where F4 comes. */
    c->port_pad[PC_PORT_PAUSE].kind = PC_IN_BUTTON;
    c->port_pad[PC_PORT_PAUSE].index = SDL_CONTROLLER_BUTTON_GUIDE;
    CHECK(SDL_JoystickSetVirtualButton(k, SDL_CONTROLLER_BUTTON_GUIDE, 1) == 0);
    CHECK(pc_input_push_key("F4", 1) == 0);
    actions_seen = 0;
    pass();
    CHECK(actions_seen == PC_ACTION_CONTROLS);
    CHECK(pc_input_push_key("F4", 0) == 0);
    button(k, SDL_CONTROLLER_BUTTON_GUIDE, 0);
    /* Alone, each still does its own thing: F6 here changes the picture filter, and nothing else. */
    actions_seen = 0;
    key("F6");
    CHECK(actions_seen == PC_ACTION_FILTER);
    actions_seen = 0;
    key("P");
    CHECK(actions_seen == PC_ACTION_PAUSE);
    actions_seen = 0;
    button(k, SDL_CONTROLLER_BUTTON_GUIDE, 1);
    button(k, SDL_CONTROLLER_BUTTON_GUIDE, 0);
    CHECK(actions_seen == PC_ACTION_PAUSE);
    memset(&c->port_pad[PC_PORT_PAUSE], 0, sizeof(PcPadInput));

    /* The key that is held as the screen closes, player 1's C here, is kept from the game until it is let go; player 2's keys are not held back. */
    remove("pc_controls_test3.cfg");
    pc_controls_set_file("pc_controls_test3.cfg");
    pc_controls_screen_show(NULL);
    settle();
    CHECK(pc_input_push_key("C", 1) == 0);
    pass();
    CHECK(pc_controls_screen_open() && pc_input_push_key("Escape", 1) == 0);
    pass();
    CHECK(!pc_controls_screen_open() && pad_now[0] == 0);
    /* The Escape that closed it ended its poll early, and SDL then gives the events after it one poll later: two polls from here on. */
    CHECK(pc_input_push_key("Escape", 0) == 0 && pc_input_push_key("3", 1) == 0);
    settle();
    CHECK(pad_now[0] == 0 && pad_now[1] == PC_PAD_C);
    CHECK(pc_input_push_key("C", 0) == 0 && pc_input_push_key("3", 0) == 0);
    settle();
    CHECK(pad_now[0] == 0 && pad_now[1] == 0);
    CHECK(pc_input_push_key("C", 1) == 0);
    settle();
    CHECK(pad_now[0] == PC_PAD_C);
    CHECK(pc_input_push_key("C", 0) == 0);
    settle();
    {
        FILE *f = fopen("pc_controls_test3.cfg", "rb");
        CHECK(f == NULL);       /* nothing changed, so nothing written */
        if (f)
            fclose(f);
    }
    pc_input_close();
    unplug(&k);
}

/* ---- the controller database */

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_database(const char *real_db)
{
    PcConfig config;
    char error[300], guid[33], text[2048], *known;
    SDL_Joystick *j, *other;
    int added, already;

    /* A pad SDL does not know, and a database that knows it: a game controller at once, read through the database's layout. */
    j = handle(attach_plain("Made-up database pad", 0x2345, 0x6789, 12, 1, 4));
    other = handle(attach_plain("Made-up known pad", 0x3456, 0x789A, 8, 0, 2));
    CHECK(j && other && !SDL_IsGameController(0) && !SDL_IsGameController(1));
    /* SDL's own mapping for the second, which the database must not replace. */
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(other), guid, sizeof(guid));
    snprintf(text, sizeof(text), "%s,SDL's own mapping,a:b0,b:b1,platform:Windows,", guid);
    CHECK(SDL_GameControllerAddMapping(text) == 1);
    {
        char g[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), g, sizeof(g));
        snprintf(text, sizeof(text),
                 "# Game Controller DB for SDL in 2.0.16 format\r\n\r\n# Windows\r\n"
                 "%s,Made-up database pad,a:b1,b:b2,x:b0,y:b3,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,platform:Windows,\r\n"
                 "%s,The database's mapping,a:b5,b:b6,platform:Windows,\r\n"
                 "%.28s9999,A Linux pad,a:b0,platform:Linux,\r\n"
                 "not a line of the database\r\n",
                 g, guid, g);
    }
    write_text("pc_controls_test_db.txt", text);
    pc_input_set_controller_db("pc_controls_test_db.txt");
    pc_config_defaults(&config);
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    settle();
    printf("%s\n", pc_input_controller_db_report());
    CHECK(strstr(pc_input_controller_db_report(), ": 1 pads added for this system, 1 left to SDL's own mappings") != NULL);
    CHECK(pc_input_device(slot_of(j)) && pc_input_device(slot_of(j))->type == SDL_CONTROLLER_TYPE_VIRTUAL);     /* SDL calls every virtual pad's kind virtual */
    known = SDL_GameControllerMappingForGUID(SDL_JoystickGetGUID(other));
    CHECK(known && strstr(known, "SDL's own mapping"));
    SDL_free(known);
    /* Through the database's layout: its b1 is the south button, so the game's B; its b0 the west, so A; its hat the D-pad. */
    CHECK(pc_input_player_device(0) == slot_of(j));
    GIVES(button(j, 1, 1), button(j, 1, 0), 0, PC_PAD_B);
    GIVES(button(j, 0, 1), button(j, 0, 0), 0, PC_PAD_A);
    GIVES(button(j, 9, 1), button(j, 9, 0), 0, PC_PAD_START);
    GIVES(hat(j, 0, SDL_HAT_DOWN), hat(j, 0, SDL_HAT_CENTERED), 0, PC_PAD_DOWN);
    pc_input_close();
    /* No database: only the pads SDL knows itself, and the program says so. */
    pc_input_set_controller_db("no_such_database.txt");
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    CHECK(strstr(pc_input_controller_db_report(), "no controller database") != NULL);
    pc_input_close();
    unplug(&j);
    unplug(&other);
    /* The real database, when the test is given it: every line for this system that SDL lacks goes in, and none is refused. */
    if (real_db) {
        int windows = 0;
        size_t size = 0;
        char *file = (char *)SDL_LoadFile(real_db, &size), *at;
        for (at = file; at && (at = strstr(at, "platform:Windows,")) != NULL; at++)
            windows++;
        SDL_free(file);
        added = pc_input_load_controller_db(real_db, &already);
        printf("the real database: %d lines for this system, %d added, %d left to SDL's own mappings, %d mappings in SDL now\n", windows, added, already,
               SDL_GameControllerNumMappings());
        CHECK(windows > 500 && added > 500 && added + already <= windows);
    }
    pc_input_set_controller_db(NULL);
}

/* ---- the screen */

static PcPicture game;

static uint32_t made_up_colour(unsigned x, unsigned y) { return (x * 251u + y * 1021u) & 0xFFFFFFu; }

/* The game behind the screen: a quarter of each colour, and a little more blue. */
static uint32_t dimmed(uint32_t c) { return (c >> 16 & 255u) / 4u << 16 | (c >> 8 & 255u) / 4u << 8 | ((c & 255u) / 4u + 20u); }

static void made_up_game(int width)
{
    unsigned x, y;
    memset(&game, 0, sizeof(game));
    game.width = (uint16_t)width;
    game.height = 224;
    for (y = 0; y < 224; y++)
        for (x = 0; x < (unsigned)width; x++)
            game.rgb[y][x] = made_up_colour(x, y);
}

static int count_in(const char *text, const char *what)
{
    int n = 0;
    for (; (text = strstr(text, what)) != NULL; text++)
        n++;
    return n;
}

static char *read_text(const char *path)
{
    size_t size = 0;
    char *t = (char *)SDL_LoadFile(path, &size);
    return t;
}

/* Row numbers of the player's page: 0 the tabs, 1 the pad, 2 buttons, 3 stick, 4 dead zone, 5 to 16 the actions, 17 reset. */
static void go_row(int now, int want)
{
    while (now < want) { key("Down"); now++; }
    while (now > want) { key("Up"); now--; }
}

static void write_ppm(const char *path, const uint32_t *pixels, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        fputc((int)(pixels[i] >> 16 & 255u), f);
        fputc((int)(pixels[i] >> 8 & 255u), f);
        fputc((int)(pixels[i] & 255u), f);
    }
    fclose(f);
}

static uint32_t screen_pixels[2048 * 1024];

static void test_screen(void)
{
    PcConfig config;
    PcControls *c, before, back;
    PcWindowOptions o;
    char error[300], *t;
    SDL_Joystick *k, *j, *wheel;
    const PcPicture *pic;
    int i, w, h, gold, row;

    remove("pc_controls_test.cfg");
    write_text("pc_controls_test.cfg", "# my own line\r\nscale=2\r\nkey_p1_a=Space\r\n\r\n");
    pc_config_defaults(&config);
    CHECK(pc_config_load("pc_controls_test.cfg", &config, error, sizeof(error)) == 0);
    memset(&o, 0, sizeof(o));
    o.hidden = 1;
    o.scale = 3;       /* the default; 896 x 672 */
    o.want_readback = 1;
    CHECK(pc_window_open(&o, error, sizeof(error)) == 0);
    k = handle(attach_known());
    j = handle(attach_plain("Made-up USB pad", 0x1234, 0x5678, 12, 1, 4));
    CHECK(pc_input_open(&config, error, sizeof(error)) == 0);
    c = pc_input_controls();
    settle();
    CHECK(c->player[0].key[PC_CTL_A] == SDL_SCANCODE_SPACE);
    pc_controls_set_file("pc_controls_test.cfg");
    made_up_game(320);

    /* F4 is the program's: it opens the screen, and the keys after it in the same pass are the screen's. */
    actions_seen = 0;
    CHECK(pc_input_push_key("F4", 1) == 0);
    CHECK(pc_input_push_key("Down", 1) == 0);
    pass();
    CHECK(actions_seen == PC_ACTION_CONTROLS);
    pc_controls_screen_show(&game);
    CHECK(pc_controls_screen_open());
    pass();
    CHECK(pc_input_push_key("F4", 0) == 0 && pc_input_push_key("Down", 0) == 0);
    pass();
    row = 1;                /* the Down after F4 moved the cursor from the tabs to the pad's row */

    /* A key by "press it now": player 1's A. */
    go_row(row, 9);
    key("Return");
    key("Q");
    CHECK(c->player[0].key[PC_CTL_A] == SDL_SCANCODE_Q);
    CHECK(pc_controls_screen_open());
    /* The same key for player 2's B takes it from player 1's A. */
    key("Tab");
    go_row(0, 10);
    key("Return");
    key("Q");
    CHECK(c->player[1].key[PC_CTL_B] == SDL_SCANCODE_Q && c->player[0].key[PC_CTL_A] == 0);
    /* Esc while it waits leaves it as it was; so does waiting 5 seconds. */
    key("Return");
    key("Escape");
    CHECK(c->player[1].key[PC_CTL_B] == SDL_SCANCODE_Q && pc_controls_screen_open());
    pc_controls_test_clock(1000);
    key("Return");
    pc_controls_test_clock(6001);
    pass();
    key("W");
    CHECK(c->player[1].key[PC_CTL_B] == SDL_SCANCODE_Q);
    pc_controls_test_clock(0);
    /* Delete clears it. */
    key("Delete");
    CHECK(c->player[1].key[PC_CTL_B] == 0);

    /* A pad input by "press it now", on player 2's pad, the plain joystick: by number. The press does not also move the cursor. */
    CHECK(pc_input_player_device(1) == slot_of(j));
    go_row(10, 11);                 /* C */
    key("Right");
    key("Return");
    hat(j, 0, SDL_HAT_RIGHT);
    hat(j, 0, SDL_HAT_CENTERED);
    CHECK(c->player[1].joy[PC_CTL_C].kind == PC_IN_JOY_HAT && c->player[1].joy[PC_CTL_C].dir == SDL_HAT_RIGHT);
    key("Return");
    axis(j, 2, 30000);
    axis(j, 2, 0);
    CHECK(c->player[1].joy[PC_CTL_C].kind == PC_IN_JOY_AXIS && c->player[1].joy[PC_CTL_C].index == 2 && c->player[1].joy[PC_CTL_C].dir == 1);
    GIVES(axis(j, 2, 30000), axis(j, 2, 0), 1, PC_PAD_C);
    /* Player 1's pad does not answer for player 2. */
    key("Return");
    button(k, SDL_CONTROLLER_BUTTON_Y, 1);
    button(k, SDL_CONTROLLER_BUTTON_Y, 0);
    button(j, 7, 1);
    button(j, 7, 0);
    CHECK(c->player[1].joy[PC_CTL_C].kind == PC_IN_JOY_BUTTON && c->player[1].joy[PC_CTL_C].index == 7);
    /* On a player's own pad an input does one thing: button 0 for C takes it from A. */
    key("Return");
    button(j, 0, 1);
    button(j, 0, 0);
    CHECK(c->player[1].joy[PC_CTL_C].index == 0 && c->player[1].joy[PC_CTL_A].kind == PC_IN_NONE);
    key("Left");

    /* Player 1's page, the pad SDL knows: by its layout's names. */
    key("Tab");
    key("Tab");
    key("Tab");
    CHECK(pc_input_player_device(0) == slot_of(k));
    go_row(9, 9);                   /* the page remembers its row: player 1's A */
    key("Right");
    key("Return");
    button(k, SDL_CONTROLLER_BUTTON_Y, 1);
    CHECK(c->player[0].pad[PC_CTL_A].kind == PC_IN_BUTTON && c->player[0].pad[PC_CTL_A].index == SDL_CONTROLLER_BUTTON_Y);
    CHECK(c->player[0].pad[PC_CTL_Y].kind == PC_IN_NONE);     /* Y had it */
    button(k, SDL_CONTROLLER_BUTTON_Y, 0);
    /* The press that is taken does not also choose: B's own button, the pad's way to choose, set for B, and the screen waits no more. */
    key("Down");
    key("Return");
    CHECK(pc_controls_screen_waiting());
    button(k, SDL_CONTROLLER_BUTTON_A, 1);
    CHECK(!pc_controls_screen_waiting() && c->player[0].pad[PC_CTL_B].kind == PC_IN_BUTTON && c->player[0].pad[PC_CTL_B].index == SDL_CONTROLLER_BUTTON_A);
    button(k, SDL_CONTROLLER_BUTTON_A, 0);
    CHECK(!pc_controls_screen_waiting());
    key("Up");
    key("Return");
    axis(k, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);      /* SDL's virtual pad keeps its triggers' axes at -32768 when let go */
    axis(k, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    CHECK(c->player[0].pad[PC_CTL_A].kind == PC_IN_AXIS && c->player[0].pad[PC_CTL_A].index == SDL_CONTROLLER_AXIS_TRIGGERLEFT && c->player[0].pad[PC_CTL_A].dir == 1);
    key("Left");

    /* The rows above the actions: three buttons, the stick, the dead zone, by Left and Right. */
    go_row(9, 2);
    key("Right");
    CHECK(c->player[0].three_button == 1);
    key("Down");
    key("Right");
    CHECK(c->player[0].stick == PC_STICK_RIGHT);
    key("Left");
    key("Left");
    CHECK(c->player[0].stick == PC_STICK_OFF);
    key("Down");
    key("Right");
    CHECK(c->player[0].deadzone == 55);
    for (i = 0; i < 30; i++)
        key("Left");
    CHECK(c->player[0].deadzone == 5);
    for (i = 0; i < 9; i++)
        key("Right");
    CHECK(c->player[0].deadzone == 50);

    /* The pad's row: any, each pad in the order they came, none, and round again. */
    go_row(4, 1);
    key("Right");
    CHECK(pc_input_player_device(0) == slot_of(k) && strncmp(c->player[0].controller, "any", 3));
    key("Right");
    CHECK(pc_input_player_device(0) == slot_of(j) && pc_input_player_device(1) == slot_of(k));
    key("Right");
    CHECK(!strcmp(c->player[0].controller, "none") && pc_input_player_device(0) == -1);
    key("Right");
    CHECK(!strcmp(c->player[0].controller, "any"));
    /* Enter, then a press on the pad wanted: that pad is player 1's. */
    key("Return");
    button(k, SDL_CONTROLLER_BUTTON_X, 1);
    button(k, SDL_CONTROLLER_BUTTON_X, 0);
    CHECK(pc_input_player_device(0) == slot_of(k) && pc_input_player_device(1) == slot_of(j));

    /* The pad moves through the screen too: its D-pad down moves, its B chooses, and the press is let go before the next one counts. */
    go_row(1, 0);
    key("Right");
    key("Right");
    key("Right");                   /* the program keys' page, after player 3's */
    key("Down");
    button(k, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 1);
    button(k, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0);       /* Pause */
    button(k, SDL_CONTROLLER_BUTTON_A, 1);               /* the game's B: choose, so it waits for a key */
    button(k, SDL_CONTROLLER_BUTTON_A, 0);
    key("Pause");
    CHECK(c->port_key[PC_PORT_PAUSE] == SDL_SCANCODE_PAUSE);
    /* A program key on a pad. */
    key("Right");
    key("Return");
    button(k, SDL_CONTROLLER_BUTTON_GUIDE, 1);
    button(k, SDL_CONTROLLER_BUTTON_GUIDE, 0);
    CHECK(c->port_pad[PC_PORT_PAUSE].kind == PC_IN_BUTTON && c->port_pad[PC_PORT_PAUSE].index == SDL_CONTROLLER_BUTTON_GUIDE);
    key("Left");
    /* The picture filter's key is on this page like the others: F7 here, by "press it now". */
    go_row(2, 1 + PC_PORT_FILTER);
    key("Return");
    key("F7");
    CHECK(c->port_key[PC_PORT_FILTER] == SDL_SCANCODE_F7 && c->port_key[PC_PORT_CONTROLS] == SDL_SCANCODE_F4);
    go_row(1 + PC_PORT_FILTER, 2);

    /* The picture: the game's, dimmed, behind the text; the window shows it with the game's shape. */
    pic = pc_controls_screen_picture();
    CHECK(pic->width == 320 && pic->height == 224);
    for (i = 0, gold = 0; i < 8 * 48; i++)
        gold += pic->rgb[4 + i / 48][8 + i % 48] == (240u << 16 | 192u << 8 | 64u);     /* the title, in gold */
    CHECK(gold > 40);
    CHECK(pic->rgb[223][319] == dimmed(made_up_colour(319, 223)));
    CHECK(game.rgb[4][8] == made_up_colour(8, 4));          /* the game's own picture is not touched */
    pc_window_present(pic);
    CHECK(pc_window_read_large(screen_pixels, 2048 * 1024, &w, &h) == 0);
    printf("the screen through the window, 4:3 at scale 3: its whole-number enlargement is %d x %d\n", w, h);
    CHECK(w == 320 * 3 && h == 224 * 3);
    {
        int x, y, bad = 0;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                bad += screen_pixels[y * w + x] != pic->rgb[y / 3][x / 3];
        CHECK(bad == 0);
    }
    write_ppm("pc_controls_test_screen.ppm", &pic->rgb[0][0], 320, 224);
    go_row(2, 0);
    key("Left");                    /* player 3's page, for a picture of its own */
    write_ppm("pc_controls_test_screen_p3.ppm", &pc_controls_screen_picture()->rgb[0][0], 320, 224);
    key("Left");                    /* player 2's page, for a second picture */
    write_ppm("pc_controls_test_screen_p2.ppm", &pc_controls_screen_picture()->rgb[0][0], 320, 224);
    key("Right");
    key("Right");

    /* Reset: the program keys' page only. Its row is the one after the last program action. */
    go_row(0, 1 + PC_PORT_N);
    key("Return");
    CHECK(c->port_key[PC_PORT_PAUSE] == SDL_SCANCODE_P && c->port_pad[PC_PORT_PAUSE].kind == PC_IN_NONE && c->player[1].key[PC_CTL_B] == 0);
    CHECK(c->port_key[PC_PORT_FILTER] == SDL_SCANCODE_F6);
    c->port_key[PC_PORT_PAUSE] = SDL_SCANCODE_PAUSE;

    /* The pad's other buttons light their actions and do nothing else: the game's C here. */
    button(k, SDL_CONTROLLER_BUTTON_B, 1);
    CHECK(pc_controls_screen_open() && (pc_input_player_bits(0, 0) & PC_PAD_C));
    button(k, SDL_CONTROLLER_BUTTON_B, 0);
    /* Closing: the pad's Start closes it, and the file is saved, the player's own lines kept. */
    memcpy(&before, c, sizeof(before));
    actions_seen = 0;
    button(k, SDL_CONTROLLER_BUTTON_START, 1);
    CHECK(actions_seen == PC_ACTION_CONTROLS_CLOSED && !pc_controls_screen_open());
    CHECK(pad_now[0] == 0);         /* the Start that closed it does not reach the game, held for two polls */
    button(k, SDL_CONTROLLER_BUTTON_START, 0);
    GIVES(button(k, SDL_CONTROLLER_BUTTON_START, 1), button(k, SDL_CONTROLLER_BUTTON_START, 0), 0, PC_PAD_START);     /* the next press does */
    t = read_text("pc_controls_test.cfg");
    CHECK(t && !strncmp(t, "# my own line\r\nscale=2\r\n\r\n# ---- Controls.", 42));
    CHECK(t && strstr(t, "\r\nkey_p1_a=\r\n") && strstr(t, "\r\nkey_p2_b=\r\n") && strstr(t, "\r\nkey_pause=Pause\r\n") && strstr(t, "\r\npad_p1_a=lefttrigger+\r\n"));
    CHECK(t && strstr(t, "\r\njoy_p2_c=button0\r\n") && strstr(t, "\r\njoy_p2_a=\r\n") && strstr(t, "\r\npad1=three\r\n") && strstr(t, "\r\nstick1=off\r\n"));
    CHECK(t && strstr(t, "\r\nkey_filter=F6\r\n") && strstr(t, "\r\nkey_controls=F4\r\n"));
    CHECK(t && strstr(t, "Space") == NULL);
    SDL_free(t);
    pc_config_defaults(&config);
    CHECK(pc_config_load("pc_controls_test.cfg", &config, error, sizeof(error)) == 0 && config.scale == 2);
    CHECK(pc_controls_from_config(&back, &config, error, sizeof(error)) == 0);
    same_controls(&before, &back, __LINE__);
    /* Changed and closed again: the controls' lines are replaced, not added a second time, and the player's own lines stay first. */
    pc_controls_screen_show(&game);
    settle();
    key("Return");                  /* the cursor is where it was: the program keys' reset */
    key("Escape");
    CHECK(!pc_controls_screen_open() && c->port_key[PC_PORT_PAUSE] == SDL_SCANCODE_P);
    t = read_text("pc_controls_test.cfg");
    CHECK(t && count_in(t, "# ---- Controls.") == 1 && count_in(t, "# Keys, by SDL") == 1 && count_in(t, "key_p1_up=") == 1 && count_in(t, "scale=2") == 1);
    CHECK(t && strstr(t, "\r\nkey_pause=P\r\n") && !strncmp(t, "# my own line\r\nscale=2\r\n\r\n# ---- Controls.", 42));
    SDL_free(t);
    /* Opened and closed again with nothing changed: the file stays as it is, even a line the player wrote at its end, which a rewrite would move above
       the controls' lines. */
    {
        FILE *f = fopen("pc_controls_test.cfg", "ab");
        if (f) {
            fputs("# a line of the player's at the end\r\n", f);
            fclose(f);
        }
    }
    t = read_text("pc_controls_test.cfg");
    pc_controls_screen_show(&game);
    settle();
    key("Escape");
    CHECK(!pc_controls_screen_open());
    {
        char *t2 = read_text("pc_controls_test.cfg");
        CHECK(t && t2 && !strcmp(t, t2));
        SDL_free(t2);
    }
    SDL_free(t);
    /* A file that cannot be written: the screen says so once and stays open; the next close goes, and the controls hold for this run. */
    pc_controls_set_file("no_such_folder/pc_controls_test.cfg");
    pc_controls_screen_show(&game);
    settle();
    for (i = 0; i < 20; i++)
        key("Up");
    key("Right");                   /* from the tabs of the program keys' page, where it closed: player 1's page, then player 2's */
    key("Right");
    go_row(0, 2);
    key("Right");
    actions_seen = 0;
    key("Escape");
    CHECK(pc_controls_screen_open() && actions_seen == 0);
    key("Escape");
    CHECK(!pc_controls_screen_open() && actions_seen == PC_ACTION_CONTROLS_CLOSED && c->player[1].three_button == 1);
    {
        /* The reason stays known until a save succeeds, so the program's Soft Reset can say the changes are not in the file. The screen's line
           names the file without its folder, and fits the screen's 52 characters. */
        int named = 0;
        const char *why = pc_controls_unsaved(&named);
        char error[300];
        CHECK(why != NULL && named == 1);
        CHECK(pc_controls_save(error, sizeof(error)) == -1 && !strncmp(error, "Not saved: pc_controls_test.cfg ", 32) && !strstr(error, "no_such_folder") &&
              strlen(error) <= 52);
    }
    /* A game picture 256 pixels wide sits in the middle, with black beside it. A file not there yet is made. */
    made_up_game(256);
    pc_controls_screen_show(&game);
    pic = pc_controls_screen_picture();
    CHECK(pic->width == 320 && pic->rgb[63][10] == 20u && pic->rgb[63][40] == dimmed(made_up_colour(8, 63)));
    remove("pc_controls_test2.cfg");
    pc_controls_set_file("pc_controls_test2.cfg");
    key("Escape");
    CHECK(!pc_controls_screen_open() && pc_controls_unsaved(NULL) == NULL);
    pc_config_defaults(&config);
    CHECK(pc_config_load("pc_controls_test2.cfg", &config, error, sizeof(error)) == 0 && config.three_button[1] == 1 && config.n_keys == PC_PLAYERS * PC_CTL_N + PC_PORT_N);
    remove("pc_controls_test2.cfg");

    /* ---- The rules the screen keeps. It opens where it closed: player 2's Buttons row. */
    remove("pc_controls_test4.cfg");
    pc_controls_set_file("pc_controls_test4.cfg");
    pc_controls_screen_show(&game);
    settle();
    go_row(2, 0);
    key("Right");                   /* player 3 */
    key("Right");                   /* the program keys */
    go_row(0, 1);                   /* Controls, its key */
    /* The Controls action keeps a way in: its only key cannot be cleared, or given to another action. */
    key("Delete");
    CHECK(c->port_key[PC_PORT_CONTROLS] == SDL_SCANCODE_F4 && pc_controls_screen_open());
    go_row(1, 0);
    key("Right");                   /* player 1 */
    go_row(0, 5);                   /* Up */
    key("Return");
    key("F4");
    CHECK(c->player[0].key[PC_CTL_UP] == SDL_SCANCODE_UP && c->port_key[PC_PORT_CONTROLS] == SDL_SCANCODE_F4 && pc_controls_screen_open());
    /* With a pad input beside it the key may go, and then the pad input stays. Taking that pad input does not also close the screen. The left stick's
       button here, not Guide: SDL drops a Guide press that comes within a quarter of a second of the last Guide release, as this test's presses do. */
    go_row(5, 0);
    key("Left");                    /* the program keys */
    go_row(0, 1);
    key("Right");
    key("Return");
    button(k, SDL_CONTROLLER_BUTTON_LEFTSTICK, 1);
    button(k, SDL_CONTROLLER_BUTTON_LEFTSTICK, 0);
    CHECK(pc_controls_screen_open() && c->port_pad[PC_PORT_CONTROLS].kind == PC_IN_BUTTON && c->port_pad[PC_PORT_CONTROLS].index == SDL_CONTROLLER_BUTTON_LEFTSTICK);
    key("Left");
    key("Delete");
    CHECK(c->port_key[PC_PORT_CONTROLS] == 0);
    key("Right");
    key("Delete");
    CHECK(c->port_pad[PC_PORT_CONTROLS].kind == PC_IN_BUTTON);
    /* That pad input closes the screen, and opens it again. */
    actions_seen = 0;
    button(k, SDL_CONTROLLER_BUTTON_LEFTSTICK, 1);
    CHECK(!pc_controls_screen_open() && actions_seen == PC_ACTION_CONTROLS_CLOSED);
    button(k, SDL_CONTROLLER_BUTTON_LEFTSTICK, 0);
    actions_seen = 0;
    button(k, SDL_CONTROLLER_BUTTON_LEFTSTICK, 1);
    CHECK(actions_seen == PC_ACTION_CONTROLS);
    pc_controls_screen_show(&game);
    button(k, SDL_CONTROLLER_BUTTON_LEFTSTICK, 0);
    CHECK(pc_controls_screen_open());
    /* Back to F4 and no pad input. */
    key("Left");
    key("Return");
    key("F4");
    key("Right");
    key("Delete");
    CHECK(c->port_key[PC_PORT_CONTROLS] == SDL_SCANCODE_F4 && c->port_pad[PC_PORT_CONTROLS].kind == PC_IN_NONE);
    key("Left");

    /* A page's reset keeps a key on one action: player 1's defaults take Z back from player 2's A. */
    go_row(1, 0);
    key("Right");                   /* player 1 */
    key("Right");                   /* player 2 */
    go_row(0, 9);                   /* A */
    key("Return");
    key("Z");
    CHECK(c->player[1].key[PC_CTL_A] == SDL_SCANCODE_Z && c->player[0].key[PC_CTL_A] == 0);
    go_row(9, 0);
    key("Left");                    /* player 1 */
    go_row(0, 17);                  /* reset */
    key("Return");
    CHECK(c->player[0].key[PC_CTL_A] == SDL_SCANCODE_Z && c->player[1].key[PC_CTL_A] == 0);
    /* But the key that is the only way left to open the screen stays the Controls key: Controls on D, then player 1's reset leaves Z with no key. */
    go_row(17, 0);
    key("Left");                    /* the program keys */
    go_row(0, 1);
    key("Return");
    key("D");
    CHECK(c->port_key[PC_PORT_CONTROLS] == SDL_SCANCODE_D && c->player[0].key[PC_CTL_Z] == 0);
    go_row(1, 0);
    key("Right");                   /* player 1 */
    go_row(0, 17);
    key("Return");
    CHECK(c->port_key[PC_PORT_CONTROLS] == SDL_SCANCODE_D && c->player[0].key[PC_CTL_Z] == 0 && c->player[0].key[PC_CTL_A] == SDL_SCANCODE_Z);
    /* The program keys' reset gives Controls F4 again, and player 1's next reset gives Z its D. */
    go_row(17, 0);
    key("Left");
    go_row(0, 1 + PC_PORT_N);
    key("Return");
    CHECK(c->port_key[PC_PORT_CONTROLS] == SDL_SCANCODE_F4);
    go_row(1 + PC_PORT_N, 0);
    key("Right");
    go_row(0, 17);
    key("Return");
    CHECK(c->player[0].key[PC_CTL_Z] == SDL_SCANCODE_D);

    /* The pad row: going along it and back leaves both players' pads as they were. */
    go_row(17, 1);
    CHECK(pc_input_player_device(0) == slot_of(k) && pc_input_player_device(1) == slot_of(j) && !strcmp(c->player[0].controller, "any"));
    key("Right");                   /* player 1's own pad */
    key("Right");                   /* player 2's: they swap */
    CHECK(pc_input_player_device(0) == slot_of(j) && pc_input_player_device(1) == slot_of(k));
    key("Right");                   /* none */
    CHECK(pc_input_player_device(0) == -1 && pc_input_player_device(1) == slot_of(j));
    key("Right");                   /* any */
    CHECK(pc_input_player_device(0) == slot_of(k) && pc_input_player_device(1) == slot_of(j) && !strcmp(c->player[0].controller, "any"));
    /* Player 1 with no pad and player 2 with the only one: going to it takes it, and going back gives it back. */
    key("Delete");
    unplug(&k);
    CHECK(pc_input_player_device(0) == -1 && pc_input_player_device(1) == slot_of(j));
    key("Left");
    CHECK(pc_input_player_device(0) == slot_of(j) && pc_input_player_device(1) == -1);
    key("Left");
    CHECK(pc_input_player_device(0) == -1 && pc_input_player_device(1) == slot_of(j) && !strcmp(c->player[0].controller, "any"));
    k = handle(attach_known());
    settle();
    CHECK(pc_input_player_device(0) == slot_of(k));

    /* A plain joystick's axes past the sixteenth are taken by "press it now" too. */
    wheel = handle(attach_plain("Made-up wheel", 0x6789, 0x3333, 4, 0, 20));
    settle();
    key("Return");                  /* Enter on the pad row, then a press on the pad wanted */
    button(wheel, 2, 1);
    button(wheel, 2, 0);
    CHECK(pc_input_player_device(0) == slot_of(wheel) && pc_input_player_device(1) == slot_of(j));
    go_row(1, 9);                   /* A */
    key("Right");
    key("Return");
    axis(wheel, 17, 30000);
    axis(wheel, 17, 0);
    CHECK(c->player[0].joy[PC_CTL_A].kind == PC_IN_JOY_AXIS && c->player[0].joy[PC_CTL_A].index == 17 && c->player[0].joy[PC_CTL_A].dir == 1);
    GIVES(axis(wheel, 17, 30000), axis(wheel, 17, 0), 0, PC_PAD_A);
    key("Left");
    /* The wheel came in as the third pad, so it was player 3's until player 1 took it, and player 3 got player 1's old pad in the swap. Pulled out:
       player 1 comes before player 3, so player 1 takes its old pad back, as with no player 3, and player 3 has none. */
    unplug(&wheel);
    CHECK(pc_input_player_device(0) == slot_of(k) && pc_input_player_device(1) == slot_of(j) && pc_input_player_device(2) == -1);
    key("Escape");
    CHECK(!pc_controls_screen_open());
    remove("pc_controls_test4.cfg");
    pc_input_print_keys(stdout);
    pc_input_close();
    unplug(&k);
    unplug(&j);
    pc_window_close();
    remove("pc_controls_test.cfg");
}

int main(int argc, char **argv)
{
    /* SDL's drivers for real pads off: only the made-up pads below exist. Without focus SDL would also ignore the pads of a window never shown. */
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    pc_input_before_sdl();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        printf("FAIL: SDL could not start: %s\n", SDL_GetError());
        return 1;
    }
    CHECK(SDL_NumJoysticks() == 0);
    pc_input_set_controller_db("no_such_database.txt");
    test_names();
    test_default_keys();
    test_lines();
    test_pads();
    test_pads_pulled();
    test_open_close();
    test_database(argc > 1 ? argv[1] : NULL);
    pc_input_set_controller_db("no_such_database.txt");
    test_screen();
    printf("%d checks, %d failed\n", checks, failed);
    printf("RESULT: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
