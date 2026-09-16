# CrowPanelPico_Arduino

Arduino port of the CircuitPython project in the repository root, targeting
the same hardware: an **Elecrow CrowPanel Pico Display 4.3" (320x240)**
touchscreen (RP2040, GT911 capacitive touch, true DVI/TMDS panel output).

It reproduces the behaviour of [`code.py`](../../code.py),
[`buttons.py`](../../buttons.py), [`buzzer.py`](../../buzzer.py) and
[`gt911.py`](../../gt911.py): a 3x2 grid of 80x80 touch buttons, two of which
("panda", "pig") latch on/off with an indicator state, the rest of which are
momentary, all with debounced touch detection and buzzer feedback.

## Why this isn't a 1:1 API translation

`code.py` drives the display with CircuitPython's built-in `picodvi` module,
which generates real DVI/TMDS signalling on GP8-GP15 (not a SPI/parallel TFT
controller). There's no off-the-shelf Arduino equivalent baked into a board
core, so this port uses the actual C library that CircuitPython's `picodvi`
module wraps: **[Adafruit's `PicoDVI` library](https://github.com/adafruit/PicoDVI)**
(`library.properties` name: `PicoDVI - Adafruit Fork`).

The board's TMDS pinout (clock on GP8/9, RGB pairs on GP10-15, inverted
diffpairs) is defined locally in [`CrowPanelDVIConfig.h`](CrowPanelDVIConfig.h)
as `crowpanel_dvi_cfg`, rather than reused from PicoDVI's bundled
`picodvi_dvi_cfg` preset even though the values are identical - that preset
is documented as being for a *different* board (the not-HDMI socket on Rev C
PicoDVI boards), and the match is coincidental. Depending on it would
silently couple this project to an unrelated upstream board preset that
PicoDVI is free to rename or change in a future release.

That library's 8-bit paletted framebuffer (`DVIGFX8`) only supports a single
256-colour palette shared by the whole screen, but the button art in
[`buttons.png`](../../buttons.png) is full 32-bit RGBA, so it can't be
reduced to one shared palette without lossy remapping. This port therefore
uses the 16-bit RGB565 framebuffer (`DVIGFX16`) instead, and the bitmaps
are pre-converted to flat RGB565 arrays at build-prep time (see
below) rather than loaded as indexed `.bmp` files at runtime.

This port also departs from a literal translation in its control flow, to
take advantage of things the CircuitPython original couldn't easily do:

- **Non-blocking main loop.** `buzzer.py`'s `play_tone()` and `code.py`'s
  main loop both block with `time.sleep()`. Here, `Buzzer::playTone()`
  starts the tone and returns immediately; `Buzzer::update()` (called once
  per `loop()`) stops it once its duration elapses. `loop()` itself has no
  `delay()` at all - see below.
- **Interrupt-driven touch.** `GT911::attachTouchInterrupt()` wires the
  controller's INT pin to a GPIO interrupt (`attachInterrupt()`), so
  `loop()` can react to new touch data immediately instead of waiting for
  the next scheduled poll. The main loop processes only fresh GT911 frames,
  including explicit zero-touch release frames, so extra interrupt edges
  cannot create false releases. A 50ms fallback poll covers missed edges.
- **I2C health and recovery.** Failed and short transactions are detected
  explicitly and rate-limited in the serial log. After three consecutive
  failures, the sketch resets and reinitializes the controller, with at most
  three recovery attempts per outage.
- **Callback-based buttons.** Instead of checking `Button::isPressed()`'s
  return value in an `if`/`else` chain, `Button::onPress()` registers a
  `std::function<void(Button&)>` handler fired automatically when a press
  is confirmed - see the lambda registered per-button in `setup()`.

## Required libraries and board package

Install via the Arduino IDE:

