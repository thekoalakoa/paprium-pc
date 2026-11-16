/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stands in for pc_window.c, pc_input.c and pc_controls.c in a program built without SDL, as the machine's own tests build it. Only headless runs work then.
 * The menu bar's own file, pc_menu.c, needs no SDL and is built with it.
 */
#include <stdio.h>

#include "pc_audio.h"
#include "pc_controls.h"
#include "pc_filter.h"
#include "pc_window.h"

int pc_window_available(void) { return 0; }
int pc_window_open(const PcWindowOptions *options, char *error, size_t error_size)
{
    (void)options;
    snprintf(error, error_size, "this program was built without SDL, so it has no window");
    return -1;
}
void pc_window_close(void) {}
void pc_window_present(const PcPicture *picture) { (void)picture; }
void pc_window_redraw(void) {}
void pc_window_set_title(const char *text) { (void)text; }
void pc_window_toggle_fullscreen(void) {}
void pc_window_toggle_shape(void) {}
int pc_window_is_square(void) { return 0; }
void pc_window_set_shape(int shape) { (void)shape; }
int pc_window_shape(void) { return 0; }
int pc_window_vsync_dropped(void) { return 0; }
int pc_window_vsync_on(void) { return 0; }
const char *pc_window_renderer(void) { return "none"; }
int pc_window_fallbacks(void) { return 0; }
void pc_window_open_times(double *sdl_ms, double *renderer_ms) { *sdl_ms = *renderer_ms = 0; }
const char *pc_window_fallback(int which) { (void)which; return ""; }
int pc_window_set_vsync(int on) { (void)on; return 0; }
void pc_window_describe(char *text, size_t size) { snprintf(text, size, "no window"); }
void pc_window_displays(char *text, size_t size) { snprintf(text, size, "no window"); }
int pc_window_refresh_hz(void) { return 0; }
void pc_window_gpu(char *text, size_t size) { snprintf(text, size, "no window"); }
void *pc_window_native(void) { return NULL; }
int pc_window_read_screen(uint32_t *out, int capacity_pixels, int *w, int *h) { (void)out; (void)capacity_pixels; (void)w; (void)h; return -1; }
int pc_window_read_large(uint32_t *out, int capacity_pixels, int *w, int *h) { (void)out; (void)capacity_pixels; (void)w; (void)h; return -1; }
void pc_window_message(const char *text) { (void)text; }
int pc_window_set_filter(const char *value, const char *librashader_dll, char *note, size_t note_size)
{
    (void)value;
    (void)librashader_dll;
    if (note_size)
        note[0] = 0;
    return -1;
}
int pc_window_next_filter(const char *key, char *note, size_t note_size) { (void)key; if (note_size) note[0] = 0; return 0; }
int pc_window_filter(void) { return 0; }
const char *pc_window_filter_label(void) { return "off"; }
int pc_window_take_note(char *note, size_t note_size) { (void)note; (void)note_size; return 0; }
void pc_window_show_text(const char *text, int ms) { (void)text; (void)ms; }
const char *pc_window_shown_text(void) { return ""; }
int pc_window_draw_offscreen(const PcPicture *picture, int w, int h, int frames, int every_ms, uint32_t *out, int capacity_pixels, double *gpu_ms_mean, double *gpu_ms_worst,
                             double *gpu_ms_each, double *cpu_ms_mean)
{
    (void)picture; (void)w; (void)h; (void)frames; (void)every_ms; (void)out; (void)capacity_pixels; (void)gpu_ms_mean; (void)gpu_ms_worst; (void)gpu_ms_each; (void)cpu_ms_mean;
    return -1;
}

int pc_input_open(const PcConfig *config, char *error, size_t error_size) { (void)config; (void)error; (void)error_size; return 0; }
void pc_input_close(void) {}
unsigned pc_input_poll(uint32_t pad[3]) { pad[0] = pad[1] = pad[2] = 0; return 0; }
int pc_input_push_key(const char *name, int down) { (void)name; (void)down; return -1; }
int pc_input_push_mouse(int x, int y, int press) { (void)x; (void)y; (void)press; return -1; }
int pc_input_take_menu_command(int *arg) { *arg = 0; return 0; }
int pc_window_is_fullscreen(void) { return 0; }
/* The menu bar's Video menu names the looks; with no window there are none to draw. */
const char *pc_filter_title(int look) { (void)look; return "off"; }
int pc_filter_needs_gl(int look) { (void)look; return 0; }
void pc_window_set_menu_bar(int on) { (void)on; }
int pc_window_menu_bar(void) { return 0; }
void pc_window_point(int window_x, int window_y, int *x, int *y) { *x = window_x; *y = window_y; }
int pc_input_controllers(void) { return 0; }
void pc_input_print_keys(FILE *to) { (void)to; }
void pc_input_before_sdl(void) {}
void pc_input_set_controller_db(const char *path) { (void)path; }
int pc_input_three_button(int player) { (void)player; return 0; }
int pc_input_player_present(int player) { return player >= 0 && player < 2; }
void pc_input_set_player3_heard(int heard) { (void)heard; }
PcControls *pc_input_controls(void) { static PcControls none; return &none; }
const char *pc_input_key_name(int scancode) { (void)scancode; return ""; }
int pc_controls_screen_open(void) { return 0; }
void pc_controls_screen_show(const PcPicture *game) { (void)game; }
const PcPicture *pc_controls_screen_picture(void) { return NULL; }
void pc_controls_set_file(const char *path) { (void)path; }
int pc_controls_save(char *error, size_t error_size) { (void)error; (void)error_size; return 0; }
const char *pc_controls_unsaved(int *named) { if (named) *named = 0; return NULL; }

int pc_audio_open(const char *device, int volume, char *error, size_t error_size)
{
    (void)device;
    (void)volume;
    snprintf(error, error_size, "this program was built without SDL, so it has no audio device");
    return -1;
}
void pc_audio_close(void) {}
void pc_audio_push(const int16_t *pairs, uint32_t n) { (void)pairs; (void)n; }
void pc_audio_toggle_mute(void) {}
int pc_audio_muted(void) { return 0; }
void pc_audio_set_volume(int volume) { (void)volume; }
int pc_audio_volume(void) { return 0; }
void pc_audio_report(void) {}
void pc_audio_describe(char *text, size_t size) { if (size) text[0] = 0; }
void pc_audio_totals(char *text, size_t size) { if (size) text[0] = 0; }
void pc_audio_end_of_run(void) {}
void pc_audio_frame(uint32_t frame) { (void)frame; }
void pc_audio_paused(int paused) { (void)paused; }
void pc_audio_pace(uint32_t behind_pairs) { (void)behind_pairs; }
int pc_audio_take_line(uint64_t *when, char *text, size_t size) { (void)when; (void)text; (void)size; return 0; }
