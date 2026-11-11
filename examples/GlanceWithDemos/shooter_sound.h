/*
 * shooter_sound.h - sound effects for Simple Shooter: several short sounds
 * mixed together and played through the watch's speaker.
 *
 * The path is I2S -> ES8311 codec -> NS4150B amplifier -> speaker.  A small
 * task mixes up to six sounds at once and keeps the I2S stream running (zeros
 * when nothing plays), so a new sound starts within ~50 ms.  The amplifier is
 * switched on only while something is playing, and off again after a quiet
 * second, so the speaker doesn't hiss between explosions.
 *
 * The ES8311 register sequence is the family's proven one (heard on the
 * Seengreat and JC8012 boards; see the board library's README-sound.md).  On
 * THIS board it is the first thing ever played -- the loudness is a guess,
 * see SHOOTER_DAC_VOLUME in shooter_sound.cpp.
 */

#pragma once

#include <AMOLED206_Board.h>
#include "shooter_assets.h"

// Bring up I2S, the codec and the mixer.  False (and silence) if the codec
// doesn't answer; the game plays on without sound.
bool shooterSoundBegin(AMOLED206_Board &board);

// Start one sound, 0..100 % loudness, mixed with whatever is playing.  If
// six are already playing, the oldest is cut off.  Safe from the LVGL task.
void shooterSoundPlay(const shooter_sound_t &snd, uint8_t volumePct);

// Amplifier off, mixer stopped, I2S released, and the codec reset -- its
// power-up defaults are where it draws least in deep sleep.
void shooterSoundEnd();
