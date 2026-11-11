/*
 * shooter_ui.cpp - Simple Shooter: a demo of sprites and sound effects.
 *
 * Translated from the MicroPython simpleshooter.py (extras/simpleshooter.py),
 * keeping its structure and names so the two can be read side by side.
 *
 * Until you touch it, the game plays itself (a "demo", for no points).
 * Touch and hold the lower part of the screen to play: the ship flies towards
 * your finger and fires.  Rocks, satellites and ice crystals fall from the
 * top; shoot them for points.  A big rock breaks into small ones.  The higher
 * your score, the faster things fall and the more of them there are.  If
 * something hits the ship it blows up; your score stays on the screen so you
 * can see it, and goes back to 0 when the new ship fires (your best stays
 * underneath).  Leave it alone for 20 seconds and the demo comes back.  The
 * BOOT button changes the sound: shots quiet, then all quiet, then all on.
 *
 * How it works:
 *   - The pictures are LVGL images compiled into flash (shooter_assets.c),
 *     already scaled for this screen, so LVGL draws them without zooming.
 *   - Every moving thing is made once, at the start, and hidden when not in
 *     use ("a pool"), instead of being made and deleted all the time.
 *   - An LVGL timer calls step() 30 times a second: that moves everything,
 *     checks what hit what, and makes the explosions.
 *   - The game was written for a 240 x 320 screen.  Everything is measured
 *     in "game units", U screen pixels each (the screen's short side over
 *     240), so it fills whatever screen it is started on.
 *   - Sounds are mixed together (shooter_sound.cpp), so shots, hits and
 *     explosions can all sound at once.
 *
 * Changes for the watch: BOOT alone changes the sound (the FT3168 has no
 * three-finger touch), the labels are kept clear of the glass's rounded
 * corners, and the hint says how to leave.  And one fix: a shot is tested
 * against a rock along its whole path through a step (shotHits()), so fast
 * pairs no longer pass through each other between frames.
 */

#include "shooter_ui.h"
#include <Arduino.h>
#include "shooter_assets.h"
#include "shooter_sound.h"
#include <math.h>
#include <esp_random.h>

static int   W = 240, H = 320;          // the screen, set by shooter_create()
static float U = 1.0f;                  // screen pixels per game unit
static int   CONTROL_TOP = 190;         // touches below this line steer the ship
static int   SHIP_Y_MIN = 200;          // how high the ship may fly
static const uint32_t FIRE_EVERY_MS   = 170;    // time between shots while you hold on
static const uint32_t IDLE_TO_DEMO_MS = 20000;  // no touch this long: back to the demo
static const int MAX_ROCKS = 12;
static const int MAX_SHOTS = 10;
static const int MAX_BOOMS = 5;
static const int STARS     = 18;

// --- random numbers -------------------------------------------------------------

static float frand(float a, float b) { return a + (esp_random() / 4294967296.0f) * (b - a); }
static int   irand(int a, int b)     { return a + (int)(esp_random() % (uint32_t)(b - a + 1)); }   // inclusive

static inline bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

// --- things that move ------------------------------------------------------------

struct Kind;

// One sprite on screen: where it is, how fast it goes, how big it is.
struct Thing {
    lv_obj_t *obj = nullptr;
    bool  alive = false;
    float x = 0, y = 0, vx = 0, vy = 0;
    float px = 0, py = 0;               // where it was at the start of this step
    int   w = 0, h = 0;
    const Kind *kind = nullptr;
    float frame = 0;                    // explosions: which picture

