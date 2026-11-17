/*
 * Hand tracking. See hands.h.
 *
 * The geometry follows MediaPipe's hand pipeline (checked against it on the
 * PC by tools/nn.py testimg and check): the palm detector sees the whole
 * picture letterboxed to 192 x 192; its box, turned so that the wrist is
 * below the middle finger's knuckle, shifted half a box towards the fingers
 * and enlarged 2.6 times, is the 224 x 224 crop for the landmarks. While
 * tracking, the next crop comes from the landmarks themselves (their box
 * around wrist and knuckles, turned the same way, 2 times, shifted a tenth).
 *
 * Two hands as in MediaPipe: a palm whose crop overlaps a tracked hand's by
 * more than half is that hand again and is not taken; two tracks whose crops
 * come to overlap that much have found the same hand, and the newer goes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "hands.h"
#include "media/camera.h"
#include "npu.h"
#include "platform/gfx.h"
#include "platform/psram.h"

LOG_MODULE_REGISTER(hands, LOG_LEVEL_INF);

#define PALM_N      192
#define HAND_N      224
#define ANCHORS     2016
#define PALM_STRIDE 18 /* box (4) and 7 key points (14) per anchor */
#define PI_F        3.14159265f
/* The landmark network's two single values, in the converter's output order
 * (that of the .tflite: Identity_2, then Identity_1). Identity_1 is the hand's
 * presence: in ST's quantized model 0.500 for any hand (it saturates there,
 * on the PC too) and about 0.01 for anything else. Identity_2 is handedness:
 * 0.93 for the test picture, 0.24 for its mirror image. Taking the second for
 * presence made the tracker drop one hand of every person. */
#define HANDS_HANDEDNESS_OUTPUT 0
#define HANDS_PRESENCE_OUTPUT   1
#define PRESENCE_FULL           0.5f

const uint8_t hand_bones[][2] = {
	{0, 1}, {1, 2}, {2, 3}, {3, 4},         /* thumb */
	{0, 5}, {5, 6}, {6, 7}, {7, 8},         /* index */
	{5, 9}, {9, 10}, {10, 11}, {11, 12},    /* middle */
	{9, 13}, {13, 14}, {14, 15}, {15, 16},  /* ring */
	{13, 17}, {0, 17}, {17, 18}, {18, 19}, {19, 20}, /* pinky, palm edge */
};
const int hand_n_bones = ARRAY_SIZE(hand_bones);

#define PALM_CAND   64   /* best palm detections kept for the suppression */
#define SAME_HAND   0.5f /* crop overlap (intersection over union) of one hand */

struct roi {
	float cx, cy, size, rot; /* camera pixels, radians */
};

/* One tracked hand: its next crop, its palm (centre and wrist to middle
 * knuckle, camera pixels, last frame), landmark smoothing, gesture debouncing. */
struct track {
	bool on;
	struct roi roi;
	float pcx, pcy, psize;
	float fx[HAND_POINTS], fy[HAND_POINTS], fdx[HAND_POINTS], fdy[HAND_POINTS];
	uint32_t t_us;
	enum hand_gesture shown, cand;
	int cand_count;
};

static atomic_t want, simulate, reset;
static uint16_t *sim_frame;
static int sim_built;
static struct k_spinlock lock;
static struct hands_result result; /* published copy */
static struct hands_result work;   /* the nn thread's */
static struct track tracks[HANDS_MAX];
static uint32_t rate_t0, rate_n;

/* thresholds: MediaPipe's defaults, 0.5 each; the palm's as a logit */
static float palm_logit = 0.0f, presence_min = 0.5f;
static int max_hands = HANDS_MAX;
static uint32_t search_ms = 500, last_search_ms;

static atomic_t grab_req;
static struct hands_grab grab;

void hands_set_thresholds(float palm, float presence)
{
	palm = CLAMP(palm, 0.01f, 0.99f);
	palm_logit = logf(palm / (1.0f - palm));
	presence_min = CLAMP(presence, 0.0f, 1.0f);
}

void hands_get_thresholds(float *palm, float *presence)
{
	*palm = 1.0f / (1.0f + expf(-palm_logit));
	*presence = presence_min;
}

void hands_set_search(int max, uint32_t ms)
{
	max_hands = CLAMP(max, 1, HANDS_MAX);
	search_ms = MIN(ms, 10000u);
}

void hands_get_search(int *max, uint32_t *ms)
{
	*max = max_hands;
	*ms = search_ms;
}

