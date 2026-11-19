# Changelog

No releases are tagged; changes are listed by day, newest first. Each line
names what a user of the demo or of its tools would notice; the commit log
has the detail, and `CLAUDE.md` the reasons behind the rules that came out of
them.

## 2026-11-18

### Fixed

- **The video player showed each frame 33 ms after its sound** (23 to 53),
  found by the board's own recorder measuring the A/V sync clip in DAC
  frames: a frame was chosen for the sample at the DAC, then decoded,
  converted and shown at the next present. It now decodes 30 ms ahead:
  -8 to +13 ms.
- **`tools/record.py` left a video-only copy of every recording behind**
  (`<name>-video.mkv`), which outlived the recording when that was
  deleted. It is removed once the MKV with sound is written.
- **`tools/record.py`'s files ran 40 s longer than the recording.** Every
  picture carried a 40 s duration (the H.264 reader's 1/1,200,000 s time
  base kept as milliseconds): the last picture stood for 40 s, and a
  re-encode at a constant frame rate held the first one for 40 s and put
  every picture after it 40 s behind its sound. Each picture now lasts
  until the next. Joining picture and sound also moved the picture ahead
  by its first time (0.03 to 0.14 s); it now stays on the sound's clock.
- **The board's recordings were darker and harsher than the panel.** The
  encoder writes full-range video and the stream did not say so, so every
  player read it as TV range: the panel's dark navy came out black. The
  stream now says full range, and a recording matches a screenshot of the
  same screen.
- **A 60 fps recording of the attract lap lost pictures in Explorer's
  zoom** (11 to 18, always in the same half second). The lap's film, hidden
  under the demos, was still decoding its black frames, a quarter of the
  CPU, and the recorder's network sender starved. Hidden, it now decodes
  nothing: a full lap at 60 fps has every picture, and the demos get that
  CPU back too.

### Changed

- **Attract mode is a lap set to the film's music**: it opens with *N6: the
  film*, cuts at 0:43 to the demos, each arriving on a bar line while the
  film's music plays on under them (Home, Flight Deck, Signal Lab, Explorer,
  Host Link, Core Map, Under the Hood), and ends with the film's last scene,
  its coda and end titles (who made it; how: the demos live, the film and
  its score written as code), then two seconds of black; 2 min 38 s, then
  again. The live Camera screen is no longer
  in it: after Signal Lab, four bars replay the board's own recording of
  the Camera screen tracking two hands, marked "Recorded on this board".
  Any touch stops it and the music.
  - The music is the film's score at lap length, with a whoosh at every
    screen change and a break under the theremin; one clip on the card
    carries it, `_attract.n6v`, made by `media/film/attract.py` (upload it
    with `n6v.py upload`). Without it the screens take turns as before.
  - On Signal Lab the theremin's ghost flies once, in the lap's three bars,
    quietly under the music instead of replacing it (`ctl.py "thmunder
    -80"` sets how far under, in tenths of a dB).
  - `ctl.py where` tells where in the lap it is (`lap_ms`, `lap_len`), and
    `record.py --attract` records exactly one lap from its first sound.

### Added

- **The board records itself**: its screen through the N6's hardware H.264
  encoder, with the sound it sends to the headphones, streamed to a PC on
  tcp/5559 (`tools/record.py`, `--fps 60`, `--attract` for one lap of
  attract mode).
  - The encoder reads the frame buffer on screen directly (no copy, no
    conversion by the CPU): about 7 ms a picture, 60 fps fits.
  - The PC side puts every picture on the sound's clock and writes MKV and
    MP4 files.
  - `tools/avsync.py` records the A/V sync clip and measures each flash
    against its beep; `ctl.py rec` reports the recorder.
  - Every picture carries the display's own numbers for its frame (the UI's
    time for it, frames presented, frames dropped: `frames`, `frame_us`,
    `dropped` in the CSV), and twelve buffers ride out short pauses of the
    network.
  - The Camera screen only when asked for (`record.py --camera`: camera
    footage, so keep faces out of the picture). There the display copies
    each finished frame for the encoder (3 ms with hands tracked, 0 dropped
    frames). Without `--camera` a recording holds the picture from just
    before it.

