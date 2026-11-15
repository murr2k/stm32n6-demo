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
.\run.cmd                   # host companion: live data for Host Link and Core Map
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
  first 2 MB reserved for the screenshot copy.
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
- Screens implement `struct screen` (ui/shell.h) and are listed in main.c.
  Index order matters to `tools/tour.py` (SCREENS list): home, deck, siglab,
  explorer, touchlab, hostlink, coremap, cinema, specimen. Home shows eight
  tiles, its current maximum; a tenth screen needs a 3 x 3 Home.

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
- **PSRAM is nearly full: 25.3 of 32 MB** (`ctl.py media`, `psram_kb`), 25.9
  once `acap` has been used (its 576 KB buffer is taken on first use). The
  read-ahead window is 4 MB for that reason; 8 MB left 2.5 MB free.
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
- Camera (IMX335) and NPU are a later addition; keep a screen slot for them.
