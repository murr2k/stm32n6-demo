/*
 * The camera: IMX335 (5 Mpx, on the B-CAMS-IMX module in the CSI connector)
 * through the DCMIPP, demosaiced and scaled in hardware to CAM_W x CAM_H
 * RGB565, about 30 frames a second, into PSRAM. Exposure, white balance and
 * colour are run by ST's ISP middleware (west module zephyr-stm32-mw-isp).
 *
 * Capturing runs only between camera_start() and camera_stop(): the Camera
 * screen calls them when it is shown and hidden.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MEDIA_CAMERA_H
#define MEDIA_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#define CAM_W 592 /* 2592 x 1944 scaled by 4.38: 4:3, a whole number of 16-byte lines */
#define CAM_H 444

struct camera_status {
	bool ready;         /* sensor and pipe found and configured */
	bool running;
	uint32_t frames;    /* since the last start */
	uint32_t fps_x10;   /* measured over about a second */
	uint32_t gain_mdb;  /* sensor analogue gain, milli-dB (as the ISP set it) */
	uint32_t exposure;  /* sensor exposure, in the driver's unit (lines) */
	const char *error;  /* NULL, or why there is no picture */
};

int camera_start(void);
void camera_stop(void);
void camera_get_status(struct camera_status *st);

/* The newest frame (RGB565, CAM_W x CAM_H, stride CAM_W) for the UI thread,
 * or NULL. It stays valid until camera_release(); `fresh` says whether it is
 * newer than the one acquired before.
 */
const uint16_t *camera_acquire(bool *fresh);
void camera_release(void);

#endif