void hands_grab_arm(void)
{
	atomic_set(&grab_req, 1);
}

void hands_grab_get(struct hands_grab *g)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*g = grab;
	k_spin_unlock(&lock, key);
}

/* nn thread: the buffers on first use (785 KB of PSRAM, only once a host asks) */
static bool grab_buffers(void)
{
	if (grab.frame == NULL) {
		grab.frame = psram_alloc(CAM_W * CAM_H * 2, 64);
		grab.palm_in = psram_alloc(PALM_N * PALM_N * 3, 64);
		grab.hand_in = psram_alloc(HAND_N * HAND_N * 3, 64);
	}
	return grab.frame != NULL && grab.palm_in != NULL && grab.hand_in != NULL;
}

void hands_enable(bool on)
{
	if (on) {
		k_spinlock_key_t key = k_spin_lock(&lock);

		/* nothing from an earlier visit */
		result.n = 0;
		memset(result.hand, 0, sizeof(result.hand));
		k_spin_unlock(&lock, key);
		atomic_set(&reset, 1);
	}
	atomic_set(&want, on);
}

void hands_simulate(int mode)
{
	atomic_set(&simulate, CLAMP(mode, 0, 2));
}

/* The palm detector's 192 x 192 test picture in a camera frame (nearest
 * pixel), the rest black: mode 1 once, 444 x 444 in the middle; mode 2 twice,
 * 296 x 296 side by side, the left copy mirrored. */
