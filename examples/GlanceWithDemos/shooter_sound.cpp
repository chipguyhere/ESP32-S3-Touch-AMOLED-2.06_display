/*
 * shooter_sound.cpp - see shooter_sound.h.
 */

#include "shooter_sound.h"
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// The sounds are 11025 Hz; the I2S port runs at twice that (MCLK = 256 fs =
// 5.6 MHz) and the mixer interpolates.  A rate the codec is happy with and an
// exact 2:1 step.
static const uint32_t RATE        = 22050;
static const uint32_t SOURCE_RATE = 11025;
static const int      VOICES      = 6;
static const int      FRAMES      = 256;      // per DMA buffer: ~12 ms
static const uint32_t AMP_OFF_MS  = 1000;     // quiet this long: amplifier off

// ES8311 DAC volume, register 0x32: 0xBF = 0 dB, 0.5 dB per step.  The
// NS4150B adds about 6 dB on a 3.3 V supply, so full scale from the codec
// would clip it; this starts 8 dB down.
// TODO (bench): raise until the explosions distort, then back off.
static const uint8_t SHOOTER_DAC_VOLUME = 0xAF;

struct Voice {
    const uint8_t *pcm;
    uint32_t len;
    uint32_t pos;          // 16.16 fixed point, in source samples
    int32_t  gain;         // 0..256
    uint32_t started;      // for "cut off the oldest"
    bool     active;
};

static AMOLED206_Board *g_board;
static i2s_chan_handle_t g_tx;
static TaskHandle_t g_task;
static volatile bool g_run;
static Voice g_voices[VOICES];
static uint32_t g_serial;
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

static bool codecWrite(uint8_t reg, uint8_t val) {
    return g_board->Wire.writeRegister(AMOLED206_Board::ES8311_ADDR, reg, val);
}

// Slave DAC on an external 256 fs MCLK, 16-bit I2S in: the family's sequence,
// after esp-adf's es8311.c (codec init + start, DAC path only).
static bool codecInit() {
    bool ok = true;
    ok &= codecWrite(0x00, 0x1F); delay(10);   // reset
    ok &= codecWrite(0x00, 0x00); delay(10);
    ok &= codecWrite(0x01, 0x30);              // clock manager: pre-config
    ok &= codecWrite(0x02, 0x00);              // MCLK pre-div 1, mult 1 (MCLK = 256 fs)
    ok &= codecWrite(0x03, 0x10);              // ADC OSR
    ok &= codecWrite(0x16, 0x24);
    ok &= codecWrite(0x04, 0x10);              // DAC OSR
    ok &= codecWrite(0x05, 0x00);              // ADC/DAC clock dividers 1
    ok &= codecWrite(0x0B, 0x00);
    ok &= codecWrite(0x0C, 0x00);
    ok &= codecWrite(0x10, 0x1F);
    ok &= codecWrite(0x11, 0x7F);
    ok &= codecWrite(0x00, 0x80);              // power on (CSM)
    ok &= codecWrite(0x01, 0x3F);              // all clocks on, MCLK from the MCLK pin
    ok &= codecWrite(0x06, 0x03);              // BCLK divider: MCLK/4 = 64 fs
    ok &= codecWrite(0x07, 0x00);              // LRCK divider high
    ok &= codecWrite(0x08, 0xFF);              // LRCK divider low: 256 -> fs
    ok &= codecWrite(0x09, 0x0C);              // DAC serial port: I2S, 16-bit
    ok &= codecWrite(0x0A, 0x0C);              // ADC serial port: I2S, 16-bit
    ok &= codecWrite(0x0D, 0x01);              // power up analog
    ok &= codecWrite(0x0E, 0x02);
    ok &= codecWrite(0x12, 0x00);              // DAC power on
    ok &= codecWrite(0x13, 0x10);
    ok &= codecWrite(0x1C, 0x6A);
    ok &= codecWrite(0x37, 0x08);
    ok &= codecWrite(0x45, 0x00);
    ok &= codecWrite(0x32, SHOOTER_DAC_VOLUME);
    ok &= codecWrite(0x31, 0x00);              // DAC unmute
    return ok;
}