## 2026-11-17

### Fixed

- **A dropped frame in Flight Deck's fade-in, at the same point of the
  attract lap**, twice in a 9-hour soak test. The new screen's settle moved
  its objects, so LVGL redrew the whole screen on every frame of the fade-in
  (9 to 13 ms on Flight Deck, while visible), and Flight Deck's label updates
  landing on one of those frames forced a layout of the moving screen
  (up to 4.4 ms). The settle is now done by the display controller (below):
  0 drops in the same attract laps, and the least margin before a swap went
  from 1.8 ms over to 2.2 ms spare.

### Changed

- Signal Lab's analysis runs only while Signal Lab or Home (its thumbnail)
  shows: every other screen has about 0.8 ms more a frame (2.9 ms at
  worst).
- **Audio blocks of 128 frames (2.7 ms) instead of 512**, for the output
  and the microphone. The microphone reaches the headphones 8.7 ms after
  it hears something (35 ms before), and the theremin's touch to sound is
  3.8 ms, worst 4.8 (was 11 to 21 ms). The tour has 0 underruns with them,
  card upload and attract lap included.
- Signal Lab's microphone spectrum uses the DAC's measured rate (47.7 kHz,
  the same clock): its frequency axis read 0.6 % high.
- **The microphone works on the replacement DK**: Signal Lab's microphone
  source shows real sound (claps 41 to 63 dB above a quiet room at
  -96 dBFS). The first board's onboard microphone had failed; the firmware
  was unchanged. The MDF now samples in low-frequency master mode, which
  RM0486 asks for at this clock ratio and which records the same.
- Host tools default to the replacement board's console, COM18
  (`N6_PORT` sets another).
- The settle of a screen change (10 px upwards while it fades in) is the
  LTDC layer's window position, like the fade, so it costs no render time.
  The title bar settles with the screen now.
- Frames are swapped by the application instead of Zephyr's display driver:
  about 1 ms more for every frame on every screen. The driver cleaned the
  data cache line by line over the whole 1.5 MB buffer before every swap
  (0.54 ms, nearly all of it a loop over clean lines) and swapped at line 0;
  a clean of the whole cache takes 25 µs, and a frame handed over during the
  vertical blanking still makes the next refresh.

### Added

- **Listen to the microphone, with a parametric equalizer under your
  fingers** (Signal Lab, MICROPHONE source).
  - LISTEN sends the microphone to the headphones (off whenever the screen
    shows); GAIN steps +20, +30 and +40 dB; EQ puts the equalizer in or
    bypasses it (held: all bands flat).
  - Five bands (low shelf, three peaks, high shelf) are handles on the
    spectrum: drag across for frequency, up and down for gain (±15 dB), and
    a second finger beside a held handle sets its width.
  - The spectrum shows what is heard, with the curve over it. A limiter at
    −6 dBFS guards the ears. Attract mode does not start while LISTEN is on.
  - Remote control: `ctl.py mon`, `monlisten`, `mongain`, `moneq`,
    `monband`, `monflat`, `monreset`.
  - `montone` is a closed-loop test: with headphones by the board it plays a
    tone into them and measures it in the microphone before and after the
    equalizer. The measured gain matched the curve within 0.2 dB in ten
    cases.
  - The tour drags a band with two fingers and checks that leaving the
    screen gives the output back with LISTEN off.
- `tools/soak.py`: the board left running for hours (attract mode and the
  camera with both networks in turn), polled every second, with a summary
  of resets, faults, leaks, frame-time drift and every dropped frame.
  First run, 9 hours: no reset or fault, LVGL pool, heap and PSRAM flat,
  frame times unchanged, 2 dropped frames (fixed above).
- Frame diagnostics: `ctl.py swtrace` (each frame of the last screen
  transition: switch time, main loop, LVGL, other threads, margin before
  the swap deadline, the layer's fade), `droptrace` (the same for the frames
  up to the last dropped one), `svct` (the main loop's time per screen
  service and tick), `mem` and `stacks` (heap, LVGL pool, PSRAM, stack left
  per thread); `stats` adds `margin_min_us` and `late_swaps`.
