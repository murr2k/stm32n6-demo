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
	/* Optional, a pair: hide LVGL objects that are costly to draw before a frame
	 * that draws the whole screen, the first after show() among them; show them
	 * again at the start of the next frame, before the tick (shell_entering()). */
	void (*hold_content)(void);
	void (*show_content)(void);
	/* Once per frame slot whether visible or not: keep live data flowing. */
	void (*service)(uint32_t now_ms);
	/* Draw a live thumbnail into r->box for the Home tile. */
	void (*mini)(const struct raster *r);
	/* Optional: text over the thumbnail (LVGL draws it after the pixels). */
	void (*mini_text)(lv_layer_t *layer, const struct raster *r);

	/* Attract mode (the demo running itself when nobody touches it). */
	uint16_t attract_secs;          /* how long to stay; 0 means the default */
	bool attract_skip;              /* never visit: needs a person to make sense */
	bool (*available)(void);        /* visit only while this returns true */
	void (*attract_start)(void);    /* called on arrival, e.g. to start a clip */
	bool (*attract_busy)(void);     /* stay until this returns false */

	/* A tile on Home, a page dot and a place in the edge-swipe order only
	 * while this returns true (NULL: always). */
	bool (*listed)(void);
};

/* The screen has a tile, a dot and a place in the swipe order now. */
bool shell_listed(int index);

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
/* Someone is there without touching (the web remote open on a phone): the
 * demo does not start by itself, and one running is not ended. Any thread. */
void shell_note_presence(void);
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

/* True while the frame that builds the current screen after a switch is being
 * made: from the switch until that frame is handed over. It is the costliest
 * frame there is: it draws the whole screen, and its budget also holds the
 * switch (the old screen's hide() and the new one's show(), run after the
 * frame before it was presented). So it draws the layout only and the next
 * frame the content; both are dark (the fade-in waits FADE_HOLD_MS):
 * - every raster widget (raster.h) is left out of it by the shell and drawn
 *   in the next frame, with no code in the screen;
 * - work that only feeds such pictures waits while this is true (Signal
 *   Lab's analysis);
 * - LVGL objects that are costly to draw (a QR code, paragraphs of text) the
 *   screen hides in hold_content() and shows again in show_content(); hiding
 *   or showing one costs tens of microseconds, so hide a container, not many
 *   objects (Home's 16 blurbs and numbers cost about 1 ms each way, and
 *   drawn in the next frame with the thumbnails they made it miss).
 * The same holds for a frame in which the display redraws everything for its
 * own reasons (frame buffer B back from the NPU, gfx_on_full_redraw()), at any
 * frame after leaving the Camera screen. */
bool shell_entering(void);

/* True while a touch that began at a screen edge is being tracked as a
 * navigation swipe. Screens that read raw touches must ignore it.
 */
bool shell_gesture_active(void);

#endif
