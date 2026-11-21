/*
 * Read-only data that interrupt handlers get as their argument, placed in
 * rom_start, ahead of Zephyr's symbol table (CMakeLists.txt says why).
 * Zephyr's STM32 GPIO interrupt driver passes each EXTI line's range
 * (line_range_N, static const) to stm32_intc_gpio_isr.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
. = ALIGN(4);
KEEP(*(.rodata.line_range_*))
