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

	/* Attract mode (the demo running itself when nobody touches it). */
	uint16_t attract_secs;          /* how long to stay; 0 means the default */
	bool attract_skip;              /* never visit: needs a person to make sense */
	bool (*available)(void);        /* visit only while this returns true */
	void (*attract_start)(void);    /* called on arrival, e.g. to start a clip */
	bool (*attract_busy)(void);     /* stay until this returns false */
};

void shell_init(const struct screen *const *screens, int count);

/* Once per frame slot from the UI loop, before LVGL runs. */
void shell_tick(void);

/* Where the main loop's time goes before LVGL (ctl "svct"): for telemetry,
 * the shell's bar and HUD, and each screen's service and tick, the worst and
 * mean microseconds per frame since the reset, as "name:worst/mean" pairs. */
void shell_cost_tele(uint32_t us);
int shell_costs(char *out, size_t len);
void shell_costs_reset(void);

void shell_goto(int index);
int shell_current(void);
int shell_count(void);
const struct screen *shell_screen(int index);
void shell_toggle_hud(void);

/* Something other than a touch counted as user input (remote control). */
void shell_note_input(void);
/* Enter attract mode now instead of after the idle timeout. */
void shell_start_attract(void);
bool shell_in_attract(void);

/* An attract program: instead of the screens taking turns (attract_secs),
 * attract mode follows a clock, the position of a soundtrack in ms, and shows
 * each step's screen from its start time on; the lap repeats every lap_ms.
 * Each change starts early by the fade-out, so the new screen arrives on its
 * time (a bar line of the music). Whether attract mode runs the program is
 * decided as it starts (available()); start() is called when attract mode
 * begins on the first step's screen, which otherwise starts the soundtrack
 * itself on arrival (attract_start). While clock_ms() is negative (nothing
 * playing) the screens hold; after 5 s of that the screens take turns instead. */
struct shell_step {
	const struct screen *screen;
	uint32_t at_ms;
};
struct shell_program {
	const struct shell_step *steps;   /* by start time, the first at 0 */
	int n_steps;
	uint32_t lap_ms;
	bool (*available)(void);
	void (*start)(void);
	int32_t (*clock_ms)(void);
};
void shell_set_program(const struct shell_program *program);
/* True while attract mode runs the program. */
bool shell_program_running(void);
/* Its position in the lap, ms, or -1 while it does not run; the lap's length,
 * or 0 while it does not run. */
int32_t shell_program_ms(void);
uint32_t shell_program_length_ms(void);

/* Hide (or show again) the status bar and the current screen while a
 * full-screen view covers them.
 */
void shell_set_cover(bool covered);

/* True while a touch that began at a screen edge is being tracked as a
 * navigation swipe. Screens that read raw touches must ignore it.
 */
bool shell_gesture_active(void);

#endif
