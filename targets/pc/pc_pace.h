/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The run loop's pace, and when it turns vsync off and back on. The game runs at its own 59.92 Hz from the PC's timer, and its speed must never follow the monitor. With vsync on, a present
 * waits for the monitor's next refresh, so a monitor slower than the game would hold the game back. Everything here is plain arithmetic on the timer's readings, with no waiting and no system
 * call, so pc_main.c, tests/pc_config_test.c and a model of the loop all run the same code.
 *
 * THE PACE. Each frame has the time it is due. A frame that ends its work early waits for that time; one that ends late goes on at once. Two rules keep a late game from slowing down:
 *   - A frame that is still a whole game frame or more behind once its wait is over is not shown: its picture would be replaced at once, and a present can take up to a whole refresh with vsync
 *     on. So the game catches up by running its frames, not by showing them, and its sound keeps coming at the game's own rate. On a monitor a little slower than the game this drops one
 *     picture now and then, about once every 17 seconds at 59.865 Hz, where the game itself would otherwise run that much slow and its sound would run dry. In a slow stretch of presents, a
 *     GPU or a monitor that cannot keep up, it shows fewer pictures while the game keeps its speed. A frame that is on time, or less than a frame late, is always shown.
 *   - A game more than PC_PACE_FRAMES_BEHIND frames behind, after a stall, drops that time, so a stall never turns into fast motion. It makes up only the last PC_PACE_MAKE_UP frames of
 *     it, at once: those frames are behind, so by the first rule they are run without being shown, and only the last of them is shown. That fills the sound's queue (pc_audio.c) in the time
 *     the frames' own work takes, a few milliseconds, whatever the monitor's refresh is.
 *
 * THE VSYNC RULE: WHAT COUNTS. Once a frame the loop says how long the frame took, from the end of one pace wait to the end of the next, how long the last present took, whether the last
 * frame was shown, and whether the game was already behind its pace when the frame began; and once in a while what the monitor's refresh is. The question is whether the game, once it is
 * behind, catches up through the frames it shows. A frame that began behind and took longer than a game frame lost time against the pace, and the part of that a wait for the monitor can
 * explain is counted as lost to it: no more than the present took, and no more than one refresh, the longest a present ever waits for the monitor. A present that takes longer than that is
 * the GPU or its driver at work, which turning vsync off would not cure. A frame that began behind and took less than a game frame gave time back, and that counts against the loss. Three
 * kinds of frame never count:
 *   - A frame that began on time. Whatever made it late, a slow present, a busy GPU or a long frame of work, would make it late on any monitor, so on its own it says nothing about this one.
 *     What a monitor slower than the game does shows in the frames after it: each of them waits for the monitor too, and the game never catches up. On a fast monitor the frames after a slow
 *     present show quickly, and the time they give back is counted. So bursts of slow presents, even several a second on a 240 Hz monitor, add up to time given back, not lost: a slow
 *     present that comes while the game is still behind from the last counts one refresh at most. The price, in a model of the loop: in a few dense bursts vsync stays on where an
 *     older rule turned it off, and the sound ran dry once in the burst: a present 40 ms longer in every 10 or 15 frames on a 75 Hz monitor, or 30 ms longer in every 3 on a 360 Hz one.
 *   - A hitch: one frame of PC_VSYNC_HITCH_FRAMES game frames or more. A GPU waking up, a window being moved, a slow disk: these stop the game whether vsync is on or off, and the pace drops that
 *     time anyway, so they say nothing about the monitor.
 *   - A frame after one that was not shown. Its time is short only because a present was left out, which is the pace's doing and not the monitor's, so it neither loses nor gives back time.
 *     Without this, a monitor much slower than the game would look like one that keeps up, its losses paid back by the frames left unshown, and vsync would never go off there.
 * A monitor no faster than the game, 60 Hz against the game's 59.92, lets a frame that fell behind catch up only by about a fiftieth of a millisecond a frame, as each present waits a whole
 * refresh. There a slow present or a frame of slow work that comes while the game is still behind from an earlier one makes the present after it wait for a refresh it would not have
 * waited for, and that wait counts. Several of those a second turn vsync off, which lets the game catch up at once: the line in stalls.txt gives the longest present and the longest frame
 * without its present in that second, so the presents can be told from the work.
 *
 * VSYNC GOES OFF after PC_VSYNC_SLOW_QUARTERS quarter seconds in a row (one second) that each lost more than a thousandth of their time this way, and steadily: none of them less than a quarter of
 * their mean. A monitor slower than the game loses about the same time in every quarter second. And a monitor whose refresh does not divide the game's frames evenly makes the frames drift late
 * and back, a few milliseconds either way, so a quarter second can lose a little now and then for that alone. Three presents of 19 ms on a 240 Hz monitor left a player's run 37 ms behind,
 * inside one quarter second, and an older rule turned vsync off for the rest of that run there. This one does not. A slow stretch where every present is slow for more than a second does turn it
 * off, as the frames after the first are behind and each is slow too, but not for long, and by the pace's first rule the game keeps its speed and its sound through such a stretch either way:
 *
 * VSYNC COMES BACK ON after PC_VSYNC_CALM_FRAMES frames in a row (10 seconds) with no hitch, no present longer than a game frame and no frame of two game frames or more within a
 * second of another. That last is slow frames that recur: vsync coming back on among them, on a 60 Hz monitor, where the frames after a slow one catch up only slowly, left the game
 * behind long enough to run its sound dry in a model of the loop. A single such frame, as a busy PC makes now and then, does not count: counting each one kept vsync off to the end
 * of hidden runs on a loaded PC. If vsync has to go off again less than a minute after coming back on, the next wait is twice as long, up to about 5 minutes; once it has held for a
 * minute, the wait is 10 seconds again. So on a monitor that keeps the game's rate, slow stretches never end the tries, however many a run has. But each wait has to pass with no
 * hitch, no present longer than a game frame and no recurring slow frames, so while those come more often than the wait, vsync stays off: in a hidden test run on one PC with slow
 * presents planted every 47 seconds, it stayed off, still trying, for the run's last 21 minutes. With vsync off, the frames of a heavy scene or of a slow stretch of presents can all
 * end in time, so vsync may come back on while one is still going, and go off again on the first second's evidence: that too only doubles the wait, and vsync comes back on once the
 * scene or the stretch is over and a whole wait then passes calm. Two things end the tries for the rest of the run, each said in stalls.txt:
 *   - a switch, either way, that the loop found slow in the renderer: more than VSYNC_SWITCH_SLOW_MS in pc_main.c. Each later return would risk another such stop of the game. On one PC
 *     Direct3D 9 slowed down whenever vsync was on, and after each return the switch off took 0.7 to 2.1 s.
 *   - vsync goes off again on the first second's evidence after it came back on, on a monitor whose refresh is clearly slower than the game's: 58 Hz or less, as the system gives it. The
 *     system gives whole hertz, rounded down, so a monitor it calls 59 Hz may run at 59.94 Hz and keep the game's 59.92; that one, and one whose refresh is not known, keep trying.
 *
 * WHAT THIS COSTS on a monitor that cannot keep the game's rate: one second of frames with some pictures left out before vsync goes off, and the same again when vsync comes back on once to
 * try, 10 seconds later; a monitor clearly slower than the game fails that try on its first second, and vsync stays off from then on, and the picture can tear. In a model of the loop that
 * held at 24 to 55 Hz. At 57 to 58.9 Hz a try could also fail later than its first second, which only doubles the wait, so vsync went off up to five times before the tries ended; and with
 * 8 ms of work in each frame, 58 to 58.9 Hz never turned vsync off at all: the pace left out a picture whenever the game fell a frame behind, about twice a second at 58 Hz. A monitor whose
 * refresh is only a little slower than the game's, 59 to 59.85 Hz, takes longer to show it, up to about 15 seconds after vsync comes back on, as its refresh drifts against the game's
 * frames: there the tries go on, after 20, 40, 80, 160 and then every 320 seconds. The game keeps its speed and its sound all through. A monitor less than a thousandth slower than the
 * game, 59.86 Hz or more, keeps vsync, and the pace leaves out a picture whenever the game has fallen a whole frame behind.
 */
