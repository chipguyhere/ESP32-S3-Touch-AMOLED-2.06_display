/* shooter_assets.h - see shooter_assets.c (generated). */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A sound: unsigned 8-bit mono samples at 11025 Hz. */
typedef struct { const uint8_t *pcm; uint32_t len; } shooter_sound_t;

extern const lv_image_dsc_t shooter_img_ship;   /* 48x55 */
extern const lv_image_dsc_t shooter_img_rock_big0;   /* 68x68 */
extern const lv_image_dsc_t shooter_img_rock_big1;   /* 65x65 */
extern const lv_image_dsc_t shooter_img_rock_big2;   /* 72x72 */
extern const lv_image_dsc_t shooter_img_rock0;   /* 38x38 */
extern const lv_image_dsc_t shooter_img_rock1;   /* 31x31 */
extern const lv_image_dsc_t shooter_img_rock2;   /* 34x34 */
extern const lv_image_dsc_t shooter_img_rock3;   /* 27x27 */
extern const lv_image_dsc_t shooter_img_rock4;   /* 41x41 */
extern const lv_image_dsc_t shooter_img_rock5;   /* 32x32 */
extern const lv_image_dsc_t shooter_img_satellite0;   /* 51x31 */
extern const lv_image_dsc_t shooter_img_satellite1;   /* 51x31 */
extern const lv_image_dsc_t shooter_img_satellite2;   /* 51x31 */
extern const lv_image_dsc_t shooter_img_crystal0;   /* 27x34 */
extern const lv_image_dsc_t shooter_img_crystal1;   /* 27x34 */
extern const lv_image_dsc_t shooter_img_boom0;   /* 62x62 */
extern const lv_image_dsc_t shooter_img_boom1;   /* 62x62 */
extern const lv_image_dsc_t shooter_img_boom2;   /* 62x62 */
extern const lv_image_dsc_t shooter_img_boom3;   /* 62x62 */
extern const lv_image_dsc_t shooter_img_boom4;   /* 62x62 */
extern const lv_image_dsc_t shooter_img_boom5;   /* 62x62 */

extern const shooter_sound_t shooter_snd_fire;   /* 1433 samples */
extern const shooter_sound_t shooter_snd_hit;   /* 3087 samples */
extern const shooter_sound_t shooter_snd_boom;   /* 9922 samples */

#ifdef __cplusplus
} /* extern "C" */
#endif
