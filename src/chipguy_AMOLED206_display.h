/*
 * CO5300 QSPI AMOLED driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * Drives the 410x502 CO5300 panel through the ESP-IDF esp_lcd SPI panel IO in
 * QUAD mode (commands 1-1-1 with opcode 0x02, pixels 1-1-4 with opcode 0x32).
 * Designed to feed an LVGL display in PARTIAL render mode through a copy of
 * the screen in PSRAM: drawRegion() puts each rendered band into it (byte
 * swap plus the rotation transform) and notes the panel rows it changed, and
 * commit() sends those rows, as full-width strips, once LVGL has finished
 * the frame.
 *
 * Board wiring (fixed on the ESP32-S3-Touch-AMOLED-2.06):
 *   SIO0..3  4 5 6 7     SCLK 11     CS 12     RESET 8 (active LOW)
 *   TE       13          the panel's tearing-effect output (unused here)
 *   DSI_PWR_EN  the module's power enable, pulled up to the AXP2101's ALDO2
 *               through 10k; begin() turns ALDO2 on via the board library.
 *   VCI / VDDIO  VCC3V3 (the AXP2101's DCDC1) -- always on.
 *
 * Four things about the CO5300 shape this driver:
 *   - Its column RAM is wider than the glass.  The visible 410 columns start
 *     at column 22; the offset is applied here and is invisible to callers.
 *   - Every address window must start on an EVEN row/column and span an EVEN
 *     number of them.  Direct callers of flushRegion() must round for this
 *     (x1, y1 even; x2, y2 odd); the framebuffer path does it itself.
 *   - A window narrower than the panel leaves a faint vertical line, a few
 *     pixels wide, at its left edge on light backgrounds (measured on two
 *     units, 2026-10-28; aligning the start to 4 or 8 columns doesn't help).
 *     So the framebuffer path only ever writes whole native rows.
 *     flushRegion() writes the window it is given and is subject to it.
 *   - It has no row/column exchange, so the 90/270 rotations are done in
 *     software (a transpose during the copy that the byte swap needs anyway).
 *     All four rotations cost the same.
 *
 * Touch is an FT3168 on the board's shared I2C bus, in the companion
 * chipguy_AMOLED206_touch driver.
 */

#pragma once

#include <Arduino.h>
#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <AMOLED206_Board.h>

#ifndef CONFIG_IDF_TARGET_ESP32S3
#error "chipguy_AMOLED206_display: this board is an ESP32-S3. Select ESP32S3 Dev Module."
#endif

// ── Board-option guard ─────────────────────────────────────────────────────
// The module is an ESP32-S3R8: 8 MB of OCTAL PSRAM.  The Arduino IDE does not
// persist board options reliably, and the "ESP32S3 Dev Module" PSRAM menu
// offers "QSPI PSRAM" (PSRAM=enabled) as well as "OPI PSRAM" (PSRAM=opi).
// QSPI mode cannot talk to an octal chip: the core probes, fails and runs with
// none.  Refuse it at compile time when ARDUINO_FQBN names it.  "Disabled" is
// left alone: it works, it just wastes 8 MB.
static constexpr bool amoled206_fqbnHasOption(const char *s, const char *opt) {
    for (; *s; ++s) {
        const char *a = s; const char *b = opt;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b && (*a == ',' || *a == 0)) return true;
    }
    return false;
}
#ifdef ARDUINO_FQBN
static_assert(!amoled206_fqbnHasOption(ARDUINO_FQBN, "PSRAM=enabled"),
    "PSRAM is set to QSPI. This board's ESP32-S3R8 has OCTAL PSRAM: select "
    "Tools > PSRAM > OPI PSRAM (or PSRAM=opi in the --fqbn).");
#endif

// Native panel geometry: portrait, the CO5300's own scan orientation.
// width()/height() reflect the active rotation.
#define AMOLED206_NATIVE_WIDTH   410
#define AMOLED206_NATIVE_HEIGHT  502

// Rotation indices, for readability at the call site.
#define AMOLED206_ROTATION_PORTRAIT          0   // 410x502 (default)
#define AMOLED206_ROTATION_LANDSCAPE         1   // 502x410
#define AMOLED206_ROTATION_PORTRAIT_FLIPPED  2   // 410x502, upside down
#define AMOLED206_ROTATION_LANDSCAPE_FLIPPED 3   // 502x410

// Largest region flushRegion() accepts, in pixels.  Two internal DMA buffers
// of this size are allocated in begin().  lv_setup's 40-row bands fit (in the
// landscape rotations a band is 502 wide).
#ifndef AMOLED206_FLUSH_MAX_PIXELS
#define AMOLED206_FLUSH_MAX_PIXELS (502 * 40)
#endif