#ifndef PC_PACE_H
#define PC_PACE_H

#include <stdint.h>
#include <string.h>

/* ---- the pace */
#define PC_PACE_FRAMES_BEHIND   4       /* further behind than this, a stall's time is dropped */
#define PC_PACE_MAKE_UP         3       /* frames of a dropped stall that are made up at once */

typedef struct PcPace {
    uint64_t period;        /* a game frame, in the timer's units */
    uint64_t next;          /* when the next frame is due */
    uint64_t late, drops, unshown;      /* frames that ended late, stalls dropped, frames not shown: for the console */
    uint64_t behind;        /* at the last pace point: how far past its due time the frame ended, 0 when it was on time */
} PcPace;

/* At the start of a run, and again after a pause: the next frame is due one frame from now. */
static inline void pc_pace_start(PcPace *p, uint64_t now, uint64_t period)
{
    p->period = period;
    p->next = now + period;
}

/* At the end of a frame's work, at now: returns when the frame's wait ends, which is now itself for a frame that is late. The caller waits until then. */
static inline uint64_t pc_pace_point(PcPace *p, uint64_t now)
{
    uint64_t point = now > p->next ? now : p->next;
    if (now > p->next)
        p->late++;
    if (now > p->next + (uint64_t)PC_PACE_FRAMES_BEHIND * p->period) {
        /* The frames still to make up are due at once, so the next is due PC_PACE_MAKE_UP - 1 frames ago, and this one counts as PC_PACE_MAKE_UP frames behind. */
        p->next = now + p->period - (uint64_t)PC_PACE_MAKE_UP * p->period;
        p->behind = (uint64_t)PC_PACE_MAKE_UP * p->period;
        p->drops++;
        return now;
    }
    p->behind = point - p->next;
    p->next += p->period;
    return point;
}

