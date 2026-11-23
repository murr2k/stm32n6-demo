/*
 * Remote control over TCP, for unattended testing from the host
 * (tools/ctl.py). Line-oriented text commands on tcp/5556:
 *
 *   screen <n>                 switch screen
 *   hud                        toggle the performance HUD
 *   attract                    start attract mode now
 *   stats                      one line of frame statistics (margin_min_us: least time left
 *                              before the swap deadline at a hand-over, since the reset)
 *   swtrace                    the frames of the last screen transition: where each one's time went (gfx.h)
 *   droptrace                  the same for the frames up to the last dropped one
 *   rec                        the recorder (tcp/5559, media/recorder.h): recording, pictures, missed,
 *                              errors, encode time, bytes, the last IDR and P picture sizes
 *   svct                       the main loop before LVGL: telemetry, bar, each screen's service and
 *                              tick, worst/mean us per frame since the reset (shell.h)
 *   mem                        kernel heap, LVGL pool, PSRAM, least stack left (a soak test's leak check)
 *   stacks                     every thread's stack never used, bytes
 *   video                      one line of player status
 *   media                      the clip list and the SD card
 *   clip <n>                   clip n: its file on the card, frames, title
 *   card <0|1>                 list the card's clips or only the flash ones
 *   sdcard [out|in|auto]       the card watcher: slot switch, mounted, gone; act as if taken out or put in (testing)
 *   play <n>                   go to Cinema and open clip n
 *   netcfg [dhcp | static IP MASK [GW]]  the board's address: show, or change now (and store)
 *   carrier <ms>               the Ethernet link as if its cable were out for ms (100 to 30000),
 *                              from 100 ms after the reply (testing: every network service's recovery)
 *   ds                         Explorer's data set: in flash or generated, address, checksum
 *   hometiles <n>              Home: n placeholder tiles after the real ones
 *   crash <1|2|3|4>            a throwaway thread faults on purpose (1 divide by zero, 2 undefined
 *                              instruction, 3 k_oops): zprof's fault capture ends it, the demo runs on;
 *                              4: the UI thread faults and the board halts (n6.cmd fault reads it).
 *                              Only with CONFIG_APP_CRASH_TEST (default y)
 *   faults [ack]               faults zprof's handler caught since boot; "ack" clears the alarm
 *                              (status bar, web remote) for those so far
 *   home                       Home: tiles (and those that open a screen), pan position and range
 *   power                      Home Power's feed: online, state, requests, the last figures
 *   power url [http://HOST[:PORT] | none]  the energy monitor's address: show, or set (kept on the card)
 *   power range <1h|6h|24h|7d|30d>  the panel's chart range
 *   powert                     Home Power: worst gauge draw, chart draw, tick and Home tile draw, us
 *   audio                      DAC half-buffer count, frames per half (block_frames) and underruns
 *   tone <hz> <ms>             test tone at -12 dBFS
 *   volume <0..100>            headphone volume
 *   mic                        microphone: samples, errors, measured rate, level of the last block
 *   mictune [values]           microphone bring-up: restart with other settings (mic.h)
 *   micbits <kbits> <delay_ns> <khz> <pull>  raw bit stream, clocked by hand (mic.h)
 *   micscope <n>               n fast samples of the clock and data pins while the MDF runs
 *   micprobe                   microphone bring-up: is the data line free and unclamped with the clock stopped
 *   micinject <hz> <clock_off> [pull]  capture self-test: a square wave on the data pin made with its pulls (mic.h)
 *   micpdm <hz> [amp_pct mode rate_khz] capture self-test: a synthetic microphone's PDM stream on the data pin (mic.h)
 *   sigsrc <0|1|2>             Signal Lab's source: generator, microphone or theremin
 *   thm [voice hz db]          theremin state; a voice by remote (hz 0 lifts it), voice -1: back to touch
 *   thmwave <0..3>, thmsnap <0|1>, thmghost <0|1>   theremin waveform, semitone snap, the ghost player
 *   thmsound <0|1>             the theremin's SOUND switch (off: it plays to the analysis only)
 *   mon                        the microphone monitor (Signal Lab with MICROPHONE): state, read
 *                              distance, delay, DSP time, limiter, the bands as hz/dB x10/Q x100
 *   monlisten <0|1>, mongain <dB>, moneq <0|1>   the headphones, the gain, the EQ in or bypassed
 *   monband <0..4> <hz> <dB x10> <Q x100>        set a band; monflat: all at 0 dB; monreset: as at start
 *   montone <hz> [dBFS x10]    closed-loop test: the tone to the DAC instead of the microphone (hz 0 ends
 *                              it); "mon" then adds it in the microphone before and after the EQ,
 *                              the gain measured between them and the curve's gain there
 *   mcap [ms]                  record the next ms of microphone samples (3 s at most), like acap
 *   acap [ms]                  record the next ms of DAC output (3 s at most);
 *                              without ms: where it is and how far it has got
 *   dump <addr> <words>        up to 64 words in hex (tools/acap.py reads the
 *                              recording with it)
 *   saiclk, peek <addr>        bring-up register readouts
 *   camsnap [step [frames]]    a still from the camera (pointed at the panel: does it show a
 *                              picture?), every step-th pixel (2), after frames for the exposure
 *                              (45); addr= w= h= of RGB565 in PSRAM, read with dump (tools/camsnap.py).
 *                              Refused while the lens cap is on, as after every reset
 *   camsnap on|off             lift the lens cap (until "off" or a reset) or put it back; cap=
 *   sysview [0|1|2]            the SystemView build only: SystemView's recorder on or off from
 *                              the board itself, with no host reading it (2: a thread empties the
 *                              buffer as the probe would); without a number, its state
 *   poke <addr> <value>        the SystemView build only: write one word
 *   reset                      clear the worst-case counters
 *   tour [begin | step NAME | alive | end WHY]   the PC's regression test (tools/tour.py) says what
 *                              it does; "tour" alone: its state. Stopped from the web remote, the
 *                              port refuses every command but "tour" and "tour end" until it ends
 *   tap <x> <y>                press and release
 *   swipe <x1> <y1> <x2> <y2> <ms>
 *   touch <slot> <x> <y> <0|1> raw contact, for multi-touch tests
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_CTL_H
#define PLATFORM_CTL_H

#include <stdbool.h>
#include <stdint.h>

enum ctl_cmd {
	CTL_NONE,
	CTL_SCREEN,
	CTL_HUD,
	CTL_ATTRACT,
	CTL_PLAY,
	CTL_HOME_TILES,
	CTL_INPUT,      /* counts as a touch: ends attract mode */
	CTL_CRASH,      /* "crash 4": the UI thread faults (a test: the board halts) */
};