    void show(float nx, float ny) {
        x = px = nx; y = py = ny;
        alive = true;
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        place();
    }
    void hide() {
        alive = false;
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    // x, y is the centre; LVGL wants the top-left corner
    void place() { lv_obj_set_pos(obj, (int)x - w / 2, (int)y - h / 2); }
    // True if the two overlap (a little generously for round things).
    bool hits(const Thing &o, float slack = 0.7f) const {
        return fabsf(x - o.x) * 2 < (w + o.w) * slack && fabsf(y - o.y) * 2 < (h + o.h) * slack;
    }
    void setPicture(const lv_image_dsc_t *pic) {
        lv_image_set_src(obj, pic);
        w = pic->header.w;
        h = pic->header.h;
    }
};

// Did this shot hit this rock at any moment during the step just taken?
//
// Not in the Python original, which tests only where the two end up.  A shot
// and a rock close on each other by up to ~28 px a step (more when a slow
// frame makes dt bigger), while a small rock's hit box is only ~31 px tall, so
// a pair could cross between two steps without ever being seen to overlap.
// Here the shot's path relative to the rock over the whole step is clipped
// against the rock's box (a slab test): any overlap along the way is a hit.
//
// The box is also kinder than hits(): the rock is shrunk to 0.8 for its round
// outline, but the shot isn't shrunk at all -- 0.7 on both made a thin shot
// visibly graze a small rock and pass through.
static bool shotHits(const Thing &shot, const Thing &rock) {
    const float limX = (shot.w + rock.w * 0.8f) / 2;
    const float limY = (shot.h + rock.h * 0.8f) / 2;
    const float x0 = shot.px - rock.px, y0 = shot.py - rock.py;   // relative, start of step
    const float dx = (shot.x - rock.x) - x0, dy = (shot.y - rock.y) - y0;
    float t0 = 0, t1 = 1;
    auto slab = [&](float p0, float d, float lim) {
        if (fabsf(d) < 1e-4f) return fabsf(p0) < lim;              // no relative motion on this axis
        float a = (-lim - p0) / d, b = (lim - p0) / d;
        if (a > b) { float t = a; a = b; b = t; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        return t0 <= t1;
    };
    return slab(x0, dx, limX) && slab(y0, dy, limY);
}

static Thing *freeOne(Thing *pool, int n) {
    for (int i = 0; i < n; i++) if (!pool[i].alive) return &pool[i];
    return nullptr;
}

// --- the game --------------------------------------------------------------------

// What falls from the top: pictures (one is picked at random, so no two
// rocks need look the same), points, chance, speed range.
struct Kind {
    const lv_image_dsc_t *const *pics;
    int npics, points, chance;
    float vmin, vmax;
};

static const lv_image_dsc_t *const BIG_PICS[]   = { &shooter_img_rock_big0, &shooter_img_rock_big1, &shooter_img_rock_big2 };
static const lv_image_dsc_t *const SMALL_PICS[] = { &shooter_img_rock0, &shooter_img_rock1, &shooter_img_rock2,
                                                    &shooter_img_rock3, &shooter_img_rock4, &shooter_img_rock5 };
static const lv_image_dsc_t *const SAT_PICS[]   = { &shooter_img_satellite0, &shooter_img_satellite1, &shooter_img_satellite2 };
static const lv_image_dsc_t *const ICE_PICS[]   = { &shooter_img_crystal0, &shooter_img_crystal1 };
static const lv_image_dsc_t *const BOOM_PICS[]  = { &shooter_img_boom0, &shooter_img_boom1, &shooter_img_boom2,
                                                    &shooter_img_boom3, &shooter_img_boom4, &shooter_img_boom5 };
static const int BOOM_FRAMES = sizeof BOOM_PICS / sizeof BOOM_PICS[0];

static const Kind BIG_ROCKS   = { BIG_PICS,   3, 20, 20, 0.9f, 1.8f };
static const Kind SMALL_ROCKS = { SMALL_PICS, 6, 10, 66, 1.4f, 3.1f };
static const Kind SATELLITES  = { SAT_PICS,   3, 50,  8, 1.0f, 2.0f };
static const Kind CRYSTALS    = { ICE_PICS,   2, 30,  6, 2.0f, 3.6f };
static const Kind *const KINDS[] = { &BIG_ROCKS, &SMALL_ROCKS, &SATELLITES, &CRYSTALS };

// BOOT steps through these.
static const char *const SOUND_MODES[] = { "sound on", "shots quiet", "sound off" };

// Everything the game keeps between steps.
static struct Game {
    lv_obj_t *screen;
    lv_timer_t *timer;
    Thing stars[STARS], shots[MAX_SHOTS], rocks[MAX_ROCKS], booms[MAX_BOOMS], ship;
    lv_obj_t *scoreLabel, *bestLabel, *hint, *note;
    int  score, best;
    bool scoreStale;                    // true from a crash until the next ship fires
    bool playing, touching;
    float targetX, targetY;
    uint32_t last, nextShot, nextRock, shipBackAt, safeUntil, lastTouch, noteUntil, demoSince;
    int  shipOpa;
    int  soundMode;
    bool bootWasDown;
} game;

// 1.0 at the start, rising with the score to 2.5 at 600 points: things fall
// faster and come more often.  Dying resets this (and the score, once the
// next ship fires).
static float difficulty() {
    if (game.scoreStale) return 1.0f;
    return fminf(2.5f, 1 + game.score / 400.0f);
}

static void spawnRock(const Kind *kind = nullptr, float x = NAN, float y = NAN,
                      float vx = NAN, float vy = NAN) {
    Thing *rock = freeOne(game.rocks, MAX_ROCKS);
    if (!rock) return;
    if (!kind) {                        // pick one by chance
        int total = 0;
        for (const Kind *k : KINDS) total += k->chance;
        int n = irand(0, total - 1);
        for (const Kind *k : KINDS) {
            n -= k->chance;
            if (n < 0) { kind = k; break; }
        }
    }
    rock->setPicture(kind->pics[irand(0, kind->npics - 1)]);
    rock->kind = kind;
    rock->vx = isnan(vx) ? frand(-0.5f, 0.5f) * U : vx;
    rock->vy = isnan(vy) ? frand(kind->vmin, kind->vmax) * U * difficulty() : vy;
    rock->show(isnan(x) ? frand(20 * U, W - 20 * U) : x,
               isnan(y) ? -20 * U : y);
}

// Play a sound effect, if the sound mode allows it.
static void sound(const shooter_sound_t &effect, uint8_t volume) {
    const int mode = game.soundMode;
    if (mode == 0 || (mode == 1 && &effect != &shooter_snd_fire)) shooterSoundPlay(effect, volume);
}

static void explode(float x, float y, bool big = false) {
    Thing *boom = freeOne(game.booms, MAX_BOOMS);
    if (boom) {
        boom->frame = 0;
        boom->setPicture(BOOM_PICS[0]);
        boom->show(x, y);
    }
    sound(big ? shooter_snd_boom : shooter_snd_hit, big ? 90 : 70);
}

static void addScore(int points) {
    if (!game.playing || game.scoreStale) return;   // the demo scores nothing, nor a dead ship
    game.score += points;
    lv_label_set_text_fmt(game.scoreLabel, "%d", game.score);
    if (game.score > game.best) {
        game.best = game.score;
        lv_label_set_text_fmt(game.bestLabel, "HI %d", game.best);
    }
}

static void shipHit() {
    explode(game.ship.x, game.ship.y, true);
    game.ship.hide();
    game.shipBackAt = lv_tick_get() + 1500;
    // Leave the score showing; it goes back to 0 when the next ship fires.
    if (game.playing) game.scoreStale = true;
}

// The new ship has fired: start again from the bottom.
static void clearStaleScore() {
    if (game.scoreStale) {
        game.scoreStale = false;
        game.score = 0;
        lv_label_set_text(game.scoreLabel, "0");
    }
}

static void startPlaying() {
    game.playing = true;
    game.scoreStale = false;
    game.score = 0;
    lv_label_set_text(game.scoreLabel, "0");
    lv_obj_add_flag(game.hint, LV_OBJ_FLAG_HIDDEN);
}

static void startDemo() {
    game.playing = false;
    game.scoreStale = false;
    game.score = 0;
    game.demoSince = lv_tick_get();
    lv_label_set_text(game.scoreLabel, "DEMO");
    lv_obj_remove_flag(game.hint, LV_OBJ_FLAG_HIDDEN);
}

// In the demo: head for whatever is lowest on the screen.
static void autopilot(float &tx, float &ty) {
    const Thing *lowest = nullptr;
    for (const Thing &rock : game.rocks)
        if (rock.alive && rock.y < game.ship.y - 20 * U && (!lowest || rock.y > lowest->y)) lowest = &rock;
    tx = lowest ? lowest->x : W / 2.0f + 60 * U * sinf(lv_tick_get() / 1500.0f);
    ty = H - 40 * U;
}

// A new press of BOOT moves to the next sound mode (once per press: holding
// it doesn't keep changing it).
static void checkSoundToggle(uint32_t now) {
    const bool down = digitalRead(AMOLED206_Board::BOOT_BUTTON_PIN) == LOW;
    if (down && !game.bootWasDown) {
        game.soundMode = (game.soundMode + 1) % 3;
        lv_label_set_text(game.note, SOUND_MODES[game.soundMode]);
        lv_obj_remove_flag(game.note, LV_OBJ_FLAG_HIDDEN);
        game.noteUntil = now + 1500;
    }
    game.bootWasDown = down;
    if (reached(now, game.noteUntil)) lv_obj_add_flag(game.note, LV_OBJ_FLAG_HIDDEN);
}

static void moveEverything() {
    const uint32_t now = lv_tick_get();
    const float dt = fminf(3.0f, (now - game.last) / 33.0f);   // 1.0 at 30 fps
    game.last = now;
    Thing &ship = game.ship;
    checkSoundToggle(now);
    if (game.playing && !game.touching && now - game.lastTouch > IDLE_TO_DEMO_MS) startDemo();

    // Stars: fall at their own speed, wrap round to the top.
    for (Thing &star : game.stars) {
        star.y += star.vy * dt;
        if (star.y > H) {
            star.y -= H + 4 * U;
            star.x = irand(0, W - 1);
        }
        lv_obj_set_pos(star.obj, (int)star.x, (int)star.y);
    }

    // The ship: back after an explosion, then towards the finger, firing.
    if (!ship.alive && reached(now, game.shipBackAt)) {
        ship.show(W / 2.0f, H - 40 * U);
        game.safeUntil = now + 1500;
    }
    if (ship.alive) {
        const bool flying = game.touching || !game.playing;
        if (flying) {
            float tx = game.targetX, ty = game.targetY;
            if (!game.playing) autopilot(tx, ty);
            ty = fmaxf(SHIP_Y_MIN, ty - 30 * U);
            const float lim = 7 * U * dt;
            ship.x += fmaxf(-lim, fminf(lim, tx - ship.x));
            ship.y += fmaxf(-lim, fminf(lim, ty - ship.y));
            if (reached(now, game.nextShot)) {
                Thing *shot = freeOne(game.shots, MAX_SHOTS);
                if (shot) {
                    if (game.playing) clearStaleScore();
                    shot->show(ship.x, ship.y - 18 * U);
                    sound(shooter_snd_fire, 45);
                }
                game.nextShot = now + FIRE_EVERY_MS;
            }
        }
        ship.place();
        // blink while it can't be hit yet (and only tell LVGL when it
        // changes: every change means a redraw)
        const bool safe = (int32_t)(game.safeUntil - now) > 0;
        const int opa = safe && (now / 120) % 2 ? 110 : 255;
        if (opa != game.shipOpa) {
            lv_obj_set_style_image_opa(ship.obj, opa, 0);
            game.shipOpa = opa;
        }
    }

    // Shots fly up.
    for (Thing &shot : game.shots) {
        if (!shot.alive) continue;
        shot.px = shot.x; shot.py = shot.y;
        shot.y -= 9 * U * dt;
        if (shot.y < -10 * U) shot.hide();
        else shot.place();
    }

    // New things fall now and then -- more often as the score goes up.
    if (reached(now, game.nextRock)) {
        spawnRock();
        const int gap = (int)(1100 / difficulty());
        game.nextRock = now + irand(gap / 2, gap);
    }

    // Rocks fall, get shot, or hit the ship.
    for (Thing &rock : game.rocks) {
        if (!rock.alive) continue;
        rock.px = rock.x; rock.py = rock.y;
        rock.x += rock.vx * dt;
        rock.y += rock.vy * dt;
        if (rock.y > H + 30 * U || rock.x < -30 * U || rock.x > W + 30 * U) {
            rock.hide();
            continue;
        }
        rock.place();
        for (Thing &shot : game.shots) {
            if (shot.alive && shotHits(shot, rock)) {
                shot.hide();
                rock.hide();
                addScore(rock.kind->points);
                const bool big = rock.kind == &BIG_ROCKS;
                explode(rock.x, rock.y, big);
                if (big) {                                 // a shower of small rocks
                    const int n = irand(3, 4);
                    for (int i = 0; i < n; i++)
                        spawnRock(&SMALL_ROCKS, rock.x, rock.y, frand(-2.2f, 2.2f) * U, frand(0.5f, 2.5f) * U);
                }
                break;
            }
        }
        if (rock.alive && ship.alive && rock.hits(ship, 0.55f) && reached(now, game.safeUntil)) {
            rock.hide();
            shipHit();
        }
    }

    // Explosions: next picture every other step, then gone.
    for (Thing &boom : game.booms) {
        if (!boom.alive) continue;
        boom.frame += 0.5f * dt;
        const int f = (int)boom.frame;
        if (f >= BOOM_FRAMES) boom.hide();
        else lv_image_set_src(boom.obj, BOOM_PICS[f]);
    }
}

static void step(lv_timer_t *) { moveEverything(); }

static void onTouch(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        if (p.y >= CONTROL_TOP) {
            if (!game.playing) startPlaying();      // the player takes over
            game.touching = true;
            game.targetX = p.x;
            game.targetY = p.y;
            game.lastTouch = lv_tick_get();
        } else {
            game.touching = false;
        }
    } else {                                        // RELEASED or PRESS_LOST
        game.touching = false;
    }
}

// The screen is going: the game goes with it.
static void onDelete(lv_event_t *) {
    if (game.timer) { lv_timer_delete(game.timer); game.timer = nullptr; }
    shooterSoundEnd();
    game.screen = nullptr;
}

// --- building it -----------------------------------------------------------------

// An LVGL image for a sprite, hidden and not clickable (let touches reach
// the screen).
static lv_obj_t *makeSprite(lv_obj_t *parent) {
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

static lv_obj_t *makeBox(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *shooter_create(AMOLED206_Board &board) {
    lv_display_t *disp = lv_display_get_default();
    W = lv_display_get_horizontal_resolution(disp);
    H = lv_display_get_vertical_resolution(disp);
    U = (W < H ? W : H) / 240.0f;
    CONTROL_TOP = H * 190 / 320;
    SHIP_Y_MIN  = H * 200 / 320;

    shooterSoundBegin(board);           // silent if the codec doesn't answer

    // Keep the best score from one game to the next; everything else is new.
    const int best = game.best;
    game = Game();
    game.best = best;

    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05060F), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    for (lv_event_code_t code : { LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST })
        lv_obj_add_event_cb(scr, onTouch, code, nullptr);
    lv_obj_add_event_cb(scr, onDelete, LV_EVENT_DELETE, nullptr);
    game.screen = scr;

    // Stars: small grey squares; the faster ones are brighter (closer).
    for (Thing &star : game.stars) {
        star.obj = makeBox(scr);
        const float near = frand(0, 1);
        const int size = (int)fmaxf(1, (near > 0.7f ? 2 : 1) * U);
        const int grey = (int)(90 + 160 * near);
        lv_obj_set_size(star.obj, size, size);
        lv_obj_set_style_bg_color(star.obj, lv_color_make(grey, grey, grey + 30 > 255 ? 255 : grey + 30), 0);
        star.vy = (0.6f + 2.4f * near) * U;
        star.show(irand(0, W - 1), irand(0, H - 1));
    }

    // Shots: thin glowing bars.
    for (Thing &shot : game.shots) {
        shot.obj = makeBox(scr);
        shot.w = (int)(3 * U);
        shot.h = (int)(10 * U);
        lv_obj_set_size(shot.obj, shot.w, shot.h);
        lv_obj_set_style_radius(shot.obj, (int)(2 * U), 0);
        lv_obj_set_style_bg_color(shot.obj, lv_color_hex(0x7FF7FF), 0);
    }

    for (Thing &rock : game.rocks) rock.obj = makeSprite(scr);

    game.ship.obj = makeSprite(scr);
    game.ship.setPicture(&shooter_img_ship);

    for (Thing &boom : game.booms) boom.obj = makeSprite(scr);

    // The labels sit down the middle: the glass has ~115 px rounded corners.
    game.scoreLabel = lv_label_create(scr);
    lv_obj_set_style_text_font(game.scoreLabel, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(game.scoreLabel, lv_color_hex(0xFFC400), 0);
    lv_obj_align(game.scoreLabel, LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(game.scoreLabel, "0");

    game.bestLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(game.bestLabel, lv_color_hex(0x8899A6), 0);
    lv_obj_align(game.bestLabel, LV_ALIGN_TOP_MID, 0, 48);
    lv_label_set_text_fmt(game.bestLabel, "HI %d", game.best);

    game.hint = lv_label_create(scr);
    lv_obj_set_style_text_color(game.hint, lv_color_hex(0x8899A6), 0);
    lv_label_set_text(game.hint, "touch and hold down here to play\nBOOT: sound     PWR: back to the watch");
    lv_obj_set_style_text_align(game.hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(game.hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    game.note = lv_label_create(scr);               // "sound off" etc., briefly
    lv_obj_set_style_text_color(game.note, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_style_bg_color(game.note, lv_color_hex(0x203040), 0);
    lv_obj_set_style_bg_opa(game.note, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(game.note, (int)(6 * U), 0);
    lv_obj_align(game.note, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_add_flag(game.note, LV_OBJ_FLAG_HIDDEN);

    const uint32_t now = lv_tick_get();
    game.touching = false;
    game.targetX = W / 2.0f;
    game.targetY = H - 40 * U;
    game.last = game.nextShot = game.nextRock = game.shipBackAt = game.safeUntil = now;
    game.lastTouch = game.noteUntil = now;
    game.shipOpa = 255;
    game.soundMode = 0;
    game.bootWasDown = true;
    startDemo();
    game.ship.show(W / 2.0f, H - 40 * U);

    game.timer = lv_timer_create(step, 33, nullptr);
    return scr;
}

uint32_t shooter_demo_ms(void) {
    if (!game.screen || game.playing) return 0;
    return lv_tick_get() - game.demoSince;
}
