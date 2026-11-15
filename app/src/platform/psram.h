/*
 * PSRAM allocator. The 32 MB HexaSPI PSRAM holds everything large that is not
 * touched per pixel per frame: datasets, histories, video buffers.
 *
 * Allocations are permanent (bump allocator): the demo's big buffers live for
 * the life of the firmware, and a map that never fragments is worth more here
 * than free().
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PLATFORM_PSRAM_H
#define PLATFORM_PSRAM_H

#include <stddef.h>
#include <stdint.h>

/* Returns NULL when the PSRAM is exhausted. align must be a power of two. */
void *psram_alloc(size_t bytes, size_t align);
size_t psram_used(void);
size_t psram_size(void);

#endif