// QSPI clock.  80 MHz measured clean on one unit (2026-10-05): no artifacts,
// and a full-frame write takes 12.6 ms against 22.8 ms at 40 MHz.  That is
// what lets tear-free mode keep ahead of the panel's ~59 Hz scan (16.95 ms
// per frame).  40 MHz is what the manufacturer's Arduino examples use; build
// with -DAMOLED206_QSPI_HZ=40000000 to drop back if a unit shows speckles.
#ifndef AMOLED206_QSPI_HZ
#define AMOLED206_QSPI_HZ 80000000
#endif

// The glass has rounded corners.  Measured on one unit (2026-10-05): the
// whole 410x502 grid is visible edge to edge (the outermost row/column about
// half masked), and each corner is cut on a circle of radius ~115 px.  Keep
// UI content inside a rounded rectangle of this radius.  Applies to every
// rotation (the corners are symmetric).
#define AMOLED206_CORNER_RADIUS 115

// Normalize a rotation argument to an index 0..3.  Accepts either the index
// directly (0..3) or degrees (0/90/180/270) as aliases for 0/1/2/3.
static inline uint8_t amoled206_normalize_rotation(uint16_t rotation) {
    switch (rotation) {
        case 90:  return 1;
        case 180: return 2;
        case 270: return 3;
        default:  return (uint8_t)(rotation & 0x03);
    }
}

class chipguy_AMOLED206_display {
public:
    // Takes the board, which owns the PMIC rail (ALDO2) the panel needs.
    explicit chipguy_AMOLED206_display(AMOLED206_Board &board);

    // Turn on the panel's power enable, initialize the QSPI bus and the
    // CO5300, clear the screen to black and switch it on.
    // rotation: 0 = portrait (410x502, the panel's native orientation),
    //           1 = landscape (502x410), 2 = portrait flipped,
    //           3 = landscape flipped.  Degrees are accepted as aliases.
    // Returns false if the bus or the DMA buffers could not be set up.
    bool begin(uint16_t rotation = AMOLED206_ROTATION_PORTRAIT);

    // Change rotation after begin().  Only changes how later flushes are
    // mapped; redraw afterwards.
    void setRotation(uint16_t rotation);

    int16_t width()  const { return _w; }
    int16_t height() const { return _h; }
    uint8_t rotation() const { return _rotation; }

    // Panel brightness, 0..100 (CO5300 register 0x51).  An AMOLED has no
    // backlight -- this scales the drive of the pixels themselves, and black
    // pixels are off whatever it is set to.  Named setBacklight() for the
    // family API; setBrightness() is the same thing.
    void setBacklight(int percentage);
    void setBrightness(int percentage) { setBacklight(percentage); }
    int  backlight() const { return _brightPct; }

    // ── The framebuffer path (what lv_setup uses) ───────────────────────────
    // A copy of the whole screen in PSRAM (~400 KB), kept in the panel's own
    // orientation and byte order.  Starts black, like the panel after begin().
    // Returns false (and the path stays off) if PSRAM can't supply it.
    bool beginFramebuffer();
    bool hasFramebuffer() const { return _nfb != nullptr; }

    // Copy a rectangle of RGB565 pixels (LVGL's little-endian order, current
    // rotation's frame, inclusive, any alignment) into the framebuffer, and
    // note the native rows it touched.  Nothing is sent yet.  Writes to PSRAM
    // run in order; the rotations read `pixels` out of order instead.
    void drawRegion(int x1, int y1, int x2, int y2, const uint16_t *pixels);

    // Send every native row changed since the last commit(), once, as
    // full-width strips from the top (waiting for TE first when tear sync is
    // on).  Returns once the last strip is queued.
    void commit();

    // Mark the whole framebuffer changed, so the next commit() resends it.
    void invalidateAll();

    // Push a rectangular region of RGB565 pixels straight to the panel,
    // bypassing the framebuffer.  Coordinates are inclusive, in the current
    // rotation's frame, and must be even-aligned (x1, y1 even; x2, y2 odd).
    // Anything narrower than the panel shows the CO5300's partial-width line
    // (see the top of this file).  `pixels` is in LVGL's native little-endian
    // order and is only read: it is free for reuse as soon as this returns.
    //
    // Asynchronous: converts into a driver DMA buffer, waits for the previous
    // flush's DMA, sends the address window, queues this region's DMA and
    // returns before it completes.
    void flushRegion(int x1, int y1, int x2, int y2, const uint16_t *pixels);

    // Block until the most recently queued flush has finished transmitting.
    void waitFlush();

    // ── Tear-free updates (the TE line) ─────────────────────────────────────
    // The CO5300 pulses TE (GPIO13) at the start of every vertical blank.
    // With sync on, flushFrameRegion() waits for that pulse and then writes
    // the dirty area top to bottom in the panel's own scan order, so the
    // write runs ahead of the scan instead of across it.  That is tear-free
    // exactly when the write is FASTER than the scan; see lateFrames() and
    // README-engineering.md "Tear-free updates".
    //
    // Turn sync on/off.  Returns false if no TE pulse arrived within 100 ms
    // (then sync stays off and flushes go out immediately).
    bool setTearSync(bool enable);
    bool tearSync() const { return _teSync; }

