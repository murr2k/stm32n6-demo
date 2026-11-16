# Changelog

No releases are tagged; changes are listed by day, newest first. Each line
names what a user of the demo or of its tools would notice; the commit log
has the detail, and `CLAUDE.md` the reasons behind the rules that came out of
them.

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

- Camera screen: the hand tracked is named left or right, with how sure the
  network is.
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
