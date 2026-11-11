/*******************************************************************************
 * GlanceWithDemos - the GlanceWatch wristwatch for the Waveshare
 * ESP32-S3-Touch-AMOLED-2.06, plus three demos on its BOOT menu.
 *
 * Everything GlanceWatch does, and: Calculator and Bouncing balls (the
 * _display library's examples, calc_ui.cpp / balls_ui.cpp) and Simple
 * Shooter (shooter_ui.cpp, a C++ translation of a MicroPython LVGL game,
 * with its pictures and sounds compiled in).  For a watch with an app of
 * your own, start from GlanceWatch instead -- this one shows how apps slot in.
 *
 *   - Raise it to look: the screen comes on with the time, date and battery,
 *     then goes dark again after a few seconds (a tap on the screen keeps it
 *     on a little longer).  If the time hasn't been set from the internet in
 *     72 hours, the face says so: check the Wi-Fi.
 *   - It sets itself from the internet (NTP) about once a day, waking up on
 *     its own with the screen off to do it, and learns how fast or slow its
 *     clock chip runs (chipguy_RealTimeClockSupport).
 *   - BOOT or PWR also show the time.  Hold BOOT for 2 s while it is showing
 *     for a menu: set the time, or open one of the demos.  "Hold for menu"
 *     shows beside BOOT while it's down.  Either button closes the menu, and
 *     PWR leaves a demo.  So that nothing left open drains the battery, the
 *     menu closes after 20 s without a touch, the calculator and balls after
 *     a minute, and the game after two minutes of playing itself.  In the
 *     game, BOOT changes the sound.
 *   - Left off the wrist, it turns itself off to spare the battery: when
 *     nothing has woken it for 12 hours and the battery is under 4.0 V, it
 *     powers off (a PWR press turns it back on).  See "Unworn power-off".
 *   - A fresh start (power on, reset, a new upload) always sets the time.
 *   - PWR is debounced, and a PWR wake with no real press behind it goes
 *     straight back to sleep: one unit's PWR line chatters by itself.
 *   - "Set the time" on the menu leads to three more (time_settings.cpp):
 *       Wi-Fi network  explains that the clock is set over Wi-Fi, and lets you
 *                      type the network's name and password on the watch
 *                      (wifi_setup.cpp); saved, and used instead of
 *                      watch_config.h's.
 *       Time zone      the US zones, or a custom fixed offset from UTC (hours
 *                      and half hours, no daylight saving); saved, and used
 *                      instead of watch_config.h's.
 *       Advanced       show the battery voltage and the clock's measured
 *                      drift on the face.  Both start off.
 *
 * Setup: put your Wi-Fi name and password, and your time zone, in
 * watch_config.h (it defaults to US Mountain time with daylight saving).
 * The "looking at it" position is fixed, not taught: the face tilted 45
 * degrees up from flat (glance pose, below).  That suits looking at it
 * standing up and lying down alike, and gives nobody a setting to get wrong.
 *
 * How the "raise to look" works: while it sleeps, the ESP32-S3's ULP
 * coprocessor reads the motion sensor a few times a second and wakes the main
 * processor only when the watch has been held in that position for
 * about half a second (AMOLED206_OrientationWatch).  It is armed with
 * "leave the position first", so a watch lying face-up on a table, which is
 * close to the looking position, does not keep waking up.
 *
 * Requires: chipguy_ESP32-S3-Touch-AMOLED-2.06_board, _display, _power and
 * _rtc, chipguy_RealTimeClockSupport, and lvgl 9.
 *
 * Arduino IDE board settings: ESP32S3 Dev Module, PSRAM "OPI PSRAM",
 * Flash Size "32MB (256Mb)", Partition Scheme "32M Flash (4.8MB APP/22MB
 * FATFS)", USB CDC On Boot "Enabled".  The USB console disappears while the
 * watch sleeps -- that's normal.
 ******************************************************************************/

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"                     // the global board, display, lv_setup
#include <AMOLED206_IMU.h>
#include <AMOLED206_OrientationWatch.h>
#include <chipguy_AMOLED206_power.h>
#include <chipguy_AMOLED206_rtc.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <sys/time.h>
#include "watch_config.h"
#include "wifi_setup.h"
#include "time_settings.h"
#include "calc_ui.h"
#include "balls_ui.h"
#include "shooter_ui.h"

AMOLED206_IMU imu(board);
AMOLED206_OrientationWatch watch(board, imu);
AMOLED206_RTC rtc(board);

static const uint32_t SHOW_MS       = 8000;        // how long a glance shows the time
static const uint32_t TAP_EXTEND_MS = 8000;
static const uint32_t MENU_MS       = 20000;       // the BOOT menu, untouched this long, closes
static const uint32_t SETUP_MS      = 60000;       // the Wi-Fi screen, untouched this long, closes
static const uint32_t APP_IDLE_MS   = 60000;       // calculator or balls untouched this long: back to the time
static const uint32_t GAME_DEMO_MS  = 120000;      // the game playing itself this long: back to the time
static const uint32_t STALE_S       = 72UL * 3600; // no sync this long: the face says to check Wi-Fi
static const uint32_t PWR_DEBOUNCE_MS = 30;        // GPIO10 must stay high this long to count as PWR
static const uint32_t PWR_WAKE_WAIT_MS = 60;       // ...and, after a PWR wake, within this long

