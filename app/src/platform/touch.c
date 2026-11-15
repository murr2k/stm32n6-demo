/*
 * Touch platform.
 *
 * The Zephyr GT911 driver reports each contact as
 *   ABS_MT_SLOT, ABS_X, ABS_Y, BTN_TOUCH (the last one carries sync).
 * Events arrive on the input thread; LVGL polls from the UI thread.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <lvgl.h>

#include "gfx.h"
#include "touch.h"

static struct touch_state state;
static struct k_spinlock lock;
static int primary = -1;       /* slot that drives the LVGL pointer */
static atomic_t user_button;

void touch_inject(int slot, int x, int y, bool down)
{
	if (slot < 0 || slot >= TOUCH_MAX_POINTS) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct touch_point *p = &state.pt[slot];

	p->x = CLAMP(x, 0, GFX_W - 1);
	p->y = CLAMP(y, 0, GFX_H - 1);
	p->down = down;
	p->t_us = gfx_now_us();
	p->seq++;
	state.reports++;
	state.last_us = p->t_us;
	if (down && primary < 0) {
		primary = slot;
	} else if (!down && primary == slot) {
		primary = -1;
	}
	k_spin_unlock(&lock, key);
}

static void touch_cb(struct input_event *evt, void *user_data)
{
	static int slot, x, y;

	ARG_UNUSED(user_data);
	switch (evt->code) {
	case INPUT_ABS_MT_SLOT:
		slot = evt->value;
		break;
	case INPUT_ABS_X:
		x = evt->value;
		break;
	case INPUT_ABS_Y:
		y = evt->value;
		break;
	case INPUT_BTN_TOUCH:
		touch_inject(slot, x, y, evt->value != 0);
		break;
	default:
		break;
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)), touch_cb, NULL);

static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_0 && evt->value) {
		atomic_set(&user_button, 1);
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_cb, NULL);

static void indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	static int16_t last_x, last_y;
	k_spinlock_key_t key = k_spin_lock(&lock);

	ARG_UNUSED(indev);
	if (primary >= 0) {
		last_x = state.pt[primary].x;
		last_y = state.pt[primary].y;
		data->state = LV_INDEV_STATE_PRESSED;
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
	k_spin_unlock(&lock, key);
	data->point.x = last_x;
	data->point.y = last_y;
}

int touch_init(void)
{
	lv_indev_t *indev = lv_indev_create();

	if (indev == NULL) {
		return -ENOMEM;
	}
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, indev_read);
	return 0;
}

void touch_get(struct touch_state *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = state;
	k_spin_unlock(&lock, key);
}

bool touch_user_button_pressed(void)
{
	return atomic_cas(&user_button, 1, 0);
}
