/*
 * Recorder: the board records its own screen with the N6's hardware H.264
 * encoder (VENC, VeriSilicon VC8000NanoE), for a video of the demo made by
 * the demo (docs/plan.html, "A video of one attract lap", V1).
 *
 * A client on tcp/5559 starts a recording by connecting and stops it by
 * closing the connection. It receives messages: a struct rec_hdr, then
 * `size` bytes. The first carries the stream's SPS and PPS; then one per
 * picture. A picture is taken at the start of every second refresh of the
 * panel (29.99 fps): the buffer on screen, read by the encoder straight from
 * the SRAM in its 32-bit RGB form, no copy and no conversion by the CPU. The
 * display does not draw into a buffer the encoder is reading
 * (gfx_reader_begin). tools/record.py writes the stream and the pictures'
 * data to captures/rec/ and wraps it in an MP4.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MEDIA_RECORDER_H
#define MEDIA_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REC_PORT 5559

/* REC_AUDIO: what was handed to the DAC from frame audio_pos on, stereo s16
 * frames (size / 4 of them); the tap starts at frame 0 with the recording.
 * A picture's audio_pos is the frame at the DAC when it appeared, so the
 * receiver can put the pictures on the sound's clock. */
enum rec_kind { REC_HEADERS = 1, REC_PICTURE = 2, REC_AUDIO = 3 };

#define REC_FLAG_SINGLE 0x01 /* the display was single-buffered: the picture may tear */

struct rec_hdr {
	uint32_t magic;     /* "N6RV" */
	uint8_t kind;       /* enum rec_kind */
	uint8_t coding;     /* 0 IDR, 1 P */
	uint8_t screen;     /* the shell's screen index */
	uint8_t flags;      /* REC_FLAG_* */
	uint32_t refresh;   /* panel refreshes since the recording began */
	uint32_t t_us;      /* gfx_now_us() when the picture was taken */
	uint32_t encode_us; /* the encoder's time for it */
	uint32_t audio_pos; /* picture: the DAC frame playing as it appeared; sound: its first frame */
	uint32_t size;      /* bytes that follow */
	uint32_t missed;    /* pictures due but not taken so far in this recording */
};

struct rec_status {
	bool recording;
	uint32_t pictures;
	uint32_t missed;      /* due but not taken: no free buffer, or the slot overslept */
	uint32_t held;        /* not taken because the display was single-buffered (the Camera screen) */
	uint32_t errors;      /* the encoder returned an error */
	uint32_t long_waits;  /* 100 ms waits for the hardware past the first (the PSRAM busy with the NPU) */
	uint32_t encode_us, encode_worst_us;
	uint32_t bytes;       /* sent in this recording */
	uint32_t idr_bytes;   /* the last IDR picture's size */
	uint32_t p_bytes;     /* the last P picture's size */
	int last_error;
};

void recorder_get(struct rec_status *st);

#endif