static const uint16_t *sim_picture(int mode)
{
	const uint8_t *t = npu_test_input(NPU_PALM);

	if (t == NULL) {
		return NULL;
	}
	if (sim_frame == NULL) {
		sim_frame = psram_alloc(CAM_W * CAM_H * 2, 64);
		if (sim_frame == NULL) {
			return NULL;
		}
		sim_built = 0;
	}
	if (sim_built != mode) {
		int side = mode == 2 ? CAM_W / 2 : CAM_H, y0 = (CAM_H - side) / 2;

		for (int y = 0; y < CAM_H; y++) {
			for (int x = 0; x < CAM_W; x++) {
				int copy = mode == 2 ? x / side : 0;
				int dx = x - (mode == 2 ? copy * side : (CAM_W - side) / 2), dy = y - y0;
				uint16_t p = 0;

				if (dx >= 0 && dx < side && dy >= 0 && dy < side) {
					int u = dx * PALM_N / side, v = dy * PALM_N / side;

					if (mode == 2 && copy == 0) {
						u = PALM_N - 1 - u;
					}
					const uint8_t *s = t + (v * PALM_N + u) * 3;

					p = ((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3);
				}
				sim_frame[y * CAM_W + x] = p;
			}
		}
		sys_cache_data_flush_range(sim_frame, CAM_W * CAM_H * 2);
		sim_built = mode;
	}
	return sim_frame;
}

bool hands_enabled(void)
{
	return atomic_get(&want) != 0;
}

void hands_get(struct hands_result *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = result;
	k_spin_unlock(&lock, key);
}

static void publish(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	work.seq++;
	result = work;
	k_spin_unlock(&lock, key);
}

/* --- pixels ---------------------------------------------------------------- */

/* An RGB565 pixel spread over a word, green at bit 21, red at 11, blue at 0,
 * each with at least five free bits above it: one multiply by a 5-bit weight
 * then scales all three channels at once. */
#define SPREAD_MASK 0x07E0F81Fu

static inline uint32_t spread(uint16_t p)
{
	uint32_t v = p;

	return (v | (v << 16)) & SPREAD_MASK;
}

static inline uint32_t blend(uint32_t a, uint32_t b, uint32_t w)
{
	return ((a * (32 - w) + b * w) >> 5) & SPREAD_MASK;
}

/* Bilinear sample at 16.16 fixed-point pixel coordinates (pixel centres at
 * .5), weights to 1/32 of a pixel, black outside the picture. */
static inline void sample(const uint16_t *f, int32_t x16, int32_t y16, uint8_t *o)
{
	x16 -= 0x8000;
	y16 -= 0x8000;
	int x0 = x16 >> 16, y0 = y16 >> 16;

	if ((unsigned int)x0 >= CAM_W - 1 || (unsigned int)y0 >= CAM_H - 1) {
		o[0] = o[1] = o[2] = 0;
		return;
	}
	uint32_t ax = (x16 >> 11) & 31, ay = (y16 >> 11) & 31;
	const uint16_t *p = f + y0 * CAM_W + x0;
	uint32_t v = blend(blend(spread(p[0]), spread(p[1]), ax), blend(spread(p[CAM_W]), spread(p[CAM_W + 1]), ax),
			   ay);
	uint32_t r = (v >> 11) & 31, g = (v >> 21) & 63, b = v & 31;

	o[0] = (r << 3) | (r >> 2);
	o[1] = (g << 2) | (g >> 4);
	o[2] = (b << 3) | (b >> 2);
}

/* The whole picture letterboxed into the palm detector's 192 x 192 input. */
#define LB_SCALE ((float)PALM_N / CAM_W)
#define LB_PAD   ((PALM_N - (int)(CAM_H * LB_SCALE + 0.5f)) / 2)

static void palm_input(const uint16_t *f, uint8_t *in)
{
	int32_t step = (int32_t)(65536.0f / LB_SCALE);

	for (int v = 0; v < PALM_N; v++) {
		uint8_t *o = in + v * PALM_N * 3;
		int32_t y16 = (int32_t)(((v - LB_PAD) + 0.5f) / LB_SCALE * 65536.0f);

		if (v < LB_PAD || v >= PALM_N - LB_PAD) {
			memset(o, 0, PALM_N * 3);
			continue;
		}
		int32_t x16 = step / 2;

		for (int u = 0; u < PALM_N; u++, x16 += step, o += 3) {
			sample(f, x16, y16, o);
		}
	}
}

/* A HAND_N x HAND_N crop of the picture around a turned square. */
static void hand_input(const uint16_t *f, const struct roi *q, uint8_t *in)
{
	float c = cosf(q->rot), s = sinf(q->rot), k = q->size / HAND_N;
	int32_t du_x = (int32_t)(k * c * 65536.0f), du_y = (int32_t)(k * s * 65536.0f);
	int32_t dv_x = (int32_t)(-k * s * 65536.0f), dv_y = (int32_t)(k * c * 65536.0f);
	float a = (0.5f / HAND_N - 0.5f) * q->size; /* offset of the first sample from the centre */
	int32_t x0 = (int32_t)((q->cx + a * c - a * s) * 65536.0f);
	int32_t y0 = (int32_t)((q->cy + a * s + a * c) * 65536.0f);

	for (int v = 0; v < HAND_N; v++, x0 += dv_x, y0 += dv_y) {
		uint8_t *o = in + v * HAND_N * 3;
		int32_t x = x0, y = y0;

		for (int u = 0; u < HAND_N; u++, x += du_x, y += du_y, o += 3) {
			sample(f, x, y, o);
		}
	}
}

/* --- geometry -------------------------------------------------------------- */

static float norm_angle(float a)
{
	while (a > PI_F) {
		a -= 2 * PI_F;
	}
	while (a < -PI_F) {
		a += 2 * PI_F;
	}
	return a;
}

/* Centre of anchor i: 24 x 24 cells with 2 anchors, then 12 x 12 with 6. */
static void anchor(int i, float *ax, float *ay)
{
	int c, n;

	if (i < 1152) {
		c = i / 2, n = 24;
	} else {
		c = (i - 1152) / 6, n = 12;
	}
	*ax = ((c % n) + 0.5f) / n;
	*ay = ((c / n) + 0.5f) / n;
}

/* Intersection over union of two boxes given as centre and size. */
static float iou(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh)
{
	float ix = MIN(ax + aw / 2, bx + bw / 2) - MAX(ax - aw / 2, bx - bw / 2);
	float iy = MIN(ay + ah / 2, by + bh / 2) - MAX(ay - ah / 2, by - bh / 2);
	float inter = MAX(ix, 0.0f) * MAX(iy, 0.0f);

	return inter / (aw * ah + bw * bh - inter + 1e-9f);
}

/* Two crops, upright squares around their centres (turning ignored). */
static float roi_overlap(const struct roi *a, const struct roi *b)
{
	return iou(a->cx, a->cy, a->size, a->size, b->cx, b->cy, b->size, b->size);
}

/* Detection i: box (centre, size) and key points, 0..1 of the 192 x 192 input. */
static void decode(const float *box, int i, float *b)
{
	const float *r = box + i * PALM_STRIDE;
	float ax, ay;

	anchor(i, &ax, &ay);
	for (int k = 0; k < PALM_STRIDE; k++) {
		b[k] = r[k] / PALM_N + ((k == 2 || k == 3) ? 0 : (k & 1) ? ay : ax);
	}
}

/* A merged detection as the crop for the landmarks. */
static void palm_crop(const float *d, struct roi *q)
{
	/* letterbox to camera pixels: x * CAM_W, y * CAM_W - padding */
	float pad = LB_PAD / LB_SCALE;
	float cx = d[0] * CAM_W, cy = d[1] * CAM_W - pad, w = d[2] * CAM_W, h = d[3] * CAM_W;
	float wx = d[4] * CAM_W, wy = d[5] * CAM_W - pad;  /* key point 0: wrist */
	float mx = d[8] * CAM_W, my = d[9] * CAM_W - pad;  /* key point 2: middle finger knuckle */

	q->rot = norm_angle(PI_F / 2 - atan2f(-(my - wy), mx - wx));
	q->cx = cx + 0.5f * h * sinf(q->rot);
	q->cy = cy - 0.5f * h * cosf(q->rot);
	q->size = MAX(w, h) * 2.6f;
}

/* A detection (merged, 0..1 of the detector's input) of a hand tracked
 * already: its palm centre near that hand's, or its crop on that hand's. The
 * crops alone miss: a palm's crop and a landmark crop of one hand can overlap
 * by less than half, and the duplicate then took the slot meant for a second
 * hand (merged away a frame later, and the same again at the next search). */
static bool tracked_already(const float *d, const struct roi *q)
{
	float pad = LB_PAD / LB_SCALE;
	float cx = d[0] * CAM_W, cy = d[1] * CAM_W - pad, size = MAX(d[2], d[3]) * CAM_W;

	for (int i = 0; i < HANDS_MAX; i++) {
		const struct track *t = &tracks[i];

		if (!t->on) {
			continue;
		}
		float dx = cx - t->pcx, dy = cy - t->pcy, near = 0.6f * MAX(size, t->psize);

		if (dx * dx + dy * dy < near * near || roi_overlap(q, &t->roi) > SAME_HAND) {
			return true;
		}
	}
	return false;
}

/* Palms, best first, each merged with the detections overlapping it
 * (MediaPipe's weighted suppression), as crops for the landmarks; palms of
 * hands tracked already are left out. Up to max; *best_p is the best score in
 * the picture whether it passes the threshold or not. */
static int palm_rois(const float *score, const float *box, struct roi *out, int max, float *best_p)
{
	static uint16_t cand[PALM_CAND];
	static bool used[PALM_CAND];
	int n = 0, best = 0, found = 0;

	for (int i = 0; i < ANCHORS; i++) {
		if (score[i] > score[best]) {
			best = i;
		}
		if (score[i] <= palm_logit || (n == PALM_CAND && score[i] <= score[cand[n - 1]])) {
			continue;
		}
		/* insert, best first, keeping the PALM_CAND best */
		int j = n < PALM_CAND ? n++ : n - 1;

		while (j > 0 && score[cand[j - 1]] < score[i]) {
			cand[j] = cand[j - 1];
			j--;
		}
		cand[j] = i;
	}
	*best_p = 1.0f / (1.0f + expf(-score[best]));
	memset(used, 0, sizeof(used));
	for (int c = 0; c < n && found < max; c++) {
		if (used[c]) {
			continue;
		}
		float top[PALM_STRIDE], b[PALM_STRIDE], d[PALM_STRIDE] = {0}, sum = 0;

		decode(box, cand[c], top);
		for (int e = c; e < n; e++) {
			if (used[e]) {
				continue;
			}
			decode(box, cand[e], b);
			if (iou(top[0], top[1], top[2], top[3], b[0], b[1], b[2], b[3]) < 0.3f) {
				continue;
			}
			float w = 1.0f / (1.0f + expf(-score[cand[e]]));

			used[e] = true;
			for (int k = 0; k < PALM_STRIDE; k++) {
				d[k] += w * b[k];
			}
			sum += w;
		}
		for (int k = 0; k < PALM_STRIDE; k++) {
			d[k] /= sum;
		}
		palm_crop(d, &out[found]);
		if (!tracked_already(d, &out[found])) {
			found++;
		}
	}
	return found;
}

/* The next crop from this frame's landmarks (camera pixels). */
static void landmark_roi(const float *x, const float *y, struct roi *q)
{
	static const uint8_t subset[] = {0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};
	float x1 = ((x[5] + x[13]) / 2 + x[9]) / 2, y1 = ((y[5] + y[13]) / 2 + y[9]) / 2;
	float rot = norm_angle(PI_F / 2 - atan2f(-(y1 - y[0]), x1 - x[0]));
	float c = cosf(rot), s = sinf(rot);
	float lo_u = 1e9f, hi_u = -1e9f, lo_v = 1e9f, hi_v = -1e9f;

	for (int i = 0; i < ARRAY_SIZE(subset); i++) {
		/* into the turned frame */
		float u = x[subset[i]] * c + y[subset[i]] * s, v = -x[subset[i]] * s + y[subset[i]] * c;

		lo_u = MIN(lo_u, u), hi_u = MAX(hi_u, u);
		lo_v = MIN(lo_v, v), hi_v = MAX(hi_v, v);
	}
	float cu = (lo_u + hi_u) / 2, cv = (lo_v + hi_v) / 2, w = hi_u - lo_u, h = hi_v - lo_v;

	q->rot = rot;
	q->cx = cu * c - cv * s;
	q->cy = cu * s + cv * c;
	/* shift a tenth of the height towards the fingers, square, twice the size */
	q->cx += 0.1f * h * s;
	q->cy -= 0.1f * h * c;
	q->size = MAX(w, h) * 2.0f;
}

/* --- gestures -------------------------------------------------------------- */

static float dist3(const float *p, int a, int b)
{
	float dx = p[a * 3] - p[b * 3], dy = p[a * 3 + 1] - p[b * 3 + 1], dz = p[a * 3 + 2] - p[b * 3 + 2];

	return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* How far a chain of joints bends: the sum of the turns at its inner joints, degrees. */
static float bend(const float *p, int a, int b, int c, int d)
{
	const int j[4] = {a, b, c, d};
	float total = 0;

	for (int i = 1; i < 3; i++) {
		float ux = p[j[i] * 3] - p[j[i - 1] * 3], uy = p[j[i] * 3 + 1] - p[j[i - 1] * 3 + 1];
		float uz = p[j[i] * 3 + 2] - p[j[i - 1] * 3 + 2];
		float vx = p[j[i + 1] * 3] - p[j[i] * 3], vy = p[j[i + 1] * 3 + 1] - p[j[i] * 3 + 1];
		float vz = p[j[i + 1] * 3 + 2] - p[j[i] * 3 + 2];
		float cosv = (ux * vx + uy * vy + uz * vz) /
			     (sqrtf(ux * ux + uy * uy + uz * uz) * sqrtf(vx * vx + vy * vy + vz * vz) + 1e-6f);

		total += acosf(CLAMP(cosv, -1.0f, 1.0f)) * (180.0f / PI_F);
	}
	return total;
}

/* From the crop's 3-D points (rotation does not matter: only angles and
 * distance ratios) and, for the thumb's direction, the picture's. */
static enum hand_gesture classify(const float *p, const float *x, const float *y, int *n_ext)
{
	static const uint8_t fingers[4][4] = {{5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}, {17, 18, 19, 20}};
	float palm = dist3(p, 0, 9) + 1e-6f;
	bool ext[5];

	ext[0] = bend(p, 1, 2, 3, 4) < 50.0f && dist3(p, 4, 5) > 0.55f * palm && dist3(p, 4, 17) > dist3(p, 2, 17);
	for (int f = 0; f < 4; f++) {
		const uint8_t *j = fingers[f];

		ext[f + 1] = bend(p, j[0], j[1], j[2], j[3]) < 60.0f && dist3(p, 0, j[3]) > dist3(p, 0, j[1]);
	}
	int n = 0, mask = 0;

	for (int i = 0; i < 5; i++) {
		n += ext[i];
		mask |= ext[i] << i;
	}
	*n_ext = n;
	/* thumb and index tips together, the other three up */
	if (dist3(p, 4, 8) < 0.35f * palm && ext[2] && ext[3] && ext[4]) {
		return G_OK;
	}
	switch (mask) {
	case 0x00:
		return G_FIST;
	case 0x1F:
		return G_OPEN;
	case 0x02:
		return G_POINT;
	case 0x06:
		return G_VICTORY;
	case 0x0E:
		return G_THREE;
	case 0x1E:
		return G_FOUR;
	case 0x12:
		return G_ROCK;
	case 0x11:
		return G_CALL;
	case 0x13:
		return G_LOVE; /* rock on with the thumb out */
	case 0x04:
	case 0x05:
		return G_BIRD; /* the middle finger up from a fist (the thumb often half out beside it) */
	case 0x01: {
		/* which way the thumb points in the picture */
		float dx = x[4] - x[2], dy = y[4] - y[2], len = sqrtf(dx * dx + dy * dy) + 1e-6f;

		return dy < -0.6f * len ? G_THUMBS_UP : dy > 0.6f * len ? G_THUMBS_DOWN : G_THUMB;
	}
	default:
		return G_OTHER;
	}
}

const char *hands_gesture_name(enum hand_gesture g, int fingers)
{
	static const char *const names[] = {
		[G_NONE] = "-", [G_FIST] = "Fist", [G_OPEN] = "Open hand", [G_POINT] = "Pointing",
		[G_VICTORY] = "Victory", [G_THREE] = "Three", [G_FOUR] = "Four", [G_THUMBS_UP] = "Thumbs up",
		[G_THUMBS_DOWN] = "Thumbs down", [G_THUMB] = "Thumb out", [G_OK] = "OK", [G_ROCK] = "Rock on",
		[G_CALL] = "Call me", [G_LOVE] = "I love you", [G_BIRD] = "The bird",
	};
	static const char *const counts[] = {"No fingers", "One finger", "Two fingers", "Three fingers",
					     "Four fingers", "Five fingers"};

	if (g == G_OTHER) {
		return counts[CLAMP(fingers, 0, 5)];
	}
	return g < ARRAY_SIZE(names) ? names[g] : "?";
}

/* --- smoothing: a one-euro filter per coordinate --------------------------- */

static float euro_alpha(float cutoff_hz, float dt)
{
	float tau = 1.0f / (2 * PI_F * cutoff_hz);

	return 1.0f / (1.0f + tau / dt);
}

static void smooth(struct track *t, const float *x, const float *y, float dt, bool start)
{
	const float min_cutoff = 1.2f, beta = 0.015f, d_cutoff = 1.0f;

	for (int i = 0; i < HAND_POINTS; i++) {
		if (start) {
			t->fx[i] = x[i], t->fy[i] = y[i], t->fdx[i] = t->fdy[i] = 0;
			continue;
		}
		float ad = euro_alpha(d_cutoff, dt);

		t->fdx[i] += ad * ((x[i] - t->fx[i]) / dt - t->fdx[i]);
		t->fdy[i] += ad * ((y[i] - t->fy[i]) / dt - t->fdy[i]);
		float speed = sqrtf(t->fdx[i] * t->fdx[i] + t->fdy[i] * t->fdy[i]);
		float a = euro_alpha(min_cutoff + beta * speed, dt);

		t->fx[i] += a * (x[i] - t->fx[i]);
		t->fy[i] += a * (y[i] - t->fy[i]);
	}
}

/* --- one frame ------------------------------------------------------------- */

/* Slot i stops tracking; counted as a lost hand if it was one. */
static void drop(int i, bool count)
{
	if (tracks[i].on && count) {
		work.lost++;
	}
	tracks[i].on = false;
	tracks[i].shown = tracks[i].cand = G_NONE;
	tracks[i].cand_count = 0;
	work.hand[i].valid = false;
	work.hand[i].gesture = G_NONE;
}

/* The landmark network's outputs: two single values (handedness, presence)
 * and two sets of 63 (points in crop pixels, points in metres). The pixel
 * set is the one with values beyond a few units. Presence is scaled to 0..1
 * from the model's 0..0.5. */
static bool read_landmarks(float *pts, float *presence, float *handedness)
{
	const float *one[2] = {NULL, NULL}, *sets[2] = {NULL, NULL};
	int n1 = 0, n63 = 0;

	for (int i = 0; i < 4; i++) {
		uint32_t n;
		const float *o = npu_output(NPU_HAND, i, &n);

		if (o == NULL) {
			continue;
		}
		if (n == 1 && n1 < 2) {
			one[n1++] = o;
		} else if (n == 63 && n63 < 2) {
			sets[n63++] = o;
		}
	}
	if (n1 < 2 || n63 < 2) {
		return false;
	}
	float m0 = 0, m1 = 0;

	for (int i = 0; i < 63; i++) {
		m0 = MAX(m0, fabsf(sets[0][i]));
		m1 = MAX(m1, fabsf(sets[1][i]));
	}
	memcpy(pts, m0 > m1 ? sets[0] : sets[1], 63 * sizeof(float));
	*presence = MIN(*one[HANDS_PRESENCE_OUTPUT] / PRESENCE_FULL, 1.0f);
	*handedness = *one[HANDS_HANDEDNESS_OUTPUT];
	return true;
}

static void release(bool sim)
{
	if (!sim) {
		camera_release_nn();
	}
}

/* The end of a grabbed frame: what it found, for the host. */
static void grab_finish(struct hands_grab *g, const struct roi *q, float presence)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	g->n = grab.n + 1;
	g->frame = grab.frame, g->palm_in = grab.palm_in, g->hand_in = grab.hand_in;
	g->palm_score = work.palm_score;
	g->presence = presence;
	if (q != NULL) {
		g->cx = q->cx, g->cy = q->cy, g->size = q->size, g->rot = q->rot;
	}
	grab = *g;
	k_spin_unlock(&lock, key);
	atomic_clear(&grab_req);
}

/* Most of b's points close to a's: b is a part of hand a read as a hand of
 * its own (a half-closed hand found once whole and once as a fist on its
 * curled fingers, seen 2026-11-16: the crops overlapped too little to tell). */
static bool inside(const struct hand *a, const struct hand *b, float near)
{
	int close = 0;

	for (int i = 0; i < HAND_POINTS; i++) {
		for (int k = 0; k < HAND_POINTS; k++) {
			float dx = b->x[i] - a->x[k], dy = b->y[i] - a->y[k];

			if (dx * dx + dy * dy < near * near) {
				close++;
				break;
			}
		}
	}
	return close > HAND_POINTS / 2;
}

/* Slot i from this frame's landmarks of its hand, found in crop q. */
static void update(int i, const float *pts, float presence, float handed, const struct roi *q, uint32_t now)
{
	struct track *t = &tracks[i];
	struct hand *h = &work.hand[i];
	bool start = !t->on;
	float x[HAND_POINTS], y[HAND_POINTS];

	/* handedness wavers from frame to frame: averaged over the track */
	h->handedness = start ? handed : h->handedness + 0.2f * (handed - h->handedness);
	/* crop pixels to camera pixels */
	float c = cosf(q->rot), s = sinf(q->rot);

	for (int k = 0; k < HAND_POINTS; k++) {
		float a = (pts[k * 3] / HAND_N - 0.5f) * q->size, b = (pts[k * 3 + 1] / HAND_N - 0.5f) * q->size;

		x[k] = q->cx + a * c - b * s;
		y[k] = q->cy + a * s + b * c;
	}
	landmark_roi(x, y, &t->roi);
	t->pcx = (x[0] + x[5] + x[9] + x[13] + x[17]) / 5;
	t->pcy = (y[0] + y[5] + y[9] + y[13] + y[17]) / 5;
	t->psize = sqrtf((x[9] - x[0]) * (x[9] - x[0]) + (y[9] - y[0]) * (y[9] - y[0]));
	smooth(t, x, y, MAX((now - t->t_us) / 1e6f, 0.005f), start);
	t->t_us = now;
	memcpy(h->x, t->fx, sizeof(h->x));
	memcpy(h->y, t->fy, sizeof(h->y));

	int nf;
	enum hand_gesture gest = classify(pts, x, y, &nf);

	if (gest == t->cand) {
		if (++t->cand_count >= 3) {
			t->shown = gest;
		}
	} else {
		t->cand = gest;
		t->cand_count = 1;
	}
	h->gesture = t->shown;
	h->fingers = nf;
	h->presence = presence;
	h->valid = true;
	t->on = true;
}

void hands_step(void)
{
	uint32_t seq, t0, t_step, us;
	struct hands_grab g = {0};
	struct roi q[HANDS_MAX], gq = {0};
	int slot[HANDS_MAX], jobs = 0, on = 0;
	bool fresh[HANDS_MAX] = {false};
	float gpresence = 0;

	int mode = atomic_get(&simulate);
	bool sim = mode != 0;

	if (sim) {
		k_msleep(33);
	} else if (camera_wait_frame(K_MSEC(100)) != 0) {
		return;
	}
	if (!gfx_borrow_begin()) {
		return; /* the display has not lent its buffer yet, or is taking it back */
	}
	if (npu_prepare() != 0) {
		work.error = npu_error();
		publish();
		gfx_borrow_end();
		k_msleep(500);
		return;
	}
	work.error = NULL;
	const uint16_t *f = sim ? sim_picture(mode) : camera_acquire_nn(&seq);

	if (f == NULL) {
		if (!sim) {
			camera_release_nn();
		}
		gfx_borrow_end();
		return;
	}
	if (!sim) {
		sys_cache_data_invd_range((void *)f, CAM_W * CAM_H * 2);
	}
	t_step = k_cycle_get_32();
	if (atomic_cas(&reset, 1, 0)) {
		for (int i = 0; i < HANDS_MAX; i++) {
			drop(i, false);
		}
	}
	bool grabbing = atomic_get(&grab_req) != 0 && grab_buffers();

	if (grabbing) {
		memcpy((void *)grab.frame, f, CAM_W * CAM_H * 2);
	}
	/* the hands tracked follow their landmarks */
	for (int i = 0; i < HANDS_MAX; i++) {
		if (tracks[i].on && on == max_hands) {
			drop(i, false); /* the maximum was lowered */
		} else if (tracks[i].on) {
			q[jobs] = tracks[i].roi, slot[jobs++] = i, on++;
		}
	}
	/* and the palm detector looks for more: on every frame with none */
	uint32_t now_ms = k_uptime_get_32();

	if (on < max_hands && (on == 0 || now_ms - last_search_ms >= search_ms)) {
		struct roi found[HANDS_MAX];

		last_search_ms = now_ms;
		t0 = k_cycle_get_32();
		palm_input(f, npu_input(NPU_PALM));
		work.prep_us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
		if (grabbing) {
			memcpy((void *)grab.palm_in, npu_input(NPU_PALM), PALM_N * PALM_N * 3);
			g.has_palm = true;
		}
		if (npu_infer(NPU_PALM, &us) != 0) {
			release(sim);
			gfx_borrow_end();
			return;
		}
		work.palm_us = us;
		work.searches++;
		uint32_t n;
		const float *score = npu_output(NPU_PALM, 0, &n);
		const float *box = npu_output(NPU_PALM, 1, &n);
		int nf = palm_rois(score, box, found, max_hands - on, &work.palm_score);

		work.palms += nf;
		for (int k = 0, i = 0; k < nf; k++) {
			/* into a free slot */
			while (tracks[i].on || fresh[i]) {
				i++;
			}
			q[jobs] = found[k], slot[jobs++] = i, fresh[i] = true;
		}
	}
	/* each hand: crop, landmarks (the networks' inputs share one place: in turn) */
	uint32_t now = gfx_now_us();

	for (int j = 0; j < jobs; j++) {
		int i = slot[j];

		t0 = k_cycle_get_32();
		hand_input(f, &q[j], npu_input(NPU_HAND));
		work.prep_us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
		if (grabbing && !g.has_hand) {
			memcpy((void *)grab.hand_in, npu_input(NPU_HAND), HAND_N * HAND_N * 3);
			g.has_hand = true;
			gq = q[j];
		}
		if (j == jobs - 1) {
			release(sim); /* the last crop is taken */
		}
		float pts[63], presence = 0, handed = 0.5f;
		int r = npu_infer(NPU_HAND, &us);

		if (r == 0) {
			work.hand_us = us;
			r = read_landmarks(pts, &presence, &handed) ? 0 : -EIO;
		}
		if (j == 0) {
			gpresence = presence;
		}
		if (r != 0 || presence < presence_min) {
			drop(i, true);
			work.hand[i].presence = presence;
			continue;
		}
		if (fresh[i]) {
			work.confirmed++;
		}
		update(i, pts, presence, handed, &q[j], now);
	}
	if (jobs == 0) {
		release(sim);
	}
	gfx_borrow_end();
	if (grabbing) {
		grab_finish(&g, g.has_hand ? &gq : NULL, gpresence);
	}
	/* two tracks on one hand (hands crossed, a palm taken twice, a part of a
	 * hand taken for one): the newer goes, or the smaller */
	if (HANDS_MAX == 2 && tracks[0].on && tracks[1].on) {
		float near = 0.3f * MAX(tracks[0].psize, tracks[1].psize);

		if (roi_overlap(&tracks[0].roi, &tracks[1].roi) > SAME_HAND ||
		    inside(&work.hand[0], &work.hand[1], near) || inside(&work.hand[1], &work.hand[0], near)) {
			int i = fresh[0] != fresh[1] ? (fresh[0] ? 0 : 1)
						     : (tracks[0].psize < tracks[1].psize ? 0 : 1);

			drop(i, false);
			work.merged++;
		}
	}
	work.n = 0;
	for (int i = 0; i < HANDS_MAX; i++) {
		work.n += tracks[i].on;
	}
	work.step_us = k_cyc_to_us_floor32(k_cycle_get_32() - t_step);
	if (work.n == 0) {
		rate_t0 = now, rate_n = 0;
	} else if (++rate_n >= 15) {
		work.fps_x10 = (uint32_t)((uint64_t)rate_n * 10000000 / MAX(now - rate_t0, 1u));
		rate_t0 = now, rate_n = 0;
	}
	publish();
}