/* After the wait, at now: whether this frame is shown. Not when the next frame is already due. */
static inline int pc_pace_show(PcPace *p, uint64_t now)
{
    if (now >= p->next) {
        p->unshown++;
        return 0;
    }
    return 1;
}

/* ---- the vsync rule */
#define PC_VSYNC_HITCH_FRAMES   4       /* a frame this long or longer is a hitch */
#define PC_VSYNC_QUARTER_FRAMES 15      /* a quarter second of frames */
#define PC_VSYNC_SLOW_QUARTERS  4       /* slow quarter seconds in a row that turn vsync off */
#define PC_VSYNC_CALM_FRAMES    600     /* calm frames in a row before vsync comes back on, the first time */
#define PC_VSYNC_CALM_MOST      19200   /* the longest that wait grows to: 32 times the first, about 5 minutes */
#define PC_VSYNC_SOON_FRAMES    3600    /* off again this soon after coming back on: the next wait is twice as long */

enum { PC_VSYNC_KEEP, PC_VSYNC_TURN_OFF, PC_VSYNC_TURN_ON };

typedef struct PcVsyncWatch {
    int64_t loss;           /* this quarter second's time lost to presents, less the time made up */
    int     frames;         /* frames so far in this quarter second */
    int     slow;           /* slow quarter seconds in a row */
    int64_t lost[PC_VSYNC_SLOW_QUARTERS];   /* what the last few quarter seconds lost, newest at slow - 1 */
    int64_t present_most, rest_most;        /* this quarter second's longest counted present, and its longest counted frame without its present */
    int64_t presents[PC_VSYNC_SLOW_QUARTERS], rests[PC_VSYNC_SLOW_QUARTERS];   /* the same for the last few quarter seconds, beside lost */
    int     calm;           /* with vsync off: frames in a row with no hitch and no long present */
    int     calm_needed;    /* how many it takes now */
    int     since_on;       /* frames since vsync last came back on, counted up to PC_VSYNC_SOON_FRAMES, or -1 when it has not */
    int     offs, ons;      /* how many times it went off and came back on */
    int     soon;           /* at the last switch off: it had come back on less than PC_VSYNC_SOON_FRAMES before */
    int     on_for;         /* and how many frames it had been on then, when soon */
    int     at_once;        /* at the last switch off: it had come back on, and went off again on the first second's evidence */
    int     slow_monitor;   /* and the monitor's refresh was clearly slower than the game's then */
    int     no_returns;     /* the tries have ended for this run: vsync stays as it is once it is off */
    int     since_slow;     /* with vsync off: frames since the last of two game frames or more, or -1 when none has come */
    int64_t why_loss;       /* at the last switch off: what the slow second lost */
    int64_t why_present, why_rest;  /* and in that second, the longest present and the longest frame without its present that counted */
    int64_t refresh;        /* the monitor's refresh period, or 0 when it is not known */
} PcVsyncWatch;

