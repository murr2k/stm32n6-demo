# stm32n6-demo

A multi-screen touch demo for the **STM32N6570-DK**: live dashboards, dense
real-time data visualization, and full-screen video with sound, built on
Zephyr 4.4.0 and LVGL 9.5. The audience is engineers and UX designers, so the
rules are: lock to the panel refresh, no fake data, and show the measurements.

> **Status: Phase 0 complete (bring-up), Phase 1 in progress.** The board boots
> MCUboot and the app with no hands on the switches, drives the panel from an
> app-owned double buffer, links at 1 Gbit/s, and serves screenshots of its own
> frame buffer over TCP. No demo screens exist yet. See [Status](#status).

The plan, with architecture, budgets, phases and risks, is in
[`docs/plan.html`](docs/plan.html).

## Hardware

| Part | Detail |
|---|---|
| Board | STM32N6570-DK (MB1939), STM32N657X0, Cortex-M55 at 800 MHz with Helium |
| Display | 5 inch 800 x 480 RGB888 panel on LTDC, GT911 5-point capacitive touch |
| Memory | 4.2 MB internal SRAM, 32 MB HexaSPI PSRAM, 128 MB octal NOR (XSPI2) |
| Network | Gigabit Ethernet, RTL8211F PHY (RGMII) |
| Audio | WM8904 codec to the headphone jack, MEMS microphone on MDF |
| Probe | On-board ST-LINK V3EC, virtual COM port (COM17 on this host) |

## Prerequisites

- Windows 10/11, Python 3.10+, Git, CMake, Ninja
- Zephyr SDK 1.0.1 (`arm-zephyr-eabi`)
- STM32CubeProgrammer 2.18 or later (provides `STM32_Programmer_CLI`, the
  signing tool and the `MX66UW1G45G_STM32N6570-DK` external loader)
- ffmpeg (video asset pipeline, later phases)

## Setup

```powershell
git clone https://github.com/murr2k/stm32n6-demo.git
cd stm32n6-demo
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install west pyserial pillow numpy
.\.venv\Scripts\west.exe init -l app
.\.venv\Scripts\west.exe update
.\.venv\Scripts\python.exe -m pip install -r zephyr\scripts\requirements-base.txt `
    -r bootloader\mcuboot\scripts\requirements.txt
```

The project root is the west workspace (manifest in `app/west.yml`, pinned to
Zephyr `v4.4.0`). `zephyr/`, `modules/` and `bootloader/` are fetched, not
committed.

## Build, flash, run

Leave the boot switches at **BOOT1 = H, BOOT0 = L** (development boot). The
whole loop then runs unattended:

```powershell
.\n6.cmd go                 # build + flash + power-cycle + start + console
.\n6.cmd go -p              # same, pristine build (needed after adding a .overlay or .conf file)
.\n6.cmd go --app-only      # skip reflashing MCUboot
.\n6.cmd run --secs 20      # restart what is in flash and watch the console
.\n6.cmd console            # just listen on the serial console
.\shot.cmd --host <ip>      # save the board's frame buffer to captures\shot.png
```

`n6.cmd` wraps `tools/n6.py`:

1. `build`: `west build --sysbuild -b stm32n6570_dk`, producing MCUboot (the
   first-stage boot loader, ST-signed) and the application (MCUboot-signed,
   executes in place from XSPI flash).
2. `flash`: power-cycles the target through the ST-LINK, then writes MCUboot
   to `0x7000_0000` and the app to slot 0 with ST's external loader over SWD.
3. `run`: power-cycles again, loads MCUboot into RAM over SWD and starts it.
   MCUboot validates slot 0 and jumps to the app.

For stand-alone use (no PC), set **BOOT1 = L** and power-cycle: the boot ROM
then loads MCUboot from flash by itself.

### Why the power cycles

ST's external loader leaves the NOR flash in octal DTR mode. A pin reset does
not undo that, and Zephyr's XSPI flash driver then stalls on its first
SPI-mode command (`XSPI reset enable (SPI/STR) failed`, MCUboot aborts). Only
removing power returns the flash to SPI mode. The ST-LINK V3EC can switch the
target supply, so `STM32_Programmer_CLI --power off/on` does it without a
finger on the board. The loader has the mirror-image problem: it only works
against the boot ROM's idle state, not against running firmware.

## Layout

```
app/                    Zephyr application (also the west manifest repo)
  boards/               devicetree overlay for the DK
  src/                  firmware sources
tools/
  n6.py                 build / flash / run / console
  shot.py               frame buffer screenshot client
  gdbrun.ps1            scripted ST-LINK gdbserver + GDB session for bring-up
docs/plan.html          the plan
captures/               screenshots and logs pulled off the board (not committed)
```

## Status

Phase 0 results, measured on the board:

| Check | Result |
|---|---|
| Unattended build, flash, run | Working, board left in development boot |
| Core clock | 800 MHz |
| Internal SRAM (AXISRAM3-6), 1.5 MB buffer | fill 945 MB/s, copy 361 MB/s, read 691 MB/s |
| PSRAM, 1.5 MB buffer | fill 466 MB/s, copy 85 MB/s, read 212 MB/s |
| Panel | 800 x 480 ARGB8888, app-owned double buffer, swap in the line interrupt |
| Refresh | 48.4 Hz with the stock 20 MHz pixel clock (to be raised to 60 Hz) |
| Ethernet | 1000 Mb full duplex, DHCP lease in about 1 s |
| Screenshot readback | 1.5 MB frame in 0.3 s over TCP |
| Touch | Driver enabled for 5 points; not yet exercised by a person |
| Hardware JPEG decode | Not started |

Not yet verified: stand-alone boot with BOOT1 = L (needs a hand on the switch).

## License

Apache-2.0 for the sources in this repository, matching Zephyr.
