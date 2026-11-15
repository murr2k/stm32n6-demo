/*
 * Touch platform: GT911 events to an LVGL pointer plus a raw multi-touch table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_TOUCH_H
#define PLATFORM_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#define TOUCH_MAX_POINTS 5

struct touch_point {
	int16_t x;
	int16_t y;
	bool down;
	uint32_t t_us;    /* gfx_now_us() of the last report for this slot */
	uint32_t seq;     /* increments on every report for this slot */
};

struct touch_state {
	struct touch_point pt[TOUCH_MAX_POINTS];
	uint32_t reports;     /* total point reports since boot */
	uint32_t last_us;     /* time of the most recent report */
};

int touch_init(void);

/* Consistent copy of the current touch table. */
void touch_get(struct touch_state *out);

/* Inject a contact as if the panel had reported it (remote control, tests). */
void touch_inject(int slot, int x, int y, bool down);

/* True once after the USER button (B2) was pressed. */
bool touch_user_button_pressed(void);

#endif