/* At the start of a run. */
static inline void pc_vsync_watch_start(PcVsyncWatch *w)
{
    memset(w, 0, sizeof(*w));
    w->calm_needed = PC_VSYNC_CALM_FRAMES;
    w->since_on = -1;
    w->since_slow = -1;
}

/* The monitor's refresh period, in the unit the watch is given its times in, or 0 when it is not known: from the display's mode, and again when the window may have moved. */
static inline void pc_vsync_watch_refresh(PcVsyncWatch *w, int64_t refresh)
{
    w->refresh = refresh > 0 ? refresh : 0;
}

/* The loop found a switch slow in the renderer: no more returns in this run. */
static inline void pc_vsync_watch_no_returns(PcVsyncWatch *w)
{
    w->no_returns = 1;
}

/* After a pause or a change to or from full screen: the frames around those say nothing about the monitor, so the evidence starts again. What happened before is kept. */
static inline void pc_vsync_watch_again(PcVsyncWatch *w)
{
    w->loss = w->present_most = w->rest_most = 0;
    w->frames = w->slow = w->calm = 0;
}

/* Called once a game frame, with vsync as it is now. All times in one unit:
 *   took        the frame's time, from the end of one pace wait to the end of this one
 *   present     how long the last present took, 0 when the last frame was not shown
 *   frame       the length of a game frame
 *   last_shown  the last frame was shown
 *   began_behind  the game was behind its pace when this frame's time began: the last pace point was past its due time (PcPace's behind was not 0)
 * Returns PC_VSYNC_TURN_OFF, PC_VSYNC_TURN_ON or PC_VSYNC_KEEP. After a switch the evidence starts again by itself. */
