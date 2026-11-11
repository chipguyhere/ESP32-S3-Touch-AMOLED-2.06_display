/*
 * balls_ui.h - The bouncing balls, as an app inside GlanceWatch.
 *
 * Copied from the _display library's LvglBouncingBalls example (its ui.cpp), changed only
 * to build on a screen of its own and to keep clear of the watch glass's
 * rounded corners.  balls_create() returns that screen; GlanceWatch loads it,
 * and deletes it to close the app, which also stops the animation timer.
 */

#pragma once

#include "lvgl.h"

lv_obj_t *balls_create(void);