static void mixTask(void *) {
    static int16_t buf[FRAMES * 2];
    const uint32_t step = (SOURCE_RATE << 16) / RATE;
    bool ampOn = false;
    uint32_t lastSound = 0;

    while (g_run) {
        bool any = false;
        for (int f = 0; f < FRAMES; f++) {
            int32_t mix = 0;
            portENTER_CRITICAL(&g_lock);
            for (Voice &v : g_voices) {
                if (!v.active) continue;
                const uint32_t i = v.pos >> 16;
                if (i >= v.len) { v.active = false; continue; }
                const int32_t a = (int32_t)v.pcm[i] - 128;
                const int32_t b = (i + 1 < v.len) ? (int32_t)v.pcm[i + 1] - 128 : 0;
                const int32_t frac = (v.pos >> 8) & 0xFF;
                const int32_t s = (a * (256 - frac) + b * frac);      // 8-bit sample << 8
                mix += (s * v.gain) >> 8;
                v.pos += step;
                any = true;
            }
            portEXIT_CRITICAL(&g_lock);
            if (mix > 32767) mix = 32767;
            if (mix < -32768) mix = -32768;
            buf[2 * f] = buf[2 * f + 1] = (int16_t)mix;          // mono codec: same in both slots
        }

        // Amplifier on only while there's sound (it starts on the stream of
        // zeros already running, which is the quiet way round).
        const uint32_t now = millis();
        if (any) {
            lastSound = now;
            if (!ampOn) { g_board->ampEnable(true); ampOn = true; }
        } else if (ampOn && now - lastSound > AMP_OFF_MS) {
            g_board->ampEnable(false);
            ampOn = false;
        }

        size_t written;
        i2s_channel_write(g_tx, buf, sizeof buf, &written, portMAX_DELAY);   // paced by the DMA
    }

    g_board->ampEnable(false);
    g_task = nullptr;
    vTaskDelete(nullptr);
}

bool shooterSoundBegin(AMOLED206_Board &board) {
    if (g_task) return true;
    g_board = &board;
    board.ampEnable(false);
    if (!board.Wire.probe(AMOLED206_Board::ES8311_ADDR)) return false;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num  = 4;
    chan.dma_frame_num = FRAMES;
    chan.auto_clear    = true;                    // underrun -> silence, not a repeat
    if (i2s_new_channel(&chan, &g_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t cfg = {};
    cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(RATE);   // MCLK = 256 fs
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    cfg.gpio_cfg.mclk = (gpio_num_t)AMOLED206_Board::I2S_MCLK_PIN;
    cfg.gpio_cfg.bclk = (gpio_num_t)AMOLED206_Board::I2S_BCLK_PIN;
    cfg.gpio_cfg.ws   = (gpio_num_t)AMOLED206_Board::I2S_WS_PIN;
    cfg.gpio_cfg.dout = (gpio_num_t)AMOLED206_Board::I2S_DOUT_PIN;
    cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(g_tx, &cfg) != ESP_OK || i2s_channel_enable(g_tx) != ESP_OK) {
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        return false;
    }

    // The codec wants MCLK running before it's configured: it is now.
    if (!codecInit()) {
        i2s_channel_disable(g_tx);
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        codecWrite(0x00, 0x1F);
        return false;
    }

    memset(g_voices, 0, sizeof g_voices);
    g_run = true;
    // Core 0, away from the loop() task that runs LVGL on core 1.
    xTaskCreatePinnedToCore(mixTask, "shooterSnd", 3072, nullptr, 5, &g_task, 0);
    return true;
}

void shooterSoundPlay(const shooter_sound_t &snd, uint8_t volumePct) {
    if (!g_task || volumePct == 0) return;
    portENTER_CRITICAL(&g_lock);
    Voice *pick = nullptr;
    for (Voice &v : g_voices) if (!v.active) { pick = &v; break; }
    if (!pick) {                                   // all busy: replace the oldest
        pick = &g_voices[0];
        for (Voice &v : g_voices) if ((int32_t)(v.started - pick->started) < 0) pick = &v;
    }
    pick->pcm = snd.pcm;
    pick->len = snd.len;
    pick->pos = 0;
    pick->gain = volumePct >= 100 ? 256 : volumePct * 256 / 100;
    pick->started = ++g_serial;
    pick->active = true;
    portEXIT_CRITICAL(&g_lock);
}

void shooterSoundEnd() {
    if (!g_tx) return;
    if (g_task) {
        g_run = false;                             // the task turns the amplifier off on its way out
        for (int i = 0; i < 50 && g_task; i++) delay(5);
    }
    g_board->ampEnable(false);
    i2s_channel_disable(g_tx);
    i2s_del_channel(g_tx);
    g_tx = nullptr;
    // Back to the codec's reset state.  Its "power-down" sequences were
    // measured to RAISE deep-sleep current (power library README-engineering),
    // so the defaults are the low-power state.  TODO (bench): confirm the
    // deep-sleep current after a game matches the usual floor.
    codecWrite(0x00, 0x1F);
}
