/*
 * lv_setup_AMOLED206.hpp - Connects LVGL to the Waveshare
 * ESP32-S3-Touch-AMOLED-2.06 display and touch.
 *
 * This is the *actual* LVGL-to-driver glue for this board.  It ships inside the
 * library (src/) so every example can share one copy.  Each example's sketch
 * folder contains a small lv_setup.hpp that simply #includes this file; that
 * thin shim is the file you edit if you want to take this code over and
 * customize it (see the comments in any example's lv_setup.hpp).
 *
 * Include this file (or, normally, the example's lv_setup.hpp) after lvgl.h.
 *
 * How the panel is fed: LVGL renders in PARTIAL mode into one band buffer in
 * internal RAM.  Each band goes into the display driver's copy of the screen
 * in PSRAM (display.drawRegion(), ~400 KB), which notes the panel rows it
 * changed.  When LVGL flushes the frame's last band, display.commit() sends
 * those rows -- each one once, whichever bands touched it -- as full-width
 * strips.  Two reasons for that shape:
 *
 *   - The CO5300 leaves a faint vertical line at the left edge of any write
 *     window narrower than the panel, so every write must be whole rows.
 *     The framebuffer supplies the parts of those rows LVGL didn't redraw.
 *   - Tear-free (LV_SETUP_TEAR_FREE, default 1): commit() waits for the
 *     panel's TE pulse (start of vertical blank) and writes top to bottom in
 *     the panel's scan order, so the write stays ahead of the scan.
 *
 * Without PSRAM (PSRAM disabled in the board options) it falls back to
 * sending each band straight to the panel, stretched to the full panel width.
 * That fallback is only line-free in the portrait rotations: in landscape
 * LVGL's bands are narrow strips of panel rows.
 */

#pragma once

#include <AMOLED206_Board.h>                 // shared I2C bus + pin map + rails
#include <chipguy_AMOLED206_display.h>
#include <chipguy_AMOLED206_touch.h>

// The board owns the shared I2C bus; the touch driver (and the PMIC, RTC, IMU
// and codecs) reach it through this object.
static AMOLED206_Board board;
static chipguy_AMOLED206_display display(board);

// 1 = wait for TE before each frame's write, when TE is toggling (see above);
// 0 = send as soon as LVGL finishes a frame.
#ifndef LV_SETUP_TEAR_FREE
#define LV_SETUP_TEAR_FREE 1
#endif

// Height (in rows) of the LVGL render band.  Must be even, and (for the
// no-PSRAM fallback) band width x rows must fit AMOLED206_FLUSH_MAX_PIXELS.
// 502 * 40 * 2 = ~40 KB.
#ifndef LV_SETUP_BUFFER_ROWS
#define LV_SETUP_BUFFER_ROWS 40
#endif