- `docs/best-practices.md`: what this project had to figure out about the
  STM32N6570-DK, its display and Zephyr (flashing, memory, the HAL beside
  Zephyr, presenting frames, LVGL, touch, audio, card, network, camera, NPU),
  for developers starting their own project on the board.

- **A theremin on Signal Lab**: a third source (SOURCE cycles generator,
  microphone, theremin) turns the spectrum into an instrument. A fingertip
  sounds the frequency of the column under it at the level of its height,
  so the peak sits under the finger; the height is a loudness taper (equal
  steps of height, equal steps of loudness), and a finger slid off the
  spectrum keeps its level until it lifts. Up to five fingers, four timbres
  (WAVE), semitone snap (SNAP), a SOUND switch (off: it plays on screen in
  silence, also in attract mode), a crosshair and readout per finger, the
  measured touch to sound on screen. Pitch and analysis use the DAC's
  measured 47.7 kHz. In attract mode a ghost plays a flying saucer.
  `ctl.py sigsrc 2`, `thm`, `thmwave`, `thmsnap`, `thmghost`, `thmsound`;
  the tour plays it and checks its sound.
- Gesture "The bird": the middle finger up from a fist.
- `ctl.py micinject`: a self-test of the microphone capture path. With the
  microphone asleep, a square wave made with the data pin's own pull
  resistors goes through the MDF filters, the DMA and the ring buffer like
  microphone bits; on this board it comes out at the right frequency and
  shape, which leaves the board's microphone as the source of its noise.
- `ctl.py micpdm`: a synthetic microphone. A sine through a sigma-delta
  modulator, put on the microphone's data pin by DMA, decoded by the same
  chain and shown on Signal Lab. `micscope` also records the data pin's
  pull, to time how the line follows it.

### Changed

- Faster Signal Lab: 4.6 ms a frame from 8.3 in the tour. Its axis captions
  are drawn with the plot instead of as label objects that were redrawn every
  frame, and the gradient fill under curves (also used elsewhere) divides
  once per column, not once per pixel.
- The microphone's findings and sources: `docs/microphone.md`.

## 2026-11-16

### Fixed

- **Hand tracking drops left hands.** The tracker read the landmark
  network's handedness output as its presence output, so one hand of every
  person failed the presence check (208 palms found, 12 confirmed). Both
  hands now track at the camera's 30 fps, palm or back, checked on the panel.
  The real presence output of ST's quantized model saturates at 0.5 for any
  hand; the board doubles it.
- **A dropped frame about once every five attract laps**, at the switch from
  a playing film into Under the Hood: the switch runs just after a present,
  so its time came out of the budget of Under the Hood's first frame (up to
  15.8 ms). Its side panel now waits one frame (0 drops in 16 switches, was 5).

### Added

- **Two hands at once** on the Camera screen, each with its own colour,
  gesture and left/right label; one hand tracks at 26 to 30 fps, two at
  15 fps. A hand already tracked, or part of one (a half-closed hand's curled
  fingers), is not taken for a second hand. `nn hands 3` tracks the test
  picture twice (one copy mirrored), `nn cfg` sets how many hands and how
  often to look for another; `nn hands` reports both.
- Camera screen: the hand tracked is named left or right, with how sure the
  network is.
- Gesture "I love you" (American Sign Language): rock on with the thumb out.
- Tracker diagnostics: `nn hands` reports the best palm score of each search
  and counts palms found, confirmed and lost; `nn thr` sets the palm and
  presence thresholds; `nn grab` copies a frame as the tracker saw it.
  `tools/nn.py watch` prints the counters over time, and `tools/nn.py grab`
  saves frames to `captures/` and runs the original models on the PC on the
  board's inputs and on variants.
- `ctl.py stats` reports `tail_worst_us`, the UI thread's time after a
  present (the cost of a screen switch). The tour enters every screen
  through a transition and makes the film to Under the Hood switch six times.
- This changelog; README sections on hand tracking and the transition budget.

### Changed

