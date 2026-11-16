/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The player's controls, and the controls screen that sets them from inside the game.
 *
 * WHAT A PLAYER CAN SET
 *   For player 1 and player 2 (the cart has two pad ports), and player 3 (who plays through the EA 4-Way Play or the arcade stick's EXT socket, see
 *   pc_scum.h): a key for each of Up, Down, Left, Right, A, B, C, X, Y, Z, Start and Mode, and an input on a pad for each; which pad is that player's; a
 *   three or six button pad; whether an analog stick moves as the D-pad too, and its dead zone. Player 3 has no keys by default and the third pad: a
 *   player 3 with neither a pad nor a key is not plugged in, so the game sees no third player. For the program's own
 *   actions (open this screen, pause, mute, the sprite limits, the picture's shape, the picture filter, full screen, quit, the menu bar): a key and a pad
 *   input each. By default no two actions share a key: F4 opens this screen, F6 changes the picture filter (pc_filter.h), F10 opens the menu bar (pc_menu.h).
 *
 * PADS
 *   Any pad SDL knows as a game controller (Xbox, PlayStation, Switch Pro and many others, wired or on Bluetooth) is read through SDL's game controller
 *   layout, so the same names fit every such pad: its buttons by place (south, east, west, north), shoulders, triggers, sticks and D-pad. SDL reads a pad
 *   on a cable and on Bluetooth through the same calls, but some pads give another product number or name on Bluetooth, so SDL may know one connection
 *   and not the other, and a player's choice of a kind of pad may not find it on the other connection.
 *   SDL reads the face buttons by their place, not by the letter printed on them, so on every pad the bottom row of face buttons is A B C by default, as the
 *   Pocket's own controls have them. A pad SDL does not know is read as a plain joystick: its buttons, hats and axes by number. Each player keeps one set of
 *   inputs for game controllers and one for plain joysticks, and uses the set that fits the pad they have now. SDL's community controller database
 *   (gamecontrollerdb.txt, beside the program) adds the pads it knows that SDL itself does not.
 *
 * WHICH PAD IS WHOSE
 *   Each player's pad is "any" (the first pad plugged in that no other player has, a game controller before a plain joystick: so by default players 1, 2
 *   and 3 have the first, second and third pad), "none" (keyboard only) or one kind of pad, by SDL's id for its model. Pads may come and go while the game
 *   runs. A pad stays with its player while it is there; a player whose pad is pulled out takes the first free pad, or, with none, their port is quiet,
 *   and when that pad comes back it goes back to them, so a short pull (a cable, a pad that went to sleep) never leaves two players with each other's
 *   pads. Which players come first depends on the game (pc_input_set_player3_heard): while the game hears player 3 (3-Player Mode, or Arcade Mode
 *   Multiplayer) no player takes another's pad; while it does not, players 1 and 2 come first, so they get exactly the pads they would get with no
 *   player 3, player 3's pad counting as free for them, and player 3 has what is left over and gets their own pad back once it is free again
 *   (pc_input.c, resolve). Two pads of the same model have the same id, and SDL gives a serial number only for some pads, so two such pads are not
 *   told apart when they come back: the first pad of that model plugged in after the pull counts as the one that came back, never one that was
 *   already plugged in then. Which of the two is whose holds only until the program ends: in the next run the two may be the other way round.
 *
 * IN THE CONFIGURATION FILE (pc_config.h)
 *   key_p1_up=Up             a key, by SDL's name for it. Empty: no key. key_controls, key_pause, key_mute, key_limits, key_shape, key_filter, key_fullscreen,
 *                            key_quit, key_menu for the program's own actions
 *   pad_p1_up=dpup           an input on a pad SDL knows: a button by SDL's name (a, b, x, y, back, guide, start, leftstick, rightstick, leftshoulder,
 *                            rightshoulder, dpup, dpdown, dpleft, dpright, misc1, paddle1 to paddle4, touchpad), or an axis and a side (leftx-, lefty+,
 *                            lefttrigger+ ...), or any of the plain joystick's names below. pad_pause and the like for the program's own actions
 *   joy_p1_up=hat0up         an input on a plain joystick: button3, hat0up / hat0right / hat0down / hat0left, axis2+ / axis2-
 *   controller1=any          player 1's pad: any, none, or SDL's 32 digit id for a kind of pad, then its name (the name is only for reading); controller2
 *                            and controller3 the same for players 2 and 3, and the lines below too
 *   pad1=six                 six or three buttons, as before
 *   stick1=left              left, right, both or off: which analog stick also moves as the D-pad
 *   deadzone1=50             how far, in percent of its travel, a stick or trigger must move to count
 *   The controls screen writes all of these at once, after the player's other lines, when it closes. A file with only some of them, or with the key_ lines of
 *   earlier versions, still works: what it leaves out keeps its default.
 *
 * THE SCREEN
 *   F4, by default, opens it over the paused game and closes it again; Esc closes it too. It has four pages: Player 1, Player 2, Player 3 and Program keys. The arrows
 *   move, Tab changes the page, Enter sets the input under the cursor by "press it now", Delete clears it. A pad moves through it with its D-pad, chooses with
 *   B and leaves with Start; its other buttons light up their actions. Each player's page lists every action with its key and its pad input, and the rows
 *   for the pad, three or six buttons, the stick and the dead zone. It is drawn into a copy of the game's picture, dimmed behind the text, so the window
 *   shows it with the game's shape and size. When it closes the game goes on as before: the screen never changes the game's frames, only how input is read.
 *   A key does one thing: setting it, or a page's reset, takes it from any other action. The Controls action always keeps a key or a pad input, as nothing
 *   else opens the screen: the screen refuses to clear the last one or give its key to another action. Left and Right on the pad row go through the
 *   choices from where the pads stood when the cursor came to the row, so going back undoes a pad taken from the other player on the way.
 */
