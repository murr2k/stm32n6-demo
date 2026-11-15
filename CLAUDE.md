# stm32n6-demo - Claude Code Configuration

## What this is

A multi-screen touch demo for the STM32N6570-DK (dashboards, real-time data
visualization, full-screen video with sound) on Zephyr 4.4.0 + LVGL 9.5.
Audience: engineers and UX designers who are hard to impress, so frame pacing,
honest data and visual polish are the product. Plan: `docs/plan.html`
(published copy: https://claude.ai/artifact/EqF45WU98B5KAZ99LwZGMk).

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
- **Keep `CONFIG_LOG_MODE_IMMEDIATE=y` during bring-up.** **Why:** with
  deferred logging an early hang shows nothing at all on the console.
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
- Touch has not been exercised by a person yet: axis orientation and real multi-touch are unverified. Everything so far used `ctl.py` injection.
- Audio out (SAI + WM8904) is not in the board devicetree; needs an overlay.
- Bluetooth speakers were requested as a later addition. The DK has no radio,
  and speakers use Bluetooth Classic A2DP, not BLE, so this needs an external
  module; keep the audio sink behind an interface.
- Camera (IMX335) and NPU are a later addition; keep a screen slot for them.
