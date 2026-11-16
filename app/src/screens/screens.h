/*
 * Screen registry.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SCREENS_H
#define SCREENS_H

#include <zephyr/sys/util.h>

#include "platform/gfx.h"
#include "ui/shell.h"

extern const struct screen screen_home;
extern const struct screen screen_deck;
extern const struct screen screen_siglab;
extern const struct screen screen_explorer;
extern const struct screen screen_touchlab;
extern const struct screen screen_cinema;
extern const struct screen screen_hostlink;
extern const struct screen screen_coremap;
extern const struct screen screen_specimen;
extern const struct screen screen_hood;
extern const struct screen screen_settings;
extern const struct screen screen_camera;

/* Home: add n placeholder tiles after the real ones, to try the panning with
 * more tiles than there are screens (remote control, tools/tour.py); 0 takes
 * them away again. UI thread.
 */
void home_test_tiles(int n);
/* Home: tiles shown, how far the field is panned and how far it can be, px. */
void home_get_status(int *tiles, int *scroll_x, int *scroll_max);

/* Under the Hood: longest sampling, stack scan, network query and side panel
 * draw since the last call, us (bring-up aid, ctl "hoodt").
 */
void hood_timing(uint32_t out[4], bool clear);

/* Cinema: open clip number `clip` once that screen is showing (remote control). */
void cinema_play(int clip);

#endif
