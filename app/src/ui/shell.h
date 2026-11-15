/*
 * Shell: status bar, screen switching, edge-swipe navigation, performance HUD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UI_SHELL_H
#define UI_SHELL_H

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#include "raster.h"

struct screen {
	const char *title;
	const char *blurb; /* one line under the title on the Home tile */
	/* Build the screen inside `root` (800 x UI_CONTENT_H, origin below the bar). */
	void (*create)(lv_obj_t *root);
	/* Once per frame slot while the screen is visible. */
	void (*tick)(uint32_t now_ms);
	void (*show)(void);
	void (*hide)(void);
	/* Once per frame slot whether visible or not: keep live data flowing. */
	void (*service)(uint32_t now_ms);
	/* Draw a live thumbnail into r->box for the Home tile. */
	void (*mini)(const struct raster *r);
};

void shell_init(const struct screen *const *screens, int count);

/* Once per frame slot from the UI loop, before LVGL runs. */
void shell_tick(void);

void shell_goto(int index);
int shell_current(void);
int shell_count(void);
const struct screen *shell_screen(int index);
void shell_toggle_hud(void);

/* Hide (or show again) the status bar and the current screen while a
 * full-screen view covers them.
 */
void shell_set_cover(bool covered);

/* True while a touch that began at a screen edge is being tracked as a
 * navigation swipe. Screens that read raw touches must ignore it.
 */
bool shell_gesture_active(void);

#endif
