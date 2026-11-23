# Moving off Zephyr v4.4.0: what upstream fixed, and what it would cost

**Result:** a survey of upstream on 2026-11-22, read from the sources and
pull requests on GitHub, not built. The demo is pinned to v4.4.0 (684c9e8f,
2026-04-14). v4.5.0 was released on 2026-10-28 (41d10b59); `main` is 4.5.99,
4,041 commits past it. Items marked **main only** are not in v4.5.0.

No workaround in [CLAUDE.md](../CLAUDE.md) can go yet: upstream has fixed
some of the problems behind them, but not the ones the demo works around in
code. A bump buys TCM nodes, the SDMMC RIF setup, LVGL 9.6, the branch cache,
a portable PMU API and a pluggable tracing format, and costs a few build
breaks and a full re-measurement.

## The demo's workarounds, upstream

| Workaround in the demo | Upstream on `main` | Where |
|---|---|---|
| No `ap=` in the SWD connect string, no `west flash` | Not fixed: `ap=1` is still passed. The runner now resets after flashing; this board opts out (`--no-reset`) | `boards/st/stm32n6570_dk/board.cmake:10`, `:11`, `:23` |
| The M55's TCMs are not in the device tree | Fixed, **main only** (#124338): `itcm` (64 KB at 0x1000_0000) and `dtcm` (128 KB at 0x3000_0000); the board chooses `zephyr,dtcm`. Both are zeroed on every reset (ECC), by MCUboot and the application alike | `dts/arm/st/n6/stm32n6.dtsi:1742-1752`, `soc/st/stm32/stm32n6x/soc.c:47-73` |
| SDMMC2 bus master RIF attributes set by `sdcard.c`; `idma` added in the overlay | RIF fixed, **main only** (#124326): set by `soc_rif_config()` with `STM32N6_RIF_OPEN`. The board's `&sdmmc2` still has no `idma`; there is still no RIF lock, so `rif_lock()` stays | `soc.c:118-123` |
| The card watcher, `HAL_SD_Init` wrap (High Speed), data timeout and card-state wraps | Not fixed: the card-detect handler still deinitialises from the system work queue under a transfer; transfers still wait `K_FOREVER`; card-state loops unbounded; no CMD6. The "Failed to configure pins for sleep (-2)" warning is gone (4c2dc25ac15f, v4.5.0). The new SDHC driver for `st,stm32-sdmmc` does not do High Speed either and is untested on the N6 | `drivers/disk/sdmmc_stm32.c:814-838`, `:574`, `:600`, `:1030`; `drivers/sdhc/sdhc_stm32.c:1758-1765` |
| `CONFIG_SYMTAB` moves rodata after the ISR tables are generated (`isr_args.ld`, `check_isr_args`) | Not fixed: the symbol table generation is byte-identical to v4.4.0; the same class of bug is acknowledged for the new switch-case ISR tables (open #125717) | `scripts/build/gen_symtab.py`, `CMakeLists.txt:1496-1508` |
| VENC: the `EWLMallocLinear` chunk leak, cache maintenance at `EWLEnableHW` | Not fixed: the driver is byte-identical to v4.4.0 | `drivers/video/video_stm32_venc.c:46`, `:193-199`, `:257`, `:269-289` |
| `APP_FPU_DOUBLE` (the SoC selects only `CPU_HAS_FPU`) | Not fixed. The flag changed: with double precision the build gets `-mfpu=auto` (DP and MVE-F from `-mcpu=cortex-m55`), without it `-mfpu=fpv5-sp-d16`. CLAUDE.md's check for `-mfpu=fpv5-d16` would change | `soc/st/stm32/stm32n6x/Kconfig:18`, `cmake/compiler/gcc/target_arm.cmake:19-25` |
| `present()` in gfx.c instead of `display_write()` | Not fixed: a full-frame write still flushes the whole buffer and swaps in the line interrupt at line 0. New: VSYNC and line-interrupt callbacks (a VSYNC callback can take the swap over), run-time timings | `drivers/display/display_stm32_ltdc.c:113-139`, `:400` |
| Audio out and the microphone through the HAL (circular DMA, MDF with DMA) | Still needed. The SAI driver still reloads DMA per block; the new MDF driver reads one sample per interrupt, with no DMA. The WM8904 accepts a plain fixed clock, **main only** (#112540): the `n6demo,audio-mclk` shim could go | `drivers/i2s/i2s_stm32_sai.c:289-302`; `drivers/audio/dmic_stm32_mdf.c:132-180` |
| The NPU runtime, JPEG, DCMIPP | No NPU runtime upstream; JPEG grayscale only; DCMIPP small fixes (CSI bit rate, Bayer decimation above 2688 px) | |
| Power-cycle before flashing and starting (NOR left in octal DTR) | Not addressed: the XSPI memory reset is unchanged | |

## What a bump would cost

- **Build breaks:**
  - SoC device-tree addresses are now absolute (v4.5.0). The overlay's `&axisram1 { reg = <0x0 DT_SIZE_K(840)>; }` must become `0x34000000`.
  - `camera.c`: `video_import_buffer()` now returns `struct video_buffer *`, and `<zephyr/drivers/video-controls.h>` became `<zephyr/video/controls.h>`.
- **LVGL 9.6:** `CONFIG_LV_COLOR_DEPTH_32` no longer selects the colour format (`CONFIG_LV_COLOR_FORMAT_*` does). Other API changes are not checked.
- **The board now enables SAI1 (A and B), GPDMA1, MDF1/pdm0 and DMA2D**, which the demo drives through the HAL with its own interrupt handlers. Nothing binds today, because `CONFIG_DMA`, `I2S` and `AUDIO_DMIC` are off, but they should be `status = "disabled"` in the overlay before anything turns them on. The board's `wm8904@1a` would merge with the demo's node and bring a mic bias and input PGA setting (effect not checked).
- **Timing moves:** the M55 branch cache is on by default (`STM32N6_BRANCH_CACHE`), with LVGL 9.6 and the Ethernet driver's restructuring (MDIO now `snps,dwmac-mdio`). That means a full tour and a soak again.
- **Tracing (zprof, part of every build since 2026-11-22, so this is the release build's tracing):** the sleep hooks became `k_thread_sleep_ticks_enter/exit` (in ticks), object tracking moved to object cores, and the hook list is in `tracing_hooks.h`. zprof could plug in through `CONFIG_TRACING_FORMAT_HEADER` instead of `TRACING_CUSTOM`. There is also a new ITM backend; SWO (PB5) is free on this board.

## For zprof's next phase (PMU, faults)

- **PMU:** v4.5.0 has a portable API (`include/zephyr/arch/pmu.h`) with only an AArch64 backend. A Cortex-M55 backend would fit behind it. CMSIS-6 `armv8m_pmu.h` (already in the v4.4.0 modules) defines `ARM_PMU_MVE_INST_RETIRED`, and the N6 header has 8 event counters (`stm32n657xx.h:267-268`).
- **A ring that survives a reset:** not the DTCM (zeroed on every reset). The candidates are the 8 KB backup SRAM node (`backup_sram`, v4.5.0, disabled by default) or a `.noinit` region kept clear of MCUboot's RAM (511 KB at 0x3418_0400; whether that holds is not checked).
- **Coredump:** the backend API is unchanged. On main the in-memory backend writes back the data cache and can add a CRC; fault status registers are in the extra exception info.
- **The open question is the reset itself:** a software reset leaves the NOR flash in octal DTR mode and MCUboot then fails (CLAUDE.md). Upstream has not changed that, so a fault captured "after the reboot" needs the reboot to work first.

## v4.5.0 or main

v4.5.0 has the tracing changes, the PMU API, the LTDC callbacks, the SAI and MDF nodes, LVGL 9.6 and the absolute addresses. Only `main` has the TCM nodes, the SDMMC RIF setup, the WM8904 clock rework and the coredump write-back.
