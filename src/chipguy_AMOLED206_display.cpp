/*
 * CO5300 QSPI AMOLED driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 * See chipguy_AMOLED206_display.h for the wiring and usage overview.
 */

#include "chipguy_AMOLED206_display.h"
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

// ── Board pin assignments (fixed on the ESP32-S3-Touch-AMOLED-2.06) ─────────
#define AMOLED_SIO0   AMOLED206_Board::LCD_SIO0_PIN
#define AMOLED_SIO1   AMOLED206_Board::LCD_SIO1_PIN
#define AMOLED_SIO2   AMOLED206_Board::LCD_SIO2_PIN
#define AMOLED_SIO3   AMOLED206_Board::LCD_SIO3_PIN
#define AMOLED_SCLK   AMOLED206_Board::LCD_SCLK_PIN
#define AMOLED_CS     AMOLED206_Board::LCD_CS_PIN
#define AMOLED_RESET  AMOLED206_Board::LCD_RESET_PIN

// SPI3_HOST, not SPI2: on the S3 the Arduino core's default `SPI` object is
// FSPI = SPI2_HOST, and the microSD slot is wired for SPI mode on its own pins
// (1/2/3/17).  Leaving SPI2 alone lets a sketch do SPI.begin(2, 3, 1, 17) +
// SD.begin(17) without colliding with the display.  SPI3 has the four data
// lines quad mode needs.
#define AMOLED_SPI_HOST SPI3_HOST

// The visible glass starts at column 22 of the CO5300's frame memory (the
// manufacturer's Arduino_GFX setup passes col_offset1 = 22).  No row offset.
#define AMOLED_COL_OFFSET 22

// QSPI framing used by the CO5300 (and its SH8601 relatives): a 32-bit
// "command" phase sent on one line = opcode, 0x00, register, 0x00.
//   0x02  write register, parameters on one line (1-1-1)
//   0x32  write pixels, pixel data on four lines (1-1-4)
#define AMOLED_OP_WRITE_CMD    0x02
#define AMOLED_OP_WRITE_COLOR  0x32
#define AMOLED_CMDWORD(op, reg) ((int)(((uint32_t)(op) << 24) | ((uint32_t)(reg) << 8)))

// CO5300 commands used here.
#define CO5300_SLPIN     0x10
#define CO5300_SLPOUT    0x11
#define CO5300_INVOFF    0x20
#define CO5300_DISPOFF   0x28
#define CO5300_DISPON    0x29
#define CO5300_CASET     0x2A
#define CO5300_RASET     0x2B
#define CO5300_RAMWR     0x2C
#define CO5300_TEON      0x35
#define CO5300_MADCTL    0x36
#define CO5300_COLMOD    0x3A
#define CO5300_WRDISBV   0x51   // brightness, normal mode
#define CO5300_WRCTRLD   0x53   // brightness control on/off
#define CO5300_WRCE      0x58   // contrast enhancement
#define CO5300_WRHBMDISBV 0x63  // brightness ceiling in high-brightness mode
#define CO5300_SPIMODECTL 0xC4
#define CO5300_CMDPAGE   0xFE
#define CO5300_DSTBY     0x4F   // deep standby mode

chipguy_AMOLED206_display::chipguy_AMOLED206_display(AMOLED206_Board &board)
    : _board(board) {}

bool IRAM_ATTR chipguy_AMOLED206_display::_onColorDone(esp_lcd_panel_io_handle_t,
                                                       esp_lcd_panel_io_event_data_t *,
                                                       void *user_ctx) {
    auto *self = (chipguy_AMOLED206_display *)user_ctx;
    self->_teAtDone = self->_teCount;      // for the tear-sync late-frame check
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->_doneSem, &woken);
    return woken == pdTRUE;
}

// Collect completion signals for transfers that are known to be finished.
// Called right after a tx_param, which esp_lcd only issues once every queued
// colour transfer has completed, so each take succeeds immediately.
void chipguy_AMOLED206_display::_reap() {
    while (_outstanding > 0 && xSemaphoreTake(_doneSem, 0) == pdTRUE) _outstanding--;
}

void chipguy_AMOLED206_display::waitFlush() {
    while (_outstanding > 0) {
        xSemaphoreTake(_doneSem, portMAX_DELAY);
        _outstanding--;
    }
}

void chipguy_AMOLED206_display::_cmd(uint8_t cmd, const uint8_t *params, size_t len) {
    esp_lcd_panel_io_tx_param(_io, AMOLED_CMDWORD(AMOLED_OP_WRITE_CMD, cmd), params, len);
    _reap();
}