    // Measure the panel's refresh rate from TE over `ms` milliseconds.  0 if
    // TE isn't toggling.  Works whether or not sync is on.
    float measureRefreshHz(uint32_t ms = 500);

    // Stream a region of a caller-owned FULL-SCREEN framebuffer (current
    // rotation's frame, stride = width(), LVGL byte order, may live in PSRAM)
    // to the panel: optionally wait for TE, then send the native rows the
    // region covers, full width, as strips from the top, converting through
    // the internal DMA buffers.  Coordinates inclusive and even-aligned as for
    // flushRegion().  Returns once the last strip is queued; `fb` may be drawn
    // into again.  (lv_setup no longer uses this: see commit().)
    void flushFrameRegion(const uint16_t *fb, int x1, int y1, int x2, int y2);

    // Diagnostics for tear sync: frames written in sync, and frames during
    // which another TE pulse arrived before the write finished (the scan
    // caught up with the write: those can tear).  A frame is judged when the
    // next one starts.  resetTearStats() zeroes both.
    uint32_t syncedFrames() const { return _syncedFrames; }
    uint32_t lateFrames()   const { return _lateFrames; }
    void     resetTearStats() { _syncedFrames = _lateFrames = 0; }

    // Fill the whole screen with one RGB565 color (bring-up tests).  Writes
    // the panel only: the framebuffer, if any, keeps what it had.
    void fillScreen(uint16_t color);

    // The panel's sunlight-readability contrast enhancement (register 0x58):
    // 0 = off (the default), 1..3 = low / medium / high.
    void setContrastEnhance(uint8_t level);

    // Escape hatch: send any CO5300 command with parameters (opcode 0x02, the
    // command in the address phase).  Waits for any in-flight flush first.
    void writeCommand(uint8_t cmd, const uint8_t *params = nullptr, size_t len = 0);

    // sleep(): display off, sleep in, then the CO5300's deep standby (the
    // lowest panel state; measured ~0.35 mA below sleep-in alone).  The frame
    // memory is NOT retained.  wake(): hardware reset and full re-init, then
    // the framebuffer is sent back if there is one; without one the panel is
    // black -- redraw afterwards (lv_setup.wake() does that for LVGL).
    // Both wait for any in-flight flush first.  ALDO2 (the module's power
    // enable) is never cut: that costs ~13 mA (measured).
    void sleep();
    void wake();
    bool asleep() const { return _asleep; }

private:
    AMOLED206_Board &_board;
    esp_lcd_panel_io_handle_t _io = nullptr;
    SemaphoreHandle_t _doneSem = nullptr;
    int  _outstanding = 0;          // queued colour transfers not yet reaped
    int16_t _w = AMOLED206_NATIVE_WIDTH;
    int16_t _h = AMOLED206_NATIVE_HEIGHT;
    uint8_t _rotation = 0;
    int _brightPct = 80;
    bool _asleep = false;

    uint16_t *_dmaBuf[2] = { nullptr, nullptr };
    int _dmaNext = 0;

    // The framebuffer path: native orientation, big-endian (as sent), PSRAM.
    uint16_t *_nfb = nullptr;
    int _dirtyY1 = AMOLED206_NATIVE_HEIGHT, _dirtyY2 = -1;   // native rows, inclusive
    void _sendRows(int ny1, int ny2);                        // TE wait + full-width strips

    // TE
    SemaphoreHandle_t _teSem = nullptr;
    volatile uint32_t _teCount = 0;
    volatile uint32_t _teAtDone = 0;     // _teCount when the last colour DMA finished
    uint32_t _teAtStart = 0;
    bool _checkPending = false;
    bool _teAttached = false;
    bool _teSync = false;
    uint32_t _syncedFrames = 0, _lateFrames = 0;
    static void IRAM_ATTR _teIsr(void *arg);
    bool _attachTE();
    bool _waitTE(uint32_t timeoutMs);
    bool _startFrame();                  // waitFlush, judge the last frame, wait for TE
    void _endFrame(bool synced);

    static bool IRAM_ATTR _onColorDone(esp_lcd_panel_io_handle_t io,
                                       esp_lcd_panel_io_event_data_t *edata,
                                       void *user_ctx);
    void _reap();
    void _initPanel();
    void _cmd(uint8_t cmd, const uint8_t *params, size_t len);
    void _cmd8(uint8_t cmd, uint8_t v) { _cmd(cmd, &v, 1); }
    void _setNativeWindow(int nx1, int ny1, int nx2, int ny2);
    void _pushNative(int nx1, int ny1, int nx2, int ny2, const uint16_t *buf, size_t pixels);
};
