/*******************************************************************************
 * ESP32-S3-Touch-AMOLED-2.06 LVGL Calculator
 *
 * For the Waveshare ESP32-S3-Touch-AMOLED-2.06: CO5300 410x502 QSPI AMOLED,
 * FT3168 capacitive touch.
 *
 * A working four-function calculator with a touch keypad, styled after the
 * familiar dark iOS calculator: a black background, dark-gray number keys, a
 * light-gray top function row (backspace / C / %) and an orange operator
 * column (/ x - + =).
 *
 * Resolution independence is the point of this example.  The whole UI is laid
 * out from the live display resolution in ui.cpp — button diameter, gaps,
 * margins and font sizes are all derived from width()/height() at runtime.
 * Nothing is hard-coded to a panel size, so the identical ui.cpp lays out a
 * proportionate keypad on any panel without edits.  See the layout math in
 * build_keypad() in ui.cpp.
 *
 * Orientation: lv_setup.begin(rotation) accepts 0 = portrait (410x502, the
 * panel's native orientation), 1 = landscape (502x410),
 * 2 = portrait flipped, 3 = landscape flipped (degrees 90/180/270 work as
 * aliases for 1/2/3).  A
 * calculator wants portrait, so this sketch passes 1.
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

// The calculator UI lives in ui.h / ui.cpp in this sketch folder.
#include "ui.h"

void setup() {
    Serial.begin(115200);
    // The USB-C port is the S3's own USB: if nothing on the computer is reading
    // it, each print would wait for a timeout and the sketch would stutter.
    Serial.setTxTimeoutMs(0);

    // Initialize display, touch, and LVGL in PORTRAIT (410x502), the panel's
    // native orientation: a calculator keypad wants the tall one.
    lv_setup.begin(AMOLED206_ROTATION_PORTRAIT);
    Serial.printf("LVGL initialized with %dx%d display, touch %s\n",
                  display.width(), display.height(),
                  lv_setup.hasTouch() ? "present" : "absent");

    // Build the calculator screen.
    ui_init();
}

void loop() {
    // Give loop control to LVGL.
    lv_timer_handler();
    delay(5);
}