void chipguy_AMOLED206_display::writeCommand(uint8_t cmd, const uint8_t *params, size_t len) {
    if (!_io) return;
    waitFlush();
    _cmd(cmd, params, len);
}

bool chipguy_AMOLED206_display::begin(uint16_t rotation) {
    _board.begin();

    // The module's power enable (DSI_PWR_EN) is pulled up to ALDO2.  The
    // AXP2101's power-on defaults appear to have it on already (the
    // manufacturer's examples never touch it), but don't rely on that.
    _board.panelPower(true);

    if (!_doneSem) _doneSem = xSemaphoreCreateCounting(8, 0);
    if (!_doneSem) return false;

    // Two DMA buffers: the conversion of region N+1 overlaps the DMA of N.
    for (int i = 0; i < 2; i++) {
        if (!_dmaBuf[i]) {
            _dmaBuf[i] = (uint16_t *)heap_caps_malloc(AMOLED206_FLUSH_MAX_PIXELS * 2,
                                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        }
        if (!_dmaBuf[i]) return false;
    }

    if (!_io) {
        spi_bus_config_t bus = {};
        bus.data0_io_num = AMOLED_SIO0;
        bus.data1_io_num = AMOLED_SIO1;
        bus.data2_io_num = AMOLED_SIO2;
        bus.data3_io_num = AMOLED_SIO3;
        bus.data4_io_num = -1;
        bus.data5_io_num = -1;
        bus.data6_io_num = -1;
        bus.data7_io_num = -1;
        bus.sclk_io_num  = AMOLED_SCLK;
        // The S3's SPI caps one transaction at 32 KB; esp_lcd splits larger
        // colour writes into chunks of this size with CS held between them.
        bus.max_transfer_sz = 32 * 1024;
        bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
        if (spi_bus_initialize(AMOLED_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

        esp_lcd_panel_io_spi_config_t io = {};
        io.cs_gpio_num = AMOLED_CS;
        io.dc_gpio_num = -1;
        io.spi_mode = 0;
        io.pclk_hz = AMOLED206_QSPI_HZ;
        io.trans_queue_depth = 4;
        io.on_color_trans_done = _onColorDone;
        io.user_ctx = this;
        io.lcd_cmd_bits = 32;
        io.lcd_param_bits = 8;
        io.flags.quad_mode = 1;
        if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)AMOLED_SPI_HOST, &io, &_io) != ESP_OK) {
            _io = nullptr;
            return false;
        }
    }

    setRotation(rotation);
    _initPanel();
    return true;
}

// Hardware reset and the init sequence; leaves the panel on, cleared to
// black, at the current brightness.  Used by begin() and by wake() (deep
// standby is left only through a hardware reset).
void chipguy_AMOLED206_display::_initPanel() {
    pinMode(AMOLED_RESET, OUTPUT);
    digitalWrite(AMOLED_RESET, HIGH);
    delay(5);
    digitalWrite(AMOLED_RESET, LOW);
    delay(20);
    digitalWrite(AMOLED_RESET, HIGH);
    delay(150);

    // Init sequence: the manufacturer's (Arduino_GFX Arduino_CO5300), plus an
    // explicit MADCTL and a dark start so the panel doesn't flash garbage.
    _cmd(CO5300_SLPOUT, nullptr, 0);
    delay(120);
    _cmd8(CO5300_CMDPAGE, 0x00);        // user command set
    _cmd8(CO5300_SPIMODECTL, 0x80);     // QSPI RAM write enabled
    _cmd8(CO5300_COLMOD, 0x55);         // 16 bits/pixel
    _cmd8(CO5300_MADCTL, 0x00);         // no flips: rotation is done in software
    _cmd8(CO5300_TEON, 0x00);           // TE output on V-blank (GPIO13; tear-free mode)
    _cmd8(CO5300_WRCTRLD, 0x20);        // brightness control on
    _cmd8(CO5300_WRHBMDISBV, 0xFF);
    _cmd8(CO5300_WRCE, 0x00);           // contrast enhancement off
    _cmd(CO5300_INVOFF, nullptr, 0);
    _cmd8(CO5300_WRDISBV, 0x00);        // dark while we clear the frame memory

    fillScreen(0x0000);
    waitFlush();
    _cmd(CO5300_DISPON, nullptr, 0);
    delay(10);
    _asleep = false;              // before setBacklight(), which skips while asleep
    setBacklight(_brightPct);
}

