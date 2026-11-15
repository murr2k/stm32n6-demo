/*
 * Explorer dataset builder and range queries. See dataset.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <arm_math.h>

#include "dataset.h"
#include "platform/psram.h"

LOG_MODULE_REGISTER(dataset, LOG_LEVEL_INF);

#define TWO_PI 6.28318530718f
#define CHUNK  4096

static int16_t *raw[DS_CH];
static struct ds_node *lvl[DS_CH][DS_LEVELS];
static volatile bool ready;
static volatile uint32_t done_samples;
static uint32_t build_ms;
static uint32_t rng = 0x9e3779b9;

static const char *const names[DS_CH] = {"VIBRATION", "PHASE CURRENT", "PRESSURE", "PWM DRIVE"};

static inline uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static inline float frand(void) /* -1..1 */
{
	return (float)(int32_t)rnd() * (1.0f / 2147483648.0f);
}

static inline float urand(void) /* 0..1 */
{
	return (float)(rnd() >> 8) * (1.0f / 16777216.0f);
}

static inline float wrap(float ph)
{
	return ph - floorf(ph);
}

/* ---- generators: each keeps its own state across chunks ------------------ */

struct gen {
	float ph, ph2, env, f, level, target, slew, lp, drift;
	uint32_t next_event;
};

/* ch0: structure-borne ring-downs. An impulse every 0.3 to 2 s excites a
 * damped resonance; in between there is only a noise floor.
 */
static float gen_vibration(struct gen *g, uint32_t i)
{
	if (i >= g->next_event) {
		g->env = 0.35f + 0.6f * urand();
		g->f = 180.0f + 720.0f * urand();
		g->slew = expf(-1.0f / ((0.02f + 0.06f * urand()) * DS_FS)); /* decay per sample */
		g->next_event = i + (uint32_t)((0.3f + 1.7f * urand()) * DS_FS);
	}
	g->ph = wrap(g->ph + g->f / DS_FS);
	g->env *= g->slew;
	return g->env * arm_sin_f32(TWO_PI * g->ph) + 0.012f * frand();
}

/* ch1: 50 Hz phase current whose amplitude follows load steps. */
static float gen_current(struct gen *g, uint32_t i)
{
	if (i >= g->next_event) {
		g->target = 0.15f + 0.75f * urand();
		g->next_event = i + (uint32_t)((2.0f + 4.0f * urand()) * DS_FS);
	}
	g->level += (g->target - g->level) * 0.0012f;
	g->ph = wrap(g->ph + 50.0f / DS_FS);
	return g->level * (arm_sin_f32(TWO_PI * g->ph) + 0.08f * arm_sin_f32(TWO_PI * wrap(g->ph * 5.0f))) +
	       0.006f * frand();
}

/* ch2: slow process variable: filtered random walk, drift, rare spikes. */
static float gen_pressure(struct gen *g, uint32_t i)
{
	g->lp += 0.0009f * frand();
	g->lp *= 0.99996f;
	g->drift = wrap(g->drift + 0.021f / DS_FS);
	float spike = 0.0f;

	if (i >= g->next_event) {
		g->env = (urand() > 0.5f ? 0.45f : -0.45f);
		g->next_event = i + (uint32_t)((5.0f + 14.0f * urand()) * DS_FS);
	}
	spike = g->env;
	g->env *= 0.9985f;
	return CLAMP(8.0f * g->lp + 0.3f * arm_sin_f32(TWO_PI * g->drift) + spike + 0.004f * frand(), -1.0f, 1.0f);
}

/* ch3: 20 Hz PWM whose duty tracks a slow command, with edge ringing. */
static float gen_pwm(struct gen *g, uint32_t i)
{
	if (i >= g->next_event) {
		g->target = 0.1f + 0.8f * urand();
		g->next_event = i + (uint32_t)((1.5f + 3.0f * urand()) * DS_FS);
	}
	g->level += (g->target - g->level) * 0.0008f;
	float prev = g->ph;

	g->ph = wrap(g->ph + 20.0f / DS_FS);
	bool high = g->ph < g->level;

	if ((prev < g->level) != high || g->ph < prev) {
		g->env = 0.18f; /* an edge: start the ringing */
		g->ph2 = 0.0f;
	}
	g->ph2 = wrap(g->ph2 + 900.0f / DS_FS);
	g->env *= 0.985f;
	return (high ? 0.7f : -0.7f) + g->env * arm_sin_f32(TWO_PI * g->ph2) + 0.004f * frand();
}

