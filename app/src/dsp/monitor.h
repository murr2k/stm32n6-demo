/*
 * Microphone monitor: the microphone to the headphones through a five-band
 * parametric equalizer, to judge the sound by ear.
 *
 * While Signal Lab shows with MICROPHONE selected, the monitor is the audio
 * output's source, as the theremin is with THEREMIN. It reads the
 * microphone's ring a fixed distance behind the newest sample: the microphone
 * and the DAC run from the same kernel clock (IC7), one microphone sample is
 * one DAC frame, and that distance stays put. Per audio block, on the
 * audio thread: gain, five filters (low shelf, three peaks, high shelf;
 * state-variable filters in float, the cookbook's responses) whose settings
 * are eased every 32 samples so that a dragged band never clicks, a peak
 * limiter, then the LISTEN fade. What it makes goes into a ring for Signal
 * Lab's analysis, so the spectrum shows what is heard; with LISTEN off the
 * DAC gets silence and the analysis still runs. LISTEN is off when the
 * monitor starts.
 *
 * For a closed-loop test (headphones next to the board) the monitor can send
 * a test tone to the DAC instead of the microphone (monitor_tone): the
 * microphone hears it, and Goertzel filters at its frequency measure it
 * before and after the equalizer. Their difference is the equalizer's gain
 * there as it plays, whatever the path through the air does.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DSP_MONITOR_H
#define DSP_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#define MON_BANDS   5
#define MON_DB_MAX  15.0f   /* a band's gain, either way */
#define MON_HZ_MIN  20.0f
#define MON_HZ_MAX  20000.0f
#define MON_Q_MIN   0.3f
#define MON_Q_MAX   12.0f
#define MON_CEIL_DB (-6.0f) /* the limiter's ceiling: a clap at the board must not hurt */

enum mon_type { MON_LOW_SHELF, MON_PEAK, MON_HIGH_SHELF };

struct mon_band {
	float hz;
	float db;
	float q; /* peaks: Q; shelves: the RBJ shelf's Q (0.71 is no overshoot) */
};

struct mon_status {
	bool enabled;          /* the audio output's source (also while fading out) */
	bool listen;           /* the DAC gets it */
	bool eq;               /* the equalizer is in (off: bypassed, for comparison) */
	int gain_db;           /* before the equalizer, on 24-bit full scale */
	uint32_t version;      /* changes with every setting below */
	struct mon_band band[MON_BANDS];
	uint32_t blocks;       /* blocks made */
	uint32_t resyncs;      /* the read position had to be set again (a gap in the microphone data) */
	uint32_t lag;          /* microphone samples between the read position and the newest, last block */
	uint32_t lag_min, lag_max;
	uint32_t delay_us;     /* microphone to DAC, about: the lag plus the DAC's queue */
	uint32_t dsp_us;       /* the last block's processing time on the audio thread */
	uint32_t dsp_worst_us;
	float reduce_db;       /* the limiter's gain reduction now (0 or less) */
	uint32_t limited;      /* blocks in which the limiter acted */
	float tone_hz;         /* the test tone, 0 when off */
	float tone_db;         /* its level at the DAC, dBFS */
	float tone_pre_db;     /* it in the microphone, after the gain and before the equalizer, dBFS */
	float tone_post_db;    /* the same after the equalizer */
};

void monitor_init(void);
/* Take the audio output (true), or give it back after a short fade (false). */
void monitor_enable(bool on);
/* UI thread, once per frame: finishes a give-back. */
void monitor_service(void);
/* Enabled and not on its way out: the analysis may read monitor_copy(). */
bool monitor_running(void);
bool monitor_enabled(void);

void monitor_set_listen(bool on);
void monitor_set_gain(int db);   /* 0 to 50 */
void monitor_set_eq(bool on);
void monitor_set_band(int i, float hz, float db, float q);
void monitor_tone(float hz, float db); /* the test tone to the DAC instead of the microphone; hz 0 ends it */
void monitor_flat(void);         /* every band to 0 dB, at its frequency */
void monitor_reset(void);        /* the bands where they start, flat */
int monitor_band_type(int i);    /* enum mon_type */
const char *monitor_band_name(int i);
void monitor_get(struct mon_status *st);

/* The equalizer's response in hundredths of a dB at Signal Lab's display
 * columns 0..n-1 (sig_col_hz), for the bands in st (what is set, not what is
 * still easing). About 0.2 ms for 768 columns. */
void monitor_curve(const struct mon_status *st, int16_t *cdb, int n);
/* The same at one frequency, in dB. */
float monitor_response_db(const struct mon_status *st, float hz);

/* What it made, mono 16-bit, like theremin_total()/theremin_copy(). */
uint32_t monitor_total(void);
uint32_t monitor_copy(uint32_t from, int16_t *dst, uint32_t n);

#endif
