/*
 * Audio output: 48 kHz stereo to the WM8904 codec (headphone jack) over SAI1.
 *
 * The stream runs continuously once started; a source callback supplies PCM
 * and silence is sent when there is none. The DAC's sample clock comes from a
 * different PLL than the system clock, so anything that must stay in step
 * with the sound (video) reads its time from audio_queued_frames(), not from
 * the kernel clock.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_AUDIO_H
#define PLATFORM_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_RATE 48000

/* Fill `frames` stereo s16 frames at `dst`. Return how many were written;
 * the rest of the block is sent as silence. Called on the audio thread.
 */
typedef uint32_t (*audio_source_t)(int16_t *dst, uint32_t frames, void *ctx);

int audio_init(void);
bool audio_ok(void);
void audio_set_source(audio_source_t fn, void *ctx);

/* Frames handed to the driver that have not reached the DAC yet, estimated
 * between block boundaries from the kernel clock.
 */
uint32_t audio_queued_frames(void);

/* Blocks of AUDIO_BLOCK_FRAMES handed to the driver, and stream restarts (underruns): the
 * block rate is a direct measurement of the DAC sample clock.
 */
#define AUDIO_BLOCK_FRAMES 512
void audio_get_counters(uint32_t *blocks, uint32_t *errors);

/* Stop and restart the stream, clearing the counters. */
void audio_restart(void);

/* Frame rate measured from the DMA position over eight short windows. */
int audio_rate_probe(char *buf, size_t len);

/* One line of driver state for bring-up. */
int audio_debug(char *buf, size_t len);

/* Play a -12 dBFS test tone on both channels for ms, overriding the source. */
void audio_tone(uint32_t hz, uint32_t ms);

/* 0..100 */
void audio_set_volume(int percent);

/* Peak level of the last block sent, 0..32767 per channel (for meters). */
void audio_get_peak(uint16_t *left, uint16_t *right);

#endif