class lv_setup_class {
public:
    // Initializes display, touch, and LVGL.
    // rotation: 0 = portrait (410x502, the panel's native orientation),
    //           1 = landscape (502x410), 2 = portrait flipped,
    //           3 = landscape flipped.  Degrees (0/90/180/270) are accepted
    //           as aliases for 0/1/2/3.
    void begin(uint16_t rotation = AMOLED206_ROTATION_PORTRAIT) {
        if (!display.begin(rotation)) {
            Serial.println("Display init failed!");
            while (1) delay(100);
        }
        Serial.println("Display initialized");

        _hasTouch = _touch.begin();
        _touch.setRotation(rotation);
        Serial.println(_hasTouch ? "Touch controller found" : "Touch controller did not answer");

        lv_init();

        static lv_display_t *disp = lv_display_create(display.width(), display.height());
        lv_display_set_user_data(disp, this);
        // Force RGB565 so the driver's byte swap (2 bytes/pixel) is always
        // correct, independent of how lv_conf.h's color depth resolved.
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

        // One band buffer is enough either way: the driver copies out of it
        // before the flush callback returns.  Internal RAM renders fastest.
        int maxW = AMOLED206_NATIVE_HEIGHT;   // widest band, in the landscape rotations
        size_t buf_bytes = (size_t)maxW * LV_SETUP_BUFFER_ROWS * 2;
        void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
        if (!buf) {
            Serial.println("LVGL buffer allocation failed!");
            while (1) delay(100);
        }
        lv_display_set_buffers(disp, buf, nullptr, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

        if (display.beginFramebuffer()) {
            // The driver rounds and widens what it sends, so LVGL's areas
            // need no rounder.
            lv_display_set_flush_cb(disp, _fb_flush);
#if LV_SETUP_TEAR_FREE
            if (display.setTearSync(true))
                Serial.printf("LVGL: framebuffer, tear-free (panel refresh %.1f Hz)\n",
                              display.measureRefreshHz(200));
            else
                Serial.println("LVGL: framebuffer, no TE signal (not tear-free)");
#else
            Serial.println("LVGL: framebuffer (tear sync off)");
#endif
        } else {
            lv_display_set_flush_cb(disp, _disp_flush);
            lv_display_add_event_cb(disp, _fullWidthRounder, LV_EVENT_INVALIDATE_AREA, nullptr);
            Serial.println("LVGL: no PSRAM framebuffer -- direct bands (landscape shows the CO5300 line)");
        }

        if (_hasTouch) {
            static lv_indev_t *indev = lv_indev_create();
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(indev, _touchpad_read);
            lv_indev_set_user_data(indev, this);
        }

        lv_tick_set_cb(_tick_get);
    }

    // Whether the touch controller answered at startup.
    bool hasTouch() const { return _hasTouch; }

    // Access the touch driver directly (touches(), sleep()/wake()).
    chipguy_AMOLED206_touch &touch() { return _touch; }

    // Panel to deep standby / back.  wake() re-initializes the panel (its
    // frame memory is lost in deep standby): with the framebuffer the driver
    // sends the screen back itself; without it LVGL redraws everything.
    void sleep() { display.sleep(); }
    void wake() {
        display.wake();
        if (!display.hasFramebuffer()) lv_obj_invalidate(lv_screen_active());
    }

    // True when running tear-free (framebuffer in PSRAM, TE sync on).
    // display.syncedFrames() / lateFrames() say how well it is keeping up.
    bool tearFree() const { return display.hasFramebuffer() && display.tearSync(); }

private:
    chipguy_AMOLED206_touch _touch{board};
    bool _hasTouch = false;

    static uint32_t _tick_get(void) { return millis(); }

    // Each band into the framebuffer; the frame's last band sends the rows
    // that changed.
    static void _fb_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        display.drawRegion(area->x1, area->y1, area->x2, area->y2, (const uint16_t *)pixelmap);
        if (lv_display_flush_is_last(disp)) display.commit();
        lv_display_flush_ready(disp);
    }

    // No-PSRAM fallback: every area spans the panel's full native width (all
    // logical columns in portrait, all logical rows in landscape), rounded to
    // even rows/columns the other way, which the CO5300 requires.  Both screen
    // dimensions are even, so this never runs off the edge.
    static void _fullWidthRounder(lv_event_t *e) {
        lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
        if (display.rotation() & 1) {
            a->y1 = 0; a->y2 = display.height() - 1;
            a->x1 &= ~1; a->x2 |= 1;
        } else {
            a->x1 = 0; a->x2 = display.width() - 1;
            a->y1 &= ~1; a->y2 |= 1;
        }
    }

    static void _disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        display.flushRegion(area->x1, area->y1, area->x2, area->y2,
                            (const uint16_t *)pixelmap);
        lv_display_flush_ready(disp);
    }

    static void _touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
        auto *self = (lv_setup_class *)lv_indev_get_user_data(indev);
        uint16_t x, y;
        if (self->_touch.read(x, y)) {
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
};

static lv_setup_class lv_setup;
