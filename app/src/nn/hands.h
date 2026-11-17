/*
 * Hand tracking on the NPU, for the Camera screen.
 *
 * MediaPipe's palm detector finds hands (any rotation); a crop turned
 * upright around each goes to the 21-point hand landmark network; from then
 * on each frame's crop follows the landmarks of the frame before. Up to
 * HANDS_MAX hands are tracked, each in its own slot for as long as it stays
 * in view. With no hand the palm detector runs on every frame; with fewer
 * than the maximum it runs every search_ms, and a palm already tracked is
 * not taken again. Gestures are read from the joint angles, so they do not
 * depend on how the hand is turned or which hand it is. Runs on the nn
 * thread (npu.c) while hands_enable(true).
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
#define HANDS_MAX   2

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

struct hand {
	bool valid;              /* this slot tracks a hand */
	float x[HAND_POINTS];    /* smoothed, camera frame pixels */
	float y[HAND_POINTS];
	float presence;          /* the landmark network's confidence, 0..1 */
	float handedness;        /* the network's handedness output, averaged over the track */
	enum hand_gesture gesture;
	int fingers;             /* extended fingers, 0..5 */
};

struct hands_result {
	uint32_t seq;            /* new result when this changes */
	int n;                   /* hands tracked */
	struct hand hand[HANDS_MAX]; /* a hand keeps its slot while it is tracked */
	uint32_t palm_us;        /* last palm detector run on the NPU */
	uint32_t hand_us;        /* last landmark run on the NPU */
	uint32_t prep_us;        /* last crop or scaling on the CPU */
	uint32_t step_us;        /* last frame's work, all networks and crops */
	uint32_t fps_x10;        /* frames with a hand tracked, per second */
	uint32_t searches;       /* palm detector runs since enabled */
	float palm_score;        /* best palm of the last search, 0..1 */
	uint32_t palms;          /* palms found (not tracked already) */
	uint32_t confirmed;      /* palms the landmark network confirmed */
	uint32_t lost;           /* tracked hands lost */
	uint32_t merged;         /* tracks dropped because they had found the same hand */
	const char *error;       /* NULL, or why nothing runs */
};

/* One frame as the tracker saw it, copied to PSRAM for the host ("nn grab",
 * tools/nn.py grab): the camera picture and the inputs the networks got. */
struct hands_grab {
	uint32_t n;              /* grabs completed */
	const uint16_t *frame;   /* CAM_W x CAM_H RGB565 */
	const uint8_t *palm_in;  /* 192 x 192 RGB, when has_palm */
	const uint8_t *hand_in;  /* 224 x 224 RGB, when has_hand */
	bool has_palm, has_hand;
	float palm_score, presence;
	float cx, cy, size, rot; /* the landmark crop, camera pixels and radians */
};

void hands_grab_arm(void);
void hands_grab_get(struct hands_grab *g);
/* Thresholds, 0..1: a palm's score and the landmark network's presence. */
void hands_set_thresholds(float palm, float presence);
void hands_get_thresholds(float *palm, float *presence);
/* How many hands at most (1..HANDS_MAX), and how often the palm detector looks
 * for another while some are tracked. */
void hands_set_search(int max_hands, uint32_t search_ms);
void hands_get_search(int *max_hands, uint32_t *search_ms);

/* The bones between the 21 points, as pairs of indices. */
extern const uint8_t hand_bones[][2];
extern const int hand_n_bones;

void hands_enable(bool on);
/* Track the built-in test picture (a thumbs up, MediaPipe's test data) in a
 * camera-sized frame instead of the camera: a check that needs no hand.
 * 0 the camera, 1 the picture, 2 the picture twice side by side (the left
 * copy mirrored: two hands). */
void hands_simulate(int mode);
bool hands_enabled(void);
void hands_get(struct hands_result *out);
const char *hands_gesture_name(enum hand_gesture g, int fingers);

/* nn thread: one camera frame's worth of work (waits for the frame). */
void hands_step(void);

#endif
