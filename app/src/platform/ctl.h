/*
 * Remote control over TCP, for unattended testing from the host
 * (tools/ctl.py). Line-oriented text commands on tcp/5556:
 *
 *   screen <n>                 switch screen
 *   hud                        toggle the performance HUD
 *   stats                      one line of frame statistics
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
};

/* UI thread: fetch the next pending UI command, if any. */
enum ctl_cmd ctl_poll(int *arg);

#endif