// Set when a PWR wake turned out to be false: going back to sleep then
// mustn't restart the unworn countdown (see goToSleep()).
static bool falseWake = false;
static const uint32_t SYNC_EVERY_S  = 24UL * 3600; // NTP about once a day

// Unworn power-off.  The watch has nothing to do but tell the time, so one
// nobody has looked at for 12 hours is probably in a drawer, and a Li-ion
// cell lasts longest stored part-charged, not run flat.  The sleep timer also
// counts down to an "unworn" check: if nothing has woken the watch by then
// and the battery is under 4.0 V, it powers off.  At 4.0 V or more it sleeps
// another hour and checks again, so a stored watch ends up off just under
// 4.0 V.  12 hours lets the wearer sleep with it on; under 3.7 V the wait is
// one hour, so it doesn't go much lower.  Never on USB power: the battery
// isn't draining then.
static const uint32_t UNWORN_S     = 12UL * 3600;
static const uint32_t UNWORN_LOW_S = 3600;         // the wait under UNWORN_LOW_V, and between re-checks
static const float    UNWORN_OFF_V = 4.00f;        // unworn and under this: power off
static const float    UNWORN_LOW_V = 3.70f;

// Kept through deep sleep (not power-off or reset).  Only a timer wake leaves
// the countdown running, and then exactly lastSleepS of it has gone by --
// counted in the seconds the timer was set for, so the sleep timer's own
// inaccuracy can't make a check come round twice or be skipped.
static RTC_DATA_ATTR uint32_t unwornLeftS  = 0;    // seconds of sleep until the unworn check
static RTC_DATA_ATTR uint32_t lastSleepS   = 0;    // what the timer was last set for
static RTC_DATA_ATTR bool     timerForSync = false; // ...and whether the time sync was due then

// Three time services run by three different operators, asked at once.  A
// time is only used when at least two of them agree, because a hard set can't
// be undone: it throws away the drift measurement along with the old time.
static const char *const NTP_SERVERS[] = { "time.google.com", "time.cloudflare.com", "time.nist.gov" };
static const int      NTP_COUNT     = 3;
static const int32_t  NTP_AGREE_MS  = 1000;        // two answers this close agree
static const uint32_t NTP_WAIT_MS   = 6000;        // then give up until the next try
static const uint32_t NTP_RETRY_MS  = 2500;        // one resend to a silent server
static const uint32_t NTP_ALIGN_MS  = 60;          // the most the UI waits for a second to begin

// One server's answer, as "UTC in ms minus millis()".  Defined up here, before
// the first function, because the .ino prototype generator would otherwise
// hoist a prototype that uses it above it.
struct NtpAnswer {
    IPAddress ip;
    bool      resolved, got;
    uint8_t   tries, stratum;
    uint32_t  nonce, sentMs, rttMs;
    int64_t   offsetMs;
};

static lv_obj_t *timeLbl, *ampmLbl, *dateLbl, *battLbl, *driftLbl, *staleLbl, *statusLbl;
static lv_obj_t *pwrBox, *pwrLbl;          // the "hold to power off" overlay
static bool pwrOverlay;                    // ...is showing
static lv_obj_t *bootHint;                 // "Hold for menu", while BOOT is down

// Where the two buttons are, as y offsets from the middle of the 502 px
// screen: both are on the right edge, BOOT level with the date and PWR level
// with the gap above the drift line -- about 125 px from the top and 117 px
// from the bottom.  Their hints are drawn beside them.
static const int BOOT_Y = -126;
static const int PWR_Y  =  134;
static lv_obj_t *menuBox;                  // the BOOT long-press menu
static lv_obj_t *faceScr;                  // the watch face's screen
static lv_obj_t *appScr;                   // an open app's screen, or nullptr
static uint32_t  appTouched;               // millis() of the app's last touch
static uint32_t (*appIdleMs)();            // how long the open app has been idle
static uint32_t  appIdleLimit;             // ... and how long it may be

// What the menu asked for.  Set from the LVGL click handler, acted on in
// loop(): the actions run lv_timer_handler() themselves, which mustn't happen
// from inside an LVGL event.
enum MenuAction { MENU_NONE, MENU_SET_TIME, MENU_CALC, MENU_BALLS, MENU_SHOOTER };
static volatile MenuAction menuAction = MENU_NONE;

// How long PWR must be held for the PMIC to cut power: AXP2101 reg 0x27
// bits 3:2 = 4/6/8/10 s (6 s at its power-on default, measured).
static int powerOffHoldSeconds() {
    uint8_t v = 0;
    if (!power.readReg(0x27, v)) return 6;
    static const int s[4] = { 4, 6, 8, 10 };
    return s[(v >> 2) & 3];
}

// ── NTP ─────────────────────────────────────────────────────────────────────

// Kept in NVS, and read once: the face asks twice a second.
static uint32_t lastSyncCache;
static bool     lastSyncLoaded;

static uint32_t lastSyncUtc() {
    if (!lastSyncLoaded) {
        Preferences p; p.begin("glancewatch", true);
        lastSyncCache = p.getUInt("lastSync", 0);
        p.end();
        lastSyncLoaded = true;
    }
    return lastSyncCache;
}

static void setLastSyncUtc(uint32_t t) {
    Preferences p; p.begin("glancewatch", false);
    p.putUInt("lastSync", t);
    p.end();
    lastSyncCache = t;
    lastSyncLoaded = true;
}

