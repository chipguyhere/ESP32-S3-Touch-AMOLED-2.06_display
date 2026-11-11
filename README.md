# chipguy_ESP32-S3-Touch-AMOLED-2.06_display

A self-contained **display + touch** driver for the **Waveshare ESP32-S3-Touch-AMOLED-2.06**.
It is a small ESP32-S3 board with a 2.06" AMOLED touch screen, 410×502 pixels, sold
as a smartwatch-style development kit. This library drives the screen and the touch
panel, so you don't have to think about either. The hardware is fixed and fully
handled, which leaves you to write an **LVGL 9** app and nothing else.

The bundled examples are the heart of the library. Each is a complete, working
**starting point**: the display, touch, and LVGL are already wired up and running
before your code gets control. Pick the one that matches how you want to build your
UI, copy it, and start replacing its placeholder UI with your own.

This library is one of four for the board:

| Library | What it does |
|---|---|
| **chipguy_ESP32-S3-Touch-AMOLED-2.06_display** (this one) | The screen, touch, and the LVGL glue |
| [chipguy_ESP32-S3-Touch-AMOLED-2.06_board](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board) | The shared I²C bus, the pin map, the buttons, and what else is on the board — speaker, microphones, vibration motor, clock, motion sensor, SD slot |
| [chipguy_ESP32-S3-Touch-AMOLED-2.06_power](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_power) | The battery, charging, the power button, power-off and sleep |
| [chipguy_ESP32-S3-Touch-AMOLED-2.06_rtc](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_rtc) | The real-time clock: keeps the date and time, even while the board is off |

You need the **board** library installed too. The display library uses it for touch
and for switching on the screen's power.

> **Status:** tested on one board (2026-10-05): the screen, colours, touch, all-round
> visibility, tear-free updates at 80 MHz and the panel's 59 Hz refresh were checked
> on real hardware. Things still to confirm are marked **TODO (bench)** in the
> engineering notes.

---

## Installation

This library is distributed by downloading the code as a ZIP from GitHub. There is
no Arduino Library Manager entry. To install:

