/*
 * Explorer dataset: 2^20 samples on four channels (8 MB of int16) plus a
 * min/max pyramid (5.6 MB) so any zoom level can be drawn by reading at most
 * a handful of nodes per pixel column.
 *
 * The data is synthetic (and labelled so on screen): machine-like signals
 * with structure at every time scale, which is what makes zooming worthwhile.
 * It is used in place from a copy in the flash, checked at boot by a
 * low-priority thread; without a valid copy that thread generates it into
 * PSRAM (see dataset.c and tools/dataset.py).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DSP_DATASET_H
#define DSP_DATASET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS_N       (1u << 20)
#define DS_CH      4
#define DS_FS      10000      /* nominal sample rate, Hz */
#define DS_LEVELS  5          /* node sizes 4, 16, 64, 256, 1024 samples */

struct ds_node {
	int16_t lo, hi;
};

bool ds_ready(void);
int ds_progress_pct(void);
uint32_t ds_build_ms(void);
const char *ds_name(int ch);
/* True when the data is the copy in flash, false when it was generated into PSRAM. */
bool ds_in_flash(void);
/* One line for the control port: "ready= source= addr= bytes= crc= ms=". */
int ds_info(char *buf, size_t len);

/* Raw sample. */
int16_t ds_sample(int ch, uint32_t i);

/* Min and max over [s0, s1). Returns the pyramid level used, or -1 for raw
 * samples. The range is widened to the node grid of the chosen level, which
 * is at most one node on each side.
 */
int ds_range(int ch, uint32_t s0, uint32_t s1, int16_t *lo, int16_t *hi);

/* Coarsest level, for overviews: DS_N / 1024 nodes. */
const struct ds_node *ds_top(int ch, int *count);

#endif
