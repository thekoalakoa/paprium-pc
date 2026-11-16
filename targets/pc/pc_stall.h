/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The freeze log. A player can see the picture stop for a second or two, on some runs and not on others. This keeps a plain text record of each such moment, always, so the next one can be read
 * afterwards: stalls.txt, beside the program.
 *
 * WHAT IT NOTES. The run loop is cut into parts: the game's work, the firmware's turns inside that work, the renderer, the pacing's wait, switching vsync off, the present, the event poll,
 * the save. A pass of the loop that takes longer than the limit, 100 ms unless a test says otherwise, gets one line: how long, in which part, how the time was shared out, the frame, the
 * time of day, whether vsync is on and whether the window is in front. While a pass is stuck, a second thread looks at the main thread and writes which modules are on its stack, at 100 ms,
 * then at each three times that, so a run that never comes back still leaves its evidence in the file. A vsync switch, off or back on, always gets a line of its own with its reason. So do the
 * sound's start and each time the sound runs dry or skips, with the time of day and the frame. Each run's block starts with a line that names the renderer in use, then the monitor's refresh rate,
 * the renderer backend, the GPU's name, the audio device with its rate and how long each part of the start-up took, and ends with the sound's totals and a line of totals. A block with
 * no closing line is a run that was killed or crashed.
 *
 * At the run's end the program closes the sound device after the game's last frame, which took about 300 ms on one PC. The game has stopped by then, so it is no stall:
 * a look at the main thread during the close says "closing the sound device" instead of "stuck", and a close longer than the limit gets one line saying how long it took.
 * After the close the program asks the log to stop, and the main thread then only waits for the second thread to write its last lines; that is no stall either, so no look
 * is taken once the stop has been asked.
 *
 * WHAT IT MUST NEVER DO. Touch the game: nothing here reads or writes the machine, and no wall clock from here reaches anything that does, so the logs of a run are byte-equal with it on and
 * off. Get in the game's way: the main thread only reads the PC's timer and fills a small table, and every file operation is on the second thread, so a slow or full disk stalls only that
 * thread. Make a noise: a failed write is ignored, with no message. Grow without limit: at most PC_STALL_MAX_LINES lines a run, PC_STALL_SOUND_LINES about the sound on an allowance of
 * their own, so the sound can never use up the room the freeze lines need, and PC_STALL_DELAY_LINES about the sound's delay on a third, so they never use up the room of the sound's
 * gaps. Once an allowance is used up, one line says so, with the time and the frame, and the rest are only counted, in the run's last line. It writes numbers and the names
 * of the program's modules, never a byte of the game.
 */
#ifndef PC_STALL_H
#define PC_STALL_H

#include <stddef.h>
#include <stdint.h>

/* The parts of a pass of the run loop. The firmware's turns are not on this list: they run inside the work, and pc_stall_start takes a flag that says when one is running. */
enum {
    PC_STALL_WORK,          /* the 68000's program and the machine */
    PC_STALL_RENDER,
    PC_STALL_PACE,          /* waiting for the game's time */
    PC_STALL_VSYNC_SWITCH,  /* the call that turns vsync off or back on, which on Direct3D 9 resets the device */
    PC_STALL_PRESENT,       /* drawing into the window and showing it */
    PC_STALL_EVENTS,        /* keys, pads and the window's messages */
    PC_STALL_SAVE,
    PC_STALL_PARTS
};

#define PC_STALL_LIMIT_MS 100
#define PC_STALL_MAX_LINES 300       /* the block's top lines, its gaps, stacks and notes, and its last lines */
#define PC_STALL_SOUND_LINES 100     /* the sound's own lines, on an allowance apart from those */
#define PC_STALL_DELAY_LINES 50      /* the lines about the sound's delay, on an allowance apart from both */

/* A line for the top of this run's block, before pc_stall_start. Up to 12 are kept. */
void pc_stall_header(const char *line);

/* Words for the block's very first line, before pc_stall_start: the renderer in use. */
void pc_stall_first_line(const char *text);

/* A line for the end of the block, just before its closing line, set before pc_stall_stop: the sound's totals. It is kept whole up to PC_STALL_FOOTER_CHARS - 1 characters. The
   sound's totals line is 380 characters with one-digit numbers and 588 with every number at the largest its type holds, so it is never cut. */
#define PC_STALL_FOOTER_CHARS 640
void pc_stall_footer(const char *line);

/* The run's end, on the main thread: closing is 1 just before the program closes the sound device and 0 once it is closed. See the top of this file. */
void pc_stall_closing_sound(int closing);

/* Lines that another thread notes in its own table, such as the sound's start and each of its gaps, before pc_stall_start. The second thread calls take at each of its passes, and once more at the
   end: take fills text and when, the reading of this file's timer, QueryPerformanceCounter, that the line is about, and returns 1, or 2 for a line about the sound's delay, or 0 when it has
   no more. Each line gets that time of day. */
void pc_stall_line_source(int (*take)(uint64_t *when, char *text, size_t size));

/* What the GPU's adapters are called, from the system's list of display adapters, joined into one line of text. Reads nothing of the renderer, so it works whatever the backend is. */
void pc_stall_adapters(char *text, size_t size);

/* Starts the watcher on the calling thread, which must be the one that runs the frame loop. path: where the text goes. limit_ms: 0 means PC_STALL_LIMIT_MS. in_firmware: reads not 0 while
   a firmware turn runs, or NULL. window: the window's system handle, to say whether it is in front, or NULL. frame: the frame about to run. */
void pc_stall_start(const char *path, int limit_ms, const volatile uint8_t *in_firmware, void *window, uint32_t frame);

/* Called on the main thread, as the loop passes from one part to the next. It costs one read of the timer. */
void pc_stall_part(int part);

/* The end of one pass. next_frame: the frame the next pass is for. firmware_ticks: the host time the firmware's turns have taken so far in all, on the same timer. */
void pc_stall_pass_end(uint32_t next_frame, uint64_t firmware_ticks);

/* What the lines say about the run from here on. vsync: 1 on, 0 off, -1 no window at all. */
void pc_stall_state(int vsync, int paused);

/* A line of its own, whatever the time. On the main thread. The text is kept whole up to PC_STALL_NOTE_CHARS - 1 characters: the longest note, the
   reason a vsync switch gives, runs to 476 at most, for a switch that took up to 99,999 ms. */
#define PC_STALL_NOTE_CHARS 500
void pc_stall_note(const char *text);

/* Writes the totals and stops the second thread. A write that hangs holds this up for a second at most. */
void pc_stall_stop(void);

/* A test: on each frame from first to last, as the loop enters this part, the main thread sleeps ms, once a frame. name: the part's word in the log. Up to 256 of these. Returns the part, or -1 for
   a word it does not know or a full table. */
int pc_stall_test_plant(int first, int last, const char *name, int ms);

#endif /* PC_STALL_H */
