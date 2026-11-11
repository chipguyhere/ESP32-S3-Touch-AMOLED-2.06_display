// watch_config.h - your settings for the GlanceWatch example.
//
// The Wi-Fi network (2.4 GHz) the watch sets its clock over, once a day.
// You can fill it in here, or leave the placeholders and type it on the
// watch: hold BOOT for the menu, then "Set the time".  A network typed on
// the watch is used instead of this one.  Keep this file out of anything you
// publish.

#pragma once

#define WATCH_WIFI_SSID      "your-wifi-name"
#define WATCH_WIFI_PASSWORD  "your-wifi-password"

// A United States zone name as chipguy_RealTimeClockSupport knows it:
// "ET", "CT", "MT", "PT" follow daylight saving; "MST" (Arizona), "EST",
// "CST", "PST", "AKT", "HST" ... see chipguy_RTCTimeZones.h.  Anywhere else,
// a fixed offset: "UTC+1", "UTC-3:30".  A zone chosen on the watch (BOOT
// menu, "Set the time", "Time zone") is used instead of this one.
#define WATCH_TIME_ZONE      "MT"

// 12-hour (1) or 24-hour (0) display.
#define WATCH_12_HOUR        1
