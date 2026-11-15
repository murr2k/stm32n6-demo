/*
 * Remote control over TCP, for unattended testing from the host
 * (tools/ctl.py). Line-oriented text commands on tcp/5556:
 *
 *   screen <n>                 switch screen
 *   hud                        toggle the performance HUD
 *   attract                    start attract mode now
 *   stats                      one line of frame statistics
 *   video                      one line of player status
 *   media                      the clip list and the SD card
 *   clip <n>                   clip n: its file on the card, frames, title
 *   card <0|1>                 list the card's clips or only the flash ones
 *   play <n>                   go to Cinema and open clip n
 *   hometiles <n>              Home: n placeholder tiles after the real ones
 *   home                       Home: tiles, pan position and range
 *   audio                      DAC half-buffer count and underruns
 *   tone <hz> <ms>             test tone at -12 dBFS
 *   volume <0..100>            headphone volume
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
