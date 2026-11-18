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
 *   play <n>                   go to Cinema and open clip n
 *   netcfg [dhcp | static IP MASK [GW]]  the board's address: show, or change now (and store)
 *   hoodt                      Under the Hood: worst sampling, stack scan, network and side panel times, us
 *   ds                         Explorer's data set: in flash or generated, address, checksum
 *   hometiles <n>              Home: n placeholder tiles after the real ones
 *   home                       Home: tiles, pan position and range
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
 *   reset                      clear the worst-case counters
 *   tap <x> <y>                press and release
 *   swipe <x1> <y1> <x2> <y2> <ms>
 *   touch <slot> <x> <y> <0|1> raw contact, for multi-touch tests
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_CTL_H
#define PLATFORM_CTL_H

#include <stdbool.h>

enum ctl_cmd {
	CTL_NONE,
	CTL_SCREEN,
	CTL_HUD,
	CTL_ATTRACT,
	CTL_PLAY,
	CTL_HOME_TILES,
};

/* UI thread: fetch the next pending UI command, if any. */
enum ctl_cmd ctl_poll(int *arg);

#endif
