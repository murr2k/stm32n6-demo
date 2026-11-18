# The onboard microphone (U13): findings and sources

**Result:** on the first board (STM32N6570-DK, MB1939 C-02) the onboard PDM
microphone U13 (ST MP23DB01HP) stopped hearing. It still runs: it takes its
clock, drives its data line and sleeps when the clock stops, but its bit
stream holds only noise, about 50 dB above the part's own floor, and nothing
it hears. The firmware is cleared:
- ST's own unmodified example gives the same noise on that board today, and
  the same binary passed taps clearly in May.
- On a replacement DK of the same revision, this firmware hears at once,
  unchanged (2026-11-17): quiet at -96 dBFS, claps 41 to 63 dB above it.

The checks below say what to look at on the old board before reworking it.

## What was measured

| Date | Test | Result |
|---|---|---|
| 2026-05-31 | ST's MDF_AudioRecorder, flashed unmodified (sibling project) | taps "clearly audible" on the headset, airborne sound weak with hiss |
| 2026-11-15 | this firmware: MDF1, 48 kHz, every clock edge, clock 0.8 to 3.2 MHz, kernel clock source, pulls, drive strength, filter settings | Gaussian noise at -60 dBFS, unchanged by any setting; a 1 kHz tone from the PC below -92 dBFS |
| 2026-11-15 | raw bits read by the CPU at the pin (`micbits`) | a healthy noise-shaped sigma-delta stream, nothing but noise in the audio band |
| 2026-11-17 | claps and taps, spoken prompts: through the MDF on both edges, and as raw bits clocked by the CPU with no MDF | no trace: within the difference between two quiet recordings |
| 2026-11-17 | capture chain self-test (`micinject`): U13 asleep, a square wave made with PE8's own pulls | right frequency and harmonics, digital zero for a held line; with U13 clocked its output overrides the wave |
| 2026-11-17 | synthetic microphone (`micpdm`): a sine through a sigma-delta modulator, put on PE8 by DMA | decoded and shown on Signal Lab; distortion from PE8's slow line only |
| 2026-11-17 | ST's MDF_AudioRecorder again, the May build, loaded into RAM | constant hiss in the headphones that nothing changes (by ear), its buffer the same in quiet, taps and claps |
| 2026-11-17 | **replacement DK** (MB1939 C-02), this firmware unchanged (normal SPI mode, Sinc5 / 16), claps and taps with spoken prompts | quiet -96 dBFS (median of 10 ms windows; the old board: -60), claps up to -8.5 dBFS peak and 41 to 63 dB above quiet per band, fingernail taps 35 to 57 dB, back to the floor afterwards |
| 2026-11-17 | the same in low-frequency master mode (`mictune 1 1 1 4 5 16 1 0 0 3`, since then the default) | the same within how much claps vary: quiet -96.7 dBFS, claps 44 to 62 dB, taps 35 to 58 dB, 0 errors |

## The board around U13 (MB1939 C-02, schematic sheet 16)

