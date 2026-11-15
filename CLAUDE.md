# stm32n6-demo - Claude Code Configuration

## What this is

A multi-screen touch demo for the STM32N6570-DK (dashboards, real-time data
visualization, full-screen video with sound) on Zephyr 4.4.0 + LVGL 9.5.
Audience: engineers and UX designers who are hard to impress, so frame pacing,
honest data and visual polish are the product. Plan: `docs/plan.html`.

Sibling project `~/projects/STM32N6570-DK` (audio visualiser) is reference
only: its MDF microphone driver and bring-up notes are reusable, its tree is
not to be modified from here.

## Run / build / test

```powershell
.\n6.cmd go                 # build + flash + power-cycle + start + console
.\n6.cmd go -p              # pristine; REQUIRED after adding any .overlay/.conf file
.\n6.cmd go --app-only      # do not reflash MCUboot
.\n6.cmd run --secs 20 --until "regex"
.\shot.cmd --host 192.168.254.86 -o captures\x.png
.\tools\gdbrun.ps1 -Elf build\mcuboot\zephyr\zephyr.elf -Ex 'load $ELF','break foo','continue','bt'
.\.venv\Scripts\python.exe tools\tour.py     # regression: per-screen frame stats, screenshots, injected gestures
.\.venv\Scripts\python.exe tools\ctl.py "screen 2" stats "tap 400 200"
```

- Verify UI work by looking: `shot.py` then Read the PNG. `tour.py` must show
  0 dropped frames on every screen before a commit.
- Build times: a source-only change is about 2 min; any `prj.conf`, Kconfig or
  CMake change rebuilds LVGL and takes 6 to 8 min. Batch config changes.

- Board stays in development boot: BOOT1 = H, BOOT0 = L. Console is the
  ST-LINK VCP, COM17, 115200.
- The board's DHCP address has been 192.168.254.86 (fixed MAC 02:80:e1:4e:36:01).
- Host tools run from the project `.venv`. ffmpeg is installed but only on the
  user PATH; a shell started before the install needs the full path under
  `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg_*`.

## Architecture

- `app/` is the Zephyr app and the west manifest repo; the project root is the
  workspace. Sysbuild builds two images: `build/mcuboot` (FSBL variant, runs
  from RAM) and `build/app` (default variant, XIP from XSPI2 NOR at slot 0).
- Display: `CONFIG_STM32_LTDC_FB_NUM=0`. The app owns the frame buffers and
  passes full frames to `display_write()`, which swaps the LTDC pointer in the
  line interrupt and blocks until the swap. No copy.
- `tools/n6.py` is the only supported way to flash and start the board.
- UI loop (main.c): `tele_poll`, `shell_tick` (edge swipes, bar, every
  screen's `service`, the visible screen's `tick`), then `gfx_run_frame`
  (LVGL timers, render, present). LVGL has no theme; everything is styled from
  `ui/theme.h` tokens.
- Dense visuals do not use LVGL draw calls: a widget's `LV_EVENT_DRAW_MAIN`
  handler gets a `struct raster` and writes pixels into the layer buffer
  (`ui/raster.c`). Safe because the software renderer runs synchronously
  (no LV_USE_OS).
- Memory: app RAM is 840 KB at 0x3400_0000; frame buffers A/B at 0x340D_2000
  and 0x3424_9000 (internal SRAM); PSRAM is a bump allocator (`psram_alloc`),
  first 2 MB reserved for the screenshot copy.
- Screens implement `struct screen` (ui/shell.h) and are listed in main.c.
  Index order matters to `tools/tour.py` (SCREENS list).

## Invariants & "do not regress"

- **Power-cycle before flashing and before starting.** `n6.py` does both.
  **Why:** ST's external loader leaves the NOR flash in octal DTR mode; a pin
  reset does not clear it and Zephyr's XSPI driver then times out on its first
  SPI command, so MCUboot aborts. Conversely the loader fails to erase if
  firmware is running. Cost about two hours to find on 2026-11-14.
- **No `ap=` in the SWD connect string**, and do not use `west flash`.
  **Why:** upstream `board.cmake` passes `ap=1`, which this probe/chip refuses.
- **Do not attach a debugger to the running app and expect it to work.**
  **Why:** with Zephyr idle in WFI the access port errors out
  (`DEV_AP_ACCESS_ERROR`, device shows as "Rev Z"). Debug from reset with
  `gdbrun.ps1` instead.
- **A new `.overlay` or `.conf` file needs a pristine build (`-p`).**
  **Why:** CMake only discovers them at configure time; an incremental build
  silently ignores the new file.
- **AXISRAM3-6 must be enabled in the overlay before touching 0x3420_0000.**
  **Why:** the banks are powered down and unclocked at reset; a write hangs
  the core with no fault output.
