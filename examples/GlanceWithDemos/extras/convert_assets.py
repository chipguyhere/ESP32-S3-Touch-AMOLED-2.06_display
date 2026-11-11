"""Convert Simple Shooter's LVGL RGB565A8 .bin sprites and 8-bit WAVs into a C
source file for the watch.  Sprites are pre-scaled by SCALE so LVGL never has
to zoom them at run time.

usage: convert_assets.py ASSET_DIR OUT_DIR SCALE [contact.png]"""
import sys, os, struct
from PIL import Image

src, out, scale = sys.argv[1], sys.argv[2], float(sys.argv[3])
contact = sys.argv[4] if len(sys.argv) > 4 else None

SPRITES = ['ship'] + ['rock_big%d' % i for i in range(3)] + ['rock%d' % i for i in range(6)] \
        + ['satellite%d' % i for i in range(3)] + ['crystal%d' % i for i in range(2)] \
        + ['boom%d' % i for i in range(6)]
SOUNDS = ['fire', 'hit', 'boom']

def decode(path):
    d = open(path, 'rb').read()
    assert d[0] == 0x19 and d[1] == 0x14, path            # LVGL 9 magic, RGB565A8
    w, h = struct.unpack('<HH', d[4:8]); stride = struct.unpack('<H', d[8:10])[0]
    px = d[12:]
    img = Image.new('RGBA', (w, h))
    a0 = stride * h
    for y in range(h):
        for x in range(w):
            v = px[y * stride + x * 2] | px[y * stride + x * 2 + 1] << 8
            r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
            img.putpixel((x, y), (r * 255 // 31, g * 255 // 63, b * 255 // 31, px[a0 + y * w + x]))
    return img

def encode(img):
    w, h = img.size
    col = bytearray(); alp = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = img.getpixel((x, y))
            v = ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)
            col += struct.pack('<H', v); alp.append(a)
    return bytes(col + alp)

def carray(name, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append('    ' + ','.join('0x%02x' % b for b in data[i:i + 24]) + ',')
    return 'static const uint8_t %s[%d] = {\n%s\n};\n' % (name, len(data), '\n'.join(lines))

c = ['/* shooter_assets.c - Simple Shooter\'s pictures and sounds, compiled in.',
     ' *',
     ' * GENERATED from the game\'s original assets (LVGL RGB565A8 .bin sprites drawn',
     ' * for 2 px per game unit, 8-bit 11025 Hz WAVs) by convert_assets.py: the',
     ' * sprites are pre-scaled by %.4f for this 410 px wide screen (1.708 px per' % scale,
     ' * game unit), so LVGL never zooms them while the game runs.  Sounds are the',
     ' * WAVs\' raw sample data: unsigned 8-bit, mono, 11025 Hz.',
     ' * Don\'t edit by hand. */', '',
     '#include "shooter_assets.h"', '']
h = ['/* shooter_assets.h - see shooter_assets.c (generated). */', '', '#pragma once', '',
     '#include "lvgl.h"', '', '#ifdef __cplusplus', 'extern "C" {', '#endif', '']
sheet = []
for n in SPRITES:
    im = decode(os.path.join(src, n + '.bin'))
    w, hh = im.size
    nw, nh = max(1, round(w * scale)), max(1, round(hh * scale))
    sm = im.convert('RGBa').resize((nw, nh), Image.LANCZOS).convert('RGBA')
    data = encode(sm)
    sheet.append((n, im, sm))
    c.append(carray('%s_map' % n, data))
    c.append('const lv_image_dsc_t shooter_img_%s = {\n'
             '    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8,\n'
             '                .flags = 0, .w = %d, .h = %d, .stride = %d },\n'
             '    .data_size = sizeof(%s_map),\n'
             '    .data = %s_map,\n};\n' % (n, nw, nh, nw * 2, n, n))
    h.append('extern const lv_image_dsc_t shooter_img_%s;   /* %dx%d */' % (n, nw, nh))
h.append('')
for n in SOUNDS:
    d = open(os.path.join(src, n + '.wav'), 'rb').read()
    i = 12; pcm = None
    while i < len(d):
        cid = d[i:i + 4]; sz = struct.unpack('<I', d[i + 4:i + 8])[0]
        if cid == b'fmt ':
            fmt, ch, rate, _, _, bits = struct.unpack('<HHIIHH', d[i + 8:i + 24])
            assert (fmt, ch, rate, bits) == (1, 1, 11025, 8), (n, fmt, ch, rate, bits)
        if cid == b'data': pcm = d[i + 8:i + 8 + sz]
        i += 8 + sz + (sz & 1)
    c.append(carray('snd_%s_pcm' % n, pcm))
    c.append('const shooter_sound_t shooter_snd_%s = { snd_%s_pcm, sizeof(snd_%s_pcm) };\n' % (n, n, n))
    h.append('extern const shooter_sound_t shooter_snd_%s;   /* %d samples */' % (n, len(pcm)))
h[h.index('#endif') + 2:h.index('#endif') + 2] = [
    '/* A sound: unsigned 8-bit mono samples at 11025 Hz. */',
    'typedef struct { const uint8_t *pcm; uint32_t len; } shooter_sound_t;', '']
h += ['', '#ifdef __cplusplus', '} /* extern "C" */', '#endif', '']
open(os.path.join(out, 'shooter_assets.c'), 'w').write('\n'.join(c))
open(os.path.join(out, 'shooter_assets.h'), 'w').write('\n'.join(h))

if contact:
    W = sum(max(o.size[0], s.size[0]) + 8 for _, o, s in sheet)
    Hh = max(o.size[1] + s.size[1] + 24 for _, o, s in sheet)
    cs = Image.new('RGBA', (W, Hh), (5, 6, 15, 255)); x = 0
    for n, o, s in sheet:
        cs.alpha_composite(o, (x, 0)); cs.alpha_composite(s, (x, o.size[1] + 16))
        x += max(o.size[0], s.size[0]) + 8
    cs.save(contact)
