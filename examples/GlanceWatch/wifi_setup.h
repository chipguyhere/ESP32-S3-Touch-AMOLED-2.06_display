/*
 * wifi_setup.h - the "Set the time" screen: it explains that the clock sets
 * itself over Wi-Fi, and lets you type in the network's name and password.
 *
 * It is a panel over the watch face (not a screen of its own), hidden until
 * wifiSetupShow().  What is typed here is saved in NVS and used from then
 * on; until something is saved, the watch_config.h network is used.  Clearing
 * the name and tapping "Set the time now" goes back to watch_config.h's.
 */

#pragma once

#include <Arduino.h>
#include "lvgl.h"

// Build the panel (hidden) on `parent`.  The defaults are watch_config.h's.
void wifiSetupBuild(lv_obj_t *parent, const char *defaultSsid, const char *defaultPassword);

void wifiSetupShow();
void wifiSetupHide();
bool wifiSetupVisible();

// True once after "Set the time now" was tapped; the network is saved by
// then.  The sketch hides the panel and syncs.
bool wifiSetupSubmitted();

// The network to use: what was saved here, else watch_config.h's.
String wifiSsid();
String wifiPassword();