static void build_pyramid(int ch)
{
	uint32_t n = DS_N / 4;
	const int16_t *s = raw[ch];
	struct ds_node *d = lvl[ch][0];

	for (uint32_t k = 0; k < n; k++, s += 4) {
		int16_t lo = MIN(MIN(s[0], s[1]), MIN(s[2], s[3]));
		int16_t hi = MAX(MAX(s[0], s[1]), MAX(s[2], s[3]));

		d[k].lo = lo;
		d[k].hi = hi;
	}
	for (int l = 1; l < DS_LEVELS; l++) {
		const struct ds_node *src = lvl[ch][l - 1];

		n /= 4;
		d = lvl[ch][l];
		for (uint32_t k = 0; k < n; k++, src += 4) {
			d[k].lo = MIN(MIN(src[0].lo, src[1].lo), MIN(src[2].lo, src[3].lo));
			d[k].hi = MAX(MAX(src[0].hi, src[1].hi), MAX(src[2].hi, src[3].hi));
		}
		k_yield();
	}
}

static void build_thread(void *p1, void *p2, void *p3)
{
	static float (*const fn[DS_CH])(struct gen *, uint32_t) = {gen_vibration, gen_current,
								   gen_pressure, gen_pwm};
	uint32_t t0 = k_uptime_get_32();

	for (int ch = 0; ch < DS_CH; ch++) {
		raw[ch] = psram_alloc(DS_N * sizeof(int16_t), 32);
		for (int l = 0, n = DS_N / 4; l < DS_LEVELS; l++, n /= 4) {
			lvl[ch][l] = psram_alloc(n * sizeof(struct ds_node), 32);
		}
		if (raw[ch] == NULL || lvl[ch][DS_LEVELS - 1] == NULL) {
			LOG_ERR("out of PSRAM");
			return;
		}
	}
	for (int ch = 0; ch < DS_CH; ch++) {
		struct gen g = {0};

		for (uint32_t i = 0; i < DS_N; i += CHUNK) {
			for (uint32_t j = i; j < i + CHUNK; j++) {
				raw[ch][j] = (int16_t)(fn[ch](&g, j) * 30000.0f);
			}
			done_samples += CHUNK;
			k_yield();
		}
		build_pyramid(ch);
	}
	build_ms = k_uptime_get_32() - t0;
	ready = true;
	LOG_INF("%u samples x %d channels + pyramid built in %u ms", DS_N, DS_CH, build_ms);
}
/* Preemptible and below the UI thread: building must not cost a frame. */
K_THREAD_DEFINE(ds_build, 4096, build_thread, NULL, NULL, NULL, 12, 0, 1500);

bool ds_ready(void)
{
	return ready;
}

int ds_progress_pct(void)
{
	return (int)((uint64_t)done_samples * 100 / (DS_N * DS_CH));
}

uint32_t ds_build_ms(void)
{
	return build_ms;
}

const char *ds_name(int ch)
{
	return names[ch];
}

int16_t ds_sample(int ch, uint32_t i)
{
	return raw[ch][MIN(i, DS_N - 1)];
}

int ds_range(int ch, uint32_t s0, uint32_t s1, int16_t *lo, int16_t *hi)
{
	uint32_t span;
	int16_t l = INT16_MAX, h = INT16_MIN;

	s1 = MIN(s1, DS_N);
	s0 = MIN(s0, s1 ? s1 - 1 : 0);
	span = s1 - s0;

	if (span < 8) {
		const int16_t *p = raw[ch] + s0;

		for (uint32_t i = 0; i < MAX(span, 1u); i++) {
			l = MIN(l, p[i]);
			h = MAX(h, p[i]);
		}
		*lo = l;
		*hi = h;
		return -1;
	}
	/* largest node that is no bigger than half the span: 2 to 8 nodes per query */
	int level = 0;
	uint32_t shift = 2;

	while (level < DS_LEVELS - 1 && (4u << shift) <= span) {
		level++;
		shift += 2;
	}
	const struct ds_node *n = lvl[ch][level];
	uint32_t k1 = (s1 - 1) >> shift;

	for (uint32_t k = s0 >> shift; k <= k1; k++) {
		l = MIN(l, n[k].lo);
		h = MAX(h, n[k].hi);
	}
	*lo = l;
	*hi = h;
	return level;
}

const struct ds_node *ds_top(int ch, int *count)
{
	*count = DS_N >> (2 * DS_LEVELS);
	return lvl[ch][DS_LEVELS - 1];
}
