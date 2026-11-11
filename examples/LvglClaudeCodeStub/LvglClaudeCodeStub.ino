/*******************************************************************************
 * ESP32-S3-Touch-AMOLED-2.06 LVGL Claude Code Stub
 *
 * For the Waveshare ESP32-S3-Touch-AMOLED-2.06: CO5300 410x502 QSPI AMOLED,
 * FT3168 capacitive touch.
 *
 * A minimal starting point for building an LVGL application from scratch on
 * this hardware.  The UI lives in a tiny ui.h / ui.cpp pair in the sketch
 * folder that draws a simple "Hello, world!" screen.
 *
 * The intent is to hand this to Claude Code (or any developer) as a clean
 * scaffold: the display, touch, and LVGL plumbing is already wired up, so you
 * can focus on writing your own UI in ui.cpp.  Just replace the contents of
 * ui_init() with your own widgets and grow from there.
 *
 * Orientation: lv_setup.begin(rotation) accepts 0 = portrait (410x502, the
 * panel's native orientation), 1 = landscape (502x410),
 * 2 = portrait flipped, 3 = landscape flipped (degrees 90/180/270 work as
 * aliases for 1/2/3).  This sketch defaults to portrait; change the
 * argument below to rotate.  Display and touch rotate together.
 *
 * Besides touch, the board has two buttons: BOOT (board.bootButtonPressed())
 * and PWR (board.pwrButtonPressed()).  Holding PWR for several seconds powers
 * the board off in hardware, so use it for short presses only.
 *
 * Requires the following libraries:
 *   - chipguy_ESP32-S3-Touch-AMOLED-2.06_display  (display + touch, this library)
 *   - chipguy_ESP32-S3-Touch-AMOLED-2.06_board    (shared I2C bus, pin map, rails)
 *   - lvgl 9
 *
 * Arduino IDE Board Settings (Tools menu):
 *   - Board: "ESP32S3 Dev Module"
 *   - PSRAM: "OPI PSRAM"            (the module has 8 MB octal PSRAM)
 *   - Flash Size: "32MB (256Mb)"     (the GD25Q256 fitted is 32 MB; 16MB also works)
 *   - Partition Scheme: a 32 MB scheme, e.g. "32M Flash (4.8MB APP/22MB FATFS)"
 *   - USB CDC On Boot: "Enabled"    (the Type-C port is the S3's native USB;
 *                                    without this, Serial prints go nowhere)
 *
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
// It's important for the config and library versions to be compatible.
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

// Fonts
// The Montserrat font is built into LVGL in multiple sizes, each size takes up memory.
// We enable the 14 point font as a default font.
#define LV_FONT_MONTSERRAT_14 1
// need other sizes?  You'll know if you see compiler errors after trying to
// use them.  Just edit lv_conf.h where FONTs are turned off (0) and turn them on (1)

// The UI for this example lives in ui.h / ui.cpp in this sketch folder.
// It is a hand-written "Hello, world!" screen meant as a starting point.
// Build your own application by editing ui_init() in ui.cpp.
#include "ui.h"

void setup() {
    Serial.begin(115200);
    // The USB-C port is the S3's own USB: if nothing on the computer is reading
    // it, each print would wait for a timeout and the sketch would stutter.
    Serial.setTxTimeoutMs(0);

    // Initialize display, touch, and LVGL.  Defaults to portrait; pass a
    // rotation to change it (0/2 = portrait, 1/3 = landscape; see header).
    lv_setup.begin();
    Serial.printf("LVGL initialized with %dx%d display, touch %s\n",
                  display.width(), display.height(),
                  lv_setup.hasTouch() ? "present" : "absent");

    // Start the application's own setup
    ui_init();
}


void loop() {
    // Give loop control to LVGL objects created by the application
    lv_timer_handler();
    delay(5);
}
