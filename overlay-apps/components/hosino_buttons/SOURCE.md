# Board button input

Original implementation for the ESP32-S3-Touch-AMOLED-2.16, SPDX Apache-2.0.
This component only reads three inputs and returns short-press release events.
It does not navigate, send messages, change power settings, or install an ISR/task.

## Wiring evidence

Reviewed the official schematic, KEYS block, on 2026-11-12:
https://www.waveshare.net/w/upload/1/14/ESP32-S3-Touch-AMOLED-2.16-Schematic.pdf

The matching local source is
`upstream/waveshare-esp32-s3-touch-amoled-2.16/schematic/ESP32-S3-Touch-AMOLED-2.16-Schematic.pdf`.

* Key3: GPIO18, R18 10k pull-up to VCC3V3, switch to GND; active low.
* Key2: GPIO0, R9 10k pull-up to VCC3V3, switch to GND; active low.
  The neighboring CHIP_PU/R10/C13 reset network is separate from Key2.
* Key1: switches the PWRON/R11 node to GND. R11 feeds the gate of T1
  (BSS138), whose source is GND and drain is SYS_OUT/GPIO16. With the button
  pressed, T1 turns off and R8 10k pulls GPIO16 high. With the button released,
  PWRON biases T1 on, pulling SYS_OUT low. Therefore GPIO16 is active high.

All three pins are configured as inputs with internal pulls disabled; the board
provides the external bias components. GPIO16 is never driven as an output.
Input polarity is supported by the schematic but still needs device verification.

BOOT remains a strapping input. Holding GPIO0 low across reset selects the ROM
download path; this component does not change that behavior:
https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html

PWRON remains connected directly to the PMU. Its hardware long-press power action
is independent of this input sampler. No AXP2101 register is read or modified.
The documented OFFLEVEL choices are 4/6/8/10 seconds; the actual board setting
has not been read for this change:
https://www.waveshare.net/w/upload/e/ed/X-power-AXP2101_SWcharge_V1.0.pdf

## Integration and lifecycle

Call `hosino_buttons_init()` once, then run `hosino_buttons_poll(now_ms)` from a
20 ms GUI timer. Use a monotonic millisecond clock, truncated to uint32_t; timer
wrap is handled with unsigned differences. Call all APIs from that same thread.
No heap allocation, new task, interrupt callback, or LVGL dependency is required.

Events begin disabled. The foreground app calls `hosino_buttons_set_active(true)`
and consumes the FIFO with `hosino_buttons_take_event()`. On background/close,
call `hosino_buttons_set_active(false)` before deleting UI. Either activation
transition clears queued and incomplete presses. Every key must then remain
released for at least 40 ms to arm, preventing startup/activation-held clicks.

Levels debounce for 40 ms. Events are generated after a stable release only when
the sampled press duration is below 800 ms. A long press never generates a
short-press event on release. The sampler cannot measure edges between polls;
threshold precision is limited by its 20 ms sampling period. A polling gap over
120 ms discards pending presses/events and requires release again. An eight-event
FIFO drops new events on overflow. The caller determines each event's UI action.

The currently reviewed BSP 2.0.1 declares `BSP_CAPS_BUTTONS=0`; the application
does not currently register GPIO0/16/18 button handlers. The original standalone
04_Immersive_block example does use GPIO0 for calibration, but that ISR was not
included in the current `hosino_gravity_app`. Do not enable both owners later.

## Validation boundary

Host tests execute this production C state machine with GPIO stubs, covering
polarity, debounce, startup-held suppression, long-press filtering, foreground
changes, queue limits, timer wrap, and missed-sampling recovery. Xtensa syntax
uses the actual ESP-IDF 5.5.5 GPIO headers. These checks do not establish physical
button behavior, PMU timing, or a successful device integration.

2026-11-12 result: the host test passed using the workspace's TCC 0.9.27, and
ESP-IDF 5.5.5 / Xtensa GCC 14.2.0 `-fsyntax-only` returned zero with no diagnostics.
Only this component was syntax-checked; no full firmware build or flash was run.

Host reproduction (run from the repository root; place the executable in an
existing scratch directory):

```powershell
& tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe `
  -Ioverlay-apps/components/hosino_buttons/include `
  -Ioverlay-apps/components/hosino_buttons/tests/stubs `
  -o C:/esp/hosino-qspi/host-console-client/test_buttons.exe `
  overlay-apps/components/hosino_buttons/tests/test_buttons.c `
  overlay-apps/components/hosino_buttons/hosino_buttons.c
& C:/esp/hosino-qspi/host-console-client/test_buttons.exe
```
