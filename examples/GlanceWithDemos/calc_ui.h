/*
 * calc_ui.h - The calculator, as an app inside GlanceWatch.
 *
 * Copied from the _display library's LvglCalculator example (its ui.cpp), changed only
 * to build on a screen of its own and to keep clear of the watch glass's
 * rounded corners.  calc_create() returns that screen; GlanceWatch loads it,
 * and deletes it to close the app (the arithmetic carries over to the next open).
 */

#pragma once

#include "lvgl.h"

lv_obj_t *calc_create(void);
