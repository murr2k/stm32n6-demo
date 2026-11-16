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
.\.venv\Scripts\python.exe tools\dataset.py info   # Explorer's data set: must say "source flash"; pull + flash put it there
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
  first 2 MB reserved for the screenshot copy. Budget, measured 2026-11-15:
  internal RAM 771 of 840 KB (LVGL pool 256 KB, heap 64 KB, the card's two
  32 KB I/O buffers, stacks), all screens' LVGL objects are built at boot and
  kept; PSRAM 11.6 of 32 MB and never freed: the player 7.0 MB (4 MB
  read-ahead window, three 576 KB decode buffers, 1 MB index), Cinema 2.4 MB,
  screenshot 2 MB (25.6 MB when Explorer's data set has to be generated
  because the flash holds no copy); code 0.89 MB in a 1.5 MB slot of a 128 MB
  flash. A new demo with buffers needs a place in this before it is written.
- Flash map (128 MB at 0x7000_0000): MCUboot and the two image slots up to
  0x7041_0000, media partition 0x7042_0000 to 0x7700_0000 (108 MB, 84 MB
  used), Explorer's data set 0x7700_0000 to the end (16 MB, 14 MB used).
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
  `mic_copy()`. Signal Lab takes it as its second source (`sig_set_source`,
  a tap on the SOURCE label, `ctl.py "sigsrc 1"`); the generator is the
  default. `ctl.py mic` shows rate, errors and level, `acap.py --mic`
  records a WAV, `ctl.py "mictune ..."` restarts the capture with other
  settings (mic.h lists the ten values), `micprobe` and `micbits` look at
  the data pin without the MDF (`micbits` stops interrupts for its
  duration: sound and picture pause, the underrun counter moves).
- Screens implement `struct screen` (ui/shell.h) and are listed in main.c.
  Index order matters to `tools/tour.py` (SCREENS list): home, deck, siglab,
  explorer, touchlab, hostlink, coremap, cinema, specimen, hood. Home shows two
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
- **`raster_line` and `raster_poly` must stay free of per-pixel-row divisions
  and edge scans.** **Why:** a 64-bit division per column made 25 wireframe
  lines cost 1.5 ms, and two edge scans per row made 22 tall posts cost 6 ms.
  The polygon fill walks its two edge chains; lines use a float slope.
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
- **PSRAM in use: 11.6 of 32 MB** (`ctl.py media`, `psram_kb`), 12.2 once
  `acap` has been used (its 576 KB buffer is taken on first use). The
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
  datasheet's. With the clock stopped the line is free and unclamped
  (`micprobe`), so the part has its supply and sleeps as it should. That
  is a microphone whose electronics run and whose transducer delivers
  nothing. Wiring per the
  schematic (MB1939 sheet 16): U13 on PE2/PE8 through analog switch U14,
  which hands PE8 to connector CN5 when a board there pulls MIC_DET low;
  VDD_MIC from 3.3 V through SB19; LR high, so data is valid while the
  clock is high (falling edge). The user's own good captures on this board
  were made with an external PUI DMM-2726-B on those pins (1.536 MHz,
  Sinc5 / 8), not with U13. ST's unmodified MDF_AudioRecorder gave "weak
  audio with hiss" here. Verify the path with a known good microphone on
  CN5 before anything else. Research on this (other owners' reports, ST's
  and third-party settings, datasheet, MDF pitfalls, ranked tests):
  `reports/STM32N6 DK microphone noise only.md`, notes in `research_notes/`.
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
  PSRAM. Discovery: `N6HQ` to udp/5557 (broadcast) is answered with an`n  `N6HA` line of the board's services (hostlink.h); the companion asks every
  10 s and sends to every board that answered.
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
- Camera (IMX335) and NPU are a later addition. Home has room now; memory is
  the question (see the budget under Architecture).
