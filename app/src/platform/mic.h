/*
 * The board's MEMS microphone (U13): a PDM bit stream turned into 48 kHz PCM
 * by the MDF1 digital filter, read by a circular DMA transfer.
 *
 * Samples are 24-bit signed, full scale +/- 2^23. The sample clock comes from
 * the same PLL output as the audio output's, so one microphone sample is
 * exactly one DAC frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_MIC_H
#define PLATFORM_MIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIC_RATE 48000 /* nominal; see audio.h for the real figure of this clock */
/* Samples per DMA half, handed over at once: 128 is 2.7 ms. The newest
 * samples wait for their half to complete, so this is the capture's share of
 * the delay from the microphone to the headphones (dsp/monitor.h). */
#define MIC_BLOCK 128

struct mic_status {
	bool ok;
	uint32_t total;      /* samples captured since the start */
	uint32_t blocks;     /* DMA halves delivered */
	uint32_t errors;     /* filter and DMA errors */
	uint32_t last_error; /* HAL MDF error bits of the last one */
	uint32_t rate_hz;    /* measured over the last second or so of blocks */
	int32_t peak;        /* largest magnitude in the last block */
	int32_t rms;         /* RMS of the last block */
	int32_t dc;          /* mean of the last block */
};

int mic_init(void);
bool mic_ok(void);
void mic_get_status(struct mic_status *st);

/* Samples captured so far. Samples total - MIC_KEEP .. total - 1 can be read
 * (85 ms: Signal Lab reads at most 2048 behind, the monitor about 1060).
 */
#define MIC_KEEP 4096
uint32_t mic_total(void);
/* Copy n samples starting at sample number `from`; returns how many were
 * still (or already) there.
 */
uint32_t mic_copy(uint32_t from, int32_t *dst, uint32_t n);

/* Bring-up: restart the capture with other settings. v[0..n-1] are, in this
 * order: low-frequency master mode (0/1), falling edge (0/1), processing clock
 * divider, microphone clock divider, sinc order (4/5), CIC decimation, gain
 * step, kernel clock (0 IC7, 1 HCLK, 2 HSI, 3 CLKP, 4 IC8, 5 MSI), pull on the
 * data pin (0 none, 1 up, 2 down), drive strength of the clock pin (0..3).
 * The defaults are 1 1 1 4 5 16 1 0 0 3. n = 0 only reports the settings and
 * the filter's registers. The output rate is kernel clock / processing
 * divider / microphone clock divider / (4 x decimation).
 */
int mic_tune(const int *v, int n, char *buf, size_t len);

/* Bring-up: stop the capture, hold the clock pin low and then high, and read
 * the data line against a pull-up and a pull-down each time; restarts the
 * capture. Takes two seconds.
 */
int mic_probe(char *buf, size_t len);

/* Bring-up: the raw bit stream, clocked by hand at khz and read delay_ns after
 * each edge, kbits thousand bits per edge, with a pull on the data pin (0, 1
 * up, 2 down). Blocks everything for the duration. See mic.c for the layout.
 */
int mic_bitbang(uint32_t kbits, uint32_t delay_ns, uint32_t khz, int pull, char *buf, size_t len);

/* Bring-up: a logic-analyser view of the clock and data pins while the MDF
 * runs (mic.c has the word layout).
 */
int mic_scope(uint32_t n, char *buf, size_t len);

/* Self-test of the capture chain from the data pin on: with clock_off the
 * microphone's clock pin is held low (the microphone sleeps and lets go of the
 * data line), and a timer flips the data pin's own pull-up and pull-down at
 * 2 x hz: a square wave on the line, read by the MDF like microphone bits (the
 * pin stays in its MDF input mode). hz 0: no flipping, the pull stays as
 * `pull` says (0 none, 1 up, 2 down). The MDF must sample with its own clock
 * for this (mictune with low-frequency master mode). micinject 0 0 0 ends it.
 */
int mic_inject(int hz, int clock_off, int pull, char *buf, size_t len);

/* Self-test with a synthetic microphone: a sine of hz at amp_pct of full
 * scale, through a second-order sigma-delta modulator, put on the data pin at
 * rate_khz entries a second by DMA (mic.c). mode 1 drives the pin (which on
 * the N6 cuts the MDF off it: kept to show that), mode 2 flips its pulls (the
 * pin stays an MDF input), mode 3 the same with return to zero (two entries a
 * bit, linear despite the pulls' slow edges); U13 sleeps meanwhile. The MDF
 * must sample with its own clock (mictune, low-frequency master mode). mode 0
 * gives the pins back to the microphone.
 */
int mic_pdm(int hz, int amp_pct, int mode, int rate_khz, char *buf, size_t len);

/* Test aid: record the next ms (up to 3 s) into PSRAM as 32-bit words. Writes
 * "addr= frames= filled=" to buf; ms = 0 only reports (tools/acap.py --mic).
 */
int mic_capture(uint32_t ms, char *buf, size_t len);

#endif