void chipguy_AMOLED206_display::setRotation(uint16_t rotation) {
    _rotation = amoled206_normalize_rotation(rotation);
    bool landscape = _rotation & 1;
    _w = landscape ? AMOLED206_NATIVE_HEIGHT : AMOLED206_NATIVE_WIDTH;
    _h = landscape ? AMOLED206_NATIVE_WIDTH  : AMOLED206_NATIVE_HEIGHT;
}

void chipguy_AMOLED206_display::setBacklight(int percentage) {
    if (percentage < 0) percentage = 0;
    if (percentage > 100) percentage = 100;
    _brightPct = percentage;
    if (!_io || _asleep) return;
    waitFlush();
    _cmd8(CO5300_WRDISBV, (uint8_t)((percentage * 255 + 50) / 100));
}

void chipguy_AMOLED206_display::setContrastEnhance(uint8_t level) {
    static const uint8_t codes[] = { 0x00, 0x05, 0x06, 0x07 };
    if (level > 3) level = 3;
    if (!_io) return;
    waitFlush();
    _cmd8(CO5300_WRCE, codes[level]);
}

void chipguy_AMOLED206_display::_setNativeWindow(int nx1, int ny1, int nx2, int ny2) {
    uint8_t c[4] = {
        (uint8_t)((nx1 + AMOLED_COL_OFFSET) >> 8), (uint8_t)(nx1 + AMOLED_COL_OFFSET),
        (uint8_t)((nx2 + AMOLED_COL_OFFSET) >> 8), (uint8_t)(nx2 + AMOLED_COL_OFFSET) };
    uint8_t r[4] = {
        (uint8_t)(ny1 >> 8), (uint8_t)ny1, (uint8_t)(ny2 >> 8), (uint8_t)ny2 };
    // tx_param waits for any queued colour DMA before it goes out, so this is
    // also the point at which the previous flush is known complete.
    _cmd(CO5300_CASET, c, 4);
    _cmd(CO5300_RASET, r, 4);
}

void chipguy_AMOLED206_display::_pushNative(int nx1, int ny1, int nx2, int ny2,
                                            const uint16_t *buf, size_t pixels) {
    _setNativeWindow(nx1, ny1, nx2, ny2);
    esp_lcd_panel_io_tx_color(_io, AMOLED_CMDWORD(AMOLED_OP_WRITE_COLOR, CO5300_RAMWR),
                              buf, pixels * 2);
    _outstanding++;
}

static inline uint16_t swap16(uint16_t p) { return (uint16_t)((p >> 8) | (p << 8)); }

void chipguy_AMOLED206_display::flushRegion(int x1, int y1, int x2, int y2,
                                            const uint16_t *pixels) {
    if (!_io) return;
    const int w = x2 - x1 + 1;
    const int h = y2 - y1 + 1;
    const size_t n = (size_t)w * h;
    if (w <= 0 || h <= 0 || n > AMOLED206_FLUSH_MAX_PIXELS) return;

    // Convert into the DMA buffer not used by the transfer that may still be
    // in flight.  (The one before that finished when that transfer's window
    // was sent -- see _setNativeWindow.)
    uint16_t *out = _dmaBuf[_dmaNext];
    _dmaNext ^= 1;

    const int NW = AMOLED206_NATIVE_WIDTH, NH = AMOLED206_NATIVE_HEIGHT;
    int nx1, ny1, nx2, ny2;

    switch (_rotation) {
    default:
    case 0:   // native
        nx1 = x1; ny1 = y1; nx2 = x2; ny2 = y2;
        for (size_t i = 0; i < n; i++) out[i] = swap16(pixels[i]);
        break;

    case 2:   // 180: native = (NW-1-x, NH-1-y); the region reverses end to end
        nx1 = NW - 1 - x2; nx2 = NW - 1 - x1;
        ny1 = NH - 1 - y2; ny2 = NH - 1 - y1;
        for (size_t i = 0; i < n; i++) out[i] = swap16(pixels[n - 1 - i]);
        break;

    case 1:   // 90: native x = NW-1-y, native y = x
        // Native rows run over logical x; within a row, native x increases as
        // logical y decreases.
        nx1 = NW - 1 - y2; nx2 = NW - 1 - y1;
        ny1 = x1;          ny2 = x2;
        for (int c = 0; c < w; c++) {
            uint16_t *row = out + (size_t)c * h;
            const uint16_t *src = pixels + (size_t)(h - 1) * w + c;
            for (int k = 0; k < h; k++, src -= w) row[k] = swap16(*src);
        }
        break;

    case 3:   // 270: native x = y, native y = NH-1-x
        nx1 = y1;          nx2 = y2;
        ny1 = NH - 1 - x2; ny2 = NH - 1 - x1;
        for (int r = 0; r < w; r++) {
            uint16_t *row = out + (size_t)r * h;
            const uint16_t *src = pixels + (w - 1 - r);
            for (int k = 0; k < h; k++, src += w) row[k] = swap16(*src);
        }
        break;
    }

    _pushNative(nx1, ny1, nx2, ny2, out, n);
}

