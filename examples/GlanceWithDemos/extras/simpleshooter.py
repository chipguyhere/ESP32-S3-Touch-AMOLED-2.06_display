"""Launcher name:Simple Shooter"""
# Simple Shooter -- a demo of sprites and sound effects.
#
# Until you touch it, the game plays itself (a "demo", for no points).
# Touch and hold the lower part of the screen to play: the ship flies towards
# your finger and fires.  Rocks, satellites and ice crystals fall from the top;
# shoot them for points.  A big rock breaks into small ones.  The higher your
# score, the faster things fall and the more of them there are.  If
# something hits the ship it blows up; your score stays on the screen so you
# can see it, and goes back to 0 when the new ship fires (your best stays at
# the top right).  Leave it alone for 20 seconds and the demo
# comes back.  A three-finger touch (or the BOOT button, under the back
# cover) changes the sound: shots quiet, then all quiet, then all on again.
#
# The game is a folder, simpleshooter/, so it can be copied as one.  It comes
# with the board, as one of the demos (club.restore('simpleshooter') puts
# back the original); its pictures and sounds are built into the firmware
# (see ASSETS below).
#
# How it works:
#   * Pictures ("sprites") are LVGL images, made ONCE at the start from
#     pictures in memory (the built-in ones are in flash, which the chip
#     reads like memory) -- drawing straight from a file would re-read it
#     every time the screen is redrawn, which is slow.
#   * Every moving thing is made once, at the start, and hidden when not in
#     use ("a pool"), instead of being made and deleted all the time.
#   * An LVGL timer calls step() 30 times a second: that moves everything,
#     checks what hit what, and makes the explosions.
#   * The game was written for a 240 x 320 screen.  Here everything is
#     measured in "game units", U screen pixels each (the screen's short side
#     over 240), so it fills whatever screen it is started on.  The pictures
#     are drawn for U = 2 and zoomed by LVGL when U is bigger.
#   * Sounds are club.effect() -- mixed together, so shots, hits and
#     explosions can all sound at once.
#   * Python tidies away unused memory ("garbage collection") when it runs
#     short, and that pauses everything for a moment -- a stutter in a game.
#     So step() collects after every frame instead: each one is a small,
#     steady cost rather than a big pause now and then.

import gc
import math
import random
import time

import machine
import lvgl as lv
import club

# The pictures and sounds come built into the firmware (the frozen
# simpleshooter_assets module), read straight from flash -- they cost no RAM
# and no room in the filesystem.  A file in an assets/ folder next to this
# one, with the same name (ship.bin, boom.wav, ...), is used instead of the
# built-in one.  To get a copy of one to look at or change:
#     club.restore('simpleshooter/assets/ship.bin')    # or .../assets: all
# Delete your file and the game goes back to the built-in one.
ASSETS = __file__.rpartition('/')[0] + '/assets/'

W, H = 240, 320                 # the screen, set by run()
U = 1.0                         # screen pixels per game unit, set by run()
SPRITE_U = 2                    # the game units the pictures are drawn for
ZOOM = 1.0                      # how much LVGL scales the pictures up
CONTROL_TOP = 190               # touches below this line steer the ship
SHIP_Y_MIN = 200                # how high the ship may fly
FIRE_EVERY_MS = 170             # time between shots while you hold on
IDLE_TO_DEMO_MS = 20000         # no touch this long: back to the demo
MAX_ROCKS = 12
MAX_SHOTS = 10
MAX_BOOMS = 5
STARS = 18


# --- loading the pictures and sounds ------------------------------------------

def built_in(filename):
    """A built-in picture or sound, as bytes that stay in flash."""
    import simpleshooter_assets
    return getattr(simpleshooter_assets, filename.replace('.', '_'))


def load_sprite(name):
    """An LVGL .bin picture: your own file in assets/ (read into memory),
    or else the built-in one (used where it is, in flash)."""
    try:
        with open(ASSETS + name + '.bin', 'rb') as f:
            # The first 12 bytes say what it is: colour format, width, height.
            head = f.read(12)
            pixels = f.read()       # the rest, read straight into its own
                                    # buffer (slicing a whole-file read would
                                    # need the memory twice for a moment)
    except OSError:                 # no file of your own: the built-in one
        data = built_in(name + '.bin')
        head = data[:12]
        pixels = memoryview(data)[12:]   # a view: no copy, still in flash
    w = head[4] | head[5] << 8
    h = head[6] | head[7] << 8
    picture = lv.image_dsc_t({
        'header': {'magic': lv.IMAGE_HEADER_MAGIC, 'cf': head[1],
                   'w': w, 'h': h, 'stride': head[8] | head[9] << 8},
        'data_size': len(pixels),
        'data': pixels,
    })
    keep.append(pixels)         # LVGL only points at the pixels: keep them
    return picture, int(w * ZOOM), int(h * ZOOM)   # its size on the screen

