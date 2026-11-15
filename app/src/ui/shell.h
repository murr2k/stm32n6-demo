/*
 * Shell: status bar, screen switching, performance HUD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UI_SHELL_H
#define UI_SHELL_H

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

struct screen {
	const char *title;
	/* Build the screen inside `root` (800 x UI_CONTENT_H, origin below the bar). */
	void (*create)(lv_obj_t *root);
	/* Called once per frame slot while the screen is visible. */
	void (*tick)(uint32_t now_ms);
	void (*show)(void);
	void (*hide)(void);
};

void shell_init(const struct screen *const *screens, int count);

/* Once per frame slot from the UI loop, before LVGL runs. */
void shell_tick(void);

void shell_goto(int index);
int shell_current(void);
void shell_toggle_hud(void);

#endif