- A hand tracked on the Camera screen counts as someone using the demo:
  attract mode does not start under it.
- Camera screen side panel: a gesture per hand (HAND 1, HAND 2, captioned
  with left or right), the number of hands with the tracking rate, gain and
  exposure on one line.

## 2026-11-15

### Added

- **Hand tracking on the NPU** (Camera screen): Google MediaPipe's palm
  detector and 21-point hand landmark network, converted with ST Edge AI and
  run by ST's runtime on Zephyr. The networks work in the display's second
  frame buffer, which the display lends while the screen is up (it runs
  single-buffered meanwhile); weights are read in place from the flash.
  Gestures from the joint angles. `tools/nn.py` fetches, converts, flashes
  and checks both networks against the original models on the PC; ST's
  runtime and the converted networks stay out of the repository (SLA0044).
- **Camera screen**: the IMX335 on the B-CAMS-IMX module at 30 fps,
  demosaiced and scaled by the DCMIPP, exposure and white balance by ST's ISP
  middleware (fetched by west, not committed), copied to the display by DMA2D.
- **Settings screen**: the board's address, DHCP or fixed (entered on a
  keypad, applied without a restart, kept on the card); a QR code for the
  board's own description at `http://<board>/` (also `/agent.md` and
  `/status.json`).
- **Under the Hood screen**: this firmware's threads as ribbons in 3D, CPU
  share over 12 s on a logarithmic axis by default, stacks, memory.
- **Core Map screen**: a PC's logical processors as posts on a floor plan of
  its chip, drag to orbit, pinch to zoom.
- **Host Link screen** and the host companion (`run.cmd`): a PC's per-core
  load, memory and network over UDP, with round-trip times. Boards are found
  by broadcast; hosts with up to 255 processors (Core Map groups large hosts).
- **microSD media library**: clips uploaded over Ethernet (2 to 4 MB/s, CRC
  checked) and streamed from the card while the demo runs.
- **Audio output and audio-clocked video**; the original 48 s film, its
  soundtrack and two companion clips, rendered from code.
- **Screen transitions** (a hardware fade with the LTDC layer alpha) and
  **attract mode** (the demo tours itself after 90 s without a touch).
- **Microphone capture path** (MDF1, 48 kHz, 24 bit) as Signal Lab's second
  source, with bring-up tools (`micprobe`, `micbits`) and a research report
  on why the board's own microphone gives only noise.
- Home pans sideways when there are more than eight screens.
- Explorer's 14 MB data set is used in place from the flash, which freed
  14 MB of PSRAM.
- `tools/acap.py` records what goes to the DAC and counts discontinuities;
  `tools/fuzz_player.py` abuses the video player with random touches.

### Fixed

- **Touch releases lost** in Zephyr's input queue, which left the Cinema
  slider pressed (the reported "crash" while scrubbing): input callbacks now
  run synchronously, with no queue.
- **Rough sound on the Cinema slider**: scrubbing plays short cross-faded
  pieces of the clip, silence while the finger rests; no step in the sound.
- **Clicks in the film** at fixed places: bass notes in the soundtrack ended
  mid-cycle; every voice now ends in a release ramp.
- Attract mode starting on about one touch release in five (an unsigned time
  difference); the control port becoming unreachable under tools that
  connected per command.
- Explorer dropping frames: the build now uses the M55's double-precision
  FPU.

## 2026-11-14

### Added

- **Video player**: Motion JPEG clips with PCM sound (`.n6v`) from the
  flash, decoded by the hardware JPEG codec at register level and converted
  by DMA2D, full screen; the Cinema screen.
- **Screens**: Home, Flight Deck, Signal Lab, Explorer, Touch Lab, Specimen.
- **Platform**: the panel at 59.98 Hz with double buffers in internal SRAM
  swapped in the line interrupt, LVGL on top, the screen shell, the remote
  control port and frame buffer readback over Ethernet.
- **Bring-up**: an unattended build, flash and run loop over the ST-LINK
  (with power cycles, because ST's external loader leaves the flash in octal
  mode), display and Ethernet.
- Apache-2.0 licence.