1. On the GitHub project page, click **Code ▸ Download ZIP**.
2. Unzip it, and move the resulting folder into your Arduino **libraries**
   directory:
   - macOS / Linux: `~/Documents/Arduino/libraries/`
   - Windows: `Documents\Arduino\libraries\`
3. Do the same for **chipguy_ESP32-S3-Touch-AMOLED-2.06_board** (and the power
   library, if you want the battery). The two watch examples, GlanceWatch and
   GlanceWithDemos, also need the power and rtc libraries and
   chipguy_RealTimeClockSupport.
4. Restart Arduino IDE so the examples appear in the File menu.

(Alternatively, in the IDE: **Sketch ▸ Include Library ▸ Add .ZIP Library…**
and select the downloaded ZIP.)

### Dependency: LVGL 9

The examples need **LVGL 9**. Install it with the Arduino **Library Manager**
(search "lvgl"). Each example sketch folder ships its own `lv_conf.h`. LVGL reads
its settings from that file, so keep it next to the `.ino`.

This library was developed against LVGL 9.3 (the current version of LVGL that is
compatible with SquareLine) but is very likely compatible with later versions.

### Arduino IDE board settings

There is no ready-made board entry for this device, so use the generic
**"ESP32S3 Dev Module"** (Tools ▸ Board ▸ esp32) and set these options:

| Tools menu item | Set it to | Why |
|---|---|---|
| **PSRAM** | **OPI PSRAM** | The chip has 8 MB of *octal* PSRAM (extra memory). "QSPI PSRAM" can't talk to it, and the library refuses to build with that setting. |
| **Flash Size** | **32MB (256Mb)** | The board's flash chip is 32 MB (checked on a real board). 16MB also works — Waveshare's own builds use it — you just don't use the top half. |
| **Partition Scheme** | a **32M** scheme, e.g. "32M Flash (4.8MB APP/22MB FATFS)" | Room for a big sketch. (With 16MB, any 16M scheme.) |
| **USB CDC On Boot** | **Enabled** | The USB-C port *is* the ESP32-S3's own USB — there is no separate serial chip. Without this, `Serial.print` goes nowhere. |

The IDE sometimes forgets board options without saying so. If the build stops with
a `static_assert` about PSRAM, set it back to OPI and rebuild.

One quirk of a board whose console is the chip's own USB: `Serial.print` **waits**
if nothing on your computer is reading the port. A sketch that prints when you touch
the screen then **stutters**: the picture freezes for a moment, then carries on.
(Seen on the bench: the pauses vanished as soon as the computer read the port.)
Every example therefore puts

```cpp
Serial.setTxTimeoutMs(0);
```

right after `Serial.begin()`, which makes it drop output instead of waiting. Do the
same in your own sketches.

**Uploading:** plug in a USB-C cable and pick the port that appears. If the board
doesn't show up as a port (it happens on a fresh board or after a bad flash), hold
**BOOT** while you turn the board on with **PWR**. It then waits for an upload.

---

## The examples — pick your starting point

Open them from **File ▸ Examples ▸ chipguy_ESP32-S3-Touch-AMOLED-2.06_display**.
All come up running immediately, so you can confirm your board works before you
change a line. The first two differ only in **how you write the UI**.

To turn any of them into your own project, **save it under a new name**
(File ▸ Save As) so your work lives outside the library, then start editing.
Each example folder is self-contained — it carries its own `lv_setup.hpp` and
`lv_conf.h` — so a copied sketch stands on its own.

### LvglClaudeCodeStub — write the UI yourself (by hand or with an AI)

Use this when you want to build the interface **in code**: directly, or by handing
the sketch to an AI coding assistant such as Claude Code.

The UI lives in a tiny `ui.h` / `ui.cpp` pair in the sketch folder. All the
hardware and LVGL setup happens before `ui_init()` is called, so `ui_init()` is a
blank canvas: it receives a live LVGL screen and you create widgets on it. To start
your app:

1. Open `ui.cpp` and look at `ui_init()`. The demo it ships with (a label that
   follows your finger, plus a color-cycling rectangle) is just there to prove the
   display and touch work — **delete it.**
2. In its place, build your own UI on `lv_screen_active()` using normal LVGL calls
   (`lv_label_create`, `lv_button_create`, event callbacks, …).
3. As your app grows, add more screens and helper functions, and declare the ones
   the sketch needs in `ui.h`.

Because everything except the UI is already wired up, this scaffold is ideal to
hand to **Claude Code**: point it at the sketch folder and describe the app you
want. It can focus entirely on `ui.cpp` without touching display, touch or LVGL
plumbing.

### Lvgl93SquarelineLauncher — design the UI visually in SquareLine Studio

Use this when you'd rather **design the interface visually** in
[SquareLine Studio](https://squareline.io/) (a drag-and-drop LVGL UI editor) and
just run the result on the board.

The sketch's `src/` folder holds a small **placeholder** UI in the exact file shape
a SquareLine export produces (`ui.c`, `ui_Screen1.c`, …). To start your app:

1. In SquareLine Studio, create a project sized to your orientation (**410×502**
   for portrait, 502×410 for landscape), **16-bit** color, with **no rotation or
   offset** — this library handles rotation.
2. Lay out your screens visually.
3. **Export ▸ UI files** (the "Arduino TFT_eSPI profile" works, among others).
4. **Replace the whole `src/` folder** with your export. Build and upload.

Treat `src/` as disposable: you overwrite it each time you re-export, so don't keep
anything there. Your own logic lives in the **sketch**: after `ui_init()`, attach
event handlers from the `.ino` to the objects SquareLine exposes (e.g.
`ui_Button1`).

### LvglBouncingBalls and LvglCalculator — resolution-independent demos

Two more complete apps, laid out entirely from the live screen size, so the same
`ui.cpp` runs unchanged on any board in this family and in any orientation.
**BouncingBalls** is a physics toy with two sliders; **Calculator** is a working
four-function calculator in portrait. The calculator's near-black design suits an
AMOLED: black pixels are simply off.

### GlanceWatch — a working wristwatch

An ordinary wristwatch, built from all four libraries:

- **Raise to look.** Tilt the watch up toward your face and the time, date and
  battery appear for 8 seconds (a tap on the screen keeps them on longer). The
  position is fixed: the face about 45° up from flat, which suits looking at it
  standing up or lying down, so there's nothing to set up. It is armed with "leave
  the position first", so a watch lying face-up on a table doesn't keep waking up.
- **Sets itself once a day.** A silent wake every 24 hours, with the screen off,
  connects to Wi-Fi and asks three internet time servers (NTP) at once. The time is
  used only when at least two of them agree to within a second; if they don't, or
  Wi-Fi fails, it tries again an hour later. Each result goes to
  `rtcSupport.setUTC()`, which learns how fast or slow the clock chip runs. If the
  time hasn't been set this way for 72 hours, the face says so and suggests checking
  the Wi-Fi.
- **Local time** for US Mountain time with daylight saving by default, or the zone
  chosen on the watch.
- **Battery** as the power chip's percentage estimate, and "charging" or "USB" when
  plugged in.
- **Buttons.** BOOT or PWR also show the time. Hold BOOT for 2 seconds for a menu on
  the touch screen. "Hold for menu" appears beside BOOT while it is down. Either
  button closes the menu, and so does 20 seconds without a touch.
- **Set the time** (on the menu) opens three more:
  - **Wi-Fi network** explains that the clock is set over Wi-Fi and lets you type
    the network's name and password on the watch. They're saved, and used from then
    on instead of the ones in `watch_config.h`; "Set the time now" then syncs
    straight away. The sync runs in the background, so the face and buttons keep
    working while it does; the watch stays awake until it finishes.
  - **Time zone** picks a US zone, or a custom fixed offset from UTC in hours and
    half hours, with no daylight saving. The custom screen spells out how far ahead
    of or behind UTC it is, and shows the date and time it gives.
  - **Advanced** turns on the battery voltage and the measured clock drift on the
    face; both are off at first. The drift reads "learning" until it has been
    measured.

  A fresh start (power on, reset, a new upload) always sets the time too. While PWR
  is held, a small box beside it counts down "Hold 6s to power off" (the hold time
  is read from the power chip).
- **Your own app.** This is the plain watch on purpose: it's the starting point for
  "a watch plus my own app". Add a menu button and a screen of your own;
  GlanceWithDemos shows how.
- **Settings** — your time zone and 12/24-hour, and optionally your Wi-Fi name and
  password (or type those on the watch) — go in `watch_config.h` next to the sketch.
  The copy in the library holds placeholders; keep your real password out of
  anything you share or publish.
- **Needs more libraries** than the other examples: the board,
  [power](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_power) and
  [rtc](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_rtc) libraries,
  [chipguy_RealTimeClockSupport](https://github.com/chipguyhere/RealTimeClockSupport),
  and LVGL 9.
- Tried on the bench (2026-10-06): NTP set the clock, the local time was right, and
  the battery read 62 % / 4.07 V while charging.

### GlanceWithDemos — the watch, with apps added

The same watch, with three demos added to the BOOT menu under "Set the time". It
shows how an app slots into the watch: each one is a screen of its own, PWR goes
back to the time, and each closes by itself when nobody is using it, so nothing left
open drains the battery. It needs the same libraries as GlanceWatch.

- **Calculator** and **Bouncing balls** — LvglCalculator and LvglBouncingBalls
  above, copied in. They close after a minute without a touch.
- **Simple Shooter** — a little space game with sound. It plays itself until you
  touch the lower part of the screen; then the ship follows your finger and fires.
  BOOT changes the sound (on, shots quiet, off). It closes after two minutes of
  playing itself. It was translated to C++ from a MicroPython game (the original is
  in the example's `extras/` folder), and its pictures and sounds are built into the
  program, so no SD card is needed. This is the first thing to use the watch's
  speaker.

---

## What the examples give you: the `lv_setup` API

Every example wires the hardware to LVGL through one small header,
**`lv_setup.hpp`** (plus **`lv_conf.h`** for LVGL's settings). Once `begin()`
returns, you are just using plain LVGL.

```cpp
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