#ifndef PC_CONTROLS_H
#define PC_CONTROLS_H

#include <stddef.h>
#include <stdint.h>

#include "pc_config.h"
#include "pc_frame.h"

/* A player's actions, in the screen's order. Each is one PC_PAD_* bit (pc_io.h). */
enum { PC_CTL_UP, PC_CTL_DOWN, PC_CTL_LEFT, PC_CTL_RIGHT, PC_CTL_A, PC_CTL_B, PC_CTL_C, PC_CTL_X, PC_CTL_Y, PC_CTL_Z, PC_CTL_START, PC_CTL_MODE, PC_CTL_N };

/* The program's own actions. Each is one PC_ACTION_* bit (pc_window.h, pc_audio.h). */
enum { PC_PORT_CONTROLS, PC_PORT_PAUSE, PC_PORT_MUTE, PC_PORT_LIMITS, PC_PORT_SHAPE, PC_PORT_FILTER, PC_PORT_FULLSCREEN, PC_PORT_QUIT, PC_PORT_MENU, PC_PORT_N };

#define PC_PLAYERS 3

/* One input on a pad. */
enum {
    PC_IN_NONE,
    PC_IN_BUTTON,       /* a game controller's button: index is SDL_GameControllerButton */
    PC_IN_AXIS,         /* a game controller's axis: index is SDL_GameControllerAxis, dir +1 or -1 */
    PC_IN_JOY_BUTTON,   /* a joystick's button by number */
    PC_IN_JOY_HAT,      /* a joystick's hat by number: dir is SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN or SDL_HAT_LEFT */
    PC_IN_JOY_AXIS      /* a joystick's axis by number, dir +1 or -1 */
};

typedef struct PcPadInput {
    uint8_t kind;
    uint8_t index;
    int8_t dir;
} PcPadInput;

/* Which analog stick also moves as the D-pad: bit 0 the left, bit 1 the right. On a plain joystick the left is axes 0 and 1, the right axes 2 and 3. */
enum { PC_STICK_OFF, PC_STICK_LEFT, PC_STICK_RIGHT, PC_STICK_BOTH };

#define PC_CONTROLLER_ANY "any"
#define PC_CONTROLLER_NONE "none"

typedef struct PcPlayerControls {
    int key[PC_CTL_N];              /* SDL scancode, 0 for none */
    PcPadInput pad[PC_CTL_N];       /* on a pad SDL knows */
    PcPadInput joy[PC_CTL_N];       /* on a plain joystick */
    char controller[PC_CONFIG_VALUE];
    int three_button;
    int stick;
    int deadzone;                   /* percent, 5 to 95 */
} PcPlayerControls;

typedef struct PcControls {
    PcPlayerControls player[PC_PLAYERS];
    int port_key[PC_PORT_N];
    PcPadInput port_pad[PC_PORT_N];
} PcControls;

/* ---- the model, in pc_controls.c */

void pc_controls_defaults(PcControls *c);
/* The defaults, then what the configuration says. Returns 0, or -1 with the reason in error for a name it does not know. */
int pc_controls_from_config(PcControls *c, const PcConfig *config, char *error, size_t error_size);
/* Every control as the configuration's lines, with comments, in text (LF line ends). Returns the length, or -1 when size is too small. */
int pc_controls_lines(const PcControls *c, char *text, size_t size);

/* Names: the configuration's (pc_pad_input_text), and the screen's (pc_pad_input_label, which knows the pad's kind: SDL_GameControllerType, or -1 for a
   plain joystick). */
void pc_pad_input_text(PcPadInput in, char *text, size_t size);
int pc_pad_input_parse(const char *text, PcPadInput *in);      /* 0, or -1 for a name it does not know. An empty text is PC_IN_NONE */
void pc_pad_input_label(PcPadInput in, int pad_type, char *text, size_t size);
int pc_pad_input_equal(PcPadInput a, PcPadInput b);
const char *pc_controls_action_name(int action);      /* "up", "a", "start" ... as in key_p1_up */
const char *pc_controls_port_name(int action);        /* "controls", "pause" ... as in key_pause */
unsigned pc_controls_pad_bit(int action);
unsigned pc_controls_port_bit(int action);