- **Logging is deferred; use `debug.conf` for an early hang.** Build with
  `n6.cmd build -- -Dapp_EXTRA_CONF_FILE=debug.conf` to get immediate mode.
  **Why:** immediate mode masks interrupts for a whole UART line
  (milliseconds), which wrecks anything timing-sensitive; deferred mode shows
  nothing if the board hangs before the log thread runs.
- **A peripheral clock enabled with a HAL macro needs its SLEEP enable too**
  (`__HAL_RCC_xxx_CLK_ENABLE()` and `__HAL_RCC_xxx_CLK_SLEEP_ENABLE()`).
  **Why:** on the N6 the sleep-enable bits (RCC_xxxLPENR) reset to 0, and the
  idle thread executes WFI many times a frame. Zephyr's clock driver sets
  both; the HAL "enable" macro sets one. SAI1 and GPDMA1 froze on every idle
  period: underruns, a stream at a fraction of real time, then a stall.
- **The audio DMA buffer and its linked-list node live in AHBSRAM1
  (0x3800_0000). Do not move them to FLEXRAM, AXISRAM or PSRAM.** **Why:**
  when the CPU streams writes into AXISRAM (LVGL filling large areas of the
  frame buffer) the DMA's reads on the AXI interconnect are held off for
  longer than the SAI FIFO lasts (8 samples, 83 us). Measured about 60
  underruns/s on Signal Lab and Cinema with the buffer in AXISRAM1, zero in
  AHBSRAM1. Reads, PSRAM and flash traffic did not matter (`ctl.py "bus k ms"`
  generates each kind of load for a regression check).
- **Audio out is one circular DMA transfer driven through the ST HAL, not
  Zephyr's I2S driver.** **Why:** that driver restarts the DMA from an ISR at
  every block boundary, which the 83 us FIFO does not survive. Also: the SAI
  FIFO threshold is 3/4 ("full" over-delivers and drops samples, "1/4"
  underruns); the WM8904 must be told it is the clock TARGET (with the
  default options it also drives BCLK/LRCLK against the SAI); the codec
  driver needs a clock controller with a cell called `name`, hence the
  `n6demo,audio-mclk` shim.
- **After any audio change run `tools/tour.py` and check `underruns 0`.** An
  underrun is recovered by restarting the stream, so a regression shows up as
  a counter, not as silence.
- **`MIN()`/`MAX()` evaluate their arguments twice.** Never pass a call with
  side effects (it registered every clip twice).
- **Never create or reposition many LVGL objects per frame.** **Why:** LVGL
  keeps 32 invalid areas per frame; one more and it redraws the whole screen
  (about 10 ms here). Moving 14 axis labels did exactly that. Draw moving text
  with `ui_draw_text()` inside the widget's draw handler, and spread bursts of
  label updates over several frames (see deck.c `update_stage`).
- **Pad JPEG input to a whole number of 32-bit words past the EOI marker.**
  **Why:** the HAL feeds the codec word by word; with a length that is not a
  multiple of four the end marker is never sent and the decode times out a
  few MCUs short (HAL error 0x8). `jpegdec.c` copies and pads.
- **Touch coordinates are panel coordinates**, not content-area coordinates:
  subtract `UI_CONTENT_Y` only when positioning objects inside a screen root.
- **Label the origin of data on screen.** Generated or synthetic sources say
  so (Signal Lab, Explorer). **Why:** the audience will look for fake data.
- **Keep `.cmd` wrappers plain ASCII.** **Why:** non-ASCII characters in a
  batch file produce parser garbage when invoked from PowerShell.
- **Commits carry no Claude attribution.**

## Open questions / known gaps

- Stand-alone boot (BOOT1 = L) has not been tested; it needs the user at the
  board.
- Touch, navigation, motion and colour were checked on the panel by the user on 2026-11-14 (all good). The repository is public: keep host names, serial numbers and personal paths out of committed files.
- Audio out works and is underrun-free in the automated checks; the sample
  clock is PLL2 (HSI-based), measured 47.6 to 47.7 kHz. Video is slaved to it.
- A formatted 32 GB microSD card is in the slot (2026-11-15). Plan: media
  library on the card with upload over Ethernet (needs the `fatfs` module and
  a read-ahead buffer in the player). Writing 84 MB of clips to the XSPI flash
  over SWD takes 8 minutes.
- Media: `media/film/*.py` render the original clips (Skia + numpy score),
  `tools/n6v.py encode|pack|flash` puts them in the media partition at
  0x7042_0000. Outputs go to `media/out/` (not committed).
- Camera (IMX335) and NPU are a later addition; keep a screen slot for them.
