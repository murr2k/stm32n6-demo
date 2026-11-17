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
.\.venv\Scripts\python.exe tools\ctl.py "screen 2" stats where "tap 400 200"
.\.venv\Scripts\python.exe tools\fuzz_player.py --secs 120   # random abuse of the video player
.\.venv\Scripts\python.exe tools\acap.py --secs 2 -o captures\x.wav --clicks "tap 40 444"   # record the DAC feed while commands run, count discontinuities
.\.venv\Scripts\python.exe tools\n6v.py upload media\out\n6_film.n6v   # clip to the microSD card over Ethernet (4 MB/s); also ls, rm, df
.\.venv\Scripts\python.exe tools\acap.py --mic --secs 3 -o captures\mic.wav   # record the microphone (24 bit), print its level
Invoke-WebRequest http://192.168.254.86/agent.md   # the board describes itself (also / and /status.json)
.\.venv\Scripts\python.exe tools\ctl.py netcfg     # the board's address; "netcfg static IP MASK GW" or "netcfg dhcp" changes it now
.\.venv\Scripts\python.exe tools\dataset.py info   # Explorer's data set: must say "source flash"; pull + flash put it there
.\.venv\Scripts\python.exe tools\nn.py fetch; generate; testimg; flash   # NPU networks into build-nn/ (ignored), weights into the flash
.\.venv\Scripts\python.exe tools\nn.py check       # both networks on the board against the original models on the PC
.\.venv\Scripts\python.exe tools\nn.py trace 0     # time per epoch block against the converter's estimate
.\.venv\Scripts\python.exe tools\nn.py inputs      # what the hand tracker's networks last saw, as PNGs in build-nn/test
.\.venv\Scripts\python.exe tools\ctl.py nn "nn hands" "nn bench 20 1"   # NPU and tracker status; times (net 0 palm, 1 hand)
.\.venv\Scripts\python.exe tools\ctl.py "screen 11" "nn hands 2"   # track the built-in thumbs-up picture: gesture=7 expected
.\.venv\Scripts\python.exe tools\ctl.py "screen 11" "nn hands 3"   # the picture twice, one copy mirrored: hands=2 g0=7 g1=7, r0/r1 about 92/24; "nn hands 1" back to the camera
.\.venv\Scripts\python.exe tools\ctl.py "nn cfg 2 500"   # at most 2 hands; with fewer tracked, look for another every 500 ms
.\.venv\Scripts\python.exe tools\ctl.py "screen 2" "sigsrc 2" "thm 0 440 -12" thm   # theremin: a voice without touching (hz 0 lifts, "thm -1" back to touch), its state
.\.venv\Scripts\python.exe tools\ctl.py "thmwave 3" "thmsnap 1" "thmghost 1" "thmsound 0"   # timbre 0..3, semitone snap, the attract ghost, the SOUND switch
.\.venv\Scripts\python.exe tools\nn.py watch 20    # tracker counters over 20 s: searches, palms found, confirmed, lost, time tracked
.\.venv\Scripts\python.exe tools\nn.py grab 5 --hand   # frames as the tracker saw them (captures/hands, ignored) and the PC's models on them and on variants
.\.venv\Scripts\python.exe tools\ctl.py "nn thr 50 50"   # the palm and presence thresholds, percent
.\run.cmd                   # host companion: live data for Host Link and Core Map; finds boards by broadcast (--host to name one)
.\.venv\Scripts\python.exe host\companion.py --host 192.168.254.86 --cores 128   # TEST ONLY: synthetic load on 128 processors
```

- Verify UI work by looking: `shot.py` then Read the PNG. `tour.py` must show
  0 dropped frames on every screen, in the transitions and in the attract-mode
  lap, and 0 stray screen changes, before a commit (about seven minutes;
  `--no-attract` for a quick pass). It starts the companion itself, plays the
  first clip from flash and from the card (`ctl.py "card 0"` hides the card's
  clips), records the sound of a drag on the slider (`discontinuities 0`), and
  uploads, plays, seeks and deletes a generated test clip.
- `CHANGELOG.md` gets a line for every change a user of the demo or its
  tools would notice, under the day it is committed (no tagged releases).
  README images are the board's own screenshots; one of the Camera screen
  must show no face (the repository is public: pick a frame with a hand only).
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
  (LVGL timers, render, present). The shell also owns transitions and attract
  mode: 90 s without a touch report starts a lap through the screens, steered
  by each screen's `attract_secs`, `attract_skip`, `available`,
  `attract_start` and `attract_busy` (ui/shell.h). LVGL has no theme; everything is styled from
  `ui/theme.h` tokens.
- Dense visuals do not use LVGL draw calls: a widget's `LV_EVENT_DRAW_MAIN`
  handler gets a `struct raster` and writes pixels into the layer buffer
  (`ui/raster.c`). Safe because the software renderer runs synchronously
  (no LV_USE_OS).
- Memory: app RAM is 840 KB at 0x3400_0000; frame buffers A/B at 0x340D_2000
  and 0x3424_9000 (internal SRAM); PSRAM is a bump allocator (`psram_alloc`),
  starting after the linker's own PSRAM section (`__PSRAM_end`: the video
  layer's pool, the NPU runtime's blob copies); the screenshot copy is taken
  from it at boot; the top 4 MB (from `PSRAM_NPU_BASE`, 0x91C0_0000) are the
  NPU's spill memory, which the converter addresses directly and
  `psram_alloc` never hands out. Budget, measured 2026-11-15: internal RAM
  815 of 840 KB (819 since the theremin, 2026-11-17: 21 KB left) (LVGL pool 256 KB, heap 64 KB, the card's two 32 KB I/O
  buffers, stacks; the NPU's thread 8 KB), all screens' LVGL objects are
  built at boot and kept; PSRAM 13.4 of 28 MB and never freed: the player
  7.0 MB (4 MB read-ahead window, three 576 KB decode buffers, 1 MB index),
  Cinema 2.4 MB, camera 2.1 MB, screenshot 1.5 MB (14 MB more when Explorer's
  data set has to be generated because the flash holds no copy); code
  1.77 MB in a 2 MB slot of a 128 MB flash. A new demo with buffers needs a place in this before it is written.
- Flash map (128 MB at 0x7000_0000): MCUboot and the two image slots up to
  0x7041_0000, media partition 0x7042_0000 to 0x7580_0000 (84 MB, 83.7 MB
  used: full, more clips go on the card), the NPU's weights 0x7580_0000 to
  0x7700_0000 (24 MB: palm detector at 0x7580_0000, hand landmarks at
  0x75A0_0000, written by `tools/nn.py flash`), Explorer's data set
  0x7700_0000 to the end (16 MB, 14 MB used).
  Read-only data belongs in flash and is used in place; `tools/dataset.py`
  (pull, flash, info, erase) is the pattern for getting it there.
- Media: a clip is either in the flash media partition (read in place) or a
  file in `/SD:/clips` on the microSD card (FAT32 or exFAT, SDMMC2 with its
  internal DMA). A card clip with the same title as a flash clip takes its
  place in the list. Threads, highest first: audio (cooperative), UI (0),
  `player_io` (2, streams the open card clip into a 4 MB PSRAM window around
  the play position), `player` (3, owns the JPEG codec), `cinema_bg` (6,
  re-reads the clip list and makes thumbnails after an upload), then the
  network services (`sd_service` on tcp/5558 takes uploads). The window is
  moved by `player_io` only; the others copy out of it and wait when their
  data is not there (the clock is held, nothing is skipped).
- Microphone (platform/mic.c): MDF1 filter 0 through the ST HAL, PDM clock
  out on PE2 (CCK0), data in on PE8 (SDI0), kernel clock IC7 (12.288 MHz, the
  SAI's, so one microphone sample is one DAC frame), CCK 3.072 MHz, Sinc5 / 16,
  reshape / 4: 48 kHz, 24 bit, read as 32-bit words by a circular GPDMA1
  channel 6 transfer into AHBSRAM1 (buffer at +0x2000, list node at +0x3000,
  beside the audio output's). A cooperative thread copies each 512-sample
  half into a 16384-sample ring in PSRAM; readers use `mic_total()` and
  `mic_copy()`. `ctl.py "mictune 1 1 1 8 5 8 1 0 0 3" "micinject 1000 1"`
  injects a 1 kHz square wave at the data pin with U13 asleep (capture
  self-test; `"micinject 0 0 0" "mictune 0 1 1 4 5 16 1 0 0 3"` restores).
  `ctl.py "mictune 1 1 1 8 5 8 1 0 0 3" "micpdm 1000 50 2 160"` puts a
  synthetic microphone's 1 kHz PDM stream on the data pin instead, for
  Signal Lab (`micpdm 0` and the default `mictune` restore).
  Signal Lab takes it as its second source (`sig_set_source`,
  a tap on the SOURCE label, `ctl.py "sigsrc 1"`; `sigsrc 2` is the theremin); the generator is the
  default. `ctl.py mic` shows rate, errors and level, `acap.py --mic`
  records a WAV, `ctl.py "mictune ..."` restarts the capture with other
  settings (mic.h lists the ten values), `micprobe` and `micbits` look at
  the data pin without the MDF (`micbits` stops interrupts for its
  duration: sound and picture pause, the underrun counter moves).
- Camera (media/camera.c, screens/camera.c): the B-CAMS-IMX shield is set in
  app/CMakeLists.txt (`SHIELD st_b_cams_imx_mb1854`). The DCMIPP demosaics and
  scales 2592 x 1944 to 592 x 444 RGB565 (`video_set_compose_format`: the
  driver wants the compose rectangle first) into four PSRAM buffers imported
  with `video_import_buffer` (the video layer's own pool is kept at its
  minimum; `CONFIG_VIDEO_BUFFER_POOL_NUM_MAX=4` also caps the imports). ST's ISP middleware (west module
  `zephyr-stm32-mw-isp` v0.4.0, fetched under `/st/`) hooks into the driver
  and runs exposure and white balance. The capture thread hands the newest
  frame to the screen, which copies it with DMA2D (`dma2d_rgb565` in
  jpegdec.c, UI thread only like the JPEG conversion), and to the NPU's
  thread (`camera_wait_frame`, `camera_acquire_nn`).
- NPU (nn/npu.c, nn/hands.c): ST's runtime (`ll_aton`, compiled from the ST
  Edge AI install) and the networks `tools/nn.py` converts into `build-nn/`
  (palm detector and hand landmarks, Google MediaPipe, Apache-2.0). Working
  memory is frame buffer B, which the display lends while the Camera screen
  shows (`gfx_lend_back_buffer`, `gfx_borrow_begin`/`end`): the display then
  runs single-buffered from A; inputs are placed there by the converter too;
  what does not fit spills to the PSRAM block at 0x91C0_0000; weights are
  read in place from the flash at 0x7580_0000. The `nn` thread (priority 5)
  serves `ctl.py nn ...` requests and runs `hands_step` per camera frame:
  up to two hands (`HANDS_MAX`), each in its own slot (`struct track`) for
  as long as it is tracked. With no hand the palm detector runs on every
  frame (about 65 ms, on the letterboxed picture); with one it runs every
  500 ms (`nn cfg`). Each hand costs a 224 x 224 crop turned upright around
  it (about 9 ms of CPU, reading PSRAM) and the landmarks (about 22 ms), the
  next crop following the landmarks; the networks' inputs share one place,
  so the hands run one after the other: 33 ms a frame with one hand, 66 ms
  with two (15 fps). Gestures from joint angles, points smoothed by a
  one-euro filter, per hand.
  Measured on the board, both networks agree with the original models on
  the PC (`nn.py check`).
- Theremin (dsp/theremin.c, Signal Lab's third source, `sigsrc 2`): Signal
  Lab's spectrum is the instrument. A touch at a column sounds
  `sig_colf_hz(col)` at the level of its height on a loudness taper
  (`thm_u_db`: -6 dBFS at the top, 10 dB less per halving of the height,
  chosen by the user over even dB steps, which left the bottom half
  inaudible), and the spectrum's level axis uses the same taper in
  instrument mode (`db_to_y`, `thm_db_u`), so the analyser shows the peak
  under the finger; one voice per touch slot (five). A slot plays only if
  it came down on the field (not one already down when the theremin was
  enabled, not one that came down on a button or an edge) and then plays
  until it lifts: off the field's top or bottom it keeps its last level,
  past an end the pitch stops there (the user: no cut-off when a finger
  runs off the chart). SOUND (`theremin_set_sound`, `thmsound`, on at
  power-up, not stored) fades the DAC feed to silence in 10 ms; the
  analysis ring still gets what is played. The synthesizer is the audio output's
  source while Signal Lab shows with THEREMIN selected (`theremin_enable`,
  given back after a fade with `audio_release_source`, which clears only if
  it is still the owner) and reads `touch_get()` itself at the start of each
  512-frame block on the audio thread: touch to sound 11 to 21 ms (the
  panel's report to the block's first sample at the DAC; `thm` reports it).
  Everything it plays goes into a PSRAM ring that sig.c analyses; pitch and
  analysis use the DAC's rate measured from its block count (47.7 kHz).
  In attract mode a ghost plays keyframes (a flying saucer); any touch ends
  it. Signal Lab's spectrum and waterfall are one pane (`plot_pane`) whose
  draw handler splits it: 146/164 px as analyser, 252/58 px as instrument.
- Screens implement `struct screen` (ui/shell.h) and are listed in main.c.
  Index order matters to `tools/tour.py` (SCREENS list): home, deck, siglab,
  explorer, touchlab, hostlink, coremap, cinema, specimen, hood, settings, camera. Home shows two
  rows of four tiles; a ninth and later ones go into further columns on the
  right (same tile size) and the field pans sideways. `ctl.py "hometiles 5"`
  adds placeholder tiles to try that with (`hometiles 0` removes them). The
  shell holds 12 screens (`MAX_SCREENS` in shell.c), Home 24 tiles.

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
- **The app is built for the double-precision FPU (`APP_FPU_DOUBLE` in
  app/Kconfig selects `CPU_HAS_FPU_DOUBLE_PRECISION`). Do not remove it.**
  **Why:** Zephyr 4.4.0's N6 SoC definition only selects `CPU_HAS_FPU`, which
  gives `-mfpu=fpv5-sp-d16` and turns every `double` operation into a library
  call (the core has DP: MVFR0 = 0x10110221). Explorer's per-column `double`
  maths cost 2 to 5 ms a frame and dropped frames during its zoom tour. Check
  with `-mfpu=fpv5-d16` in `build/app/compile_commands.json`.
- **A click at the same place in a clip on every run: suspect the clip before
  the playback.** Check before touching firmware: extract the PCM from the
  `.n6v`, compare it with the WAV and look for isolated steps. **Why:** the
  film had steps of 4000 LSB at 6.24 s and 9.6 s, bass notes that stopped
  mid-cycle in `media/film/score.py`; the playback faults seen so far all
  showed in the underrun counter. Every voice mixed by `score.add()` gets a release ramp,
  and clips take audio from the WAV (`n6v.py encode --audio`), never from the
  MP4's AAC track.
- **Screen transitions fade with the LTDC layer alpha (`gfx_set_fade`), not
  by blending in the frame buffer.** **Why:** a software veil over the content
  area dropped one frame on every transition; the hardware fade costs nothing
  and hides the frame that builds the new screen. `LTDC->BCCR` must stay equal
  to `C_BG0` or the fade flashes a different colour.
- **A screen switch is work done after a present, inside the budget of the
  frame that builds the new screen: keep `show()`/`hide()` short and a
  screen's first frame well under 15 ms.** `switch_to` (the old screen's
  `hide()`, the new one's `show()`) runs in the fade-out's animation
  callback, and LVGL's animation timer runs after the display's refresh
  timer, so that time is not in `render_us`; `ctl.py stats` reports it as
  `tail_worst_us`. **Why:** Under the Hood's first frame (chart and side
  panel at once, up to 15.8 ms) after closing a playing film (a tail of about
  1 ms) missed a refresh on about one attract lap in three (5 drops in 16
  switches); its side panel now waits one frame (0 in 16). The miss falls in
  the black hold of the fade, so nobody sees it, but the counter on Deck does.
  Deck's own first frame is 13 to 15 ms with a 0.8 ms tail: about 1 ms of
  margin, one drop in 45 minutes of attract mode. The tour's transition test
  enters every screen and makes the film to Under the Hood switch six times.
- **A screenshot (`shot.py`, tcp/5555) costs the UI one frame.** **Why:** the
  1.5 MB copy to PSRAM takes about 18 ms. Never grab inside a window whose
  dropped-frame count is being measured (tour.py reads the stats first).
- **Compare elapsed time signed when the timestamp may be newer than `now`**:
  `(int32_t)(now - stamp) > LIMIT`. **Why:** the shell samples `now` once
  per frame, `shell_note_input()` stamped the input with a fresh clock read a
  millisecond later, the unsigned difference wrapped to four billion and
  attract mode started on about one touch release in five (the screen jumped
  to the next one). `tour.py` checks 80 releases with the `where` command.
- **The control port (tcp/5556) and the frame dump (tcp/5555) serve one
  client at a time; host tools must reuse one connection (`ctl.send` does).**
  **Why:** Zephyr has 16 TCP slots here and a closed connection lingers; a
  script that connected per command (80 a second in an injected drag) made
  the board unreachable for tens of seconds, which looked like a crash. Both
  servers time out a silent client after 5 s. A board that does not answer
  but shows the WFI debugger signature (Rev Z, `DEV_AP_ACCESS_ERROR`) is in
  this state, not halted. Do not probe a running board with a hot-plug
  `-halt`: it stops the core.
- **A full-screen view (`shell_set_cover`) owns every touch: no edge swipes
  under it.**
- **`raster_line`, `raster_poly` and `raster_area` must stay free of
  per-pixel-row divisions and edge scans.** **Why:** a 64-bit division per
  column made 25 wireframe lines cost 1.5 ms, and two edge scans per row made
  22 tall posts cost 6 ms. The polygon fill walks its two edge chains; lines
  use a float slope; the area fill steps its fade in 16.16 fixed point (one
  division a column: 0.9 ms less a frame for a loud spectrum).
- **Label objects that lie on a pane redrawn every frame are redrawn every
  frame too; draw such captions in the pane's handler (`ui_draw_text`).**
  **Why:** Signal Lab's seven axis captions were label objects over its
  spectrum: with them drawn by the handler instead, its frame went from 8.3
  to 4.6 ms. And never move or resize objects in a screen switch: each
  position is a style write (about 185 us), and resizing two panes and moving
  seven labels added 2.1 ms to the switch tail, which dropped a frame in an
  attract lap. Signal Lab's two layouts are one pane split by its handler.
- **The theremin draws a glow under its spectrum (`raster_glow`), not
  `raster_area`.** **Why:** a fill to the floor costs with how loud the
  spectrum is: five loud voices filled most of the 252 px pane, 12 ms frames
  and dropped frames. The glow is 28 px deep whatever the sound. Synthesis is
  not the cost (0.25 to 0.8 ms per 10.7 ms block for five voices, `thm`).
- **The theremin's sound never steps: phase-continuous oscillators, pitch
  glides in log frequency, every level ramps, lifts decay, a soft limiter
  (ceiling -0.9 dBFS).** The tour records a glide, a chord and lifts and
  counts the discontinuities: 0.
- **Core Map zoom is capped at 2.5x.** **Why:** it is fill-rate bound; at 3.5x
  tall posts overdrew the whole view several times (11.8 ms draw, dropped
  frames). Faces of one post overlap by a pixel under the face drawn after
  them, or anti-aliased shared edges let the background show through.
- **The SDMMC2 node has `idma` and `sdcard.c` gives the SDMMC2 bus master
  secure, privileged RIF attributes before mounting. Keep both.** **Why:**
  without `idma` the driver moves the data in an interrupt per 32 bytes
  (by arithmetic about 370 000 a second during a read at 12 MB/s: UI frame
  time; not measured, the DMA path was used from the start). With it the
  peripheral writes memory itself, and Zephyr's SoC code sets RIF master
  attributes for the display, camera, Ethernet, DMA2D, NPU and video encoder
  but not for the SDMMC. Checked by leaving the call out: the mount fails
  with `fs mount error (-19)`, the DMA's writes do not land.
- **Card reads go through `sdcard_read_at()`: whole sectors into one
  cache-line-aligned buffer, then a copy.** **Why:** the driver invalidates
  the data cache over the DMA target, so a buffer that shares a cache line
  with other data can lose that data. `CONFIG_FS_FATFS_WINDOW_ALIGNMENT=32`
  does the same for FatFs's own sector buffer.
- **`CONFIG_FS_FATFS_MOUNT_MKFS=n`. Never let a failed mount format the
  card** (Zephyr's default does).
- **No card I/O and no thumbnail decoding on the UI thread once the demo is
  running.** `cinema_bg` reads the list (`player_scan`) and makes the
  pictures; the UI thread only swaps the list in (`player_commit`) and
  rebuilds the cards. **Why:** done on the UI thread, a batch of uploads
  froze the display for 167 ms (three thumbnails). Thumbnails are filtered
  straight from the codec's YCbCr blocks, so they need neither DMA2D nor the
  full-size RGB buffer the player view uses.
- **`player_io` runs above `player`.** **Why:** the reader waits on the card,
  the worker polls the codec; with the reader below, a seek on the 14 Mbit/s
  clip left playback on an empty window (16 stalls in one fuzz run, 0 after).
- **`player_close()` does not wait for the worker**, and the worker copies a
  frame out of the window without holding `win_lock`. **Why:** Back blocked
  the UI behind a decode in progress (10 to 25 ms), and the audio thread,
  waiting for the lock, lent the worker a priority above the UI.
- **A seek belongs to the frame decoded after it, not to the one in flight**
  (`jump_gen`, read as a pair with the clock in `player_thread`). **Why:**
  with a plain flag the frame being decoded when the finger moved consumed
  it, and the seek then counted as a stall or as dropped frames.
- **The player view answers `LV_EVENT_COVER_CHECK` with "covers".** **Why:**
  LVGL cannot tell that a self-drawing widget is opaque and filled the whole
  screen background before every video frame (about 0.9 ms of 7).
- **Uploads go in 8 KB pieces, each acknowledged by one byte from the
  board** (`SD_PIECE`). **Why:** Zephyr's TCP acknowledges at once only a
  segment with PSH and otherwise after 100 ms; a host sets PSH when its send
  buffer runs empty, so a plain bulk send moved one 21 KB window per 100 ms:
  0.11 to 0.2 MB/s. With the credits: 4 MB/s, and the card write is no
  longer hidden behind the network (it does 9 to 12 MB/s).
- **Explorer's data set lives in the flash and is used in place; keep it out
  of PSRAM.** **Why:** generated into PSRAM it held 14 of 32 MB for good
  (25.3 MB in use, 6.7 free, no room for a camera or NPU demo). From the
  flash the worst Explorer frame is the same within 0.3 ms (14.1 against
  13.8 ms in the zoom tour, 12.3 against 12.0 under gestures) and the boot
  work is a 0.44 s checksum instead of 1.5 s of generating. `ctl.py ds` or
  `dataset.py info` must say `source flash`; `generated` means the copy is
  missing or damaged and 14 MB of PSRAM are gone again (the firmware falls
  back by itself, and the log says so). After a change to the generator or
  to DS_N: `dataset.py erase`, start, `pull`, `flash`.
- **PSRAM in use: 13.4 of 28 MB** (`ctl.py media`, `psram_kb`), 13.9 once
  `acap` has been used (its 576 KB buffer is taken on first use); 0.5 MB more
  once `nn hands 2` or `3` has run (the test picture as a camera frame), 0.8 MB more
  once `nn grab` has. The
  read-ahead window was cut from 8 to 4 MB when PSRAM was nearly full; that
  reason is gone, the 4 MB have been enough (0 stalls).
- **The player's sound never steps: every change of position (pause, play,
  seek, loop, a finger on the slider) is a 21 ms cross-fade, and whatever
  fades plays from samples already copied to memory** (`audio_source` in
  player.c: `ahead`, `fader`, the grain buffers). **Why:** the slider used to
  restart the sound at the finger on every UI frame: 35 to 50 full-scale
  steps a second in a drag, the same 16 ms over and over under a resting
  finger. A first version that faded voices while reading them from the clip
  still clicked 15 times a second in a fast drag, because the card reader had
  not reached the new position yet, or had left the old one. While a finger
  is on the slider (`player_scrub`) the sound is 85 ms grains from the frame
  under it, silent while it rests, and the clock holds that frame. Check with
  `acap.py --clicks` or the `scrub sound` line of the tour: 0.
- **State that a guard reads must be reset together with it.** The faders
  were "cleared" on a clip change by zeroing `n` but not `i`: `n - i` wrapped,
  the guard passed and `fader_mix` divided by zero (UsageFault, board dead on
  the network). Found from the registers: on a board that no longer pings,
  `STM32_Programmer_CLI -c port=swd mode=HOTPLUG -halt -coreReg` works (the
  core is not in WFI), `-r32 <PSP> 0x20` gives the stacked frame, word 6 is
  the faulting PC, `build/app/zephyr/zephyr.map` names the function. And
  keep `n6.cmd console --log` running during every test run: this one had
  none (it was stopped for a flash) and the fault message was lost.
- **An injected `swipe` across the Cinema slider takes several times its
  nominal duration** (it runs in the control thread, below the decoder, which
  a drag keeps busy). Pace drags from the host with `touch` commands.
- **An injected drag on the Cinema slider only scrubs while the controls are
  showing** (they hide 3.2 s after the last touch while playing). **Why:** a
  measurement made with them hidden reported 6.3 ms "scrub" frames that were
  plain playback, and a Back tap that only re-showed the controls looked like
  a firmware fault. Tap the picture first, or scrub within 3 s of opening.
- **Pad JPEG input to a whole number of 32-bit words past the EOI marker.**
  **Why:** the HAL feeds the codec word by word; with a length that is not a
  multiple of four the end marker is never sent and the decode times out a
  few MCUs short (HAL error 0x8). `jpegdec.c` copies and pads.
- **`CONFIG_INPUT_MODE_SYNCHRONOUS=y`: touch callbacks run in the GT911 work
  item. Do not go back to Zephyr's input thread.** **Why:** the default is a
  16-event queue drained by a thread at the lowest priority, the driver sends
  4 events per contact in one burst from the cooperative system work queue
  and what does not fit is dropped: five contacts never fit, fewer do not
  when the UI and the player keep the thread waiting. A dropped release
  leaves the contact down for good. Seen 2026-11-15 in the console log (over
  1000 `Event dropped, queue full` in 10 s of real touches) and in the touch
  table afterwards (slots 2 and 3 down with nobody at the panel). Injected
  touches (`ctl.py touch`, tour, fuzz) bypass the driver and cannot show it.
- **Home does not use LVGL's scroll snapping; it rests on a column in its own
  `LV_EVENT_SCROLL_END` handler.** **Why:** `LV_SCROLL_SNAP_START` lets the
  last child snap to the left edge, so a flick ended three columns past the
  tiles on an almost empty screen. The edge guards (as in Cinema) keep a
  drag from the shell's edge zone from panning the field as well; with eight
  tiles or fewer neither the scrolling nor the guards exist.
- **Ask before `fs_unlink` and `fs_mkdir` (`exists()` in sdcard.c).** **Why:**
  Zephyr's fs layer logs an error for every failing call, also where failing
  is the normal case; every upload and every boot put `<err>` lines into the
  log that fault hunting relies on. One warning is left at boot,
  `stm32_sdmmc: Failed to configure pins for sleep (-2)`: the driver asks for
  a pinctrl sleep state, which Zephyr compiles out without
  `CONFIG_PM_DEVICE` (adding the state to the overlay changes nothing).
- **The microphone's DMA list node and buffer are in AHBSRAM1 and the node
  is flushed from the data cache before the transfer starts, as for the
  audio output.** **Why:** the DMA engine reloads the node from memory on
  every lap of a circular transfer; a node the cache has not written back
  is a corrupt descriptor some seconds later (seen by the user in another
  project as "corrupted after 1 to 17 s", and the reason that project fell
  back to re-arming a one-shot transfer). Here: 0 errors over the tour.
  MDF1 needs its sleep clock enabled like every HAL-enabled peripheral, and
  its clock source is chosen with `LL_RCC_SetMDFClockSource` (a plain
  CCIPR5 write), not `HAL_RCCEx_PeriphCLKConfig`, which reprograms the IC
  divider behind the Zephyr clock driver's back.
- **The onboard microphone U13 delivers noise on this board; do not tune
  the firmware against it.** Measured 2026-11-15: Gaussian noise at
  -60 dBFS (about 50 dB above the MP23DB01HP's own noise, a slope of 2 dB
  an octave, no tones), unchanged by the clock edge, the serial interface
  mode, the kernel clock source (RC PLL, crystal bus clock, RC), a PDM clock
  of 1.5 to 3.2 MHz, the clock pin's drive strength, or a pull-up or
  pull-down on the data pin (so the line is driven); it scales exactly with
  the filter gain (so it is in the bit stream) and the stream goes to
  digital silence below 1 MHz (so the part reacts to its clock). A 1 kHz
  tone from the PC that the user heard is not in a 1.6 s coherent
  measurement on either edge (below -92 dBFS, under 50 dB SPL by the
  datasheet sensitivity of -41 dBFS at 94 dB SPL), and the user saw no
  response to sounds or to tapping the board. It is not an edge or an
  oversampling problem either (fast processing clocks, 40 to 280 ns after
  the rising edge: the same noise), and the raw stream, clocked by hand and
  read at the pin (`ctl.py "micbits ..."`), is a proper noise-shaped
  sigma-delta stream: the bit changes about 100 ns after the rising clock
  edge and holds for the whole cycle, identical on both edges, shaped noise
  above 50 kHz, and in the audio band a floor some 40 dB above the
  datasheet's. Claps and fingernail taps at the board (2026-11-17, spoken
  prompts, the user at the board) left no trace: through the MDF on the
  falling and on the rising edge (10 ms windows and bands within 0.7 dB of
  quiet), and in raw bits clocked by the CPU at 2.4 MHz with no MDF at all
  (within the 2.7 dB that two quiet recordings differ by; a working part
  would show 20 to 30 dB at every clap). So no MDF setting can bring a
  signal out: it is not in the bits. The raw stream's first ~100 ms after
  the clock starts is a burst near -13 dBFS (the part starting up); leave
  it out of any comparison. ST's own unmodified MDF_AudioRecorder (the
  build of 2026-05-30 in the sibling project, loaded into RAM: power cycle,
  `STM32_Programmer_CLI -c port=swd mode=HOTPLUG -halt -d <elf> -s
  0x34180400`) gives the same on 2026-11-17: a constant hiss in the
  headphones that nothing near the board changes (the user, by ear), and
  its buffer read over SWD the same in quiet, taps and claps. On
  2026-05-31 the same binary on this board made taps "clearly audible"
  (sibling project, docs/dmic-stm32-mdf/NEXT.md). So the firmware is
  cleared, ST's and ours: U13, its supply or its sound port changed after
  May 31. Open: how and when the external PUI microphone was wired (to CN5
  with pin 10 grounded, or straight to PE8/TP18, where it would have fought
  U13's output), VDD_MIC at CN5 pin 2, the sound port. The capture chain
  itself is verified from the
  data pin on (`ctl.py "micinject ..."`, 2026-11-17): with U13 asleep (its
  clock pin held low) and the MDF in low-frequency master mode (it samples
  with its own clock; normal SPI mode takes the clock from the pin and then
  delivers nothing), a square wave made by flipping PE8's own pull-up and
  pull-down comes out at the right frequency (1004, 252 and 2508 Hz for
  1000, 250 and 2500: the sample clock's known 0.45 % shortfall), with odd
  harmonics as a square wave has them, 0 errors; a held pull gives digital
  zero (-138.5 dBFS). With U13 clocked the injected wave vanishes and only
  its noise remains: it drives the line, and the noise is its own output,
  not the chain's. Mode 1 of `mictune` (Sinc5 / 8, 1.536 MHz) has 30 dB less
  gain than the default (Sinc5 / 16): compare levels within one setting.
  A synthetic microphone too (`micpdm`): a sine through a second-order
  sigma-delta modulator, put on PE8 by GPDMA1 channel 7 paced by TIM6
  (table in AHBSRAM2), decodes to the right frequency and shows on Signal
  Lab. Two limits found on the way: a pin set as a GPIO output no longer
  feeds its alternate-function input (PE8 toggled, IDR showed it, the MDF
  read a still line), so only the pulls can put bits on it; and on PE8 the
  pulls are slow and lopsided (the line falls about 5 us after the pull-down
  comes on and rises at once: much capacitance or something holding it up,
  U13 asleep, U14, the trace to CN5), so 160 to 192 kbit/s is the most it
  carries, at about 8 dB SINAD (second harmonic -15 to -21 dBc).
  Return-to-zero coding does not help on this line. A clean MHz-rate test
  would need a free MDF data input (another SDIx on an unloaded pin).
  With the clock stopped the line is free and unclamped
  (`micprobe`), so the part has its supply and sleeps as it should. That
  is a microphone whose electronics run and whose transducer delivers
  nothing. Wiring per the
  schematic (MB1939 sheet 16): U13 on PE2/PE8 through analog switch U14,
  which hands PE8 to connector CN5 when a board there pulls MIC_DET low;
  VDD_MIC from 3.3 V through SB19; LR high, so data is valid while the
  clock is high (falling edge). The user's own good captures on this board
  were made with an external PUI DMM-2726-B on those pins (1.536 MHz,
  Sinc5 / 8), not with U13 (how it was wired is not recorded). ST's
  unmodified MDF_AudioRecorder passed taps on 2026-05-31 and nothing on
  2026-11-17 (above). **Results, the board circuit around U13 (SB19, U14,
  MIC_DET, CN5), what to check on this board and on its replacement, and
  links to every source: `docs/microphone.md`.** The user will swap in a
  replacement DK. First round of research (MDF settings of ST and others):
  `reports/STM32N6 DK microphone noise only.md`. The second round's working
  notes stay local and are not committed (the user: no need to commit what
  can be retrieved from the sources; document the results with links).
- **Under the Hood: logarithmic height by default, ribbons not curtains, and
  a run of equal samples as one polygon** (hood.c). **Why:** on a linear axis
  the UI thread (about 50 %) was a wall and every other thread a flat line
  (the user: "not impressive"); 0.01 % to 100 % shows them all. On that axis
  every ridge is tall, and curtains filled down to the floor overdrew the
  view several times (10.4 ms a frame, dropped frames): a band under each
  top edge plus a line on the floor costs 7.8 ms, worst 12.6 ms. Each
  `raster_poly` call also costs tens of microseconds whatever its size, so
  equal runs are merged; 8 ribbons of 30 samples. `ctl.py hoodt` gives the
  worst sampling, stack scan, network query and side panel times (the side
  panel is 3.3 ms, every 0.5 s). Its numbers come
  from `k_thread_runtime_stats_get` (interrupt time is charged to the
  interrupted thread) and `k_thread_stack_space_get`, which needs
  `CONFIG_INIT_STACKS`.
- **Core Map draws at most 64 posts; a larger host is shown a post per core
  (SMT threads averaged) or per run of processors, and the header says so.**
  **Why:** posts overlap and overdraw each other: 64 cost 11.9 ms, 128 cost
  21 ms (17.8 ms even as one outline polygon plus a top each) and dropped
  every other frame. Host Link draws all 128 (7.5 ms). The datagram's core
  count is a byte (up to 255); the board keeps 128, the core history in
  PSRAM. Discovery: `N6HQ` to udp/5557 (broadcast) is answered with an
  `N6HA` line of the board's services (hostlink.h); the companion asks every
  10 s and sends to every board that answered.
- **A change of the board's address goes through `netcfg_apply()`, which
  hands it to the `netcfg_save` thread; `net_use_config()` waits for DHCP to
  release its lease before adding a fixed address.** **Why:** DHCP lets go
  asynchronously; a fixed address equal to the old lease was added and then
  deleted by the lease's late removal, and the board answered nothing (not
  even ARP) until power-off. Different addresses and fixed to DHCP worked.
  Never reboot the board from firmware to apply a setting: a software reset
  leaves the flash in octal mode and MCUboot fails (see the power-cycle
  invariant). The setting is kept in `/SD:/n6demo.cfg`; USER held at power-up
  forces DHCP.
- **Home does not pan by itself in attract mode.** **Why:** while the field
  moves, every visible tile redraws its live thumbnail each frame; with ten
  screens that reached 15.7 ms and dropped a frame. Dragging by hand
  measured 13.6 ms worst.
- **`/st/` (ST's ISP middleware), `build-nn/` (the converted networks) and
  ST's NPU runtime are never committed.** **Why:** their licence (SLA0044)
  allows use on ST devices and redistribution but forbids putting them under
  open-source terms, and this repository is public and Apache-2.0.
  `west update` fetches the ISP; `tools/nn.py fetch` and `generate` make
  `build-nn/`; the runtime is compiled from the ST Edge AI install
  (`STEDGEAI_CORE_DIR`, default `C:\ST\STEdgeAI\4.0`). Without them the app
  builds with the NPU left out (`APP_HAS_NPU` undefined).
- **The camera starts and stops on its capture thread** (`camera_start` and
  `camera_stop` only ask). **Why:** both talk to the sensor over I2C and wait
  for the pipe; on the UI thread each opening or closing of the screen cost
  a frame.
- **Under the Hood resets its view on every visit; its zoom stops at 1.8.**
  **Why:** a view left zoomed in by earlier touches made the ribbons fill the
  screen: 18 ms frames and 7 dropped per attract visit.
- **Face signatures, names and anything else that identifies a person stay
  in the DK's own non-volatile storage (the microSD card in its slot) and are
  never committed, pushed or sent off the board.** The user's rule,
  2026-11-15. So: no control command, web page or status JSON gives out
  signatures or names (counts only); deleting them happens on the board;
  test images and screenshots of faces stay in `captures/` (ignored); the
  ignore patterns in .gitignore cover signature files copied to the PC by
  mistake. Check `git status` for them before every commit.
- **The NPU's networks run only between `gfx_borrow_begin()` and
  `gfx_borrow_end()`: their working memory and inputs are frame buffer B.**
  **Why:** with all working memory in PSRAM the palm detector took 938 ms and
  the hand landmarks 350 ms (the converter's own model underestimates PSRAM
  about 6 times for large feature maps; `nn.py trace` shows it per epoch
  block); in the lent buffer they take about 65 and 22 ms. `NPU_RAM` in
  tools/nn.py and `GFX_FB_B` must agree (a BUILD_ASSERT checks). After a change
  to the converter's options or memory: `nn.py generate`, rebuild, and
  `nn.py flash` if the weights' CRCs changed (the firmware checks them and
  `ctl.py nn` says so).
- **While frame buffer B is lent the display is single-buffered: every frame
  starts at the top of the panel's refresh (`wait_refresh_start` in gfx.c),
  the camera view is drawn first, by DMA2D, and answers
  `LV_EVENT_COVER_CHECK`.** **Why:** LVGL then draws into the buffer the panel
  scans; the picture stays ahead of the scanout only if nothing is painted
  under it first and the copy starts at the top. Lending and returning happen
  in `show()`/`hide()`, under the transition fade; the return waits for the
  NPU to let go. The tour checks 0 dropped frames through these switches.
- **The landmark network's presence is its output `Identity_1`, handedness is
  `Identity_2`; the converter keeps the .tflite's output order (Identity_2
  first), so `HANDS_PRESENCE_OUTPUT` is 1.** In ST's quantized model presence
  saturates at 0.500 for any hand (about 0.01 for anything else, on the PC
  too), so the board doubles it; handedness is 0.93 for the test picture and
  0.24 for its mirror image. **Why:** the tracker used handedness as presence:
  one hand of every person was dropped (the user: "right hand works well,
  left hand is barely recognized"; 208 palms found, 12 confirmed), and
  presence's flat 0.5 had been taken for a broken handedness output. Check
  a network's outputs on a mirrored and a non-hand input, not only on the
  test picture (`nn.py grab` makes such variants from a real frame).
- **Two tracks must never sit on one hand. A palm detection is a new hand
  only if its palm centre is more than 0.6 palm lengths from every tracked
  palm and its crop overlaps none by half (`tracked_already`); after the
  landmarks, two tracks whose crops overlap by half, or most of whose points
  lie near the other's (`inside`), are one hand and the newer (or smaller)
  goes.** **Why:** with crops alone, the palm detector re-found the hand
  already tracked (a palm's crop and a landmark crop of the same hand can
  overlap by less than half), the duplicate took the slot meant for a second
  hand, was merged away a frame later, and the same happened at every
  search: the user's second hand was hardly ever picked up (1 s of 60).
  And a half-closed hand was tracked twice, whole and as a "fist" on its own
  curled fingers. After both checks: 53 s of 60 with two real hands, and one
  hand opening and closing for 15 s never counted as two. `nn hands 3`
  covers the search with the test picture, not these cases.
- **Image slots are 2 MB, in app/boards/stm32n6570_dk.overlay and in
  app/sysbuild/mcuboot.overlay; the two must agree, and MCUboot is reflashed
  after a change (`n6.cmd flash` without `--app-only`).** **Why:** the
  networks' epoch controller programs (about 460 KB) overflowed the board's
  1.5 MB slot; slot 0 grew into the gap before slot 1, slot 1 up to the
  storage partition.
- **Take register addresses from build/app/zephyr/zephyr.dts or the CMSIS
  header, never by arithmetic from memory.** **Why:** a `dump` of 0x580B_FC00
  (AHB5 assumed at 0x5800_0000; it is 0x5802_0000, so the NPU cache is at
  0x580D_FC00) hung the bus without a fault message, and only a power cycle
  brought the board back.
- **Nothing at fixed PSRAM addresses: buffers come from `psram_alloc`.**
  **Why:** the linker puts its own PSRAM section (the video layer's pool, the
  NPU runtime's blob copies) at 0x9000_0000, where the screenshot copy used to
  be: every screenshot overwrote the video pool's heap.
- **Touch coordinates are panel coordinates**, not content-area coordinates:
  subtract `UI_CONTENT_Y` only when positioning objects inside a screen root.
- **Label the origin of data on screen.** Generated or synthetic sources say
  so (Signal Lab, Explorer). **Why:** the audience will look for fake data.
- **Keep `.cmd` wrappers plain ASCII.** **Why:** non-ASCII characters in a
  batch file produce parser garbage when invoked from PowerShell.
- **Commits carry no Claude attribution.**

## Open questions / known gaps

- Stand-alone boot (BOOT1 = L) works: tried by the user on 2026-11-15, the
  demo starts by itself after a reset (console: MCUboot from flash, then the
  application, network up 6.7 s after reset). A board that does not answer
  and sits in the boot ROM (hot-plug: Rev B, PC 0x1800_xxxx, R3 = 0xCAFEDEAD)
  was reset or power-cycled in development boot (BOOT1 = H), where nothing
  starts it again but `n6.cmd run`: that is not a firmware halt.
- Touch, navigation, motion and colour were checked on the panel by the user on 2026-11-14 (all good). The repository is public: keep host names, serial numbers and personal paths out of committed files.
- Audio out works and is underrun-free in the automated checks; the sample
  clock is PLL2 (HSI-based), measured 47.6 to 47.7 kHz. Video is slaved to it.
- The film's crackle (same place every run) was the soundtrack's note-end
  clicks; fixed and confirmed by ear by the user on 2026-11-15, along with
  transitions and attract mode ("sound is excellent, graphics smooth and crisp").
- **Cinema slider "crash" (2026-11-15, reported by the user: the movie
  crashed while operating the slider, "playing a repeating noise"): probable
  cause found and fixed the same day.** Five fingers on Touch Lab checked by
  the user afterwards (all five slots reported and released, no event
  dropped in the console log), and the slider: "no longer hangs the cinema".
  The new scrub sound (see the invariant) was confirmed by ear by the user
  the same day ("works great, sounds great"). A
  touch release lost in Zephyr's input queue (see the invariant on
  `CONFIG_INPUT_MODE_SYNCHRONOUS`) leaves the slider pressed: Cinema seeks to
  the same frame on every UI frame, so the picture freezes and the audio
  restarts there 60 times a second, until the same slot is touched and
  released again. Reproduced with an injected contact that is never lifted
  (frame held for 8 s with `playing=1`). What does not fit: `ctl.py` timed
  out right after the report, which a stuck press does not explain (the
  tools then opened a connection per command, see the control-port
  invariant). So a real halt is not excluded. History of the search, before
  this was found: the fault state was lost to a reset. Not reproduced since, on
  the old player (450 random actions) or on the reworked one (about 2000,
  console logged): `tools/fuzz_player.py` scrubs, plays, pauses, goes back
  and re-opens (edge starts, a second contact; flash and card clips) without
  a halt or a fault message. Caveat on all but the last 440: until the fuzz was made to return
  to Cinema, a drag from the screen edge with the player closed swiped it to
  other screens, so part of every run abused those instead (and one of them
  drops a frame under a wild two-finger drag; which one is not identified).
  Fixed on the way: a drag starting in the edge-swipe zone closed the film;
  seeks counted as dropped frames and as stalls; Back blocked the UI behind
  a decode; the attract false start below. If it happens again do NOT reset:
  read `captures/console.log` (keep `n6.cmd console --log` running; the log
  is flushed per chunk) and the CPU state.
- The board announces its address when DHCP assigns it
  (`CONFIG_NET_ARP_GRATUITOUS_TRANSMISSION`). Checked 2026-11-15: with the
  companion sending through a power cycle the board answered 0.1 s after
  getting its address; before, Windows held it unreachable for up to a
  minute.
- A 32 GB microSD card (FAT32) is in the slot and holds the three clips in
  `/clips`; they stand in for the flash copies in the list, so attract mode
  plays the film from the card. The flash media partition is still written
  and is what plays when the card is out. Writing 84 MB to the flash over
  SWD takes 8 minutes; to the card over Ethernet, about 22 s.
- Media: `media/film/*.py` render the original clips (Skia + numpy score),
  `tools/n6v.py encode` makes the `.n6v`, `upload` puts it on the card,
  `pack` and `flash` in the media partition at 0x7042_0000. Outputs go to
  `media/out/` (not committed).
- Not built: mounting a card put in after boot without a host command (the
  card-detect pin is not watched; `n6v.py mount` asks for it, and that path
  has not been tried with a real insertion), fast seeks in
  very large files (FatFs walks the cluster chain: a backward seek in a
  1 GB clip on FAT32 may take a few hundred ms; untested), FAT timestamps.
- Microphone: the capture path runs (48 kHz, 0 errors) and Signal Lab can
  show it, but it has not shown a real sound yet: U13 gives noise only (see
  the invariant). Open: a known good microphone on CN5 (the PUI part needs
  `mictune 0 1 1 8 5 8 1`: 1.536 MHz, Sinc5 / 8), or a tap test on U13.
- Unexplained, once (2026-11-15): during a tour the board stopped answering
  the network and the debug probe could no longer see the processor at all,
  even after the probe's own power cycle (`No STM32 target found`); the
  user had touched the screen meanwhile. Only unplugging the board's power
  brought it back, and the next tour passed. The console log had no fault
  message. If it recurs, note what was attached (the camera module had just
  been fitted) and measure the board supply before cycling it.
- NPU, hands first, then faces: plan and steps N1 to N4 in docs/plan.html
  ("The NPU on the camera picture"). N1 done: runtime on Zephyr, both hand
  networks verified against the PC. N2 built: hand tracking on the Camera
  screen, checked end to end with the built-in test picture (`nn hands 2`:
  Thumbs up) and by the user with real hands on 2026-11-16: the right hand
  tracked well, the left barely, which was the presence/handedness mix-up
  (see the invariant); after the fix "tracking was excellent for each hand,
  both front and back". Two hands at once since 2026-11-16 (see the
  invariant on tracks), checked by the user in a spoken, guided session.
  Left/right: two palms shown together get opposite labels, each matching
  its hand's shape in the picture, and the user found the labels correct
  on the panel. An earlier timed test seemed to show the label following
  palm against back, but the user had not held the hands in the order asked,
  so it shows nothing. Open: two hands run at 15 fps because each hand's
  crop (about 9 ms of CPU, reading PSRAM) and landmarks (22 ms) run one
  after the other; cropping the second hand into a staging buffer while the
  NPU runs the first would give about 22 fps, but needs the runtime driven
  step by step. A search for a second hand costs the first a 130 ms frame
  twice a second. The palm detector varies 58 to 165 ms (142 to 165 in
  attract laps, cause not known; it only delays finding a hand). Faces (N3)
  and the soak test (N4) next.
- The shell holds 12 screens and all 12 are used: a thirteenth needs
  `MAX_SCREENS` raised in shell.c.
