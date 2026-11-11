/*
 * time_settings.cpp - see time_settings.h.
 *
 * Layout, on the 410 x 502 screen with its ~115 px rounded corners: titles sit
 * at the top in the middle, and everything touchable stays inside the band
 * the corners leave clear (~20 px in from the sides at y 70..430).
 */

#include "time_settings.h"
#include <Preferences.h>
#include "watch_config.h"

static const char *PREFS = "glancewatch";      // same namespace as the sketch's lastSync

// The custom offset's range, in minutes east of UTC: every zone in use
// today falls inside it (Baker Island -12:00 to Kiribati +14:00).
static const int CUSTOM_MIN = -12 * 60;
static const int CUSTOM_MAX =  14 * 60;

// The US zones offered, as chipguy_RTCTimeZones names.  Eight fill the
// 2 x 4 grid; "Custom" takes the row below.
struct ZoneChoice { const char *label, *zone; };
static const ZoneChoice ZONES[] = {
    { "Eastern",  "ET"  }, { "Central",     "CT"  },
    { "Mountain", "MT"  }, { "Arizona",     "MST" },
    { "Pacific",  "PT"  }, { "Alaska",      "AKT" },
    { "Hawaii",   "HST" }, { "Puerto Rico", "AST" },
};
static const int ZONE_COUNT = sizeof ZONES / sizeof ZONES[0];

static const lv_color_t BTN_BG    = lv_color_make(45, 45, 70);
static const lv_color_t BTN_SEL   = lv_color_make(30, 110, 60);    // the zone in use
static const lv_color_t TITLE_FG  = lv_color_make(230, 180, 60);
static const lv_color_t NOTE_FG   = lv_color_make(150, 150, 150);

// What's in use.  A custom offset is a zone name too ("UTC-3:30"), which
// chipguy_RTCTimeZones handles as a fixed offset with no daylight saving.
static String s_zone;
static bool   s_showVolt, s_showDrift;

static lv_obj_t *s_menu, *s_zones, *s_customBox, *s_adv;
static lv_obj_t *s_zoneBtn[ZONE_COUNT], *s_customBtn, *s_customBtnLbl;
static lv_obj_t *s_ofsLbl, *s_aheadLbl, *s_previewLbl;
static lv_obj_t *s_voltSw, *s_driftSw;
static int  s_edit;                            // the custom offset being edited
static bool s_wifiReq;
static char s_msg[48];
static bool s_haveMsg;

// True, with its offset, when the zone in use is a custom fixed offset
// rather than one of the named zones.
static bool customOffset(int16_t &min) {
    return !chipguy_tzFind(s_zone.c_str()) && chipguy_tzParseOffset(s_zone.c_str(), min);
}
static bool isCustom() { int16_t m; return customOffset(m); }

static void save() {
    Preferences p; p.begin(PREFS, false);
    p.putString("tz", s_zone);
    p.putBool("showVolt", s_showVolt);
    p.putBool("showDrift", s_showDrift);
    p.end();
}

static void message(const char *s) {
    snprintf(s_msg, sizeof s_msg, "%s", s);
    s_haveMsg = true;
}

void settingsBegin(const char *defaultZone) {
    Preferences p; p.begin(PREFS, true);       // fails harmlessly before anything is saved
    s_zone      = p.getString("tz", defaultZone);
    s_showVolt  = p.getBool("showVolt", false);
    s_showDrift = p.getBool("showDrift", false);
    p.end();
    if (!chipguy_tzValid(s_zone.c_str())) s_zone = defaultZone;
}

chipguy_rtc_time_t settingsToLocal(chipguy_rtc_time_t utc) {
    return chipguy_tzToLocal(s_zone.c_str(), utc);
}

// For a custom offset this is the zone name itself, e.g. "UTC-3:30".
const char *settingsZoneAbbrev(chipguy_rtc_time_t utc) {
    const char *a = chipguy_tzAbbrev(s_zone.c_str(), utc);
    return a ? a : "";
}

bool settingsShowVoltage() { return s_showVolt; }
bool settingsShowDrift()   { return s_showDrift; }

bool settingsWifiRequested() {
    const bool r = s_wifiReq;
    s_wifiReq = false;
    return r;
}

const char *settingsMessage() {
    if (!s_haveMsg) return nullptr;
    s_haveMsg = false;
    return s_msg;
}

// ── Building blocks ─────────────────────────────────────────────────────────

static lv_obj_t *panel(lv_obj_t *parent, const char *title) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(p);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, TITLE_FG, 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, x, y);
    lv_label_set_text(l, "");
    return l;
}