- U13 gets **VDD_MIC = VDD3V3 (the buck converter's 3.3 V) through solder
  bridge SB19 only**, with C117 (100 nF) and C118 (1 uF) at the pin. UM3300
  does not mention SB19.
- Clock: PE2 (MDF1_CCK0) through R104 (0 ohm). LR is pulled to VDD_MIC by
  R106 (10 k): data valid while the clock is high.
- Data: U13 DOUT (TP20) goes to input Y1 of **U14, an NX3L1T3157 switch**,
  whose common pin is PE8 (TP18); its other input Y0 is CN5 pin 6 (TP19).
- U14's select is **MIC_DET: pulled up to VDD_MIC by R105 (10 k), connected
  only to CN5 pin 10**. No MCU pin, expander or software touches it; with CN5
  empty, U13 is on PE8. A module that grounds pin 10 takes PE8 over (ST's
  STEVAL-MIC008A does it with 100 ohm).
- CN5: 1 and 20 GND, 2, 12 and 19 VDD_MIC, 4 clock, 6 data, 5 second data
  to PE12 (SB41, not fitted), 10 MIC_DET.
- PE2 and PE8 are used by nothing else and sit in the MCU's main VDD domain
  (3.3 V), which has no enable or supply-valid bit.
- The slow fall of PE8 against the internal pull-down (about 5 us) is U14's
  on-state capacitance, about 130 pF: U14 is powered and conducting.

## What to check on the old board, in order

1. Nothing attached to CN5, TP18 to TP20 or the sound port. How the external
   PUI DMM-2726-B microphone used earlier was wired is not recorded: straight
   to PE8 or TP18 it would have driven the line against U13's output.
2. VDD_MIC at CN5 pin 2 (DMM): 3.3 V with the demo capturing and with the
   board idle. About 2.5 to 2.9 V while clocked, collapsing when idle, means
   SB19 is open and U13 was powered through its clock input, which would
   damage a replacement too.
3. A known good PDM microphone on CN5 (VDD pin 2, GND pin 1, CLK pin 4, DATA
   pin 6, pin 10 to GND): if it hears and U13 does not, U13 is convicted.

## On the replacement board

- Checked on 2026-11-17 (see the table above): it hears, with the firmware
  as it was.
- Low-frequency master mode is now the default, and it records the same.
  - Why: in normal SPI mode the processing clock was exactly 4 times the
    microphone clock, and RM0486 asks for more than 4.
  - It keeps 3.072 MHz and 48 kHz.
- `ctl.py "sigsrc 1" mic` and Signal Lab show the microphone; `acap.py --mic`
  records it.
- The bring-up commands stay: `mictune`, `mcap`, `micbits`, `micscope`,
  `micprobe`, `micinject`, `micpdm` (`app/src/platform/mic.h`).

## Sources

Board and parts:
- [UM3300, STM32N6570-DK user manual](https://www.st.com/resource/en/user_manual/um3300-discovery-kit-with-stm32n657x0-mcu-stmicroelectronics.pdf): section 7.15 (microphone, CN5), Table 14 (CN5 pinout), Table 24 (I/O assignment), Tables 25 and 26 (board revisions)
- [MB1939 C-02 schematic](https://www.st.com/resource/en/schematic_pack/mb1939-n6570-c02-schematic.pdf): sheet 16 (microphone), sheets 5, 13 and 14 (supplies)
- [MP23DB01HP product page and datasheet DS13336](https://www.st.com/en/mems-and-sensors/mp23db01hp.html)
- [NX3L1G3157 datasheet](https://www.farnell.com/datasheets/1645545.pdf), sister part of U14 (on-state capacitance)
- [STM32N657 datasheet DS14791](https://www.st.com/resource/en/datasheet/stm32n657x0.pdf): Table 18 (pin supply domains), Tables 65 to 67 (I/O levels, pull resistors)
- [RM0486, STM32N6 reference manual](https://www.st.com/resource/en/reference_manual/rm0486-stm32n6x5x7xx-armbased-32bit-mcus-stmicroelectronics.pdf): section 13.5.7 (independent I/O supplies), MDF chapter, Table 280 (clock ratio)
- [AN5795, sound capture with the MDF](https://www.st.com/resource/en/application_note/an5795-how-to-use-multifunction-digital-filter-mdf-to-capture-sound-on-stm32-mcus-stmicroelectronics.pdf)
- [AN4428, MEMS microphone tutorial](https://www.st.com/resource/en/application_note/dm00103202.pdf) (damaged or contaminated membranes)

ST software:
- [BSP audio driver v1.4.1](https://github.com/STMicroelectronics/stm32n6570-dk-bsp/blob/v1.4.1/stm32n6570_discovery_audio.c)
- [MDF_AudioRecorder example v1.4.1](https://github.com/STMicroelectronics/STM32CubeN6/tree/v1.4.1/Projects/STM32N6570-DK/Examples/MDF/MDF_AudioRecorder)
- [STM32N6-GettingStarted-Audio](https://github.com/STMicroelectronics/STM32N6-GettingStarted-Audio)

Reports from other DK owners:
- [ST Community 167402](https://community.st.com/stm32-mcus-embedded-software-32/stm32n6-nucleo-board-mdf-issue-mics-records-noise-167402), [159675](https://community.st.com/stm32-mcus-embedded-software-32/stm32n6570-dk-mic-and-screen-setup-159675), [168802](https://community.st.com/stm32-mcus-embedded-software-32/stm32n6570-dk-mp23db01hp-on-mdf1-recommended-cic-gain-hpf-pdm-clock-settings-for-16-khz-audio-and-start-up-thump-168802), [168817](https://community.st.com/edge-ai-134/streaming-speech-recognition-on-the-stm32n6-npu-could-you-evaluate-my-result-168817) (working), [158093](https://community.st.com/stm32-mcus-embedded-software-32/interleave-audio-by-connecting-steval-mic008a-dual-mems-to-stm32n6570-dk-cn5-158093) (STEVAL-MIC008A on CN5)
- [LarocheC/stm32n6-stt](https://github.com/LarocheC/stm32n6-stt): a working DK microphone, desk speech at -23.5 dBFS RMS with ST's stock BSP settings

The first round of research, with the MDF settings of ST's and other
projects, is in [reports/STM32N6 DK microphone noise only.md](../reports/STM32N6%20DK%20microphone%20noise%20only.md).
