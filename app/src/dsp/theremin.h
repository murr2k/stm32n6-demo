/*
 * Theremin: a synthesizer played on Signal Lab's spectrum.
 *
 * A fingertip on the spectrum sounds a tone at the spectrum's own frequency
 * under it (the log axis, sig_colf_hz) and at the spectrum's own level at its
 * height (a loudness taper, thm_u_db), so the analyser shows its peak under
 * the finger. One voice per touch slot, up to five. A finger plays from where
 * it comes down on the field until it lifts: off the field's top or bottom it
 * keeps its last level. The synthesizer is the audio output's
 * source while it is enabled and reads the touch state itself at the start of
 * each audio block (AUDIO_BLOCK_FRAMES), from the audio thread: a touch reaches the sound in
 * one or two blocks, not through the UI frame. Every change is a ramp (pitch
 * glides in log frequency, levels follow, a lift decays), so the sound never
 * steps. What it plays is kept in a ring for Signal Lab's analysis.
 *
 * In attract mode a ghost plays it instead: a flying saucer. With the sound
 * switched off (theremin_set_sound) it still plays and is analysed, and the
 * DAC gets silence.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DSP_THEREMIN_H
#define DSP_THEREMIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define THM_VOICES 5        /* one per touch slot */
#define THM_TOP_DB (-6.0f)  /* the loudest a voice gets: the top of the field */
/* Under the attract lap's music the ghost plays this much quieter: about 5 dB
 * under the break it plays over (the music -19.7 dBFS there, the ghost alone
 * about -17). Set with theremin_set_under_db (ctl "thmunder"). */
#define THM_UNDER_DB (-8.0f)

/* The level axis, a loudness taper: equal steps of height are equal steps of
 * loudness (it doubles every 10 dB), THM_TOP_DB at the top of the field and
 * silence at its floor. u is the height above the floor, 0..1. The touch and
 * the spectrum's level axis in instrument mode both use it. */
float thm_u_db(float u);
float thm_db_u(float db);

enum thm_wave { THM_SINE, THM_THEREMIN, THM_TRIANGLE, THM_SAW, THM_WAVES };
enum thm_mode { THM_TOUCH, THM_GHOST, THM_REMOTE };

struct thm_voice_view {
	bool on;    /* sounding, or still fading */
	bool held;  /* a finger (or the ghost, or a remote command) holds it */
	float hz;   /* pitch now, without vibrato */
	float db;   /* level of the fundamental now, dBFS */
};

struct thm_status {
	bool enabled;
	int mode;              /* enum thm_mode */
	int wave;              /* enum thm_wave */
	bool snap;
	bool sound;            /* false: it plays to the analysis only, the DAC gets silence */
	bool under;            /* mixed under another source (the attract lap's music) */
	float under_db;        /* its level there, relative to its own */
	float rate_hz;         /* the DAC's sample rate, measured */
	uint32_t blocks;       /* blocks synthesized */
	uint32_t synth_us;     /* the last block's synthesis time, on the audio thread */
	uint32_t synth_worst_us;
	uint32_t lat_last_us;  /* touch report to the first sample of the note at the DAC */
	uint32_t lat_worst_us;
	uint32_t lat_n;        /* notes measured */
	struct thm_voice_view v[THM_VOICES];
};

void theremin_init(void);
/* Take the audio output (true), or give it back after a short fade (false). */
void theremin_enable(bool on);
/* UI thread, once per frame: measures the DAC's rate, finishes a give-back. */
void theremin_service(void);
/* The playing field in panel coordinates: the spectrum pane (x1..x2, y1..y2,
 * inclusive) and the columns a touch may play (play_x0..play_x1). */
void theremin_set_field(int x1, int y1, int x2, int y2, int play_x0, int play_x1);
void theremin_set_wave(int wave);
const char *theremin_wave_name(int wave);
void theremin_set_snap(bool on);
/* The SOUND switch: off fades the output to silence in 10 ms (touch, ghost and
 * remote alike); the analysis still gets what it plays. On at power-up. */
void theremin_set_sound(bool on);
void theremin_ghost(bool on);
/* The ghost's three-bar flight for the attract lap (7.2 s) instead of its 13 s loop. */
void theremin_ghost_short(bool on);
/* Under other sound (true): the next theremin_enable(true) mixes the theremin
 * over the audio output's source at theremin_under_db() instead of taking the
 * output (audio_set_overlay). The analysis still gets its own level. false
 * while it plays under: it takes the output 60 ms later, at its own level
 * (attract mode ended and the music with it). */
void theremin_set_overlay(bool on);
void theremin_set_under_db(float db);
float theremin_under_db(void);
/* Remote control (ctl "thm"): voice v to hz at db, hz 0 lifts it; v < 0 hands
 * the voices back to the touch panel. */
void theremin_remote(int v, float hz, float db);
void theremin_get(struct thm_status *st);
float theremin_rate(void);

/* What it played, mono 16-bit, like mic_total()/mic_copy(). */
uint32_t theremin_total(void);
uint32_t theremin_copy(uint32_t from, int16_t *dst, uint32_t n);

/* Note name and cents for a pitch, e.g. "A4" or "C#5 +12". */
void theremin_note(float hz, char *buf, size_t len);

#endif
