/*
 * Signal path for the Signal Lab: a 48 kHz source, a 1024-point Helium FFT
 * per display frame, a log-frequency spectrum and a waterfall history.
 *
 * There are three sources, and the screen says which one is on: a built-in
 * signal generator, the board's MEMS microphone (platform/mic.h), whose
 * 48 kHz come from the same clock as the audio output's, and the theremin
 * (dsp/theremin.h): what it sends to the DAC, analysed at the DAC's measured
 * rate so that a tone sits on the axis where it sounds.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DSP_SIG_H
#define DSP_SIG_H

#include <stdbool.h>
#include <stdint.h>

#define SIG_FS     48000
#define SIG_FFT    1024
#define SIG_COLS   768      /* display columns, 20 Hz to 20 kHz, log spaced */
#define SIG_SCOPE  768      /* samples in the triggered scope window */
#define SIG_WF_H   164      /* waterfall rows kept */
#define SIG_DB_MIN (-100.0f)

struct sig_view {
	const float *col_db;      /* SIG_COLS, dBFS, attack/release smoothed */
	const float *col_peak;    /* SIG_COLS, peak hold */
	const int16_t *scope;     /* SIG_SCOPE samples starting at a rising zero crossing */
	const uint8_t *waterfall; /* SIG_WF_H rows of SIG_COLS, ring */
	int wf_head;              /* index of the newest waterfall row */
	float peak_hz;
	float peak_db;
	float rms_db;
	uint32_t frames;          /* analysis frames since boot */
	uint32_t fft_us;          /* time of the last window + FFT + magnitude pass */
	const char *source;
};

enum { SIG_SRC_GENERATOR, SIG_SRC_MIC, SIG_SRC_THEREMIN };

void sig_init(void);
/* Choose the source; the microphone only if it is running. */
void sig_set_source(int src);
int sig_source(void);
/* Advance the source to now and analyse one frame. Call once per frame slot. */
void sig_service(uint32_t now_ms);
const struct sig_view *sig_view(void);
/* Run/stop, like a scope: while held, no new samples are analysed. */
void sig_hold(bool hold);
bool sig_held(void);

/* Frequency at a display column, and the inverse. */
float sig_col_hz(int col);
int sig_hz_col(float hz);
/* The same between fractions of a column (no clamping). */
float sig_colf_hz(float col);
float sig_hz_colf(float hz);

#endif