/* UI thread: fetch the next pending UI command, if any. */
enum ctl_cmd ctl_poll(int *arg);

/* Any thread: queue a command for the UI thread (the web remote's way in).
 * False if the queue is full. */
bool ctl_post(enum ctl_cmd cmd, int arg);

/* The PC's regression test (tools/tour.py), as the board knows it from the
 * tour's own commands on this port. The web remote outranks it: it can ask
 * the companion on the PC to start one (the request rides on the host link's
 * replies, hostlink.c), and any of its actions stops one. A stopped tour has
 * every further command refused ("err stopped from the web remote") until it
 * says "tour end", so it quits at its next command, and the board undoes the
 * test states it may have left (held touches, a card acted out as taken out,
 * the card's clips hidden, Home's placeholder tiles, the theremin's or the
 * monitor's test settings, attract mode). A tour silent for 30 s, or a
 * request not taken up in 20 s, is given up. */
enum ctl_tour_state {
	CTL_TOUR_IDLE,
	CTL_TOUR_REQUESTED,  /* asked of the companion, not begun yet */
	CTL_TOUR_RUNNING,
	CTL_TOUR_STOPPING,   /* stopped from the remote, not ended yet */
};

struct ctl_tour {
	enum ctl_tour_state state;
	char step[24];
	uint32_t secs;       /* since it began or was asked for */
	char last[48];       /* how the last one ended, "" before the first */
};

void ctl_tour_get(struct ctl_tour *t);
/* The remote: ask the PC for a tour. False when one runs or is asked for. */
bool ctl_tour_request(void);
/* The remote: stop the tour or the request, if any. True if a running tour
 * was stopped. */
bool ctl_tour_stop(void);
/* The host link, for each reply: a request to hand to that companion, once
 * (a number that differs from the last one handed out), or 0. */
uint32_t ctl_tour_take_request(void);

#endif
