# STM32N6570-DK with Zephyr: best practices

This is what we learned building a 60 Hz touch demo on the STM32N6570-DK
with Zephyr 4.4.0 and LVGL 9.5. It is written for developers starting their
own project on this board. It covers:

- the 5 inch 800 x 480 panel on the LTDC, and GT911 touch;
- audio out through the SAI and the WM8904, and the microphone;
- the microSD slot and Ethernet;
- the B-CAMS-IMX camera and the NPU.

Most of these points took hours to find, and few are in the documentation.

Each item gives the rule, why it matters (what went wrong and what was
measured), and where the working code is in this repository. The numbers
were measured on one board (MB1939 rev C02) with this firmware; they are not
datasheet figures. This project's full list of rules is in
[`CLAUDE.md`](../CLAUDE.md), and the microphone investigation is in
[`microphone.md`](microphone.md).

## Contents

- [Flashing, booting and debugging](#flashing-booting-and-debugging)
- [Build configuration](#build-configuration)
- [Memory](#memory)
- [Clocks and peripherals through the HAL](#clocks-and-peripherals-through-the-hal)
- [The display](#the-display)
- [LVGL on this display](#lvgl-on-this-display)
- [Frame timing](#frame-timing)
- [Touch](#touch)
- [Threads and blocking](#threads-and-blocking)
- [Audio](#audio)
- [Video decode](#video-decode)
- [microSD card](#microsd-card)
- [Network](#network)
- [Camera](#camera)
- [NPU](#npu)
- [Microphone](#microphone)
- [Testing and measurement](#testing-and-measurement)
- [Small things that bit](#small-things-that-bit)

## Flashing, booting and debugging

**Power-cycle the board before you flash it and before you start it.**
- ST's external loader (`MX66UW1G45G_STM32N6570-DK.stldr`) leaves the NOR
  flash in octal DTR mode. A pin reset does not clear that.
- Zephyr's XSPI driver then fails its first SPI-mode command
  (`XSPI reset enable (SPI/STR) failed`) and MCUboot aborts.
- Only removing power puts the flash back in SPI mode.
- The reverse also holds: the loader cannot erase while firmware is running
  from the flash.

The DK's ST-LINK can switch the target's power:
`STM32_Programmer_CLI -c port=swd mode=UR --power off index=0 --power on index=0`.
[`tools/n6.py`](../tools/n6.py) does this before every flash and every
start.

**Never reset the chip from firmware**, for example to apply a setting. A
software reset also leaves the flash in octal mode, and MCUboot fails. Apply
settings at run time instead.

**Leave `ap=` out of the SWD connect string, and don't use `west flash`.**
The upstream `board.cmake` passes `ap=1`, which this probe and chip revision
refuse. `-c port=swd mode=HOTPLUG` works for both flashing and starting.

**Develop in development boot (BOOT1 = H, BOOT0 = L).** Nobody has to touch
the switches:
1. Power-cycle the board.
2. Load MCUboot over SWD and start it:
   `-halt -d build/mcuboot/zephyr/zephyr.elf -s 0x34180400`. This is
   sysbuild's FSBL variant, which runs from RAM.
3. MCUboot chain-loads the application from slot 0 of the NOR flash, where
   it runs in place.

In flash boot (BOOT1 = L), the boot ROM loads MCUboot from the flash at every
power-up and the board runs on its own. A board in development boot that was
reset by hand just sits in the boot ROM; that is not a firmware hang. A
hot-plug read shows PC 0x1800_xxxx and R3 = 0xCAFEDEAD.

**Put ST's signing tool on the PATH for sysbuild.** The FSBL image is signed
with `STM32_SigningTool_CLI`, which is in STM32CubeProgrammer's `bin`
directory.

**Grow the image slots in both overlays, then reflash MCUboot.**
- The application's overlay and `app/sysbuild/mcuboot.overlay` must describe
  the same slots.
- MCUboot only learns a new table when it is reflashed.
- The board's slots are 1.5 MB. The NPU's compiled programs pushed the image
  past that, so here the slots are 2 MB.

**Don't attach a debugger to the running application.**
- With Zephyr idle in WFI, the access port errors out
  (`DEV_AP_ACCESS_ERROR`, and the device shows as "Rev Z").
- Debug from reset instead: [`tools/gdbrun.ps1`](../tools/gdbrun.ps1)
  starts the ST-LINK gdbserver and GDB and loads the ELF.
- Don't use a hot-plug `-halt` to "just look" at a running board either. It
  stops the core.

**When a board stops answering, read the fault before you reset it.** A core
that has faulted is not in WFI, so hot-plug works:
- `STM32_Programmer_CLI -c port=swd mode=HOTPLUG -halt -coreReg` gives the
  registers.
- `-r32 <PSP> 0x20` gives the stacked exception frame. Word 6 is the
  faulting PC.
- `build/app/zephyr/zephyr.map` names the function.

Keep a console logger running in every test (`n6.cmd console --log`). The
fault message from a crash nobody recorded is lost for good. A board that
ignores the network but shows the WFI signature is still running; see
[Network](#network) for one way that happens.

**Log in deferred mode; use immediate mode only to chase an early hang.**
- Immediate mode masks interrupts for a whole UART line (milliseconds),
  which breaks audio and display timing.
- Deferred mode shows nothing if the board hangs before the log thread runs.
- So keep a `debug.conf` that switches to immediate mode, and build with it
  when needed (`n6.cmd build -- -Dapp_EXTRA_CONF_FILE=debug.conf`).

## Build configuration

**A new `.overlay` or `.conf` file needs a pristine build.** CMake only looks
for them when it configures, so an incremental build silently ignores a new
one.

**Select the double-precision FPU.**
- The N657's Cortex-M55 has one (MVFR0 = 0x10110221).
- Zephyr 4.4.0's SoC definition selects only `CPU_HAS_FPU`, so the build
  uses `-mfpu=fpv5-sp-d16`. Every `double` operation then becomes a library
  call. Here that cost one screen 2 to 5 ms per frame.
- The fix is an application Kconfig symbol that selects
  `CPU_HAS_FPU_DOUBLE_PRECISION` ([`app/Kconfig`](../app/Kconfig)).
- To check, look for `-mfpu=fpv5-d16` in `build/app/compile_commands.json`.

**...but keep per-sample DSP in single precision.** The M55's double unit is
there, but it is slow for chains of dependent multiply-adds.
- Five double-precision biquads cost about 1160 cycles per sample: 0.75 ms
  of every 10.7 ms audio block, on the audio thread. The disassembly showed
  every double operation as an inline FPU instruction, not a library call.
- Float biquads are too coarse near 20 Hz. A state-variable filter (Andrew
  Simper's trapezoidal SVF) is not, and it cost about a third as much.
- Use double only for one-off work, and count that cost too. An equalizer
  curve drawn in double, with a `pow`, a `cos` and five divisions per
  column, took 2 to 6 ms of a frame and dropped one. The same curve from the
  filters' analogue prototypes in float, with a table of `tan` values per
  column, takes a fraction of a millisecond.

**Add LVGL's Helium assembly by hand.**
- With `CONFIG_LV_DRAW_SW_ASM_HELIUM=y` the link fails: Zephyr's LVGL module
  lists the C sources but not `src/draw/sw/blend/helium/lv_blend_helium.S`.
- Add that file to the app.
- For that one file, pre-define `ZEPHYR_MODULES_LVGL_LV_CONF_H_`. Zephyr's
  `lv_conf.h` includes `<string.h>`, which the assembler can't read.

See [`app/CMakeLists.txt`](../app/CMakeLists.txt).

**Select the HAL modules you drive yourself.** `USE_STM32_HAL_SAI`, `_DMA`,
`_DMA_EX`, `_MDF`, `_JPEG` and `_DMA2D` are hidden symbols that drivers
normally select. Where no Zephyr driver does, select them from an
application symbol ([`app/Kconfig`](../app/Kconfig)).

**Set a MAC address.** The OTP cell for it is not programmed on this board:
it reads 00:00:00:00:00:00. Put a fixed, locally administered address in the
overlay (`local-mac-address`, with the `nvmem-cells` properties deleted).
That keeps the DHCP lease, and so the IP address, the same across builds.

**Batch configuration changes.** A source change rebuilds in about
2 minutes. Any change to `prj.conf`, Kconfig or CMake rebuilds LVGL, which
takes 6 to 8 minutes.

## Memory

The N657 has:
- 4.2 MB of internal SRAM, in banks;
- 32 MB of HexaSPI PSRAM at 0x9000_0000;
- 128 MB of octal NOR on XSPI2, memory-mapped at 0x7000_0000.

**AXISRAM3 to 6 are off at reset: enable their nodes before you touch
0x3420_0000.** These banks are powered down and unclocked, and a write to
them hangs the core with no fault output. Setting `status = "okay"` on
`&axisram3` to `&axisram6` makes Zephyr's `stm32n6_axisram` driver switch
them on before the kernel starts.

**Put the frame buffers in internal SRAM.** It measured 2 to 4 times faster
than the PSRAM for rendering.
- FLEXRAM with AXISRAM1 and 2 (2 MB at 0x3400_0000) and AXISRAM3 to 6
  (1.75 MB at 0x3420_0000) form one contiguous block.
- Two 800 x 480 x 4-byte buffers take the top 3 MB, and the application gets
  the 840 KB below them.
- Shrink the application's RAM node to exactly that size (see the
  [overlay](../app/boards/stm32n6570_dk.overlay)). The linker then fails
  loudly when the app outgrows it, instead of growing into a frame buffer.

**Use no fixed PSRAM addresses.**
- The linker puts its own PSRAM section at 0x9000_0000: the video layer's
  buffer pool, and copies that the NPU runtime makes.
- A screenshot buffer hard-coded at that address overwrote the video pool's
  heap with every screenshot.
- Allocate from after the linker's section (`__PSRAM_end`). Here that is a
  bump allocator, [`psram.c`](../app/src/platform/psram.c).
- Keep any block that another bus master addresses directly, such as the
  NPU's spill memory, out of the allocator.

**Use read-only data in place from the flash.** The NOR is memory-mapped and
cached. Reading a 14 MB data set in place, instead of from a PSRAM copy, made
the worst frame only 0.3 ms slower (14.1 against 13.8 ms) and gave 14 MB of
PSRAM back. Check a CRC at boot, and fall back if the copy is missing or
stale.

**Take register addresses from `build/app/zephyr/zephyr.dts` or the CMSIS
header, never from memory.** AHB5 starts at 0x5802_0000, not 0x5800_0000. A
read at an address worked out from the wrong base hung the bus without a
fault message, and only a power cycle brought the board back.

**Mind the data cache around every DMA transfer.** The cache is write-back.
- **Buffers a DMA writes** must start and end on 32-byte cache lines. The
  driver invalidates the cache over them, and data that shares a line with
  the buffer is lost. Read card sectors into an aligned buffer, then copy.
- **Linked-list nodes:** flush a GPDMA node from the cache before the
  transfer starts. A circular transfer reloads it from memory on every lap,
  and a node still held in the cache becomes a corrupt descriptor seconds
  later.
- **Direction:** flush what the CPU wrote before a DMA reads it. Invalidate
  what a DMA wrote before the CPU reads it.

## Clocks and peripherals through the HAL

For several N6 peripherals, Zephyr 4.4.0 has no driver, or not the one we
needed. This project drives the SAI, GPDMA, MDF, JPEG codec and DMA2D through
the ST HAL alongside Zephyr. Four things go wrong when you do that.

**Enable each peripheral's sleep clock too.**
- `__HAL_RCC_xxx_CLK_ENABLE()` sets only the RCC enable bit.
- On the N6 the low-power enable bits (`RCC_xxxLPENR`) reset to 0. So the
  clock stops whenever the CPU executes WFI, which Zephyr's idle thread does
  many times per frame.
- Add `__HAL_RCC_xxx_CLK_SLEEP_ENABLE()`. Zephyr's clock driver sets both
  bits for its own peripherals.
- Without it, the SAI and GPDMA froze in every idle period: first underruns,
  then a stream running at a fraction of real time, then a stall.
- Memories that a DMA uses need it as well
  (`__HAL_RCC_AHBSRAM1_MEM_CLK_SLEEP_ENABLE()`).

**Don't let the HAL reprogram clocks that Zephyr owns.**
`HAL_RCCEx_PeriphCLKConfig` reprograms IC dividers behind the Zephyr clock
driver's back. Set the dividers in the devicetree instead
(`&ic7 { pll-src = <2>; ic-div = <125>; status = "okay"; }`). Choose a
kernel clock with a plain LL call (`LL_RCC_SetSAIClockSource`,
`LL_RCC_SetMDFClockSource`).

**Give every bus master its RIF attributes.**
- On the N6, the resource isolation framework (RIF) gives each bus master
  security attributes.
- Zephyr's SoC code sets them for the display, camera, Ethernet, DMA2D, NPU
  and video encoder, but not for the SDMMC.
- Without them, the card's internal DMA wrote nothing, and the mount failed
  with `fs mount error (-19)`.
- Calling `HAL_RIF_RIMC_ConfigMasterAttributes()` with secure, privileged
  attributes before mounting fixed it
  ([`sdcard.c`](../app/src/media/sdcard.c)).
- Check this for any new bus master.

**Keep one owner per peripheral.** Leave Zephyr's devicetree node for a
peripheral you drive through the HAL disabled (`&sai1_a` and `&gpdma1`
here).

## The display

**The stock timing is 48.8 Hz; 60 Hz takes two overlay changes.**
- The board's devicetree gives the LTDC a 20 MHz pixel clock from the
  HSI-fed PLL4, with 820 x 500 total: 48.8 Hz.
- Change 1: a 25 MHz pixel clock from the crystal-fed PLL1
  (`&ic16 { pll-src = <1>; ic-div = <96>; }`, 2400 MHz / 96).
- Change 2: longer front porches (`hfront-porch = <20>`,
  `vfront-porch = <9>`), for 832 x 501 total.
- Result: 25 MHz / 416 832 = 59.98 Hz.
- Measure the period at boot rather than assuming it: time two buffers
  swapped 30 times.

**Own the frame buffers.**
- `CONFIG_STM32_LTDC_FB_NUM=0`: the driver allocates no frame buffers.
- Put two full-screen buffers in internal SRAM.
- Run LVGL in DIRECT mode (`LV_DISPLAY_RENDER_MODE_DIRECT`). It draws
  straight into the buffer that will be shown next, so no frame is copied,
  and it keeps both buffers complete by copying only the areas that changed.
- Create the LVGL display yourself (`CONFIG_LV_Z_AUTO_INIT=n`,
  `CONFIG_LV_Z_FLUSH_THREAD=n`), so that the flush callback, where the frame
  is presented, runs on the UI thread.

**Present frames yourself, not with `display_write()`.**

What the driver does: with FB_NUM=0, `display_write()` cleans the data cache
by address over the whole 1.5 MB buffer before every swap. It then swaps the
buffer in its line interrupt at line 0.

Why that hurts:
- The clean took 541 µs per frame, nearly all of it the loop itself: the
  lines were clean already.
- None of the application's timing saw it. A frame that appeared to finish
  406 µs early still missed its refresh.

What [`present()` in gfx.c](../app/src/platform/gfx.c) does instead:
1. Cleans the whole data cache by set and way (`SCB_CleanDCache()`, 25 µs).
2. Writes the layer's address (`LTDC_Layer1->CFBAR`).
3. With interrupts locked, reads the current line (`LTDC->CPSR`):
   - in the vertical blanking, it reloads at once
     (`LTDC->SRCR = LTDC_SRCR_IMR`), with a guard of two lines before the
     active area;
   - otherwise it asks for a reload at the start of the next blanking
     (`LTDC_SRCR_VBR`), sleeps until shortly before then, and polls the bit.

The result is about 1 ms more for every frame. A frame handed over anywhere
in the blanking, not only before line 0, still shows in the next refresh.

Read the geometry from the LTDC registers (TWCR, BPCR, AWCR). On this panel:
- line 0 starts the vertical sync;
- the active area is lines 12 to 491 of 501;
- one line takes 33.3 µs.

The driver is still used to set up the LTDC and the panel, for the boot-time
measurement, and for `display_blanking_off()`.

**Fade and settle with the LTDC, not with pixels.**
- **Fade:** the layer's constant alpha (`LTDC_Layer1->CACR`) fades the
  picture to the background colour (`LTDC->BCCR`) at no cost. A software
  veil over the content dropped a frame in every transition.
- **Background colour:** set `BCCR` to the UI's background colour, or the
  fade flashes a different colour.
- **Settle:** moving the start line of the layer window (`WVPCR`) slides the
  picture vertically, also at no cost.
- Done instead by moving LVGL objects, the settle redrew the whole screen on
  every frame of a fade-in. A 9-hour soak test caught that as dropped
  frames.
- **Reload:** both are shadow registers. Write them, then set
  `LTDC->SRCR = LTDC_SRCR_VBR` to load them at the next vertical blanking.

**Copy pictures into the frame buffer with DMA2D.**
- DMA2D, never the CPU, converts camera frames (RGB565) and decoded video
  (the JPEG codec's YCbCr) into the 32-bit frame buffer.
- Flush the source from the cache before the copy.
- Clean and invalidate the destination, so that no dirty cache line
  overwrites the copy later.
- Use DMA2D from one thread only (the UI thread here).

**Reading back the frame buffer costs a frame.** A screenshot copies 1.5 MB
to PSRAM, which takes about 18 ms. Never grab one inside a window whose
dropped frames you are counting.

## LVGL on this display

A full redraw of the 800 x 480 x 32-bit screen costs about 10 ms of the
16.7 ms budget. So most of these rules are about not triggering one.

**Know what invalidates the whole screen.**
- **Too many areas:** LVGL keeps at most 32 invalid areas per frame
  (`LV_INV_BUF_SIZE`). One more, and it redraws the whole screen. Moving 14
  axis labels did exactly that. Spread bursts of label updates over several
  frames.
- **Moving a screen:** animating the position of a screen's root object
  redraws the whole screen on every frame of the animation.
- **Labels on a redrawn pane:** labels that lie on a pane redrawn every frame
  are redrawn every frame too. Draw that text in the pane's own draw handler
  instead. Moving seven axis captions into the handler took one screen from
  8.3 to 4.6 ms per frame.

**`lv_obj_align_to()` forces a layout.** On a screen whose root was moving,
that took up to 4.4 ms.

**Draw dense visuals into the layer yourself.**
- Spectra, plots and 3D views handle `LV_EVENT_DRAW_MAIN`, take the layer's
  buffer and write the pixels directly
  ([`raster.c`](../app/src/ui/raster.c)).
- This is safe because LVGL's software renderer runs synchronously on the
  UI thread (no `LV_USE_OS`).
- Keep divisions and edge scans out of per-pixel and per-row loops. A 64-bit
  division per column made 25 lines cost 1.5 ms; two edge scans per row made
  22 polygons cost 6 ms.
- A polygon call costs tens of µs whatever its size, so merge runs.

**A widget that covers its whole area must say so.** Answer
`LV_EVENT_COVER_CHECK` with `LV_COVER_RES_COVER`. Otherwise LVGL first fills
the background under the widget: 0.9 ms of a 7 ms video frame.

**Fill rate is the limit, so count overdraw.** Each of these painted the
view several times over and dropped frames:
- fills down to the floor under a loud spectrum;
- overlapping 3D posts at high zoom;
- ribbons drawn as curtains.

The fixes: cap the zoom, draw bands instead of fills, and cap the number of
objects drawn.

**Build every screen at boot and keep it. Don't move or resize objects when
switching screens.** Each change of position or size is a style write, about
185 µs. Resizing two panes and moving seven labels added 2.1 ms to a switch.

**Turn on Helium** (`CONFIG_LV_DRAW_SW_ASM_HELIUM`,
`CONFIG_LV_USE_NATIVE_HELIUM_ASM`; see
[Build configuration](#build-configuration)).

## Frame timing

**Work after a present comes out of the next frame's budget.**
- A screen switch done in an LVGL animation callback runs after the
  display's refresh timer, in the same `lv_timer_handler()` call.
- So it runs after the present. A render-time measurement never sees it, but
  the next frame has that much less time.
- Keep `show()` and `hide()` short. Let a heavy first frame draw part of
  itself a frame later. One screen's side panel waiting one frame cut the
  drops in 16 switches from 5 to 0.
- Measure that tail separately.

**Measure the margin before the deadline, not the render time.** "14 ms to
render" doesn't tell you whether the frame made its refresh. For each frame,
record:
- how soon the thread woke after the previous swap;
- the tail;
- the main loop before LVGL;
- LVGL itself;
- the time other threads took: wall time minus the UI thread's own run time
  from `k_thread_runtime_stats_get()`;
- the time left before the swap deadline.

Keep the last few frames in a ring, and copy it out when a frame drops
(`ctl.py swtrace`, `droptrace`). A single run of this explained the drops
that the soak test found.

**Sleep, then poll.** To act right after a display event (a swap, or the
start of a refresh), `k_usleep()` until a few hundred µs before it, then poll
the register. Here the UI thread wakes within one line (33 µs) of the swap.

**Count dropped frames, and require zero.**
- A counter catches what eyes don't, including frames missed during the dark
  part of a fade, which nobody sees.
- Record the layer's alpha with each frame, to tell those apart from visible
  drops.

**When the display has to run from one buffer** (here while the NPU borrows
the second one as working memory):
- start every frame at the top of the refresh;
- draw the top of the screen first, so that drawing stays ahead of the
  scan-out (here the camera picture goes in first, by DMA2D, and covers the
  screen);
- lend and return the buffer under a transition fade.

## Touch

**Set `CONFIG_INPUT_MODE_SYNCHRONOUS=y`.**

The default, and why it fails:
- Input events go through a 16-event queue, drained by a thread at the
  lowest priority.
- The GT911 driver sends 4 events per contact in one burst, from the
  cooperative system work queue.
- Five contacts never fit. Fewer don't either, while the UI and other
  threads keep the input thread waiting.
- A dropped release leaves a contact down for good. Here a video slider
  stayed pressed: the picture froze and the sound repeated, which looked like
  a crash.
- The console showed over 1000 `Event dropped, queue full` messages in 10 s
  of real touches.

In synchronous mode the callback runs in the driver's work item. Keep it to
a table update.

**Injected touches don't test this.** Touches injected by a remote-control
command bypass the driver and its queue. Test with real fingers too.

**Touch coordinates are panel coordinates.** Subtract a content area's
offset only when positioning objects inside that area.

## Threads and blocking

**No blocking I/O on the UI thread once the UI is running.**
- Card reads, thumbnail decoding, and starting or stopping the camera (I2C
  to the sensor, then a wait for the pipe) each cost frames.
- Three thumbnails decoded on the UI thread froze the display for 167 ms.
- Do this work on its own threads, and swap the results in.

**Put the thread that waits on I/O above the thread that consumes its data.**
With the card reader below the video decoder (which polls the codec), a seek
left playback on an empty window: 16 stalls in one fuzz run. With the reader
above the decoder: 0.

**Don't let a lock lend priority to a long job.**
- The audio thread (cooperative, the highest priority) waited on a lock the
  video worker held. That lent the worker a priority above the UI.
- Copy outside locks.
- Never make the UI wait for a worker to finish. Closing the player used to
  wait for a decode in progress: 10 to 25 ms.

**Thread CPU shares include interrupt time.**
`k_thread_runtime_stats_get()` charges interrupt time to the thread that was
interrupted. Stack high-water marks need `CONFIG_INIT_STACKS`.

## Audio

**Use one circular DMA transfer through the HAL, not Zephyr's I2S driver.**
- That driver starts a new DMA transfer from the interrupt at every block
  boundary.
- The SAI FIFO holds 8 samples, 83 µs at 48 kHz stereo. Any interrupt
  latency longer than that underruns the SAI.
- After an underrun the SAI stays out of step with its DMA: the sound was
  scratchy, and blocks drained four times too fast.
- Instead, run one circular linked-list transfer over a buffer in two
  halves. The half-transfer and transfer-complete interrupts wake a thread
  that refills the half just played. This is what ST's BSP does
  ([`audio.c`](../app/src/platform/audio.c)).

**Set the SAI FIFO threshold to three quarters.** At "full" the DMA
over-delivers: samples are lost and playback runs fast. At "quarter" only
20 µs are left in the FIFO, and it underruns.

**Put the audio DMA buffer and its node in AHBSRAM1 (0x3800_0000), not in
AXISRAM, FLEXRAM or PSRAM.**
- While the CPU streams writes into AXISRAM (LVGL filling large areas of a
  frame buffer), the DMA's reads on the AXI interconnect are held off for
  longer than the FIFO lasts.
- Measured: about 60 underruns per second with the buffer in AXISRAM1, and
  none in AHBSRAM1.
- Reads, PSRAM traffic and flash traffic made no difference.
- Enable AHBSRAM1's clock and its sleep clock first, and check that the RAM
  reads back.

**Tell the WM8904 it is the clock target**
(`I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET`). With the default
options it also drives BCLK and LRCLK, against the SAI.

**The WM8904 driver needs a clock controller for MCLK.** It expects a
one-cell specifier named `name` (the NXP convention). Here the SAI generates
MCLK, so a small node just answers with the rate
([`n6demo,audio-mclk`](../app/dts/bindings/clock/n6demo,audio-mclk.yaml)).

**The sample rate is 47.7 kHz, not 48.** MCLK is IC7 = PLL2 / 125 =
12.288 MHz, but PLL2 runs from the HSI: 47.6 to 47.7 kHz measured. Measure
the rate from the DMA's block count, and slave everything else to it (video,
pitch, analysis).

**The DMA block size sets the latency.**
- A source's samples reach the DAC one to two blocks after the source is
  asked for them.
- A captured sample waits until its DMA half completes.
- With 512-frame halves on both sides, the microphone reached the
  headphones 35 ms late. With 128-frame halves it is 8.7 ms, and the
  theremin's touch to sound shrank too.
- The price: each refill has to finish within one block (2.7 ms at 128),
  so no cooperative thread may run longer than that without waiting.

**Clock the microphone and the DAC from the same source, and you need no
resampler.**
- Here the MDF's kernel clock is the SAI's (IC7), so one microphone sample
  is one DAC frame.
- Read the capture ring a fixed distance behind its newest sample. Over
  thousands of blocks, that distance never moved.

**Hand the audio output from one source to the next only after the first
has faded out and let go.** Replacing the source at once cuts the old
source's fade short, and its state machine waits for a fade that never
finishes.

**Make underruns a counter, and check it after every change.** The stream
recovers by restarting, so a regression shows up as a number, not as
silence.

**Never step the signal.** Make every change of position or level a ramp or
a cross-fade, and end every note with a release. A click at the same place
on every run is in the content: look for steps in the PCM before you suspect
the playback.

## Video decode

**Decode JPEG with the HAL.** Zephyr 4.4.0's JPEG driver only encodes. An
800 x 480 Motion JPEG frame decodes in about 10.5 ms and converts by DMA2D in
5 ms, so 30 fps runs comfortably.

**Pad the codec's input to whole 32-bit words past the end marker.**
- The HAL feeds the codec one word at a time.
- If the length is not a multiple of four, the EOI marker is never sent.
- The decode then times out a few MCUs short of the end (HAL error 0x8).

See [`jpegdec.c`](../app/src/media/jpegdec.c).

**Let the audio clock drive the video.** The video player takes its time
from the DAC's measured rate (see [Audio](#audio)), so picture and sound
never drift apart.

## microSD card

**Use the internal DMA: add `idma;` to `&sdmmc2`**, and give it RIF
attributes (see
[Clocks and peripherals](#clocks-and-peripherals-through-the-hal)). Without
it, the driver moves the data with one interrupt per 32 bytes. At the card's
12 MB/s that works out to about 370 000 interrupts per second.

**Set `CONFIG_FS_FATFS_MOUNT_MKFS=n`.** Zephyr's default formats the card
when a mount fails.

**Set `CONFIG_FS_FATFS_WINDOW_ALIGNMENT=32`, and read whole sectors into one
aligned buffer.** The driver invalidates the cache over the DMA target.

**Check before calling `fs_unlink()` or `fs_mkdir()`.** Zephyr's file system
layer logs an error for every failing call, even when failing is the normal
case. That fills the log you rely on when hunting faults.

**One warning at boot is harmless:** `stm32_sdmmc: Failed to configure pins
for sleep (-2)`. The driver asks for a pinctrl sleep state, which is
compiled out without `CONFIG_PM_DEVICE`.

## Network

**Raise `CONFIG_NET_MAX_CONN` and `CONFIG_NET_MAX_CONTEXTS`** if you run more
than a couple of services. With the defaults (4 and 6), new connections are
refused.

**Host tools should reuse one connection, and servers should drop silent
clients.**
- A closed TCP connection lingers for a while.
- A host tool that connected once per command (80 times a second during an
  injected drag) used up the board's connection slots. The board was then
  unreachable for tens of seconds, which looked like a crash.
- Serve one client at a time, and time it out after 5 s of silence.

**Acknowledge bulk uploads in the application.**
- Zephyr's TCP acknowledges a segment at once only if it carries PSH;
  otherwise it waits 100 ms.
- A PC sets PSH only when its send buffer runs empty. So a plain bulk send
  moved one 21 KB window per 100 ms: 0.11 to 0.2 MB/s.
- Sent in 8 KB pieces, each answered by one byte from the board: 4 MB/s.

**Set `CONFIG_NET_ARP_GRATUITOUS_TRANSMISSION=y`.** The board then announces
its address as soon as DHCP assigns it.
- Without it, a PC that kept sending through a power cycle held a stale ARP
  entry, and Windows could not reach the board for up to a minute.
- With it, the board was reachable 0.1 s after getting its address.

**DHCP releases its lease asynchronously.** Before adding a fixed address
that equals the old lease, wait for the release. Otherwise the late removal
deletes the new address, and the board answers nothing, not even ARP.

## Camera

**Name the shield before `find_package(Zephyr)`**
(`set(SHIELD st_b_cams_imx_mb1854)`).

**Use `video_set_compose_format()` for a scaled picture.** The DCMIPP driver
wants the compose rectangle before the format. Here it scales 2592 x 1944
to 592 x 444.

**Put the capture buffers in PSRAM with `video_import_buffer()`.**
- Keep the video layer's own pool, which sits in internal RAM, at its
  minimum.
- `CONFIG_VIDEO_BUFFER_POOL_NUM_MAX` also caps the number of imported
  buffers.

**Exposure and white balance need ST's ISP middleware.** It is the west
module `zephyr-stm32-mw-isp`, enabled with `CONFIG_USE_STM32_MW_ISP=y`, and
it hooks into the driver.

**Start and stop the sensor on the capture thread.** Both talk to the sensor
over I2C and wait for the pipe. On the UI thread, each cost a frame.

## NPU

**Give the networks internal SRAM.**
- With all their working memory in PSRAM, the palm detector took 938 ms and
  the hand landmarks 350 ms. In internal SRAM they take 65 ms and 22 ms.
- The converter's estimate is about 6 times too optimistic about PSRAM for
  large feature maps. Compare the measured time per epoch block against it.
- Here the working memory is the second frame buffer, which the display
  lends while the camera screen is up. The overflow goes to a PSRAM block
  that the allocator never hands out.

**Read the weights in place from the flash.**

**Select `USE_STM32_HAL_CACHEAXI` and `DYNAMIC_INTERRUPTS`.** ST's runtime
maintains the NPU cache through the HAL and installs its interrupt handlers
at run time.

**Check the converted network against the original on the PC, with more
than the test input.**
- The converter keeps the .tflite's output order.
- Here, reading handedness as presence dropped every left hand.
- ST's quantized presence output saturates at 0.5, which made it look
  broken.
- Feed it mirrored inputs and non-hand inputs too.

**Keep ST's code out of open-source repositories.** ST's NPU runtime, its
ISP middleware and the converter's output are under SLA0044. That licence
allows use on ST devices and redistribution, but not open-source terms.
Fetch them with west or build them from the ST Edge AI install, and make the
application build without them.

## Microphone

**The capture path works.** It runs as:
- MDF1 filter 0, with the PDM clock out on PE2 and data in on PE8;
- kernel clock IC7, shared with the SAI, so one microphone sample is one DAC
  frame;
- a 3.072 MHz PDM clock, Sinc5 / 16, giving 48 kHz at 24 bits;
- a circular GPDMA transfer into AHBSRAM1.

**Use low-frequency master mode.** With a 12.288 MHz kernel clock and a
3.072 MHz microphone clock, normal SPI mode runs the processing clock at
exactly 4 times the microphone clock. RM0486 asks for more than 4. In
low-frequency master mode the MDF samples the data line with its own clock.
On a working board, both modes recorded the same.

**A microphone can fail on its own.** The onboard microphone (U13) on our
first board gave only noise: a steady −60 dBFS that no setting or sound
changed. ST's own unmodified example gave the same noise on it, though it
had worked on that board in May. On a replacement DK of the same revision,
the same firmware heard at once: quiet at −96 dBFS, claps 41 to 63 dB above
it.

**Before tuning firmware against a silent microphone, prove the chain with
a known signal:**
- **Pull-up and pull-down injection** (`ctl.py micinject`):
  - put the microphone to sleep by holding its clock low;
  - run the MDF in low-frequency master mode, where it samples with its own
    clock (normal SPI mode takes the clock from the pin and then delivers
    nothing);
  - switch the data pin's own pull-up and pull-down to inject a square wave;
  - only the pulls can put bits on the pin: once it is set as a GPIO output,
    it no longer feeds its alternate-function input.
- **A synthetic PDM stream:** put one on the pin by DMA (`micpdm`).
- **Raw bits:** clock the microphone by hand and read the raw bits at the
  pin, with no MDF in the path (`micbits`).

The board's circuit (SB19, the U14 switch, MIC_DET, CN5) and what to check
on a board are in [`microphone.md`](microphone.md).

## Testing and measurement

**Run a regression tour before every commit.** Here that is
[`tools/tour.py`](../tools/tour.py), about seven minutes. It enters every
screen through a transition and requires:
- 0 dropped frames;
- 0 audio underruns;
- 0 discontinuities in the recorded sound;
- 0 stray screen changes in 80 touch releases.

That needs a control port that injects touches and reads counters, and
screenshots read back from the frame buffer.

**Close the loop through the air.** With headphones lying by the board:
1. Play a known tone into them.
2. Measure that tone in the microphone before and after your processing,
   with Goertzel filters at the tone's frequency.
3. The difference is the processing's real gain, whatever the path through
   the air does.

This project's equalizer agreed with its drawn curve within 0.2 dB this way.
Keep the microphone itself out of the headphones while you do it.

**Soak overnight.**
- Nine hours of attract mode, plus the camera with both networks, found two
  dropped frames that the tours had never shown. Both were at the same point
  of a lap.
- Poll the heap, the LVGL pool, PSRAM and stack headroom every second, and
  look for drift ([`tools/soak.py`](../tools/soak.py)).

**Know what disturbs a measurement:**
- a screenshot (it costs one frame);
- immediate logging;
- a debugger halt;
- an injected drag that runs in a thread below the video decoder (it takes
  several times its nominal duration).

## Small things that bit

- `MIN()` and `MAX()` evaluate their arguments twice. Never pass a call with
  side effects: one of ours registered every clip twice.
- Compare times as signed values when the timestamp may be newer than `now`:
  `(int32_t)(now - stamp) > LIMIT`. A touch stamped a millisecond after the
  frame's `now` made the unsigned difference wrap to four billion, and an
  idle timeout fired on one touch release in five.
- Reset any state that a guard reads together with the guard. Zeroing a
  count but not its index let `n - i` wrap, the guard passed, and a division
  by zero faulted the board.
