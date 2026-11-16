/*
 * Hand tracking on the NPU, for the Camera screen.
 *
 * MediaPipe's palm detector finds a hand (any rotation); a crop turned
 * upright around it goes to the 21-point hand landmark network; from then on
 * each frame's crop follows the landmarks of the frame before, and the palm
 * detector runs again only when the hand is lost. Gestures are read from the
 * joint angles, so they do not depend on how the hand is turned or which
 * hand it is. Runs on the nn thread (npu.c) while hands_enable(true).
 *
 * Coordinates are camera frame pixels (CAM_W x CAM_H), as the picture is
 * shown on screen.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NN_HANDS_H
#define NN_HANDS_H

#include <stdbool.h>
#include <stdint.h>

#define HAND_POINTS 21

enum hand_gesture {
	G_NONE,
	G_FIST,
	G_OPEN,
	G_POINT,
	G_VICTORY,
	G_THREE,
	G_FOUR,
	G_THUMBS_UP,
	G_THUMBS_DOWN,
	G_THUMB,
	G_OK,
	G_ROCK,
	G_CALL,
	G_OTHER, /* named by its finger count */
};

struct hands_result {
	uint32_t seq;            /* new result when this changes */
	bool valid;              /* a hand is being tracked */
	float x[HAND_POINTS];    /* smoothed, camera frame pixels */
	float y[HAND_POINTS];
	float presence;          /* the landmark network's confidence, 0..1 */
	enum hand_gesture gesture;
	int fingers;             /* extended fingers, 0..5 */
	uint32_t palm_us;        /* last palm detector run on the NPU */
	uint32_t hand_us;        /* last landmark run on the NPU */
	uint32_t prep_us;        /* last crop or scaling on the CPU */
	uint32_t fps_x10;        /* landmark results per second */
	uint32_t searches;       /* palm detector runs since enabled */
	const char *error;       /* NULL, or why nothing runs */
};

/* The bones between the 21 points, as pairs of indices. */
extern const uint8_t hand_bones[][2];
extern const int hand_n_bones;

void hands_enable(bool on);
/* Track the built-in test picture (a thumbs up, MediaPipe's test data) in a
 * camera-sized frame instead of the camera: a check that needs no hand. */
void hands_simulate(bool on);
bool hands_enabled(void);
void hands_get(struct hands_result *out);
const char *hands_gesture_name(enum hand_gesture g, int fingers);

/* nn thread: one camera frame's worth of work (waits for the frame). */
void hands_step(void);

#endif
