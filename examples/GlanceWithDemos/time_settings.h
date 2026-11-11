/*
 * time_settings.h - the "Set the time" menu and the screens behind it:
 *
 *   Wi-Fi network   hands over to wifi_setup (the sketch opens it)
 *   Time zone       the US zones, or a custom fixed offset from UTC in
 *                   half-hour steps, with no daylight saving
 *   Advanced        whether the face shows the battery voltage and the
 *                   clock's drift (both off until turned on)
 *
 * Each is a full-screen panel over the watch face, hidden until shown.  The
 * choices are saved in NVS; until a zone is chosen here, watch_config.h's
 * WATCH_TIME_ZONE is used.
 */

#pragma once

#include <Arduino.h>
#include "lvgl.h"
#include <chipguy_RealTimeClockSupport.h>

// Load the saved choices.  Call before the face is first drawn, and before
// settingsToLocal().  `defaultZone` is watch_config.h's.
void settingsBegin(const char *defaultZone);

// Build the panels (hidden) on `parent`.
void settingsBuild(lv_obj_t *parent);

void settingsShowMenu();          // the "Set the time" menu
void settingsHide();              // whichever panel is up
bool settingsVisible();

// True once after "Wi-Fi network" was tapped.  The panel is hidden by then;
// the sketch opens the Wi-Fi screen.
bool settingsWifiRequested();

// A line for the face's status label after a change ("Time zone: ..."),
// once; null otherwise.
const char *settingsMessage();

// UTC to local wall-clock time (shifted unix seconds, for chipguy_rtcBreakDown),
// and the zone's short name for that instant ("MDT", "UTC+5:30").
chipguy_rtc_time_t settingsToLocal(chipguy_rtc_time_t utc);
const char *settingsZoneAbbrev(chipguy_rtc_time_t utc);

// The "Advanced" choices.
bool settingsShowVoltage();
bool settingsShowDrift();