void chipguy_AMOLED206_display::fillScreen(uint16_t color) {
    if (!_io) return;
    const int NW = AMOLED206_NATIVE_WIDTH, NH = AMOLED206_NATIVE_HEIGHT;
    // Even number of rows per chunk (the CO5300's alignment rule).
    int rows = (AMOLED206_FLUSH_MAX_PIXELS / NW) & ~1;
    uint16_t sw = swap16(color);
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < rows * NW; k++) _dmaBuf[i][k] = sw;
    for (int y = 0; y < NH; y += rows) {
        int y2 = y + rows - 1;
        if (y2 > NH - 1) y2 = NH - 1;
        uint16_t *buf = _dmaBuf[_dmaNext];
        _dmaNext ^= 1;
        _pushNative(0, y, NW - 1, y2, buf, (size_t)NW * (y2 - y + 1));
    }
}

void chipguy_AMOLED206_display::sleep() {
    if (!_io || _asleep) return;
    waitFlush();
    _cmd(CO5300_DISPOFF, nullptr, 0);
    _cmd(CO5300_SLPIN, nullptr, 0);
    delay(120);
    // Deep standby: measured ~0.35 mA lower than sleep-in alone (2026-10-06).
    // The frame memory is lost and only a hardware reset brings the panel
    // back, which wake() does.
    _cmd8(CO5300_DSTBY, 0x01);
    _asleep = true;
}

void chipguy_AMOLED206_display::wake() {
    if (!_io || !_asleep) return;
    waitFlush();
    _initPanel();                 // reset + init: deep standby needs a reset
    if (_nfb) {                   // the panel lost its frame memory; we didn't
        invalidateAll();
        commit();
    }
}

// ── Tear-free updates ───────────────────────────────────────────────────────