void setup() {
    Serial.begin(115200);

    lv_setup.begin();           // display + touch + LVGL; portrait 410x502

    // ...build your UI on lv_screen_active() here (or call ui_init())...
}

void loop() {
    lv_timer_handler();         // let LVGL run
    delay(5);
}
```

What the header provides:

- **`lv_setup.begin(uint16_t rotation = 0)`** — call once in `setup()`. Turns on
  the screen, starts touch and LVGL, and creates the LVGL display and touch input.
- **`lv_setup.hasTouch()`** — `true` if the touch controller answered at startup.
- **`lv_setup.tearFree()`** — `true` when running tear-free (see [Tearing](#tearing)).
- **`lv_setup.sleep()` / `lv_setup.wake()`** — screen off into its deepest sleep, and
  back on with LVGL redrawing the screen (see [Brightness and sleep](#brightness-and-sleep)).
- **`display`** — the screen. `display.width()`, `display.height()` (for the
  current rotation), `display.setBacklight(0..100)` for brightness, and
  `display.sleep()` / `display.wake()` (wake leaves the screen black — prefer
  `lv_setup.wake()` with LVGL).
- **`board`** — the board object from the board library: `board.bootButtonPressed()`,
  `board.pwrButtonPressed()`, `board.vibrate(true/false)`, and `board.Wire` for the
  internal I²C bus.

You don't draw or send pixels yourself — LVGL does, through the callbacks
`lv_setup` installed. Your job is to create LVGL widgets and call
`lv_timer_handler()` in `loop()`.

### Portable across chipguy board libraries

Every hardware detail lives in two files — **`lv_setup.hpp`** and **`lv_conf.h`** —
so your application code (`ui.cpp`, or the SquareLine `src/` files) only ever talks
to LVGL. To move an app to a **different display board**, drop in that board's
`lv_setup.hpp` and `lv_conf.h` and the rest of the sketch is unchanged. Layouts
based on alignment travel best; fixed pixel positions may need adjusting.

---

## Rotation

Pass `0`–`3` (or the matching degrees) to `lv_setup.begin()`. Display and touch
rotate together.

| rotation | degrees | orientation | resolution |
|---|---|---|---|
| 0 | 0 | portrait — the panel's own orientation (default) | 410×502 |
| 1 | 90 | landscape | 502×410 |
| 2 | 180 | portrait, upside down | 410×502 |
| 3 | 270 | landscape, the other way | 502×410 |

`90`/`180`/`270` are accepted as aliases for `1`/`2`/`3`. Named constants exist too:
`AMOLED206_ROTATION_PORTRAIT`, `AMOLED206_ROTATION_LANDSCAPE`, and the `_FLIPPED`
pair.

The screen's controller chip cannot rotate on its own, so the library turns the
picture as it sends it. All four rotations cost the same. Why, and how, is in
[README-engineering.md](README-engineering.md#rotation-is-done-in-software).

## The screen has rounded corners

The glass is not a perfect rectangle. **Each corner is rounded off on a curve of
about 115 pixels radius**, and anything you draw in that corner area is simply not
visible. The straight edges are fine: every pixel along them shows, right up to
the edge (the outermost row is a little dimmer).

So when you design a screen, keep buttons, text and anything important **inside a
rectangle with rounded corners of radius 115** — the library gives you that number
as `AMOLED206_CORNER_RADIUS`. Some easy ways:

- Put content in the middle and along the edges, not in the corners.
- For a full-screen panel or background shape, round it to match:
  `lv_obj_set_style_radius(obj, AMOLED206_CORNER_RADIUS, 0);`
- Keep corner-aligned labels in by about 35 pixels from both edges
  (`LV_ALIGN_TOP_LEFT` with an offset of 35, 35), or move them toward the middle
  of the edge.

The same applies in every rotation. How it was measured is in
[README-engineering.md](README-engineering.md#the-visible-area-and-the-rounded-corners).

## Touch

The board always has a capacitive touch panel. While the board sleeps, the touch
chip keeps scanning for about 14 seconds and then settles into its own low-power
idle; nothing in software shortens that (details in
[README-engineering.md](README-engineering.md#touch-and-sleep)). It reports positions in screen
pixels, so **no calibration is needed**: on the test board the touch point landed
under the finger across the whole screen. `lv_setup.begin()` starts it and prints
whether it answered. If touch lands in the wrong place in some rotation, that's a
bug worth reporting.

## Brightness and sleep

`display.setBacklight(pct)` sets the brightness from 0 to 100. An AMOLED has no
backlight: each pixel makes its own light, and this setting turns them all up or
down. **Black pixels are off whatever the brightness**, so a dark UI with little
lit area is the single biggest power saving on this board. (`setBrightness()` is
the same call under a more accurate name.)

`display.sleep()` turns the screen off and puts it in its **deepest** low-power
mode (measured on the bench: about 0.35 mA less than the ordinary sleep mode). In
that mode the screen **forgets its picture**, so `display.wake()` restarts it and
leaves it black. With LVGL, use **`lv_setup.sleep()` / `lv_setup.wake()`**: the wake
also tells LVGL to redraw the whole screen. Call `display.sleep()` before the power
library's `deepSleep()`.

There is no separate power switch to cut for the screen: the board library won't
turn the screen's power-enable rail off, because that was measured to make the
board draw about **13 mA more**, not less.

## Tearing

The screen repaints itself from top to bottom many times a second. If a new picture
is being sent while it repaints, the top of the screen can show the new picture while
the bottom still shows the old one. That split is called **tearing**. It looks like a
fast-moving object breaking into two pieces for an instant.

This library avoids it. The screen sends a signal at the moment it starts a new
repaint. `lv_setup` waits for that signal and then sends the changed part of the
picture from the top down, so it stays ahead of the repaint. It's **on by default**.
It uses about **400 KB of the board's PSRAM** (extra memory) for a full copy of the
screen, which the board has plenty of. You'll see `LVGL: framebuffer, tear-free` on
the Serial Monitor at startup.

That copy of the screen also works around a quirk of the screen's controller chip.
If only part of a row is sent, it leaves a faint vertical line on light
backgrounds. So `lv_setup` always sends whole rows, taking the parts that didn't
change from its copy. You don't need to do anything about it. The details are in
[README-engineering.md](README-engineering.md#partial-width-writes-leave-a-line).

It keeps up: on the test board the screen repaints 59 times a second, and with the
library's default (80 MHz) connection even a full-screen change goes out in about
13 ms — quicker than one repaint. Hundreds of frames of a dragged object were sent
in step with the screen, with none late. To stop it waiting for the screen's signal,
add `#define LV_SETUP_TEAR_FREE 0` before `#include "lv_setup.hpp"`. The **TearDemo**
sketch in [extras/](extras/) shows the difference and prints how well it keeps up.
The details are in
[README-engineering.md](README-engineering.md#tear-free-updates).

---

## What else is on the board

This library only does the screen. The rest is documented in the board and power
libraries, in plain language first with detail behind links:

- **Speaker and microphones** — a small speaker, two microphones, an audio codec
  and an amplifier. This library doesn't play sound; how the audio is wired and
  where to start is in
  [README-sound.md](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README-sound.md#sound-characteristics-at-a-glance).
- **TF / microSD card slot** — it does **not** share anything with the display,
  so the two run together. See
  [README-sdcard.md](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README-sdcard.md).
- **Vibration motor** — `board.vibrate(true)` buzzes it. See
  [README-board.md](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README-board.md#vibration-motor).
- **Clock and motion sensor** — a real-time clock that keeps time while the board
  is off (its own library,
  [chipguy_ESP32-S3-Touch-AMOLED-2.06_rtc](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_rtc/blob/main/README.md)),
  and a 6-axis motion sensor that can wake the board when it moves (see
  [the motion sensor](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README-board.md#motion-sensor-imu))
  or only when you raise it to look at it
  ([raise to look](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README-orientation.md)).
  This library's [**GlanceWatch**](#glancewatch--a-working-wristwatch) example puts
  it all together as a working watch face.
- **Battery, charging, power button** — the
  [power library README](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_power/blob/main/README.md#power-characteristics-at-a-glance)
  answers the standard power questions. The short version: a power chip charges
  the battery, software can read it and see whether USB is plugged in, and
  software **can** turn the board fully off.

---

## Where to go next

| File | For whom |
|---|---|
| [README-engineering.md](README-engineering.md) | Engineers: the QSPI protocol, the init sequence, the 22-column offset and the even-alignment rule, the visible area and rounded corners, why rotation is in software, the DMA flush pipeline and its timing, tear-free updates on the TE line, the panel's power enable, and the touch controller's registers and power modes. |
| [extras/](extras/) | A diagnostic sketch, **TearDemo**: see tearing with and without TE sync, and the panel's measured refresh rate. |
| [board library README](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README.md) | Anyone using the buttons, the motor, the clock or the motion sensor, or wanting the pin map. |
| [README-board.md](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README-board.md) | Developers: every pin, the I²C bus and its addresses, the buttons, the motor, the RTC and IMU. |
| [README-sound.md](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README-sound.md) | Developers who want sound out of the speaker or audio in from the mics. |
| [README-sdcard.md](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README-sdcard.md) | Developers mounting the card with the driver of their choice. |
| [README-limitations-errata.md](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/README-limitations-errata.md) | Anyone choosing this board or starting a feature: what it can't do or does wrong, the screen's quirks included. |
| [power library README](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_power/blob/main/README.md) | Developers running on battery: what the readings mean, what can sleep, wake and shut off. |
| [schematic-description.md](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/schematic-description.md) | Engineers, and LLMs without the drawing: the manufacturer's schematic narrated net by net. |

---

## License

MIT — see [LICENSE](LICENSE).

The CO5300 initialization sequence follows the manufacturer's Arduino examples
(Waveshare's ESP32-S3-Touch-AMOLED-2.06 repository, Apache-2.0, which use
Arduino_GFX, BSD). `lv_conf.h` is based on LVGL's template (LVGL is MIT-licensed).

Simple Shooter in GlanceWithDemos — its code, pictures and sounds — is original work
by the same author, ported from a MicroPython version, and is under the same MIT
license.