// A button centered at (x, y) from the middle of the screen.  Returns it;
// `lblOut` gets its label.
static lv_obj_t *button(lv_obj_t *parent, const char *text, int w, int h, int x, int y,
                        lv_event_cb_t cb, intptr_t data, lv_obj_t **lblOut = nullptr) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, BTN_BG, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)data);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (lblOut) *lblOut = l;
    return b;
}

static void show(lv_obj_t *p) {
    settingsHide();
    lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
}

static intptr_t data(lv_event_t *e) { return (intptr_t)lv_event_get_user_data(e); }

// ── Time zone ───────────────────────────────────────────────────────────────

static void showZones() {
    for (int i = 0; i < ZONE_COUNT; i++)
        lv_obj_set_style_bg_color(s_zoneBtn[i],
            !isCustom() && s_zone.equalsIgnoreCase(ZONES[i].zone) ? BTN_SEL : BTN_BG, 0);
    int16_t m;
    const bool custom = customOffset(m);
    lv_obj_set_style_bg_color(s_customBtn, custom ? BTN_SEL : BTN_BG, 0);
    if (custom) {
        char buf[16];
        chipguy_tzFormatOffset(buf, sizeof buf, m, true);
        lv_label_set_text_fmt(s_customBtnLbl, "Custom: %s", buf);
    } else
        lv_label_set_text(s_customBtnLbl, "Custom offset...");
    show(s_zones);
}

static void chooseZone(lv_event_t *e) {
    const ZoneChoice &z = ZONES[data(e)];
    s_zone = z.zone;
    save();
    const chipguy_TimeZone *tz = chipguy_tzFind(z.zone);
    char buf[48];
    snprintf(buf, sizeof buf, "Time zone: %s", tz ? tz->description : z.label);
    message(buf);
    settingsHide();
}

// ── Custom offset ───────────────────────────────────────────────────────────

static void refreshCustom() {
    char buf[48];
    chipguy_tzFormatOffset(buf, sizeof buf, s_edit, true);
    lv_label_set_text(s_ofsLbl, buf);

    // Ahead or behind, in words: the sign alone is easy to misread.
    const int m = abs(s_edit);
    if (m == 0)
        lv_label_set_text(s_aheadLbl, "the same as UTC");
    else {
        char part[24] = "";
        if (m >= 60) snprintf(part, sizeof part, "%d hour%s%s", m / 60, m >= 120 ? "s" : "", m % 60 ? " " : "");
        if (m % 60) strlcat(part, "30 min", sizeof part);
        lv_label_set_text_fmt(s_aheadLbl, "%s %s", part, s_edit > 0 ? "AHEAD of UTC" : "BEHIND UTC");
    }
    lv_obj_set_style_text_color(s_aheadLbl,
        s_edit > 0 ? lv_color_make(120, 200, 255) : s_edit < 0 ? lv_color_make(255, 160, 90) : NOTE_FG, 0);

    // What the face would show with it.
    chipguy_rtc_time_t utc;
    if (rtcSupport.getUTC(utc)) {
        const chipguy_rtc_time_t local = utc + (chipguy_rtc_time_t)s_edit * 60;
        chipguy_RTCDateTime dt;
        chipguy_rtcBreakDown(local, dt);
        const char *wd = chipguy_rtcWeekdayName(chipguy_rtcWeekday(local), true);
        const char *mo = chipguy_rtcMonthName(dt.month, true);
#if WATCH_12_HOUR
        int h = dt.hour % 12; if (h == 0) h = 12;
        snprintf(buf, sizeof buf, "%s, %s %d   %d:%02d %s", wd, mo, dt.day, h, dt.minute, dt.hour < 12 ? "AM" : "PM");
#else
        snprintf(buf, sizeof buf, "%s, %s %d   %02d:%02d", wd, mo, dt.day, dt.hour, dt.minute);
#endif
        lv_label_set_text(s_previewLbl, buf);
    } else
        lv_label_set_text(s_previewLbl, "(the time isn't set yet)");
}

static void showCustom(lv_event_t *) {
    // Start from what's in use, so a small change is a small edit.
    chipguy_rtc_time_t utc = 0;
    rtcSupport.getUTC(utc);
    s_edit = constrain(chipguy_tzOffsetSeconds(s_zone.c_str(), utc) / 60, CUSTOM_MIN, CUSTOM_MAX);
    refreshCustom();
    show(s_customBox);
}

static void nudge(lv_event_t *e) {
    s_edit = constrain(s_edit + (int)data(e), CUSTOM_MIN, CUSTOM_MAX);
    refreshCustom();
}