1. **Board package**: "Raspberry Pi Pico/RP2040" by Earle Philhower
   (arduino-pico core) - Boards Manager URL:
   `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
   Select board **Raspberry Pi Pico** (or the specific RP2040 board matching
   the CrowPanel's module).
2. **Library**: `PicoDVI` (Library Manager entry "PicoDVI - Adafruit Fork",
   or install from https://github.com/adafruit/PicoDVI).
3. **Library**: `Adafruit GFX Library` (PicoDVI's dependency; installed
   automatically if you install PicoDVI via the Library Manager).


No touch or buzzer library is required - `GT911.h/.cpp` and `Buzzer.h/.cpp`
in this sketch talk to the hardware directly via `Wire` and `tone()`/`noTone()`.

## Files

| File | Ports | Notes |
| --- | --- | --- |
| `CrowPanelPico_Arduino.ino` | `code.py` | Pin setup, display/touch init, button grid, main loop |
| `Button.h` / `Button.cpp` | `buttons.py` | Same debounced state machine; draws 80x80 RGB565 tiles directly into the DVI framebuffer |
| `Buzzer.h` / `Buzzer.cpp` | `buzzer.py` | `tone()`/`noTone()` instead of `pwmio` |
| `GT911.h` / `GT911.cpp` | `gt911.py` | Same register protocol, over `Wire` instead of `busio.I2C` |
| `CrowPanelDVIConfig.h` | n/a | This board's own TMDS pin config (`crowpanel_dvi_cfg`), independent of PicoDVI's presets |
| `TouchPoint.h` | n/a | Shared touch-point struct (replaces Python `(x, y, area)` tuples) |
| `Images.h` | `buttons.png` | Generated RGB565 tile data - see below |
| `tools/convert_images.py` | n/a | Regenerates `Images.h` from `buttons.png` |

## Regenerating `Images.h`

`Images.h` is generated directly from the shared
[`buttons.png`](../../buttons.png) sprite sheet in the repository root -
the same 320x480 sheet (4 columns x 6 rows of 80x80 tiles) that the
CircuitPython side slices into per-animal `image/*.bmp` files for
`displayio.OnDiskBitmap`. If `buttons.png` changes, regenerate `Images.h`
with:

```powershell
python arduino\CrowPanelPico_Arduino\tools\convert_images.py
```

This decodes `buttons.png` (an 8-bit RGBA PNG) directly with no third-party
imaging library, slices out each animal's row, converts every pixel to
RGB565, and writes `Images.h` with one `const uint16_t <name>_data[]` array
per animal (each a horizontal strip of 80x80 tiles, same layout as the
CircuitPython `TileGrid`s: tile 0 = normal, 1 = pressed, 2 = indicator,
3 = indicator+pressed). Row order in the sheet is pig, panda, deer, tiger,
elephant, fox; non-latching animals only use tiles 0-1, so their sheet
columns 2-3 (indicator states) are ignored.

## Pin assignments (matches `code.py`)

| Signal | GPIO |
| --- | --- |
| DVI clock +/- | GP9 / GP8 |
| DVI red +/- | GP11 / GP10 |
| DVI green +/- | GP13 / GP12 |
| DVI blue +/- | GP15 / GP14 |
| Buzzer (PWM) | GP19 |
| Backlight enable (active-low digital output) | GP24 |
| Touch I2C SDA | GP20 |
| Touch I2C SCL | GP21 |
| Touch RESET | GP29 |
| Touch INT | GP25 |

## Backlight

GP24 is an active-low enable on this panel. The sketch configures it as a
digital output and holds it low before and after DVI initialization. PWM is
intentionally not used because its polarity and interaction with PicoDVI's
system-clock change previously left the display dark.


## Building

Open `CrowPanelPico_Arduino.ino` in the Arduino IDE (or `arduino-cli compile
--fqbn rp2040:rp2040:rpipico arduino/CrowPanelPico_Arduino`), select the Pico
board and a serial port, then upload. Touch and button-press events are
logged to `Serial` at 115200 baud, matching the `print()` calls in `code.py`.
Define `CROWPANEL_TOUCH_DEBUG=0` at compile time to omit the high-volume raw
touch-coordinate log while retaining startup, button, and error diagnostics.