void IRAM_ATTR chipguy_AMOLED206_display::_teIsr(void *arg) {
    auto *self = (chipguy_AMOLED206_display *)arg;
    self->_teCount = self->_teCount + 1;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->_teSem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

bool chipguy_AMOLED206_display::_attachTE() {
    if (_teAttached) return true;
    if (!_teSem) _teSem = xSemaphoreCreateBinary();
    if (!_teSem) return false;
    pinMode(AMOLED206_Board::LCD_TE_PIN, INPUT);
    // TE is high during vertical blank (MIPI DCS TE mode 0, set by TEON 0x00
    // in begin()); its rising edge is the start of blanking, just before the
    // scan restarts at the top row.
    attachInterruptArg(digitalPinToInterrupt(AMOLED206_Board::LCD_TE_PIN),
                       _teIsr, this, RISING);
    _teAttached = true;
    return true;
}

bool chipguy_AMOLED206_display::_waitTE(uint32_t timeoutMs) {
    xSemaphoreTake(_teSem, 0);                               // drop a stale pulse
    return xSemaphoreTake(_teSem, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

float chipguy_AMOLED206_display::measureRefreshHz(uint32_t ms) {
    if (!_attachTE()) return 0.0f;
    if (!_waitTE(100)) return 0.0f;                          // align to an edge
    uint32_t c0 = _teCount, t0 = micros();
    delay(ms);
    uint32_t c1 = _teCount, t1 = micros();
    if (c1 == c0) return 0.0f;
    return (c1 - c0) * 1e6f / (float)(t1 - t0);
}

bool chipguy_AMOLED206_display::setTearSync(bool enable) {
    if (!enable) { _teSync = false; return true; }
    if (!_attachTE() || !_waitTE(100)) { _teSync = false; return false; }
    _teSync = true;
    return true;
}

void chipguy_AMOLED206_display::flushFrameRegion(const uint16_t *fb,
                                                 int x1, int y1, int x2, int y2) {
    if (!_io || x2 < x1 || y2 < y1) return;
    const int NW = AMOLED206_NATIVE_WIDTH, NH = AMOLED206_NATIVE_HEIGHT;
    const int W = _w;                                         // framebuffer stride

    // The native rows the region covers.  Columns are always the panel's full
    // width: a narrower window leaves the CO5300's partial-width line.
    int ny1, ny2;
    switch (_rotation) {
        default:
        case 0: ny1 = y1;          ny2 = y2;          break;
        case 1: ny1 = x1;          ny2 = x2;          break;
        case 2: ny1 = NH - 1 - y2; ny2 = NH - 1 - y1; break;
        case 3: ny1 = NH - 1 - x2; ny2 = NH - 1 - x1; break;
    }
    const int nx1 = 0, nx2 = NW - 1, nw = NW;
    const int rowsPerStrip = (int)(AMOLED206_FLUSH_MAX_PIXELS / nw) & ~1;   // even

    const bool synced = _startFrame();

    for (int s = ny1; s <= ny2; s += rowsPerStrip) {
        int e = s + rowsPerStrip - 1;
        if (e > ny2) e = ny2;
        const int k = e - s + 1;
        uint16_t *out = _dmaBuf[_dmaNext];
        _dmaNext ^= 1;

        switch (_rotation) {
        default:
        case 0:   // native = logical
            for (int r = 0; r < k; r++) {
                const uint16_t *src = fb + (size_t)(s + r) * W + nx1;
                uint16_t *dst = out + (size_t)r * nw;
                for (int c = 0; c < nw; c++) dst[c] = swap16(src[c]);
            }
            break;
        case 2:   // x = NW-1-nx, y = NH-1-ny
            for (int r = 0; r < k; r++) {
                const uint16_t *src = fb + (size_t)(NH - 1 - (s + r)) * W + (NW - 1 - nx1);
                uint16_t *dst = out + (size_t)r * nw;
                for (int c = 0; c < nw; c++) dst[c] = swap16(*(src - c));
            }
            break;
        case 1:   // x = ny, y = NW-1-nx.  Walk logical rows (cache-friendly in
                  // PSRAM): each supplies one native column of the strip.
            for (int c = 0; c < nw; c++) {
                const uint16_t *src = fb + (size_t)(NW - 1 - (nx1 + c)) * W + s;
                for (int r = 0; r < k; r++) out[(size_t)r * nw + c] = swap16(src[r]);
            }
            break;
        case 3:   // x = NH-1-ny, y = nx
            for (int c = 0; c < nw; c++) {
                const uint16_t *src = fb + (size_t)(nx1 + c) * W + (NH - 1 - s);
                for (int r = 0; r < k; r++) out[(size_t)r * nw + c] = swap16(*(src - r));
            }
            break;
        }
        _pushNative(nx1, s, nx2, e, out, (size_t)nw * k);
    }

    _endFrame(synced);
}

// Make sure the previous frame is completely out, then (if syncing) wait for
// the start of the next vertical blank so this write starts at the top just
// as the scan does.  True if this frame is TE-synced.
bool chipguy_AMOLED206_display::_startFrame() {
    waitFlush();

    // Judge the previous synced frame now that its last strip is known done:
    // if a TE pulse arrived between its start and its completion, the scan
    // caught up with the write and that frame may have torn.
    if (_checkPending) {
        _checkPending = false;
        if (_teAtDone != _teAtStart) _lateFrames++;
    }

    const bool synced = _teSync && _waitTE(50);
    _teAtStart = _teCount;
    return synced;
}

void chipguy_AMOLED206_display::_endFrame(bool synced) {
    if (synced) {
        _syncedFrames++;
        _checkPending = true;          // judged at the next _startFrame()
    }
}

// ── The framebuffer path ────────────────────────────────────────────────────

bool chipguy_AMOLED206_display::beginFramebuffer() {
    if (_nfb) return true;
    const size_t n = (size_t)AMOLED206_NATIVE_WIDTH * AMOLED206_NATIVE_HEIGHT;
    _nfb = (uint16_t *)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
    if (!_nfb) return false;
    memset(_nfb, 0, n * 2);           // black, as begin() left the panel
    _dirtyY1 = AMOLED206_NATIVE_HEIGHT;
    _dirtyY2 = -1;
    return true;
}

void chipguy_AMOLED206_display::drawRegion(int x1, int y1, int x2, int y2, const uint16_t *pixels) {
    if (!_nfb) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > _w - 1) x2 = _w - 1;
    if (y2 > _h - 1) y2 = _h - 1;
    if (x2 < x1 || y2 < y1) return;
    const int NW = AMOLED206_NATIVE_WIDTH, NH = AMOLED206_NATIVE_HEIGHT;
    const int w = x2 - x1 + 1, h = y2 - y1 + 1;

    // Every case walks native rows top to bottom and each row left to right,
    // so the PSRAM writes are sequential; `pixels` (normally the LVGL band in
    // internal RAM) takes the out-of-order reads.
    int ny1, ny2;
    switch (_rotation) {
    default:
    case 0:   // native = logical
        ny1 = y1; ny2 = y2;
        for (int r = 0; r < h; r++) {
            const uint16_t *src = pixels + (size_t)r * w;
            uint16_t *dst = _nfb + (size_t)(y1 + r) * NW + x1;
            for (int c = 0; c < w; c++) dst[c] = swap16(src[c]);
        }
        break;

    case 2:   // native = (NW-1-x, NH-1-y): rows and columns both reversed
        ny1 = NH - 1 - y2; ny2 = NH - 1 - y1;
        for (int r = 0; r < h; r++) {
            const uint16_t *src = pixels + (size_t)(h - 1 - r) * w + (w - 1);
            uint16_t *dst = _nfb + (size_t)(ny1 + r) * NW + (NW - 1 - x2);
            for (int c = 0; c < w; c++) dst[c] = swap16(*(src - c));
        }
        break;

    case 1:   // native = (NW-1-y, x): a native row is a logical column, read bottom up
        ny1 = x1; ny2 = x2;
        for (int r = 0; r < w; r++) {
            const uint16_t *src = pixels + (size_t)(h - 1) * w + r;
            uint16_t *dst = _nfb + (size_t)(x1 + r) * NW + (NW - 1 - y2);
            for (int c = 0; c < h; c++, src -= w) dst[c] = swap16(*src);
        }
        break;

    case 3:   // native = (y, NH-1-x): a native row is a logical column, read top down,
              // and native rows run right to left across the logical area
        ny1 = NH - 1 - x2; ny2 = NH - 1 - x1;
        for (int r = 0; r < w; r++) {
            const uint16_t *src = pixels + (w - 1 - r);
            uint16_t *dst = _nfb + (size_t)(ny1 + r) * NW + y1;
            for (int c = 0; c < h; c++, src += w) dst[c] = swap16(*src);
        }
        break;
    }
    if (ny1 < _dirtyY1) _dirtyY1 = ny1;
    if (ny2 > _dirtyY2) _dirtyY2 = ny2;
}

void chipguy_AMOLED206_display::invalidateAll() {
    _dirtyY1 = 0;
    _dirtyY2 = AMOLED206_NATIVE_HEIGHT - 1;
}

void chipguy_AMOLED206_display::commit() {
    if (!_nfb || !_io || _asleep || _dirtyY2 < _dirtyY1) return;
    // Even start, odd end (the CO5300's rule); NH is even, so never past the end.
    const int ny1 = _dirtyY1 & ~1, ny2 = _dirtyY2 | 1;
    _dirtyY1 = AMOLED206_NATIVE_HEIGHT;
    _dirtyY2 = -1;
    _sendRows(ny1, ny2);
}

// Native rows ny1..ny2 (even-aligned) of the framebuffer, full width, as
// strips from the top.  Already in panel byte order, so each strip is one
// sequential copy from PSRAM into an internal DMA buffer (the S3's SPI DMA
// can't read PSRAM).  While strip n is on the wire, strip n+1 is copied.
void chipguy_AMOLED206_display::_sendRows(int ny1, int ny2) {
    const int NW = AMOLED206_NATIVE_WIDTH;
    const int rowsPerStrip = (int)(AMOLED206_FLUSH_MAX_PIXELS / NW) & ~1;   // even

    const bool synced = _startFrame();
    for (int s = ny1; s <= ny2; s += rowsPerStrip) {
        int e = s + rowsPerStrip - 1;
        if (e > ny2) e = ny2;
        const size_t n = (size_t)NW * (e - s + 1);
        uint16_t *out = _dmaBuf[_dmaNext];
        _dmaNext ^= 1;
        memcpy(out, _nfb + (size_t)s * NW, n * 2);
        _pushNative(0, s, NW - 1, e, out, n);
    }
    _endFrame(synced);
}