def load_sound(name):
    """A WAV sound: your own file in assets/, or else the built-in one."""
    try:
        with open(ASSETS + name + '.wav', 'rb') as f:
            return f.read()
    except OSError:
        return built_in(name + '.wav')


# --- things that move ---------------------------------------------------------

class Thing:
    """One sprite on screen: where it is, how fast it goes, how big it is."""
    def __init__(self, obj):
        self.obj = obj
        self.alive = False
        self.x = self.y = self.vx = self.vy = 0.0
        self.w = self.h = 0
        self.kind = None

    def show(self, x, y):
        self.x, self.y = x, y
        self.alive = True
        self.obj.remove_flag(lv.obj.FLAG.HIDDEN)
        self.place()

    def hide(self):
        self.alive = False
        self.obj.add_flag(lv.obj.FLAG.HIDDEN)

    def place(self):
        # x, y is the centre; LVGL wants the top-left corner
        self.obj.set_pos(int(self.x) - self.w // 2, int(self.y) - self.h // 2)

    def hits(self, other, slack=0.7):
        """True if the two overlap (a little generously for round things)."""
        dx = abs(self.x - other.x) * 2
        dy = abs(self.y - other.y) * 2
        return dx < (self.w + other.w) * slack and dy < (self.h + other.h) * slack


def sprite(parent):
    """An LVGL image for a sprite, zoomed if this screen needs it (from the
    top-left corner, so place() can position it by its zoomed size)."""
    img = lv.image(parent)
    if ZOOM != 1.0:
        img.set_pivot(0, 0)
        img.set_scale(int(256 * ZOOM))
    return img


def make_pool(n, make):
    pool = []
    for i in range(n):
        t = Thing(make())
        t.obj.add_flag(lv.obj.FLAG.HIDDEN)
        t.obj.remove_flag(lv.obj.FLAG.CLICKABLE)    # let touches reach the screen
        pool.append(t)
    return pool

def free_one(pool):
    for t in pool:
        if not t.alive:
            return t
    return None


# --- the game -----------------------------------------------------------------

keep = []                       # pixel data LVGL is drawing from
game = {}                       # everything else, so step() can reach it

# What falls from the top: pictures (one is picked at random, so no two
# rocks need look the same), points, chance, speed range.
BIG_ROCKS = (('rock_big0', 'rock_big1', 'rock_big2'), 20, 20, (0.9, 1.8))
SMALL_ROCKS = (('rock0', 'rock1', 'rock2', 'rock3', 'rock4', 'rock5'), 10, 66, (1.4, 3.1))
KINDS = [
    BIG_ROCKS,
    SMALL_ROCKS,
    (('satellite0', 'satellite1', 'satellite2'), 50, 8, (1.0, 2.0)),
    (('crystal0', 'crystal1'), 30, 6, (2.0, 3.6)),
]


def difficulty():
    """1.0 at the start, rising with the score to 2.5 at 600 points: things
    fall faster and come more often.  Dying resets this (and the score,
    once the next ship fires)."""
    if game['score_stale']:
        return 1.0
    return min(2.5, 1 + game['score'] / 400)


def spawn_rock(kind=None, x=None, y=None, vx=None, vy=None):
    rock = free_one(game['rocks'])
    if rock is None:
        return
    if kind is None:            # pick one by chance
        n = random.randrange(sum(k[2] for k in KINDS))
        for k in KINDS:
            n -= k[2]
            if n < 0:
                kind = k
                break
    picture, rock.w, rock.h = game['sprites'][random.choice(kind[0])]
    rock.obj.set_src(picture)
    rock.kind = kind
    rock.vx = random.uniform(-0.5, 0.5) * U if vx is None else vx
    rock.vy = random.uniform(*kind[3]) * U * difficulty() if vy is None else vy
    rock.show(random.uniform(20 * U, W - 20 * U) if x is None else x,
              -20 * U if y is None else y)


# A three-finger touch or the BOOT button steps through these.
SOUND_MODES = ('sound on', 'shots quiet', 'sound off')


def sound(effect, volume):
    """Play a sound effect, if the sound mode allows it."""
    mode = game['sound_mode']
    if mode == 0 or (mode == 1 and effect is not game['sound_fire']):
        club.effect(effect, volume)


def explode(x, y, big=False):
    boom = free_one(game['booms'])
    if boom is not None:
        boom.frame = 0
        boom.w = boom.h = game['boom_size']
        boom.obj.set_src(game['boom_frames'][0])
        boom.show(x, y)
    sound(game['sound_boom'] if big else game['sound_hit'], 90 if big else 70)


def add_score(points):
    if not game['playing'] or game['score_stale']:
        return                     # the demo scores nothing, nor a dead ship
    game['score'] += points
    game['score_label'].set_text('%d' % game['score'])
    if game['score'] > game['best']:
        game['best'] = game['score']
        game['best_label'].set_text('HI %d' % game['best'])


def ship_hit():
    ship = game['ship']
    explode(ship.x, ship.y, big=True)
    ship.hide()
    game['ship_back_at'] = time.ticks_add(time.ticks_ms(), 1500)
    if game['playing']:
        # Leave the score showing; it goes back to 0 when the next ship fires.
        game['score_stale'] = True


def clear_stale_score():
    """The new ship has fired: start again from the bottom."""
    if game['score_stale']:
        game['score_stale'] = False
        game['score'] = 0
        game['score_label'].set_text('0')


def start_playing():
    game['playing'] = True
    game['score_stale'] = False
    game['score'] = 0
    game['score_label'].set_text('0')
    game['hint'].add_flag(lv.obj.FLAG.HIDDEN)


def start_demo():
    game['playing'] = False
    game['score_stale'] = False
    game['score'] = 0
    game['score_label'].set_text('DEMO')
    game['hint'].remove_flag(lv.obj.FLAG.HIDDEN)


def autopilot():
    """In the demo: head for whatever is lowest on the screen."""
    ship = game['ship']
    lowest = None
    for rock in game['rocks']:
        if rock.alive and rock.y < ship.y - 20 * U and (lowest is None or rock.y > lowest.y):
            lowest = rock
    x = lowest.x if lowest else W / 2 + 60 * U * math.sin(time.ticks_ms() / 1500)
    return (x, H - 40 * U)


def check_sound_toggle():
    """A new three-finger touch or press of BOOT moves to the next sound
    mode (once per touch or press: holding on doesn't keep changing it)."""
    down = game['boot'].value() == 0
    three = club.three_fingers()
    if (down and not game['boot_was_down']) or (three and not game['three_was_down']):
        game['sound_mode'] = (game['sound_mode'] + 1) % len(SOUND_MODES)
        game['note'].set_text(SOUND_MODES[game['sound_mode']])
        game['note'].remove_flag(lv.obj.FLAG.HIDDEN)
        game['note_until'] = time.ticks_add(time.ticks_ms(), 1500)
    game['boot_was_down'] = down
    game['three_was_down'] = three
    if time.ticks_diff(time.ticks_ms(), game['note_until']) >= 0:
        game['note'].add_flag(lv.obj.FLAG.HIDDEN)


def step(timer):
    # Stop quietly if our screen has gone (another program was started).
    try:
        if not game['screen'].is_valid():
            raise ValueError
    except Exception:
        timer.delete()
        return
    try:
        move_everything()
        gc.collect()              # a little every frame, not a pause later
    except Exception as e:        # an error here must not stop LVGL itself
        timer.delete()
        club.print('Simple Shooter stopped: %s' % e, 0xFF6060)


def move_everything():
    now = time.ticks_ms()
    dt = min(3.0, time.ticks_diff(now, game['last']) / 33)   # 1.0 at 30 fps
    game['last'] = now
    ship = game['ship']
    check_sound_toggle()
    if (game['playing'] and not game['touching']
            and time.ticks_diff(now, game['last_touch']) > IDLE_TO_DEMO_MS):
        start_demo()

    # Stars: fall at their own speed, wrap round to the top.
    for star in game['stars']:
        star.y += star.vy * dt
        if star.y > H:
            star.y -= H + 4 * U
            star.x = random.randrange(W)
        star.obj.set_pos(int(star.x), int(star.y))

    # The ship: back after an explosion, then towards the finger, firing.
    if not ship.alive and time.ticks_diff(now, game['ship_back_at']) >= 0:
        ship.show(W / 2, H - 40 * U)
        game['safe_until'] = time.ticks_add(now, 1500)
    if ship.alive:
        flying = game['touching'] or not game['playing']
        if flying:
            tx, ty = game['target'] if game['playing'] else autopilot()
            for axis, target in (('x', tx), ('y', max(SHIP_Y_MIN, ty - 30 * U))):
                pos = getattr(ship, axis)
                move = max(-7 * U * dt, min(7 * U * dt, target - pos))
                setattr(ship, axis, pos + move)
            if time.ticks_diff(now, game['next_shot']) >= 0:
                shot = free_one(game['shots'])
                if shot is not None:
                    if game['playing']:
                        clear_stale_score()
                    shot.show(ship.x, ship.y - 18 * U)
                    sound(game['sound_fire'], 45)
                game['next_shot'] = time.ticks_add(now, FIRE_EVERY_MS)
        ship.place()
        # blink while it can't be hit yet (and only tell LVGL when it changes:
        # every change means a redraw)
        safe = time.ticks_diff(game['safe_until'], now) > 0
        opa = 110 if safe and (now // 120) % 2 else 255
        if opa != game['ship_opa']:
            ship.obj.set_style_image_opa(opa, 0)
            game['ship_opa'] = opa

    # Shots fly up.
    for shot in game['shots']:
        if shot.alive:
            shot.y -= 9 * U * dt
            if shot.y < -10 * U:
                shot.hide()
            else:
                shot.place()

    # New things fall now and then -- more often as the score goes up.
    if time.ticks_diff(now, game['next_rock']) >= 0:
        spawn_rock()
        gap = int(1100 / difficulty())
        game['next_rock'] = time.ticks_add(now, random.randint(gap // 2, gap))

    # Rocks fall, get shot, or hit the ship.
    for rock in game['rocks']:
        if not rock.alive:
            continue
        rock.x += rock.vx * dt
        rock.y += rock.vy * dt
        if rock.y > H + 30 * U or rock.x < -30 * U or rock.x > W + 30 * U:
            rock.hide()
            continue
        rock.place()
        for shot in game['shots']:
            if shot.alive and shot.hits(rock):
                shot.hide()
                rock.hide()
                add_score(rock.kind[1])
                big = rock.kind is BIG_ROCKS
                explode(rock.x, rock.y, big)
                if big:                    # a shower of small rocks
                    for i in range(random.randint(3, 4)):
                        spawn_rock(SMALL_ROCKS, rock.x, rock.y,
                                   random.uniform(-2.2, 2.2) * U, random.uniform(0.5, 2.5) * U)
                break
        if (rock.alive and ship.alive and rock.hits(ship, 0.55)
                and time.ticks_diff(now, game['safe_until']) >= 0):
            rock.hide()
            ship_hit()

    # Explosions: next picture every other step, then gone.
    for boom in game['booms']:
        if boom.alive:
            boom.frame += 0.5 * dt
            f = int(boom.frame)
            if f >= len(game['boom_frames']):
                boom.hide()
            else:
                boom.obj.set_src(game['boom_frames'][f])


def on_touch(event):
    code = event.get_code()
    if code == lv.EVENT.PRESSED or code == lv.EVENT.PRESSING:
        point = lv.point_t()
        lv.indev_active().get_point(point)
        if point.y >= CONTROL_TOP:
            if not game['playing']:
                start_playing()                # the player takes over
            game['touching'] = True
            game['target'] = (point.x, point.y)
            game['last_touch'] = time.ticks_ms()
        else:
            game['touching'] = False
    else:                                      # RELEASED or PRESS_LOST
        game['touching'] = False


def run():
    global W, H, U, ZOOM, CONTROL_TOP, SHIP_Y_MIN
    club.init()
    random.seed(time.ticks_ms())
    W, H = club.display.width(), club.display.height()
    U = min(W, H) / 240
    ZOOM = U / SPRITE_U
    if abs(ZOOM - 1) < 0.1:
        ZOOM = 1.0                  # near enough: no zooming, sharper and faster
    CONTROL_TOP = H * 190 // 320
    SHIP_Y_MIN = H * 200 // 320

    gc.collect()                # tidy memory before loading ~80 KB of assets
    # Sounds first, while memory is in big pieces (see help('sound')).
    game['sound_fire'] = load_sound('fire')
    game['sound_hit'] = load_sound('hit')
    game['sound_boom'] = load_sound('boom')
    club.effect(game['sound_fire'], 0)         # starts the sound player, silently

    sprites = {'ship': load_sprite('ship')}
    for kind in KINDS:
        for name in kind[0]:
            sprites[name] = load_sprite(name)
    game['sprites'] = sprites
    booms = [load_sprite('boom%d' % i) for i in range(6)]
    game['boom_frames'] = [b[0] for b in booms]
    game['boom_size'] = booms[0][1]

    scr = lv.obj()
    scr.set_style_bg_color(lv.color_hex(0x05060F), 0)
    scr.remove_flag(lv.obj.FLAG.SCROLLABLE)
    for code in (lv.EVENT.PRESSED, lv.EVENT.PRESSING, lv.EVENT.RELEASED, lv.EVENT.PRESS_LOST):
        scr.add_event_cb(on_touch, code, None)
    game['screen'] = scr

    # Stars: small grey squares; the faster ones are brighter (closer).
    def make_star():
        star = lv.obj(scr)
        star.remove_style_all()
        star.set_style_bg_opa(lv.OPA.COVER, 0)
        return star
    game['stars'] = make_pool(STARS, make_star)
    for star in game['stars']:
        near = random.random()
        size = max(1, int((2 if near > 0.7 else 1) * U))
        grey = int(90 + 160 * near)
        star.obj.set_size(size, size)
        star.obj.set_style_bg_color(lv.color_make(grey, grey, min(255, grey + 30)), 0)
        star.vy = (0.6 + 2.4 * near) * U
        star.show(random.randrange(W), random.randrange(H))

    # Shots: thin glowing bars.
    def make_shot():
        shot = lv.obj(scr)
        shot.remove_style_all()
        shot.set_size(int(3 * U), int(10 * U))
        shot.set_style_radius(int(2 * U), 0)
        shot.set_style_bg_opa(lv.OPA.COVER, 0)
        shot.set_style_bg_color(lv.color_hex(0x7FF7FF), 0)
        return shot
    game['shots'] = make_pool(MAX_SHOTS, make_shot)
    for shot in game['shots']:
        shot.w, shot.h = int(3 * U), int(10 * U)

    game['rocks'] = make_pool(MAX_ROCKS, lambda: sprite(scr))

    ship = Thing(sprite(scr))
    picture, ship.w, ship.h = sprites['ship']
    ship.obj.set_src(picture)
    ship.obj.remove_flag(lv.obj.FLAG.CLICKABLE)
    game['ship'] = ship

    game['booms'] = make_pool(MAX_BOOMS, lambda: sprite(scr))

    score = lv.label(scr)
    score.set_style_text_font(club.font(28), 0)
    score.set_style_text_color(lv.color_hex(0xFFC400), 0)
    score.align(lv.ALIGN.TOP_MID, 0, club.dp(6))
    score.set_text('0')
    game['score_label'] = score
    game['score'] = 0
    game['score_stale'] = False    # True from a crash until the next ship fires

    best = lv.label(scr)
    best.set_style_text_color(lv.color_hex(0x8899A6), 0)
    best.align(lv.ALIGN.TOP_RIGHT, -club.dp(10), club.dp(12))
    best.set_text('HI 0')
    game['best_label'] = best
    game['best'] = 0

    hint = lv.label(scr)
    hint.set_style_text_color(lv.color_hex(0x8899A6), 0)
    hint.set_text('touch and hold down here to play\n'
                  'three fingers on the screen: sound on/off')
    hint.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
    hint.align(lv.ALIGN.BOTTOM_MID, 0, -club.dp(12))
    game['hint'] = hint

    note = lv.label(scr)                       # 'sound off' etc., briefly
    note.set_style_text_color(lv.color_hex(0xE6EDF3), 0)
    note.set_style_bg_color(lv.color_hex(0x203040), 0)
    note.set_style_bg_opa(lv.OPA.COVER, 0)
    note.set_style_pad_all(club.dp(6), 0)
    note.align(lv.ALIGN.TOP_LEFT, club.dp(10), club.dp(12))
    note.add_flag(lv.obj.FLAG.HIDDEN)
    game['note'] = note
    game['boot'] = machine.Pin(club.BOOT_BUTTON, machine.Pin.IN)

    now = time.ticks_ms()
    game.update(touching=False, target=(W / 2, H - 40 * U), last=now, next_shot=now,
                next_rock=now, ship_back_at=now, safe_until=now, ship_opa=255,
                last_touch=now, sound_mode=0, boot_was_down=True,
                three_was_down=True, note_until=now)
    start_demo()
    ship.show(W / 2, H - 40 * U)

    lv.screen_load_anim(scr, lv.SCREEN_LOAD_ANIM.NONE, 0, 0, True)
    # Keep the timer: LVGL calls step() through it, and a timer nothing in
    # Python refers to could have its callback cleaned up from under it.
    game['timer'] = lv.timer_create(step, 33, None)
