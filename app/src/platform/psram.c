/*
 * PSRAM bump allocator.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "psram.h"

#define PSRAM_BASE 0x90000000u
/* The last 32 bytes are off limits: STM32N6 errata ES0620 2.4.5. */
#define PSRAM_END  (PSRAM_BASE + MB(32) - 32)
/* The top 4 MB are the NPU's working memory (tools/nn.py ACT_BASE). */
#define PSRAM_TOP  PSRAM_NPU_BASE
/* The linker's PSRAM section (Z_GENERIC_SECTION(PSRAM): the video layer's
 * buffer pool, the NPU runtime's blob copies) comes first; the allocator
 * starts after it. It used to start 2 MB in, with gfx.c's screenshot copy at
 * the very start, on top of the linker's data. */
extern char __PSRAM_end[];

static uintptr_t next = (uintptr_t)__PSRAM_end;
static struct k_spinlock lock;

void *psram_alloc(size_t bytes, size_t align)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uintptr_t p = ROUND_UP(next, MAX(align, 32)); /* at least one cache line */
	void *ret = NULL;

	if (p + bytes <= PSRAM_TOP) {
		next = p + bytes;
		ret = (void *)p;
	}
	k_spin_unlock(&lock, key);
	return ret;
}

size_t psram_used(void)
{
	return next - PSRAM_BASE;
}

size_t psram_size(void)
{
	return PSRAM_TOP - PSRAM_BASE;
}