static void status(const char *s) {
    Serial.println(s);
    if (statusLbl) { lv_label_set_text(statusLbl, s); lv_timer_handler(); }
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// An NTP timestamp (seconds since 1900 + 32-bit fraction) as unix ms.
static int64_t ntpToUnixMs(const uint8_t *p) {
    uint32_t sec = be32(p), frac = be32(p + 4);
    int64_t s = (int64_t)sec + ((sec & 0x80000000u) ? 0 : 0x100000000LL);   // era 1 begins in 2036
    return (s - 2208988800LL) * 1000 + (int64_t)(((uint64_t)frac * 1000) >> 32);
}

// Ask every server at once and wait for the answers (or NTP_WAIT_MS).  On
// success, `offsetMs` is UTC in ms minus millis(), from the two answers that
// agree most closely.  False unless at least two agree within NTP_AGREE_MS.
//
// Not the ESP32's SNTP client: it asks one server at a time, and its result
// lands in the system clock, which survives deep sleep on the chip's RC
// oscillator -- so "the system time looks plausible" is no evidence that an
// answer arrived.  That mistake once set this watch 19 minutes fast.
//
// Runs on the NTP task (below), so nothing in here may touch LVGL.
static bool ntpAgreedOffset(int64_t &offsetMs) {
    NtpAnswer a[NTP_COUNT] = {};
    int resolved = 0;
    for (int i = 0; i < NTP_COUNT; i++) {
        a[i].resolved = WiFi.hostByName(NTP_SERVERS[i], a[i].ip) == 1;
        if (a[i].resolved) resolved++;
        else Serial.printf("NTP %s: no DNS answer\n", NTP_SERVERS[i]);
    }
    if (resolved < 2) return false;

    WiFiUDP udp;
    if (!udp.begin(12123)) return false;

    auto send = [&](NtpAnswer &x) {
        uint8_t pkt[48] = { 0x23 };                  // LI 0, version 4, mode 3 (client)
        x.nonce = esp_random();                      // as our transmit time; echoed back
        memcpy(pkt + 40, &x.nonce, 4);
        udp.beginPacket(x.ip, 123);
        udp.write(pkt, sizeof pkt);
        udp.endPacket();
        x.sentMs = millis();
        x.tries++;
    };
    for (auto &x : a) if (x.resolved) send(x);

    int got = 0;
    const uint32_t t0 = millis();
    while (got < resolved && millis() - t0 < NTP_WAIT_MS) {
        int n = udp.parsePacket();
        if (n <= 0) {
            for (auto &x : a)
                if (x.resolved && !x.got && x.tries == 1 && millis() - x.sentMs > NTP_RETRY_MS) send(x);
            delay(2);
            continue;
        }
        const uint32_t rx = millis();
        uint8_t r[48];
        if (n < 48 || udp.read(r, sizeof r) != 48) { udp.flush(); continue; }
        const IPAddress from = udp.remoteIP();
        udp.flush();

        for (auto &x : a) {
            // Our nonce must come back as the originate timestamp: that both
            // matches the reply to the request and rejects stale or forged ones.
            if (x.got || x.ip != from || memcmp(r + 24, &x.nonce, 4) != 0) continue;
            const uint8_t li = r[0] >> 6, mode = r[0] & 7, stratum = r[1];
            if (mode != 4 || li == 3 || stratum == 0 || stratum > 15) {
                Serial.printf("NTP %s: unsynchronized reply, ignored\n", from.toString().c_str());
                break;
            }
            const int64_t t2 = ntpToUnixMs(r + 32), t3 = ntpToUnixMs(r + 40);
            int64_t rtt = (int64_t)(rx - x.sentMs) - (t3 - t2);
            if (rtt < 0) rtt = 0;
            x.rttMs    = (uint32_t)rtt;
            x.stratum  = stratum;
            x.offsetMs = t3 + rtt / 2 - (int64_t)rx;
            x.got      = true;
            got++;
            break;
        }
    }
    udp.stop();

    // The closest-agreeing pair decides.
    int bi = -1, bj = -1;
    int64_t best = INT64_MAX;
    for (int i = 0; i < NTP_COUNT; i++)
        for (int j = i + 1; j < NTP_COUNT; j++)
            if (a[i].got && a[j].got && llabs(a[i].offsetMs - a[j].offsetMs) < best) {
                best = llabs(a[i].offsetMs - a[j].offsetMs);
                bi = i; bj = j;
            }
    const bool ok = bi >= 0 && best <= NTP_AGREE_MS;
    if (ok) offsetMs = (a[bi].offsetMs + a[bj].offsetMs) / 2;

    for (int i = 0; i < NTP_COUNT; i++) {
        if (!a[i].resolved) continue;
        if (!a[i].got) { Serial.printf("NTP %-20s no answer\n", NTP_SERVERS[i]); continue; }
        Serial.printf("NTP %-20s stratum %u  rtt %4lu ms  %+lld ms vs %s\n", NTP_SERVERS[i],
                      a[i].stratum, (unsigned long)a[i].rttMs,
                      (long long)(a[i].offsetMs - (ok ? offsetMs : a[i].offsetMs)),
                      ok ? "agreed time" : "itself");
    }
    if (!ok) Serial.println("NTP: fewer than two servers agree -- not setting the clock");
    return ok;
}

// The sync runs in two halves.  The network half -- join Wi-Fi, ask the
// servers, leave -- takes from about a second to ~25 s, so it runs in a task
// of its own (ntpTask, on core 0 beside the Wi-Fi stack) and the face keeps
// drawing, the buttons keep working and the menu stays usable meanwhile.  It
// touches only Wi-Fi and Serial: no LVGL, no I2C, no NVS (the main task reads
// the Wi-Fi name and password for it).  It hands the agreed offset back
// through a queue, and the main task (ntpSyncPoll, from loop()) does the
// clock half -- the RTC chip, the drift sample, NVS, the status line -- since
// I2C and LVGL belong to it.  The offset is "UTC minus millis()", so it stays
// good however long it waits in the queue.
//
// While a sync is under way the watch stays awake: deep sleep would cut the
// task off mid-sync.

// What the task hands back.  (Not in any function signature, so it needn't
// sit above the first function for the .ino prototype generator.)
struct NtpResult {
    bool        ok;
    int64_t     offsetMs;
    const char *why;                         // a string literal, when !ok
};

static QueueHandle_t ntpQueue;               // the task's one answer
static bool  ntpBusy;                        // a sync is under way (main task only)
static bool  ntpAnnounce;                    // say "Time set" when it succeeds
static char  ntpSsid[33], ntpPass[65];       // copied in by the main task

// The network half.  On success, `offsetMs` is UTC in ms minus millis().
static bool ntpFetch(const char *ssid, const char *pass, int64_t &offsetMs, const char *&why) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
    bool ok = false;
    why = "Couldn't reach Wi-Fi";
    if (WiFi.status() == WL_CONNECTED) {
        why = "Time servers didn't agree";
        ok = ntpAgreedOffset(offsetMs);
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return ok;
}

static void ntpTask(void *) {
    NtpResult r = {};
    r.ok = ntpFetch(ntpSsid, ntpPass, r.offsetMs, r.why);
    xQueueSend(ntpQueue, &r, portMAX_DELAY);
    vTaskDelete(nullptr);
}

// The clock half, on the main task, called the instant a second begins so the
// whole-second value is right to within a few ms: hand the time to the
// support layer (which decides whether to set the chip or keep it as a drift
// sample) and remember when.
static bool ntpApply(int64_t offsetMs) {
    const chipguy_rtc_time_t utc = ((int64_t)millis() + offsetMs) / 1000;

    chipguy_rtc_time_t before;
    if (rtcSupport.getUTC(before))
        Serial.printf("NTP: watch was %+ld s off\n", (long)(before - utc));

    chipguy_RTCSetResult r = rtcSupport.setUTC(utc);
    static const char *names[] = { "rejected", "set", "sampled", "committed", "ignored", "unavailable" };
    Serial.printf("NTP %ld -> setUTC: %s\n", (long)utc, names[r]);
    const bool ok = (r != CHIPGUY_RTC_SET_REJECTED && r != CHIPGUY_RTC_SET_UNAVAILABLE);
    if (ok) setLastSyncUtc((uint32_t)utc);
    return ok;
}

// Start a sync in the background.  If one is already running, it carries on
// (and says "Time set" at the end if `announce` asks for it).
static void ntpSyncStart(bool announce) {
    if (ntpBusy) { ntpAnnounce |= announce; status("Setting the time..."); return; }
    if (!ntpQueue) ntpQueue = xQueueCreate(1, sizeof(NtpResult));
    strlcpy(ntpSsid, wifiSsid().c_str(), sizeof ntpSsid);
    strlcpy(ntpPass, wifiPassword().c_str(), sizeof ntpPass);
    if (!ntpQueue || xTaskCreatePinnedToCore(ntpTask, "ntp", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        status("Couldn't start the time sync");
        return;
    }
    ntpBusy = true;
    ntpAnnounce = announce;
    status("Setting the time...");
}

static void updateFace();                    // defined with the face, below

// From loop(): finish a sync whose answer is in.  True while one is still
// under way.  It sets the clock only within NTP_ALIGN_MS of a second
// beginning and waits out that little, so it never holds the UI longer.
static bool ntpSyncPoll() {
    static NtpResult r;
    static bool have = false;
    if (!ntpBusy) return false;
    if (!have) {
        if (xQueueReceive(ntpQueue, &r, 0) != pdTRUE) return true;
        have = true;
    }
    bool set = false;
    if (r.ok) {
        const uint32_t toNext = (1000 - (uint32_t)(((int64_t)millis() + r.offsetMs) % 1000)) % 1000;
        if (toNext > NTP_ALIGN_MS) return true;
        delay(toNext);
        set = ntpApply(r.offsetMs);
    }
    have = false;
    ntpBusy = false;
    status(set ? (ntpAnnounce ? "Time set" : "") : r.ok ? "Couldn't set the clock" : r.why);
    if (timeLbl) updateFace();
    return false;
}

// The timer wake: screen off and nothing else to do, so wait for it.
static void ntpSyncNow() {
    ntpSyncStart(false);
    while (ntpSyncPoll()) delay(5);
}

static bool syncDue() {
    chipguy_rtc_time_t utc;
    if (!rtcSupport.getUTC(utc)) return true;          // don't know the time at all
    uint32_t last = lastSyncUtc();
    return last == 0 || (uint32_t)utc - last >= SYNC_EVERY_S;
}

// ── The face ────────────────────────────────────────────────────────────────

static void buildFace() {
    lv_obj_t *scr = faceScr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    auto label = [&](const lv_font_t *f, lv_color_t c, int y) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, c, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
        lv_label_set_text(l, "");
        return l;
    };
    // Everything sits in the middle: the glass has ~115 px rounded corners.
    dateLbl   = label(&lv_font_montserrat_28, lv_color_make(180, 180, 180), -120);
    timeLbl   = label(&lv_font_montserrat_48, lv_color_white(), -40);
    lv_obj_set_style_transform_scale(timeLbl, 512, 0);       // 2x: ~96 px digits
    ampmLbl   = label(&lv_font_montserrat_24, lv_color_make(180, 180, 180), 40);
    battLbl   = label(&lv_font_montserrat_22, lv_color_make(120, 220, 120), 110);
    // These three stack below the battery, closing up when one is off
    // (updateFace() places them).
    staleLbl  = label(&lv_font_montserrat_18, lv_color_make(255, 120, 80), 150);
    driftLbl  = label(&lv_font_montserrat_18, lv_color_make(140, 140, 220), 186);
    statusLbl = label(&lv_font_montserrat_18, lv_color_make(230, 180, 60), 222);

    // Shown while PWR is held: a small floating box with a red border over
    // the face (this panel is very bright -- a full red screen is alarming).
    // Beside BOOT while it's held.  The date it sits next to is centered and
    // short enough ("Wed, Sep 30" at most) not to reach it.
    bootHint = lv_label_create(scr);
    lv_obj_set_style_text_font(bootHint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bootHint, lv_color_make(230, 180, 60), 0);
    lv_obj_set_style_text_align(bootHint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(bootHint, "Hold for\nmenu " LV_SYMBOL_RIGHT);
    lv_obj_align(bootHint, LV_ALIGN_RIGHT_MID, -10, BOOT_Y);
    lv_obj_add_flag(bootHint, LV_OBJ_FLAG_HIDDEN);

    // Beside PWR while it's held.  It would cover the right ends of the
    // battery line and the one below it, so those are hidden while it shows.
    pwrBox = lv_obj_create(scr);
    lv_obj_remove_style_all(pwrBox);
    lv_obj_set_size(pwrBox, 176, 72);
    lv_obj_align(pwrBox, LV_ALIGN_RIGHT_MID, -6, PWR_Y);
    lv_obj_set_style_bg_color(pwrBox, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pwrBox, LV_OPA_90, 0);
    lv_obj_set_style_border_color(pwrBox, lv_color_make(200, 30, 30), 0);
    lv_obj_set_style_border_width(pwrBox, 3, 0);
    lv_obj_set_style_radius(pwrBox, 14, 0);
    pwrLbl = lv_label_create(pwrBox);
    lv_obj_set_style_text_font(pwrLbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pwrLbl, lv_color_make(220, 220, 220), 0);
    lv_obj_set_style_text_align(pwrLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(pwrLbl);
    lv_obj_add_flag(pwrBox, LV_OBJ_FLAG_HIDDEN);

    // The BOOT menu: full screen, over everything, big touch targets.
    menuBox = lv_obj_create(scr);
    lv_obj_remove_style_all(menuBox);
    lv_obj_set_size(menuBox, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(menuBox, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(menuBox, LV_OPA_COVER, 0);
    lv_obj_remove_flag(menuBox, LV_OBJ_FLAG_SCROLLABLE);
    auto button = [](const char *text, MenuAction action, int y) {
        lv_obj_t *b = lv_button_create(menuBox);
        lv_obj_set_size(b, 300, 72);
        lv_obj_align(b, LV_ALIGN_CENTER, 0, y);
        lv_obj_set_style_radius(b, 24, 0);
        lv_obj_set_style_bg_color(b, lv_color_make(45, 45, 70), 0);
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            menuAction = (MenuAction)(intptr_t)lv_event_get_user_data(e);
        }, LV_EVENT_CLICKED, (void *)(intptr_t)action);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, text);
        lv_obj_center(l);
    };
    // No Close button: BOOT or PWR close the menu.
    button("Set the time",      MENU_SET_TIME, -129);
    button("Calculator",        MENU_CALC,    -43);
    button("Bouncing balls",    MENU_BALLS,    43);
    button("Simple Shooter",    MENU_SHOOTER, 129);
    lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);

    settingsBuild(scr);
    wifiSetupBuild(scr, WATCH_WIFI_SSID, WATCH_WIFI_PASSWORD);
}

// While PWR is held, count down to the PMIC's hardware power-off.  Returns
// true while the button is down.  `heldSince` is when we first saw it.
static void updateFace();

static bool powerButtonOverlay(bool down, uint32_t &heldSince) {
    if (!down) {
        if (heldSince) {
            heldSince = 0;
            pwrOverlay = false;
            lv_obj_add_flag(pwrBox, LV_OBJ_FLAG_HIDDEN);
            updateFace();                    // puts back what the box covered
        }
        return false;
    }
    if (!heldSince) {
        heldSince = millis();
        pwrOverlay = true;
        lv_obj_remove_flag(pwrBox, LV_OBJ_FLAG_HIDDEN);
        updateFace();
    }
    static int total = 0;
    if (!total) total = powerOffHoldSeconds();
    int left = total - (int)((millis() - heldSince) / 1000);
    if (left < 1) left = 1;
    lv_label_set_text_fmt(pwrLbl, "Hold %ds\nto power off", left);
    return true;
}

static void updateFace() {
    chipguy_rtc_time_t utc;
    if (rtcSupport.getUTC(utc)) {
        chipguy_rtc_time_t local = settingsToLocal(utc);
        chipguy_RTCDateTime dt;
        chipguy_rtcBreakDown(local, dt);
        int h = dt.hour;
#if WATCH_12_HOUR
        const char *ampm = h < 12 ? "AM" : "PM";
        h = h % 12; if (h == 0) h = 12;
        lv_label_set_text_fmt(timeLbl, "%d:%02d", h, dt.minute);
        lv_label_set_text_fmt(ampmLbl, "%s  %s", ampm, settingsZoneAbbrev(utc));
#else
        lv_label_set_text_fmt(timeLbl, "%02d:%02d", h, dt.minute);
        lv_label_set_text(ampmLbl, settingsZoneAbbrev(utc));
#endif
        lv_label_set_text_fmt(dateLbl, "%s, %s %d", chipguy_rtcWeekdayName(chipguy_rtcWeekday(local), true),
                              chipguy_rtcMonthName(dt.month, true), dt.day);
    } else {
        lv_label_set_text(timeLbl, "--:--");
        lv_label_set_text(ampmLbl, "");
        lv_label_set_text(dateLbl, "time not set");
    }

    // (LVGL's own printf has no %f, so floats go through snprintf.)
    // The voltage only when "Advanced" asks for it -- or when the fuel gauge
    // has no percentage to give.
    char buf[64];
    if (power.batteryPresent()) {
        int pct = power.getBatteryPercent();
        const char *chg = power.isCharging() ? "  charging" : power.usbPowerPresent() ? "  USB" : "";
        if (pct < 0)                    snprintf(buf, sizeof buf, "%.2f V%s", power.getBatteryVoltage(), chg);
        else if (settingsShowVoltage()) snprintf(buf, sizeof buf, "%d%%  %.2f V%s", pct, power.getBatteryVoltage(), chg);
        else                            snprintf(buf, sizeof buf, "%d%%%s", pct, chg);
        lv_label_set_text(battLbl, buf);
    } else {
        lv_label_set_text(battLbl, power.usbPowerPresent() ? "USB power, no battery" : "");
    }

    // A hard set (the first NTP sync) starts calibration at zero; only a
    // committed sample is a measurement, so say "learning" until then.
    if (rtcSupport.haveCalibration() && rtcSupport.commitCount() > 0) {
        snprintf(buf, sizeof buf, "clock drift %+.2f s/day", rtcSupport.driftSecondsPerDay());
        lv_label_set_text(driftLbl, buf);
    } else
        lv_label_set_text_fmt(driftLbl, "clock drift: learning (%lu samples)",
                              (unsigned long)rtcSupport.totalSamplesSeen());

    // Not set from the internet for 72 hours: shown whatever "Advanced" says.
    const uint32_t last = lastSyncUtc();
    const bool known = rtcSupport.getUTC(utc);
    const bool stale = !known || last == 0 || utc - (chipguy_rtc_time_t)last >= STALE_S;
    lv_label_set_text(staleLbl, last == 0 ? LV_SYMBOL_WARNING " Time never set: check Wi-Fi"
                                          : LV_SYMBOL_WARNING " Time not set in 72 hours: check Wi-Fi");

    // Below the battery: the warning, the drift, the status, closed up.
    // The PWR box covers the battery line and the one below it, so while it
    // shows only the status is left.
    if (pwrOverlay) lv_obj_add_flag(battLbl, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_remove_flag(battLbl, LV_OBJ_FLAG_HIDDEN);
    int y = 150;
    auto place = [&](lv_obj_t *l, bool show) {
        if (!show) { lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); return; }
        lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
        y += 36;
    };
    place(staleLbl, stale && !pwrOverlay);
    place(driftLbl, settingsShowDrift() && !pwrOverlay);
    place(statusLbl, true);
}

// ── Apps ────────────────────────────────────────────────────────────────────

// Idle for the calculator and the balls: time since the last touch.
static uint32_t touchIdleMs() { return millis() - appTouched; }

// An app builds its own screen; the face's screen stays as it was underneath.
// It closes after `idleLimit` ms of whatever `idleMs` measures.
static void openApp(lv_obj_t *(*create)(void), uint32_t (*idleMs)(), uint32_t idleLimit) {
    appScr = create();
    lv_screen_load(appScr);
    appTouched = millis();
    appIdleMs = idleMs;
    appIdleLimit = idleLimit;
}

static void closeApp() {
    lv_screen_load(faceScr);
    lv_obj_delete(appScr);                  // and with it the app's widgets and timers
    appScr = nullptr;
    updateFace();
}

// ── The glance pose ─────────────────────────────────────────────────────────

// The watch face tilted 45 degrees up from flat, toward the board's +X axis,
// +/-350 mg on every axis (roughly 25..65 degrees).  In the board's axes
// face-up is -Z, so that's X = +sin 45 g, Y = 0, Z = -cos 45 g.  Taken from
// the pose taught on the bench unit (X +0.73, Y +0.05, Z -0.70 g: 46 degrees).
static const int GLANCE_MG = 707;
static const int GLANCE_TOLERANCE_MG = 350;

static void setGlancePose() {
    watch.setWindow(GLANCE_MG - GLANCE_TOLERANCE_MG, GLANCE_MG + GLANCE_TOLERANCE_MG,
                    -GLANCE_TOLERANCE_MG, GLANCE_TOLERANCE_MG,
                    -GLANCE_MG - GLANCE_TOLERANCE_MG, -GLANCE_MG + GLANCE_TOLERANCE_MG);
}

// ── The PWR button, debounced ──────────────────────────────────────────────

// GPIO10 must stay high PWR_DEBOUNCE_MS before it counts as PWR.  One unit
// here read bursts of 1-2 ms "presses" that nobody made, a few ms apart,
// waking it and popping up the power-off box by themselves; a real press
// lasts 100 ms or more, so the filter costs nothing on a good unit.
static bool pwrPressed() {
    static uint32_t highSince = 0;
    if (!board.pwrButtonPressed()) { highSince = 0; return false; }
    if (!highSince) highSince = millis() | 1;
    return millis() - highSince >= PWR_DEBOUNCE_MS;
}

// After a PWR wake: was there a real press?  Yes if the PMIC latched one of
// its own PWR events -- a press edge, a short or a long press (0x49 b1, b3,
// b2) -- which it does only for a press long enough to pass its debounce, or
// if GPIO10 is still held for PWR_DEBOUNCE_MS within PWR_WAKE_WAIT_MS.
static bool realPwrWake(uint8_t keyEvents) {
    if (keyEvents & 0x0E) return true;
    const uint32_t t0 = millis();
    while (millis() - t0 < PWR_WAKE_WAIT_MS + PWR_DEBOUNCE_MS) {
        if (pwrPressed()) return true;
        if (millis() - t0 > PWR_WAKE_WAIT_MS && !board.pwrButtonPressed()) break;
        delay(1);
    }
    return false;
}

// ── Sleep ───────────────────────────────────────────────────────────────────

// On a timer wake: take the sleep just finished off the unworn countdown,
// and if it has run out, power off or put the next check an hour away.
// Doesn't return if it powers off.
static void unwornCheck() {
    unwornLeftS = unwornLeftS > lastSleepS ? unwornLeftS - lastSleepS : 0;
    if (unwornLeftS) return;
    const float v = power.getBatteryVoltage();
    if (power.batteryPresent() && !power.usbPowerPresent() && v < UNWORN_OFF_V) {
        Serial.printf("Unworn, battery %.2f V: powering off.\n", v);
        power.powerOff();
    }
    Serial.printf("Unworn, battery %.2f V: checking again in an hour.\n", v);
    unwornLeftS = UNWORN_LOW_S;
}

static void goToSleep() {
    Serial.println("Sleeping until a glance, BOOT, PWR, the daily time sync, or the unworn check.");
    Serial.flush();
    while (board.pwrButtonPressed() || board.bootButtonPressed()) delay(10);
    // Wake for the next sync when it's due; the glance wakes it sooner.
    uint32_t sleepS = SYNC_EVERY_S;
    chipguy_rtc_time_t utc;
    if (rtcSupport.getUTC(utc) && lastSyncUtc()) {
        uint32_t since = (uint32_t)utc - lastSyncUtc();
        sleepS = since >= SYNC_EVERY_S ? 3600 : SYNC_EVERY_S - since;   // failed sync: retry hourly
    }
    // A glance, a button or a fresh start restarts the unworn countdown; a
    // timer wake leaves it running, and so does a false PWR wake (the part of
    // the sleep it cut short is lost, so the check comes a little late).
    if (power.wakeReason() != AMOLED206_Wake::Timer && !falseWake)
        unwornLeftS = power.batteryPresent() && power.getBatteryVoltage() < UNWORN_LOW_V ? UNWORN_LOW_S : UNWORN_S;
    timerForSync = sleepS <= unwornLeftS;
    if (sleepS > unwornLeftS) sleepS = unwornLeftS;
    lastSleepS = sleepS;
    Serial.printf("Timer: %lu s (%s)\n", (unsigned long)sleepS, timerForSync ? "time sync" : "unworn check");
    lv_setup.sleep();                        // panel to deep standby (a no-op on the timer path)
    power.prepareForSleep();                 // while I2C is still ours
    bool armed = watch.arm(true);
    Serial.printf("orientation watch %s (ULP program %d of 121 words)\n",
                  armed ? "armed" : "FAILED to arm", AMOLED206_OrientationWatch::programWords());
    power.deepSleep(sleepS);
}

// ── Main ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    board.begin();                           // first: takes the bus back from the ULP after a wake
    uint8_t keyEvents = 0;                   // the PMIC's own PWR events, before power.begin() clears them
    board.pmicRead(0x49, keyEvents);
    power.begin(board);
    rtcSupport.setSanityFloor(CHIPGUY_RTC_APP_SANITY_FLOOR);
    if (!rtcSupport.begin(&rtc)) Serial.println("RTC not answering");
    setGlancePose();

    AMOLED206_Wake why = power.wakeReason();

    // A PWR wake nobody made: straight back to sleep, screen never on.
    if (why == AMOLED206_Wake::PwrButton && !realPwrWake(keyEvents)) {
        Serial.println("PWR wake with no real press: back to sleep.");
        falseWake = true;
        goToSleep();
    }

    // A timer wake: the unworn check and/or the daily sync, whichever the
    // timer was set for, with the screen off, then straight back to sleep.
    if (why == AMOLED206_Wake::Timer) {
        unwornCheck();                       // doesn't return if it powers off
        if (timerForSync) ntpSyncNow();
        goToSleep();
    }

    settingsBegin(WATCH_TIME_ZONE);
    lv_setup.begin();
    buildFace();
    updateFace();
    lv_timer_handler();

    // A fresh start syncs whether or not one is due: after an upload or a
    // reset is exactly when you want to see the time come right.
    if (why == AMOLED206_Wake::PowerOn || syncDue()) ntpSyncStart(false);   // in the background
}

