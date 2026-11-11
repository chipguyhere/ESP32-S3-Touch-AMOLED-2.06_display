/*
 * FT3168 capacitive touch driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 * See chipguy_AMOLED206_touch.h.
 */

#include "chipguy_AMOLED206_touch.h"
#include "chipguy_AMOLED206_display.h"   // native geometry + rotation helper

// FT3x68 registers (FocalTech FT6x36-family layout).
#define FT_REG_TD_STATUS  0x02   // number of touch points, low nibble
                                 // 0x03..0x06: P1 XH XL YH YL; XH bits 7:6 = event
#define FT_REG_POWER_MODE 0xA5   // 0 active, 1 monitor, 2 standby, 3 hibernate
#define FT_REG_CHIP_ID    0xA0   // documented 0x03 = FT3168; reads 0x00 on the bench unit

#define FT_EVENT_LIFT_UP  1

chipguy_AMOLED206_touch::chipguy_AMOLED206_touch(AMOLED206_Board &board)
    : _board(board) {}

void IRAM_ATTR chipguy_AMOLED206_touch::_isr(void *arg) {
    ((chipguy_AMOLED206_touch *)arg)->_irq = true;
}

void chipguy_AMOLED206_touch::_reset() {
    pinMode(AMOLED206_Board::TOUCH_RESET_PIN, OUTPUT);
    digitalWrite(AMOLED206_Board::TOUCH_RESET_PIN, HIGH);
    delay(1);
    digitalWrite(AMOLED206_Board::TOUCH_RESET_PIN, LOW);
    delay(20);
    digitalWrite(AMOLED206_Board::TOUCH_RESET_PIN, HIGH);
}

bool chipguy_AMOLED206_touch::begin() {
    _board.begin();
    _reset();

    // The controller needs a moment after reset before it answers I2C.  The
    // manufacturer waits 50 ms; poll for up to 300 ms instead of guessing.
    _present = false;
    uint32_t t0 = millis();
    do {
        delay(20);
        if (_board.Wire.readRegister(AMOLED206_Board::FT3168_ADDR, FT_REG_CHIP_ID, _chipId)) {
            _present = true;
            break;
        }
    } while (millis() - t0 < 300);
    if (!_present) return false;

    _board.Wire.writeRegister(AMOLED206_Board::FT3168_ADDR, FT_REG_POWER_MODE, 0x01);   // monitor

    pinMode(AMOLED206_Board::TOUCH_INT_PIN, INPUT_PULLUP);
    attachInterruptArg(digitalPinToInterrupt(AMOLED206_Board::TOUCH_INT_PIN),
                       _isr, this, FALLING);
    return true;
}

void chipguy_AMOLED206_touch::setRotation(uint16_t rotation) {
    _rotation = amoled206_normalize_rotation(rotation);
}

bool chipguy_AMOLED206_touch::read(uint16_t &x, uint16_t &y) {
    if (!_present) return false;

    // Only touch the bus when something is happening: INT fired since the
    // last read, INT is held low right now, or a touch is in progress (so the
    // lift-off gets seen).
    bool active = _irq || _down ||
                  digitalRead(AMOLED206_Board::TOUCH_INT_PIN) == LOW;
    if (!active) { _touches = 0; return false; }
    _irq = false;

    uint8_t d[5];
    if (!_board.Wire.readRegister(AMOLED206_Board::FT3168_ADDR, FT_REG_TD_STATUS, d, sizeof d)) {
        _down = false;
        _touches = 0;
        return false;
    }
    _touches = d[0] & 0x0F;
    uint8_t event = d[1] >> 6;
    if (_touches == 0 || _touches > 2 || event == FT_EVENT_LIFT_UP) {
        _down = false;
        _touches = 0;
        return false;
    }

    int tx = ((d[1] & 0x0F) << 8) | d[2];
    int ty = ((d[3] & 0x0F) << 8) | d[4];
    const int NW = AMOLED206_NATIVE_WIDTH, NH = AMOLED206_NATIVE_HEIGHT;
    if (tx > NW - 1) tx = NW - 1;
    if (ty > NH - 1) ty = NH - 1;

    // Inverse of the display's logical -> native mapping (see flushRegion).
    switch (_rotation) {
        default:
        case 0: x = tx;          y = ty;          break;
        case 1: x = ty;          y = NW - 1 - tx; break;
        case 2: x = NW - 1 - tx; y = NH - 1 - ty; break;
        case 3: x = NH - 1 - ty; y = tx;          break;
    }
    _down = true;
    return true;
}

void chipguy_AMOLED206_touch::sleep() {
    if (!_present) return;
    _board.Wire.writeRegister(AMOLED206_Board::FT3168_ADDR, FT_REG_POWER_MODE, 0x03);
    _down = false;
}

void chipguy_AMOLED206_touch::wake() {
    if (!_present) return;
    _reset();
    delay(60);
    _board.Wire.writeRegister(AMOLED206_Board::FT3168_ADDR, FT_REG_POWER_MODE, 0x01);
    _irq = false;
}
