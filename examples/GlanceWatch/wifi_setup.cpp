/*
 * wifi_setup.cpp - see wifi_setup.h.
 *
 * Layout, on the 410 x 502 screen with its ~115 px rounded corners: the title
 * and the two fields sit down the middle at the top, the keyboard spans the
 * straight-sided band between the corners (y 178..378), and the button sits
 * low in the middle.
 */

#include "wifi_setup.h"
#include <Preferences.h>

static const char *PREFS = "glancewatch";      // same namespace as the sketch's lastSync

static const char *s_defSsid = "";
static const char *s_defPass = "";
static lv_obj_t *s_panel, *s_ssid, *s_pass, *s_kb;
static bool s_submitted;

String wifiSsid() {
    Preferences p; p.begin(PREFS, true);
    String s = p.getString("ssid", "");
    p.end();
    return s.length() ? s : String(s_defSsid);
}

String wifiPassword() {
    Preferences p; p.begin(PREFS, true);
    const bool saved = p.getString("ssid", "").length() > 0;
    String s = p.getString("pass", "");
    p.end();
    return saved ? s : String(s_defPass);
}

// Point the keyboard at a field, and show the cursor there.  (These fields
// aren't in an LVGL group, so focus is done by hand.)
static void target(lv_obj_t *ta) {
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_remove_state(ta == s_ssid ? s_pass : s_ssid, LV_STATE_FOCUSED);
}

static void submit() {
    const char *ssid = lv_textarea_get_text(s_ssid);
    Preferences p; p.begin(PREFS, false);
    if (*ssid) {
        p.putString("ssid", ssid);
        p.putString("pass", lv_textarea_get_text(s_pass));
    } else {                                   // name cleared: back to watch_config.h
        p.remove("ssid");
        p.remove("pass");
    }
    p.end();
    s_submitted = true;
}

static lv_obj_t *field(int y, int width, int xOfs, const char *placeholder) {
    lv_obj_t *ta = lv_textarea_create(s_panel);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, width);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, xOfs, y);
    lv_obj_add_event_cb(ta, [](lv_event_t *e) { target((lv_obj_t *)lv_event_get_target(e)); },
                        LV_EVENT_CLICKED, nullptr);
    return ta;
}

void wifiSetupBuild(lv_obj_t *parent, const char *defaultSsid, const char *defaultPassword) {
    s_defSsid = defaultSsid;
    s_defPass = defaultPassword;

    s_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_make(230, 180, 60), 0);
    lv_label_set_text(title, "The clock is set over Wi-Fi");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    s_ssid = field(72, 300, 0, "Wi-Fi name");
    s_pass = field(124, 244, -28, "Password");
    lv_textarea_set_password_mode(s_pass, true);

    // Show / hide the password: typing blind on a watch keyboard is hard.
    lv_obj_t *eye = lv_button_create(s_panel);
    lv_obj_set_size(eye, 50, 44);
    lv_obj_align_to(eye, s_pass, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_t *eyeLbl = lv_label_create(eye);
    lv_label_set_text(eyeLbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(eyeLbl);
    lv_obj_add_event_cb(eye, [](lv_event_t *) {
        lv_textarea_set_password_mode(s_pass, !lv_textarea_get_password_mode(s_pass));
    }, LV_EVENT_CLICKED, nullptr);

    s_kb = lv_keyboard_create(s_panel);
    lv_obj_set_size(s_kb, 390, 200);
    lv_obj_align(s_kb, LV_ALIGN_TOP_MID, 0, 178);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_18, LV_PART_ITEMS);
    // The keyboard's check key: from the name, move on to the password; from
    // the password, go.  Its close key does nothing -- BOOT or PWR leave.
    lv_obj_add_event_cb(s_kb, [](lv_event_t *) {
        if (lv_keyboard_get_textarea(s_kb) == s_ssid) target(s_pass);
        else submit();
    }, LV_EVENT_READY, nullptr);

    lv_obj_t *go = lv_button_create(s_panel);
    lv_obj_set_size(go, 260, 56);
    lv_obj_align(go, LV_ALIGN_TOP_MID, 0, 392);
    lv_obj_set_style_radius(go, 24, 0);
    lv_obj_t *goLbl = lv_label_create(go);
    lv_obj_set_style_text_font(goLbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(goLbl, "Set the time now");
    lv_obj_center(goLbl);
    lv_obj_add_event_cb(go, [](lv_event_t *) { submit(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void wifiSetupShow() {
    lv_textarea_set_text(s_ssid, wifiSsid().c_str());
    lv_textarea_set_text(s_pass, wifiPassword().c_str());
    lv_textarea_set_password_mode(s_pass, true);
    target(s_ssid);
    s_submitted = false;
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void wifiSetupHide() { lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN); }

bool wifiSetupVisible() { return s_panel && !lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN); }

bool wifiSetupSubmitted() {
    const bool s = s_submitted;
    s_submitted = false;
    return s;
}