void loop() {
    static uint32_t until = millis() + SHOW_MS;
    static uint32_t lastUpdate = 0;
    static uint32_t bootSince = 0;
    static uint32_t pwrSince = 0;
    static uint32_t menuTouched = 0;
    static uint32_t setupTouched = 0;
    // The button press in progress has already done its job (opened the
    // menu, closed it, left an app): ignore both buttons until both are up,
    // so the same press doesn't also start a hint or a power-off countdown.
    // (Holding PWR still powers off -- that's the PMIC, not us.)
    static bool latched = false;

    const bool bootDown = board.bootButtonPressed();
    const bool pwrDown  = pwrPressed();
    lv_indev_t *in = lv_indev_get_next(nullptr);
    const bool touched = in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED;

    if (latched && !bootDown && !pwrDown) latched = false;

    // An app runs until PWR, or until it has been idle too long.
    if (appScr) {
        if (touched) appTouched = millis();
        if (pwrDown && !latched) {
            closeApp();
            latched = true;
            until = millis() + SHOW_MS;
        } else if (appIdleMs() > appIdleLimit) {
            closeApp();
            until = millis() + SHOW_MS;
        } else {
            ntpSyncPoll();                   // a sync finishes even with an app open
            lv_timer_handler();
            delay(5);
            return;
        }
    }

    if (!lv_obj_has_flag(menuBox, LV_OBJ_FLAG_HIDDEN)) {
        // The menu is up: either button closes it, and so does 20 s
        // without a touch.  Until then it holds the watch awake.
        if (touched) menuTouched = millis();
        if (!latched && (bootDown || pwrDown)) {
            lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
            latched = true;
        } else if (millis() - menuTouched > MENU_MS) {
            lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
        }
        until = millis() + SHOW_MS;
    } else if (settingsVisible()) {
        // "Set the time" and the screens behind it: the same as the menu.
        if (touched) menuTouched = millis();
        if (!latched && (bootDown || pwrDown)) {
            settingsHide();
            latched = true;
        } else if (millis() - menuTouched > MENU_MS) {
            settingsHide();
        }
        until = millis() + SHOW_MS;
    } else if (wifiSetupVisible()) {
        // The Wi-Fi screen: the same, with longer to type.
        if (touched) setupTouched = millis();
        if (!latched && (bootDown || pwrDown)) {
            wifiSetupHide();
            latched = true;
        } else if (millis() - setupTouched > SETUP_MS) {
            wifiSetupHide();
        }
        until = millis() + SHOW_MS;
    } else if (!latched) {
        // BOOT: "Hold for menu" while it's down, and the menu at 2 s.
        if (bootDown) {
            if (!bootSince) {
                bootSince = millis();
                lv_obj_remove_flag(bootHint, LV_OBJ_FLAG_HIDDEN);
            }
            if (millis() - bootSince > 2000) {
                bootSince = 0;
                lv_obj_add_flag(bootHint, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
                menuTouched = millis();
                latched = true;
            }
            until = millis() + SHOW_MS;
        } else if (bootSince) {
            bootSince = 0;
            lv_obj_add_flag(bootHint, LV_OBJ_FLAG_HIDDEN);
        }
        if (powerButtonOverlay(pwrDown, pwrSince)) until = millis() + SHOW_MS;   // stay awake while PWR is down
    }
    if (latched) until = millis() + SHOW_MS;

    lv_timer_handler();
    if (millis() - lastUpdate >= 500) { lastUpdate = millis(); updateFace(); }

    // a tap keeps it on a little longer (never shorter than it already was)
    if (touched && (int32_t)(millis() + TAP_EXTEND_MS - until) > 0) until = millis() + TAP_EXTEND_MS;

    MenuAction act = menuAction;
    if (act != MENU_NONE) {
        menuAction = MENU_NONE;
        lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
        if (act == MENU_SET_TIME) {
            settingsShowMenu();
            menuTouched = millis();
        } else if (act == MENU_CALC) {
            openApp(calc_create, touchIdleMs, APP_IDLE_MS);
        } else if (act == MENU_BALLS) {
            openApp(balls_create, touchIdleMs, APP_IDLE_MS);
        } else if (act == MENU_SHOOTER) {
            // Played with touches, so its idle is its own: time in the demo.
            openApp([] { return shooter_create(board); }, shooter_demo_ms, GAME_DEMO_MS);
        }
        until = millis() + SHOW_MS;
    }

    // "Wi-Fi network" on the "Set the time" menu.
    if (settingsWifiRequested()) {
        wifiSetupShow();
        setupTouched = millis();
        until = millis() + SHOW_MS;
    }

    // A time zone was chosen: back on the face, say so.
    if (const char *m = settingsMessage()) {
        updateFace();
        status(m);
        until = millis() + SHOW_MS;
    }

    // "Set the time now" on the Wi-Fi screen: back to the face, and sync
    // in the background.
    if (wifiSetupSubmitted()) {
        wifiSetupHide();
        ntpSyncStart(true);
        until = millis() + SHOW_MS;
    }

    // A sync under way holds the watch awake (sleep would cut it off), and
    // its result stays on the face for SHOW_MS.
    if (ntpSyncPoll()) until = millis() + SHOW_MS;

    if ((int32_t)(millis() - until) > 0) goToSleep();
    delay(5);
}
