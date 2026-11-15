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
/* First 2 MB: screenshot snapshot buffer, owned by gfx.c. */
#define PSRAM_HEAP (PSRAM_BASE + MB(2))

static uintptr_t next = PSRAM_HEAP;
static struct k_spinlock lock;

void *psram_alloc(size_t bytes, size_t align)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uintptr_t p = ROUND_UP(next, MAX(align, 32)); /* at least one cache line */
	void *ret = NULL;

	if (p + bytes <= PSRAM_END) {
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
	return PSRAM_END - PSRAM_BASE;
}
