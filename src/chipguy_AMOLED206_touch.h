/*
 * FT3168 capacitive touch driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * Board wiring (fixed):
 *   SDA 15   SCL 14   the shared bus, owned by AMOLED206_Board (address 0x38)
 *   INT 38   active LOW, pulses while a finger is down
 *   RST  9   active LOW
 *
 * The controller reports pixel coordinates in the panel's native 410x502
 * portrait frame; read() maps them to the active rotation with the same
 * convention as chipguy_AMOLED206_display.  No calibration is needed.
 *
 * It is put in the FT3x68 "monitor" power mode, as the manufacturer's
 * examples do: it drops to a slow scan when idle and wakes on a touch.  In
 * that state it may not answer I2C, so read() only talks to it after INT has
 * fired (or while a touch is in progress) and otherwise answers "released"
 * without touching the bus.
 */

#pragma once

#include <Arduino.h>
#include <AMOLED206_Board.h>

class chipguy_AMOLED206_touch {
public:
    explicit chipguy_AMOLED206_touch(AMOLED206_Board &board);

    // Reset the controller, wait for it to boot, and check that it answers.
    // Returns false if it doesn't (read() then always reports released).
    bool begin();

    bool present() const { return _present; }

    // Match the display's rotation: an index 0..3 or degrees 0/90/180/270
    // (0 = portrait, the panel's native orientation).
    void setRotation(uint16_t rotation);

    // Read the first touch point.  Returns true while the screen is pressed,
    // with x,y in screen pixels for the active rotation.
    bool read(uint16_t &x, uint16_t &y);

    // Number of fingers the last read() saw (0..2).  Only the first is mapped.
    uint8_t touches() const { return _touches; }

    // The chip ID register (0xA0), as read in begin().  FocalTech documents
    // 0x03 for the FT3168, but the bench unit (2026-10-05) reads 0x00 there
    // while working normally -- don't use it to identify the part.
    uint8_t chipId() const { return _chipId; }

    // Controller power.  sleep() sends the FocalTech hibernate command
    // (0xA5 = 3) and wake() resets the controller.  MEASURED (2026-10-06): this
    // FT3168 does not honour hibernate, nor the monitor-timing registers
    // 0x86/0x87 -- after the board sleeps it scans for ~13.7 s (~1.3 mA extra,
    // 60 ms pulses) and then drops to its own idle (~100 ms pulses) whatever
    // is written.  Holding it in reset avoids the burst but costs +0.28 mA
    // continuously, so for sleeps longer than about a minute, letting it idle
    // is cheaper.  See the display README-engineering, "Touch and sleep".
    void sleep();
    void wake();

private:
    AMOLED206_Board &_board;
    bool _present = false;
    uint8_t _rotation = 0;
    uint8_t _touches = 0;
    uint8_t _chipId = 0;
    bool _down = false;
    volatile bool _irq = false;

    static void IRAM_ATTR _isr(void *arg);
    void _reset();
};
