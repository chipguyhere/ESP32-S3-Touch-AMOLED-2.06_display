/*
 * shooter_ui.h - Simple Shooter, as an app inside GlanceWithDemos.
 *
 * A C++ translation of the MicroPython game simpleshooter.py (the original is
 * in extras/ for comparison), with its pictures and sounds compiled in
 * (shooter_assets.c).  shooter_create() returns the game's own screen;
 * GlanceWithDemos loads it, and deletes it to close the game, which also
 * stops the game's timer and its sound.
 *
 * On the watch: BOOT changes the sound (on, shots quiet, off); the sketch
 * takes PWR to leave the game.  shooter_demo_ms() says how long the game has
 * been playing itself, so the sketch can close it when nobody is playing.
 */

#pragma once

#include "lvgl.h"
#include <AMOLED206_Board.h>

lv_obj_t *shooter_create(AMOLED206_Board &board);

// How long the self-playing demo has been running, in ms; 0 while someone
// is playing.
uint32_t shooter_demo_ms(void);