static inline int pc_vsync_watch(PcVsyncWatch *w, int vsync_on, int64_t took, int64_t present, int64_t frame, int last_shown, int began_behind)
{
    const int hitch = took >= PC_VSYNC_HITCH_FRAMES * frame;
    if (!vsync_on) {
        /* A frame of two game frames or more, less than a second after the last: slow frames that recur. */
        const int again = took >= 2 * frame && w->since_slow >= 0 && w->since_slow < PC_VSYNC_SLOW_QUARTERS * PC_VSYNC_QUARTER_FRAMES;
        if (took >= 2 * frame)
            w->since_slow = 0;
        else if (w->since_slow >= 0 && w->since_slow < PC_VSYNC_SLOW_QUARTERS * PC_VSYNC_QUARTER_FRAMES)
            w->since_slow++;
        if (w->no_returns)
            return PC_VSYNC_KEEP;
        if (hitch || present > frame || again) {
            w->calm = 0;
            return PC_VSYNC_KEEP;
        }
        if (++w->calm < w->calm_needed)
            return PC_VSYNC_KEEP;
        pc_vsync_watch_again(w);
        w->since_on = 0;
        w->ons++;
        return PC_VSYNC_TURN_ON;
    }
    if (w->since_on >= 0 && w->since_on < PC_VSYNC_SOON_FRAMES)
        w->since_on++;
    if (!hitch && last_shown && began_behind) {
        /* The part a wait for the monitor can explain: no more than the present took, and no more than one refresh, the longest a present waits for the monitor. */
        int64_t late = took - frame, wait = w->refresh > 0 && present > w->refresh ? w->refresh : present;
        w->loss += late > 0 && late > wait ? wait : late;
        if (present > w->present_most)
            w->present_most = present;
        if (took - present > w->rest_most)
            w->rest_most = took - present;
    }
    if (++w->frames < PC_VSYNC_QUARTER_FRAMES)
        return PC_VSYNC_KEEP;
    /* A quarter second is over. Slow: it lost more than a thousandth of its time. */
    if (w->loss * 1000 > (int64_t)PC_VSYNC_QUARTER_FRAMES * frame) {
        w->presents[w->slow % PC_VSYNC_SLOW_QUARTERS] = w->present_most;
        w->rests[w->slow % PC_VSYNC_SLOW_QUARTERS] = w->rest_most;
        w->lost[w->slow++ % PC_VSYNC_SLOW_QUARTERS] = w->loss;
    } else {
        w->slow = 0;
    }
    w->loss = w->present_most = w->rest_most = 0;
    w->frames = 0;
    if (w->slow < PC_VSYNC_SLOW_QUARTERS)
        return PC_VSYNC_KEEP;
    {
        /* Steady: no quarter lost less than a quarter of their mean. A slow stretch that fills three quarters, next to one where the frames only drift against the monitor's refresh,
           is not that. */
        int64_t sum = 0, least = w->lost[0], present_most = 0, rest_most = 0;
        int i;
        for (i = 0; i < PC_VSYNC_SLOW_QUARTERS; i++) {
            sum += w->lost[i];
            if (w->lost[i] < least)
                least = w->lost[i];
            if (w->presents[i] > present_most)
                present_most = w->presents[i];
            if (w->rests[i] > rest_most)
                rest_most = w->rests[i];
        }
        if (least * 4 * PC_VSYNC_SLOW_QUARTERS < sum)
            return PC_VSYNC_KEEP;
        w->why_loss = sum;
        w->why_present = present_most;
        w->why_rest = rest_most;
    }
    /* Off again soon after coming back on: the next wait is twice as long. Held for a minute: it is the first wait again. */
    w->soon = w->since_on >= 0 && w->since_on < PC_VSYNC_SOON_FRAMES;
    w->on_for = w->since_on;
    /* The slow quarters start again when vsync comes back on, so off at PC_VSYNC_SLOW_QUARTERS quarters after it means every one of them since was slow. On a monitor clearly slower than
       the game, 58 Hz or less (its refresh more than 60/59 of a game frame: 59 Hz as the system gives it can be 59.94), that ends the tries. Anywhere else it is one more off soon after a
       return: a heavy scene or a slow stretch can still be going when vsync comes back on. */
    w->at_once = w->since_on >= 0 && w->since_on <= PC_VSYNC_SLOW_QUARTERS * PC_VSYNC_QUARTER_FRAMES;
    w->slow_monitor = w->refresh > 0 && w->refresh * 59 > frame * 60;
    if (w->at_once && w->slow_monitor)
        w->no_returns = 1;
    if (!w->soon)
        w->calm_needed = PC_VSYNC_CALM_FRAMES;
    else if (w->calm_needed < PC_VSYNC_CALM_MOST)
        w->calm_needed *= 2;
    pc_vsync_watch_again(w);
    w->offs++;
    return PC_VSYNC_TURN_OFF;
}

#endif /* PC_PACE_H */