static void saveCustom(lv_event_t *) {
    char buf[16], msg[48];
    chipguy_tzFormatOffset(buf, sizeof buf, s_edit);
    s_zone = buf;
    save();
    chipguy_tzFormatOffset(buf, sizeof buf, s_edit, true);
    snprintf(msg, sizeof msg, "Time zone: %s, no DST", buf);
    message(msg);
    settingsHide();
}

// ── Advanced ────────────────────────────────────────────────────────────────

static void toggled(lv_event_t *e) {
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (sw == s_voltSw) s_showVolt = on; else s_showDrift = on;
    save();
}

static lv_obj_t *toggleRow(lv_obj_t *parent, const char *text, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_CENTER, -55, y);
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 96, 52);
    lv_obj_align(sw, LV_ALIGN_CENTER, 125, y);
    lv_obj_add_event_cb(sw, toggled, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}

static void showAdvanced() {
    if (s_showVolt) lv_obj_add_state(s_voltSw, LV_STATE_CHECKED); else lv_obj_remove_state(s_voltSw, LV_STATE_CHECKED);
    if (s_showDrift) lv_obj_add_state(s_driftSw, LV_STATE_CHECKED); else lv_obj_remove_state(s_driftSw, LV_STATE_CHECKED);
    show(s_adv);
}

// ── Build ───────────────────────────────────────────────────────────────────

void settingsBuild(lv_obj_t *parent) {
    // The "Set the time" menu.
    s_menu = panel(parent, "Set the time");
    button(s_menu, "Wi-Fi network", 320, 84, 0, -100, [](lv_event_t *) { s_wifiReq = true; settingsHide(); }, 0);
    button(s_menu, "Time zone",     320, 84, 0,    0, [](lv_event_t *) { showZones(); }, 0);
    button(s_menu, "Advanced",      320, 84, 0,  100, [](lv_event_t *) { showAdvanced(); }, 0);

    // Time zone: a 2 x 4 grid, then "Custom" across the bottom.
    s_zones = panel(parent, "Time zone");
    for (int i = 0; i < ZONE_COUNT; i++)
        s_zoneBtn[i] = button(s_zones, ZONES[i].label, 180, 62, i % 2 ? 94 : -94, -140 + (i / 2) * 72,
                              chooseZone, i);
    s_customBtn = button(s_zones, "", 368, 62, 0, 158, showCustom, 0, &s_customBtnLbl);

    // Custom offset: the offset, ahead/behind in words, the date and time it
    // gives, hour and half-hour steps, and Save.
    s_customBox = panel(parent, "Custom offset");
    s_ofsLbl     = label(s_customBox, &lv_font_montserrat_48, lv_color_white(), 0, -150);
    s_aheadLbl   = label(s_customBox, &lv_font_montserrat_22, NOTE_FG, 0, -100);
    lv_obj_t *n  = label(s_customBox, &lv_font_montserrat_18, NOTE_FG, 0, -70);
    lv_label_set_text(n, "fixed: no daylight saving");
    s_previewLbl = label(s_customBox, &lv_font_montserrat_22, lv_color_make(180, 180, 180), 0, -36);
    lv_obj_t *hl = label(s_customBox, &lv_font_montserrat_24, lv_color_white(), -115, 22);
    lv_label_set_text(hl, "Hours");
    button(s_customBox, LV_SYMBOL_MINUS, 96, 62,  25, 22, nudge, -60);
    button(s_customBox, LV_SYMBOL_PLUS,  96, 62, 131, 22, nudge,  60);
    lv_obj_t *ml = label(s_customBox, &lv_font_montserrat_24, lv_color_white(), -115, 98);
    lv_label_set_text(ml, "Minutes");
    button(s_customBox, "-30", 96, 62,  25, 98, nudge, -30);
    button(s_customBox, "+30", 96, 62, 131, 98, nudge,  30);
    lv_obj_t *sv = button(s_customBox, "Save", 220, 62, 0, 182, saveCustom, 0);
    lv_obj_set_style_bg_color(sv, BTN_SEL, 0);

    // Advanced: what the face shows besides the time.
    s_adv = panel(parent, "Advanced");
    lv_obj_t *a = label(s_adv, &lv_font_montserrat_20, NOTE_FG, 0, -130);
    lv_label_set_text(a, "Show on the watch face:");
    s_voltSw  = toggleRow(s_adv, "Battery voltage", -60);
    s_driftSw = toggleRow(s_adv, "Clock drift",      30);
}

void settingsShowMenu() { show(s_menu); }

void settingsHide() {
    for (lv_obj_t *p : { s_menu, s_zones, s_customBox, s_adv })
        if (p) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

bool settingsVisible() {
    for (lv_obj_t *p : { s_menu, s_zones, s_customBox, s_adv })
        if (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) return true;
    return false;
}