/* ---- the screen, in pc_controls.c. pc_input.c feeds it while it is open (pc_input_poll), and pc_main.c shows its picture. */

int pc_controls_screen_open(void);
int pc_controls_screen_waiting(void);       /* it waits for a key or a pad's press now */
/* Opens it over game, the picture the window showed last: it keeps a copy, dimmed, behind the text. */
void pc_controls_screen_show(const PcPicture *game);
/* The picture to present while it is open. */
const PcPicture *pc_controls_screen_picture(void);
/* Where the controls are saved when the screen closes: the configuration file the program read, or would have read. */
void pc_controls_set_file(const char *path);
/* Saves now if anything changed since the last save. Returns 0, or -1 with the reason in error, in a few words that fit the screen's one line: the
   settings file's name, not its whole path. */
int pc_controls_save(char *error, size_t error_size);
/* After a save of the controls failed, and until one succeeds: the reason in a few words ("cannot be replaced"), which follow the settings file's name
   when *named is 1. NULL while every change of the screen is saved. The changes then hold until a Soft Reset or the end of the program, which start
   from the settings file as it is. named may be NULL. */
const char *pc_controls_unsaved(int *named);
/* A test's clock in milliseconds in place of SDL's, for the 5 seconds a "press it now" waits. 0 goes back to SDL's. */
void pc_controls_test_clock(uint64_t ms);

/* What pc_input.c gives the screen while it is open (pc_input_poll): a key pressed, a pad's input newly pressed (on a pad SDL knows, by its game
   controller name when the input is one of the layout's, else by number), the program's controls key pressed on a pad, and one pass.
   pc_controls_screen_key returns 1 when the key asked the screen to close. pc_controls_screen_tick returns PC_ACTION_CONTROLS_CLOSED on the pass the
   screen closes. */
int pc_controls_screen_key(int scancode, int repeat);
void pc_controls_screen_pad(int slot, PcPadInput in);
void pc_controls_screen_ask_close(void);
unsigned pc_controls_screen_tick(void);

/* ---- pc_input.c's part, for the screen and the program */

/* Beside pc_window.h's and pc_audio.h's PC_ACTION_ bits: the key that opens the controls screen, and the screen closing. */
#define PC_ACTION_CONTROLS 64
#define PC_ACTION_CONTROLS_CLOSED 128

typedef struct PcPadDevice {
    char name[64];
    char guid[33];          /* SDL's id for the kind of pad, 32 hex digits */
    int type;               /* SDL_GameControllerType, or -1 for a plain joystick */
} PcPadDevice;

/* SDL's hints the controls need, before SDL starts (pc_window_open): the face buttons by place on every pad. */
void pc_input_before_sdl(void);
/* The database of pads SDL lacks, by default gamecontrollerdb.txt beside the program. A test names another before pc_input_open; NULL for the default. */
void pc_input_set_controller_db(const char *path);
/* What the last pc_input_open did with the database, for the console. */
const char *pc_input_controller_db_report(void);
/* Adds the database's mappings for this system whose pads SDL has no mapping for. Returns how many it added, or -1 when the file cannot be read. */
int pc_input_load_controller_db(const char *path, int *already_known);

PcControls *pc_input_controls(void);
void pc_input_controls_changed(void);           /* after the screen changed which pad a player wants */
/* Whether the game hears player 3: the ports hold the stick with its EXT socket or the EA 4-Way Play (pc_io.h). It decides who comes first for pads
   (WHICH PAD IS WHOSE above); the program sets it from what the ports hold before pc_input_open. */
void pc_input_set_player3_heard(int heard);
int pc_input_device_slots(int *slots, int max); /* the pads open now, in the order they came */
const PcPadDevice *pc_input_device(int slot);   /* NULL for a slot with no pad */
int pc_input_player_device(int player);         /* the slot of that player's pad, or -1 */
void pc_input_choose(int player, int slot);     /* that pad for that player, swapping with the player who had it if need be; -1 any, -2 none */
/* Whether a player is plugged in now: players 1 and 2 always; player 3 while they have a pad or a key on the keyboard. */
int pc_input_player_present(int player);
/* Which pad each player has now, as slots (-1 for none), and back again: the screen's pad row starts each step from where the pads stood. A slot whose
   pad has gone counts as none. pc_input_devices_changed counts each pad opened or closed, so a saved assignment is known to be out of date. */
void pc_input_assignment(int slots[PC_PLAYERS]);
void pc_input_set_assignment(const int slots[PC_PLAYERS]);
unsigned pc_input_devices_changed(void);
unsigned pc_input_player_bits(int player, int with_keys);  /* the PC_PAD_* bits held now: on the player's pad, and on the keyboard too if asked */
const char *pc_input_key_name(int scancode);    /* SDL's name for a key, "" for none */
int pc_input_three_button(int player);

#endif /* PC_CONTROLS_H */
