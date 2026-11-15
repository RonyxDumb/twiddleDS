#!/usr/bin/env python3
"""Twiddlefinger DS asset converter.

Usage:  python3 tools/convert.py <path to fnf-TwiddleFinger checkout>

Reads the original HaxeFlixel/Psych Engine mod assets and writes:
  nitrofiles/...          binary data loaded at runtime from NitroFS
  maxmod_data/*.wav       sound effects (built into a maxmod soundbank)
  source/gen_assets.c     generated metadata tables
  include/gen_assets.h
"""
import json
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = sys.argv[1] if len(sys.argv) > 1 else '/home/user/fixeddata/fnf-twiddlefinger'
A = os.path.join(MOD, 'assets')
EX = os.path.join(MOD, 'example_mods')
OUT = os.path.join(ROOT, 'nitrofiles')

C_OUT = []      # generated C source
H_OUT = []      # generated header


def outpath(*p):
    path = os.path.join(OUT, *p)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def cfloat(v):
    return repr(float(v)) + 'f'


# ---------------------------------------------------------------------------
# Sheets: groups of textures + frames made of textured parts.
# ---------------------------------------------------------------------------
class Sheet:
    def __init__(self, name, scale, fmt=FMT_256COL, atlas_w=256, colors=255, dither=False):
        self.name = name
        self.scale = scale          # texels per drawn unit
        self.fmt = fmt
        self.atlas_w = atlas_w
        self.ncolors = colors
        self.dither = dither
        self.items = []             # (frame name, kind, image, origin)

    def add_tiled(self, fname, img, ox=0, oy=0):
        """Big image split in power-of-two tiles. (ox, oy) = position in
        texels of the image's top-left relative to the frame origin."""
        self.items.append((fname, 'tiled', img, (ox, oy)))

    def add_packed(self, fname, img, ox=0, oy=0):
        self.items.append((fname, 'packed', img, (ox, oy)))

    def build(self):
        imgs = [it[2] for it in self.items]
        if self.fmt == FMT_256COL:
            palimg, colors = build_palette(imgs, self.ncolors)
        elif self.fmt == FMT_16COL:
            palimg, colors = build_palette(imgs, 15)
        else:
            raise ValueError
        textures = []   # (w, h, bytes per pixel index array)
        frames = []
        parts = []
        # packed atlas state
        atlas = None

        def new_atlas():
            return {'img': Image.new('RGBA', (self.atlas_w, 256), (0, 0, 0, 0)),
                    'x': 0, 'y': 0, 'rowh': 0, 'used_h': 0, 'tex': None, 'members': []}
        atlases = []
        for fname, kind, img, (ox, oy) in self.items:
            if kind == 'tiled':
                w, h = img.size
                first = len(parts)
                ys = 0
                for th in pow2_pieces(h):
                    xs = 0
                    for tw in pow2_pieces(w):
                        tile = Image.new('RGBA', (tw, th), (0, 0, 0, 0))
                        tile.paste(img.crop((xs, ys, min(xs + tw, w), min(ys + th, h))), (0, 0))
                        cw = min(tw, w - xs)
                        ch = min(th, h - ys)
                        textures.append(('img', tile))
                        parts.append((len(textures) - 1, 0, 0, cw, ch, ox + xs, oy + ys))
                        xs += tw
                    ys += th
                frames.append((fname, first, len(parts) - first))
            else:
                w, h = img.size
                if atlas is None or atlas['x'] + w > self.atlas_w:
                    if atlas is not None:
                        atlas['y'] += atlas['rowh'] + 1
                        atlas['x'] = 0
                        atlas['rowh'] = 0
                if atlas is None or atlas['y'] + h > 256:
                    atlas = new_atlas()
                    atlases.append(atlas)
                    textures.append(('atlas', atlas))
                    atlas['tex'] = len(textures) - 1
                atlas['img'].paste(img, (atlas['x'], atlas['y']))
                parts.append((atlas['tex'], atlas['x'], atlas['y'], w, h, ox, oy))
                frames.append((fname, len(parts) - 1, 1))
                atlas['x'] += w + 1
                atlas['rowh'] = max(atlas['rowh'], h)
                atlas['used_h'] = max(atlas['used_h'], atlas['y'] + h)
        # finalize textures
        blob = bytearray()
        pal = pal_bytes(colors, 256 if self.fmt == FMT_256COL else 16)
        blob += pal
        texdefs = []
        for kind, obj in textures:
            if kind == 'atlas':
                hh = 8
                while hh < obj['used_h']:
                    hh *= 2
                timg = obj['img'].crop((0, 0, self.atlas_w, hh))
            else:
                timg = obj
            tw, th = timg.size
            idx = index_image(timg, palimg, dither=self.dither)
            if self.fmt == FMT_16COL:
                packed = bytearray(len(idx) // 2)
                for i in range(0, len(idx), 2):
                    packed[i // 2] = (idx[i] & 15) | ((idx[i + 1] & 15) << 4)
                idx = packed
            off = len(blob)
            blob += idx
            while len(blob) % 4:
                blob.append(0)
            texdefs.append((self.fmt, log2size(tw), log2size(th), off, len(idx)))
        fn = 'gfx/%s.bin' % self.name
        with open(outpath(fn), 'wb') as f:
            f.write(blob)
        # C output
        n = self.name
        C_OUT.append('static const TexDef %s_tex[] = {%s};' % (n, ','.join(
            '{%d,%d,%d,0,%d,%d}' % t for t in texdefs)))
        C_OUT.append('static const PartDef %s_parts[] = {%s};' % (n, ','.join(
            '{%d,0,%d,%d,%d,%d,%d,%d}' % p for p in parts)))
        C_OUT.append('static const FrameDef %s_frames[] = {%s};' % (n, ','.join(
            '{%d,%d}' % (f[1], f[2]) for f in frames)))
        C_OUT.append('const SheetDef sheet_%s = {%s, %s, %d, %d, %d, 0, %s_tex, %s_frames, %s_parts};' % (
            n, cstr('/' + fn), cfloat(self.scale), len(texdefs), len(frames),
            256 if self.fmt == FMT_256COL else 16, n, n, n))
        H_OUT.append('extern const SheetDef sheet_%s;' % n)
        for i, f in enumerate(frames):
            H_OUT.append('#define %s_%s %d' % (n.upper(), f[0].upper(), i))
        tot = sum(t[4] for t in texdefs)
        print('sheet %-8s %3d textures %3d frames %7d bytes vram' % (n, len(texdefs), len(frames), tot))
        return self


def scaled(img, s):
    w = max(1, int(round(img.width * s)))
    h = max(1, int(round(img.height * s)))
    return img.resize((w, h), Image.LANCZOS)


def trim(img):
    bb = img.getchannel('A').point(lambda a: 255 if a >= 40 else 0).getbbox()
    if bb is None:
        return img.crop((0, 0, 1, 1)), (0, 0)
    return img.crop(bb), (bb[0], bb[1])


# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------
STAGE_SCALE = 2.25


def convert_stage():
    img, frames = parse_sparrow(os.path.join(EX, 'images/bgasset.xml'), os.path.join(EX, 'images/bgasset.png'))
    byname = {f['name']: f for f in frames}
    # sky (separate low-res sheet)
    sky = full_frame(img, byname['sky'])
    S_SKY = 256.0 / (sky.width * STAGE_SCALE)
    sh = Sheet('sky', S_SKY)
    sky_s = sky.resize((256, int(round(sky.height * STAGE_SCALE * S_SKY))), Image.LANCZOS)
    sh.add_tiled('main', sky_s)
    sh.build()

    # main building composite (bg1 + bg2 + bg3, all share the same source box)
    S = 0.065
    comp = Image.new('RGBA', (byname['sky']['fw'], byname['sky']['fh']), (0, 0, 0, 0))
    for n in ('bg1', 'bg2', 'bg3'):
        comp.alpha_composite(full_frame(img, byname[n]))
    t, (bx, by) = trim(comp)
    s = STAGE_SCALE * S
    t_s = scaled(t, s)
    sh = Sheet('stage', S)
    sh.add_tiled('main', t_s, int(round(bx * s)), int(round(by * s)))
    sh.build()

    # tree (16 colors)
    tree = full_frame(img, byname['tree'])
    t, (bx, by) = trim(tree)
    S_T = 0.06
    s = STAGE_SCALE * S_T
    sh = Sheet('tree', S_T, fmt=FMT_16COL)
    sh.add_tiled('main', scaled(t, s), int(round(bx * s)), int(round(by * s)))
    sh.build()

    # fake intro background
    og = Image.open(os.path.join(EX, 'images/og.png')).convert('RGBA')
    S_O = 0.065
    sh = Sheet('og', S_O)
    sh.add_tiled('main', scaled(og, 1.75 * S_O))
    sh.build()

    H_OUT.append('#define STAGE_SKY_W %s' % cfloat(sky.width * STAGE_SCALE))
    H_OUT.append('#define STAGE_SKY_H %s' % cfloat(sky.height * STAGE_SCALE))


# ---------------------------------------------------------------------------
# Characters
# ---------------------------------------------------------------------------
CHARS = [
    # name, storage scale (texels per world unit)
    ('nugget', 0.12),
    ('nugget_hat', 0.104),
    ('maxpro', 0.12),
    ('rednugget', 0.12),
    ('rednughat', 0.112),
    ('redmax', 0.07),
]
ICONS = ['nug', 'nuggetcool', 'max', 'face']


def convert_characters():
    H_OUT.append('enum { %s, CHR_COUNT };' % ', '.join('CHR_' + c[0].upper() for c in CHARS))
    defs = []
    for cname, ts in CHARS:
        d = json.load(open(os.path.join(EX, 'characters', cname + '.json')))
        base = os.path.join(EX, 'images', d['image'])
        img, frames = parse_sparrow(base + '.xml', base + '.png')
        sc = d['scale']
        f0 = frames[0]
        # Psych: setGraphicSize(Std.int(width * scale)); updateHitbox()
        width = int(f0['fw'] * sc)
        sx = width / f0['fw']
        sy = sx
        height = f0['fh'] * sy
        ox, oy = f0['fw'] / 2.0, f0['fh'] / 2.0       # origin (centerOrigin)
        uniq = {}
        uframes = []
        anims = []
        for a in d['animations']:
            sel = frames_by_prefix(frames, a['name'])
            if a['indices']:
                sel = [sel[i] for i in a['indices'] if i < len(sel)]
            ids = []
            for f in sel:
                key = (f['x'], f['y'], f['w'], f['h'], f['fx'], f['fy'], f['rot'])
                if key not in uniq:
                    uniq[key] = len(uframes)
                    uframes.append(f)
                ids.append(uniq[key])
            anims.append((a['anim'], a['fps'], a['loop'], a['offsets'], ids))
        # scale and trim frames
        crops = []
        meta = []
        for f in uframes:
            c = crop_frame(img, f)
            cs = c.resize((max(1, int(round(c.width * sx * ts))), max(1, int(round(c.height * sy * ts)))), Image.LANCZOS)
            # world-space top-left of the trimmed image relative to (x - offset.x)
            lx, ly = -f['fx'], -f['fy']
            dx = ox + (lx - ox) * sx
            dy = oy + (ly - oy) * sy
            crops.append(cs)
            meta.append((dx, dy, c.width * sx, c.height * sy))
        palimg, colors = build_palette(crops, 255)
        blob = bytearray(pal_bytes(colors, 256))
        cframes = []
        maxw = maxh = 0
        for cs, (dx, dy, ww, wh) in zip(crops, meta):
            w4 = (cs.width + 3) & ~3
            pad = Image.new('RGBA', (w4, cs.height), (0, 0, 0, 0))
            pad.paste(cs, (0, 0))
            idx = index_image(pad, palimg)
            cframes.append((len(blob), w4, cs.height, cs.width, dx, dy, ww, wh))
            blob += idx
            maxw = max(maxw, w4)
            maxh = max(maxh, cs.height)
        fn = 'chr/%s.bin' % cname
        with open(outpath(fn), 'wb') as fo:
            fo.write(blob)
        n = 'chr_' + cname
        C_OUT.append('static const ChrFrame %s_frames[] = {%s};' % (n, ','.join(
            '{%d,%d,%d,%d,0,%s,%s,%s,%s}' % (o, w4, h, cw, cfloat(dx), cfloat(dy), cfloat(ww), cfloat(wh))
            for (o, w4, h, cw, dx, dy, ww, wh) in cframes)))
        for ai, (an, fps, loop, offs, ids) in enumerate(anims):
            C_OUT.append('static const u16 %s_a%d[] = {%s};' % (n, ai, ','.join(map(str, ids))))
        C_OUT.append('static const ChrAnim %s_anims[] = {%s};' % (n, ','.join(
            '{%s,%d,%d,%d,%d,%d,%s_a%d}' % (cstr(an), fps, 1 if loop else 0, offs[0], offs[1], len(ids), n, ai)
            for ai, (an, fps, loop, offs, ids) in enumerate(anims))))
        icon = ICONS.index(d['healthicon']) if d['healthicon'] in ICONS else 0
        C_OUT.append('const ChrDef %s = {%s, %s, %d, %d, %s, %s, %s, %s, %s, %d, %d, %d, %s_frames, %d, %s_anims};' % (
            n, cstr('/' + fn), cfloat(ts), log2size(maxw), log2size(maxh), cfloat(width), cfloat(height),
            cfloat(d['camera_position'][0]), cfloat(d['camera_position'][1]), cfloat(d['sing_duration']),
            icon, 1 if d.get('flip_x') else 0, len(cframes), n, len(anims), n))
        defs.append(n)
        print('char %-11s %2d frames slot %dx%d  ram %d' % (cname, len(cframes), 8 << log2size(maxw), 8 << log2size(maxh), len(blob)))
    C_OUT.append('const ChrDef *const chr_defs[] = {%s};' % ','.join('&' + d for d in defs))
    H_OUT.append('extern const ChrDef *const chr_defs[];')


# ---------------------------------------------------------------------------
# HUD: notes, battery, icons, help button
# ---------------------------------------------------------------------------
S_HUD = 0.8 * K


def convert_hud():
    sh = Sheet('hud', S_HUD)
    img, frames = parse_sparrow(os.path.join(A, 'shared/images/noteSkins/NOTE_assets.xml'),
                                os.path.join(A, 'shared/images/noteSkins/NOTE_assets.png'))
    s = 0.7 * S_HUD
    dirs = ['left', 'down', 'up', 'right']
    colors = ['purple', 'blue', 'green', 'red']
    arrows = ['arrowLEFT', 'arrowDOWN', 'arrowUP', 'arrowRIGHT']

    ARROW_RGB = [[0xC24B99, 0xFFFFFF, 0x3C1F56], [0x00FFFF, 0xFFFFFF, 0x1542B7],
                 [0x12FA05, 0xFFFFFF, 0x0A4447], [0xF9393F, 0xFFFFFF, 0x651038]]

    def rgb_shader(im, lane):
        # Psych 0.7 RGBPalette shader: rgb = min(r*R + g*G + b*B, 1)
        cols = [((c >> 16) & 255, (c >> 8) & 255, c & 255) for c in ARROW_RGB[lane]]
        px = im.load()
        for y in range(im.height):
            for x in range(im.width):
                r, g, b, a = px[x, y]
                if a == 0:
                    continue
                out = []
                for ch in range(3):
                    v = (r * cols[0][ch] + g * cols[1][ch] + b * cols[2][ch]) / 255.0
                    out.append(min(255, int(v)))
                px[x, y] = (out[0], out[1], out[2], a)
        return im

    def add_centered(name, f, lane=-1):
        full = full_frame(img, f)
        if lane >= 0:
            full = rgb_shader(full, lane)
        t, (bx, by) = trim(full)
        ts = scaled(t, s)
        # origin = center of the untrimmed source box
        ox = (bx - full.width / 2.0) * s
        oy = (by - full.height / 2.0) * s
        sh.add_packed(name, ts, int(round(ox)), int(round(oy)))

    for i in range(4):
        add_centered('static%d' % i, frames_by_prefix(frames, arrows[i])[0])
        add_centered('note%d' % i, frames_by_prefix(frames, colors[i] + '0')[0], i)
        for j, f in enumerate(frames_by_prefix(frames, dirs[i] + ' press')):
            add_centered('press%d_%d' % (i, j), f, i)
        for j, f in enumerate(frames_by_prefix(frames, dirs[i] + ' confirm')):
            add_centered('confirm%d_%d' % (i, j), f, i)
    # hold pieces: keep top-left origin, stretched at runtime
    endnames = {'purple': 'pruple end hold', 'blue': 'blue hold end', 'green': 'green hold end', 'red': 'red hold end'}
    for i in range(4):
        for nm, key in (('hold', colors[i] + ' hold piece'), ('end', endnames[colors[i]])):
            sel = frames_by_prefix(frames, key)
            if not sel:
                sel = frames_by_prefix(frames, colors[i] + ' hold end')
            f = sel[0]
            c = rgb_shader(crop_frame(img, f), i)
            cs = scaled(c, s)
            sh.add_packed('%s%d' % (nm, i), cs, -cs.width // 2, 0)
    # battery bar
    hp = Image.open(os.path.join(EX, 'images/hpbar.png')).convert('RGBA')
    sh.add_packed('hpbar', scaled(hp, S_HUD))
    bw = Image.open(os.path.join(EX, 'images/barwhiteohio.png')).convert('RGBA')
    sh.add_packed('barwhite', scaled(bw, S_HUD))
    # icons (2 frames each)
    for ic in ICONS:
        p = os.path.join(EX, 'images/icons', ic + '.png')
        if not os.path.exists(p):
            p = os.path.join(A, 'shared/images/icons/icon-' + ic + '.png')
        im = Image.open(p).convert('RGBA')
        for k in range(2):
            fr = im.crop((k * 150, 0, k * 150 + 150, 150)) if im.width >= 300 else im
            sh.add_packed('icon_%s_%d' % (ic, k), scaled(fr, S_HUD))
    here = Image.open(os.path.join(EX, 'images/here2.png')).convert('RGBA')
    hs = scaled(here, 0.35 * S_HUD)
    sh.add_packed('here', hs, -hs.width // 2, -hs.height // 2)
    sh.build()
    H_OUT.append('#define HPBAR_W %s' % cfloat(hp.width))
    H_OUT.append('#define HPBAR_H %s' % cfloat(hp.height))
    H_OUT.append('#define HERE_W %s' % cfloat(here.width * 0.35))
    H_OUT.append('#define HERE_H %s' % cfloat(here.height * 0.35))


# ---------------------------------------------------------------------------
# Menu (MaxMenu)
# ---------------------------------------------------------------------------
def convert_menu():
    sh = Sheet('menu', K)
    # hisface/eye: 1800x1800 centered on the 1280x720 screen. Only the part
    # visible on the DS (center 960x720 virtual) is stored.
    for nm in ('hisface', 'eye'):
        im = Image.open(os.path.join(A, 'shared/images', nm + '.png')).convert('RGBA')
        x0 = (im.width - 960) // 2
        y0 = (im.height - 720) // 2
        crop = im.crop((x0, y0, x0 + 960, y0 + 720)).resize((256, 192), Image.LANCZOS)
        t, (bx, by) = trim(crop)
        sh.add_tiled(nm, t, bx - 128, by - 96)
    # title texts (Knicknack 48px)
    font = ImageFont.truetype(os.path.join(A, 'fonts/Knicknack Regular.otf'), 48)
    for nm, col in (('title_dark', (0x24, 0x23, 0x20, 255)), ('title', (255, 255, 255, 255))):
        tmp = Image.new('RGBA', (1280, 120), (0, 0, 0, 0))
        dr = ImageDraw.Draw(tmp)
        tw = dr.textlength('MAX DESIGN PRO', font=font)
        dr.text(((1280 - tw) / 2, 0), 'MAX DESIGN PRO', font=font, fill=col)
        t, (bx, by) = trim(tmp)
        ts = scaled(t, K)
        sh.add_packed(nm, ts, int(round((bx - 640) * K)), int(round(by * K)))
    img, frames = parse_sparrow(os.path.join(A, 'shared/images/buttons.xml'), os.path.join(A, 'shared/images/buttons.png'))
    for f in frames:
        c = crop_frame(img, f)
        sh.add_packed(f['name'], scaled(c, K), int(round(-f['fx'] * K)), int(round(-f['fy'] * K)))
    sh.add_packed('pixel', Image.new('RGBA', (8, 8), (0x24, 0x23, 0x20, 255)))
    sh.build()
    # height of the FlxText (for the separator line position)
    H_OUT.append('#define MENU_TEXT_H %s' % cfloat(font.getbbox('Mg')[3] + 6))


# ---------------------------------------------------------------------------
# End images
# ---------------------------------------------------------------------------
def convert_end():
    for nm, fn in (('goodend', 'goodEND'), ('badend', 'badEND')):
        im = Image.open(os.path.join(EX, 'images', fn + '.png')).convert('RGBA')
        # setGraphicSize(0, FlxG.height) + screenCenter
        h = 192
        w = int(round(im.width * 192.0 / im.height))
        im = im.resize((w, h), Image.LANCZOS)
        if w > 256:
            x0 = (w - 256) // 2
            im = im.crop((x0, 0, x0 + 256, 192))
            ox = -128
        else:
            ox = -w // 2
        sh = Sheet(nm, K)
        sh.add_tiled('main', im, ox, -96)
        sh.build()


# ---------------------------------------------------------------------------
# Fonts: 8-bit alpha glyph atlases (used for 3D A5I3 textures and the
# software-rendered bottom screen).
# ---------------------------------------------------------------------------
FONTS = [
    ('vcr', os.path.join(A, 'fonts/vcr.ttf'), 10),
    ('vcrbig', os.path.join(A, 'fonts/vcr.ttf'), 16),
    ('subs', os.path.join(EX, 'fonts/mottrfmn.ttf'), 14),
    ('knick', os.path.join(A, 'fonts/Knicknack Regular.otf'), 18),
]


def convert_fonts():
    H_OUT.append('enum { %s, FONT_COUNT };' % ', '.join('FONT_' + f[0].upper() for f in FONTS))
    names = []
    for nm, path, size in FONTS:
        font = ImageFont.truetype(path, size)
        asc, desc = font.getmetrics()
        lh = asc + desc
        glyphs = []
        for ch in range(32, 127):
            c = chr(ch)
            bb = font.getbbox(c)
            adv = int(round(font.getlength(c)))
            w = max(1, bb[2] - min(0, bb[0]))
            gi = Image.new('L', (w + 2, lh + 2), 0)
            ImageDraw.Draw(gi).text((1 - min(0, bb[0]), 1), c, font=font, fill=255)
            bbx = gi.getbbox()
            if bbx is None:
                glyphs.append((ch, None, 0, 0, adv))
                continue
            g = gi.crop(bbx)
            glyphs.append((ch, g, bbx[0] - 1 + min(0, bb[0]), bbx[1] - 1, adv))
        # pack into 128 or 256 wide atlas
        W = 256
        x = y = rowh = 0
        place = []
        for ch, g, ox, oy, adv in glyphs:
            if g is None:
                place.append((0, 0, 0, 0, 0, 0, adv))
                continue
            if x + g.width > W:
                x = 0
                y += rowh + 1
                rowh = 0
            place.append((x, y, g.width, g.height, ox, oy, adv))
            x += g.width + 1
            rowh = max(rowh, g.height)
        H = 8
        while H < y + rowh:
            H *= 2
        atlas = Image.new('L', (W, H), 0)
        for (ch, g, *_), (px, py, *_) in zip(glyphs, place):
            if g is not None:
                atlas.paste(g, (px, py))
        fn = 'font/%s.bin' % nm
        with open(outpath(fn), 'wb') as fo:
            fo.write(atlas.tobytes())
        C_OUT.append('static const Glyph font_%s_g[] = {%s};' % (nm, ','.join(
            '{%d,%d,%d,%d,%d,%d,%d}' % p for p in place)))
        C_OUT.append('const FontDef font_%s = {%s, %d, %d, %d, %d, font_%s_g};' % (
            nm, cstr('/' + fn), log2size(W), log2size(H), lh, asc, nm))
        names.append('&font_' + nm)
        print('font %-6s %dx%d' % (nm, W, H))
    C_OUT.append('const FontDef *const font_defs[] = {%s};' % ','.join(names))
    H_OUT.append('extern const FontDef *const font_defs[];')


# ---------------------------------------------------------------------------
# Bottom-screen images (RGB15 + alpha bit)
# ---------------------------------------------------------------------------
def write_b16(name, im):
    im = im.convert('RGBA')
    out = bytearray()
    out += bytes((im.width & 255, im.width >> 8, im.height & 255, im.height >> 8))
    for (r, g, b, a) in list(im.convert('RGBA').tobytes()[i:i + 4] for i in range(0, im.width * im.height * 4, 4)):
        v = rgb15(r, g, b) | (0x8000 if a >= 128 else 0)
        out += bytes((v & 255, v >> 8))
    with open(outpath('sub', name + '.b16'), 'wb') as f:
        f.write(out)


def convert_sub():
    here = Image.open(os.path.join(EX, 'images/here2.png')).convert('RGBA')
    write_b16('here', scaled(here, 232.0 / here.width))
    hp = Image.open(os.path.join(EX, 'images/hpbar.png')).convert('RGBA')
    write_b16('hpbar', scaled(hp, 0.5))
    bw = Image.open(os.path.join(EX, 'images/barwhiteohio.png')).convert('RGBA')
    write_b16('barwhite', scaled(bw, 0.5))
    for ic in ICONS:
        p = os.path.join(EX, 'images/icons', ic + '.png')
        if not os.path.exists(p):
            p = os.path.join(A, 'shared/images/icons/icon-' + ic + '.png')
        im = Image.open(p).convert('RGBA')
        for k in range(2):
            fr = im.crop((k * 150, 0, k * 150 + 150, 150)) if im.width >= 300 else im
            write_b16('icon_%s_%d' % (ic, k), fr.resize((56, 56), Image.LANCZOS))
    face = Image.open(os.path.join(A, 'shared/images/hisface.png')).convert('RGBA')
    bg = Image.new('RGBA', (256, 192), (0, 0, 0, 255))
    f = face.crop((450, 450, 1350, 1125)).resize((256, 192), Image.LANCZOS)
    f.putalpha(f.getchannel('A').point(lambda a: a * 40 // 255))
    bg.alpha_composite(f)
    write_b16('menubg', bg)


# ---------------------------------------------------------------------------
# Chart + events
# ---------------------------------------------------------------------------
EVENTS = ['begin', 'tweenIn', 'tweenOut1', 'beatsPerZoom', 'infryzoom', 'addCamZoom', 'fire',
          'helpMe', 'judge', 'cutscene', 'subs', 'bringnotes', 'playBadVid', 'playGoodVid',
          'panCenter', 'panUP', 'fadeIMG']


def convert_chart():
    song = json.load(open(os.path.join(A, 'shared/data/twiddlefinger/twiddlefinger.json')))['song']
    evs = json.load(open(os.path.join(A, 'shared/data/twiddlefinger/events.json')))['song']['events']
    bpm = song['bpm']
    notes = []
    sections = []
    for s in song['notes']:
        sections.append(1 if s['mustHitSection'] else 0)
        for n in s['sectionNotes']:
            t, k, l = n[0], int(n[1]), n[2]
            player = (k < 4) == s['mustHitSection']
            notes.append((t, k % 4, l, 1 if player else 0))
    notes.sort()
    # drop exact duplicates (stacked notes)
    ded = []
    for n in notes:
        if ded and abs(ded[-1][0] - n[0]) < 1 and ded[-1][1] == n[1] and ded[-1][3] == n[3]:
            continue
        ded.append(n)
    notes = ded
    C_OUT.append('const NoteDef chart_notes[] = {%s};' % ','.join(
        '{%s,%s,%d,%d}' % (cfloat(t), cfloat(l), d, p) for t, d, l, p in notes))
    C_OUT.append('const u8 chart_sections[] = {%s};' % ','.join(map(str, sections)))
    out = []
    for t, lst in evs:
        for name, v1, v2 in lst:
            a = b = 0.0
            s = ''
            if name == '':
                key = v1
                if key == 'subs':
                    s = v2
            elif name == 'Set Property' and v1 == 'beatsPerZoom':
                key = 'beatsPerZoom'
                a = float(v2)
            elif name == 'infryzoom':
                key = 'infryzoom'
                a, b = float(v1), float(v2.strip())
            elif name == 'Add Camera Zoom':
                key = 'addCamZoom'
                a = float(v1) if v1 else 0.015
                b = float(v2) if v2 else 0.03
            else:
                print('unknown event', name, v1, v2)
                continue
            if key not in EVENTS:
                print('unknown event', key)
                continue
            out.append((t, EVENTS.index(key), a, b, s))
    out.sort(key=lambda e: e[0])
    C_OUT.append('const EventDef chart_events[] = {%s};' % ','.join(
        '{%s,%d,%s,%s,%s}' % (cfloat(t), k, cfloat(a), cfloat(b), cstr(s)) for t, k, a, b, s in out))
    H_OUT.append('enum { %s };' % ', '.join('EV_' + e.upper() for e in EVENTS))
    H_OUT.append('#define CHART_NOTES %d' % len(notes))
    H_OUT.append('#define CHART_SECTIONS %d' % len(sections))
    H_OUT.append('#define CHART_EVENTS %d' % len(out))
    H_OUT.append('#define CHART_BPM %s' % cfloat(bpm))
    H_OUT.append('#define CHART_SPEED %s' % cfloat(song['speed']))
    H_OUT.append('extern const NoteDef chart_notes[];')
    H_OUT.append('extern const u8 chart_sections[];')
    H_OUT.append('extern const EventDef chart_events[];')
    print('chart: %d notes, %d events' % (len(notes), len(out)))


# ---------------------------------------------------------------------------
# Audio
# ---------------------------------------------------------------------------
RATE = 22050


def ff(src, dst, extra=()):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, *extra, dst], check=True)


def convert_audio():
    for nm, src in (('inst', 'songs/twiddlefinger/Inst.ogg'), ('voices', 'songs/twiddlefinger/Voices.ogg'),
                    ('menu', 'shared/music/menu.ogg'), ('pause', 'shared/music/tea-time.ogg'),
                    ('gameover', 'shared/music/gameOver.ogg')):
        dst = outpath('snd', nm + '.raw')
        extra = ['-t', '60'] if nm == 'pause' else []
        ff(os.path.join(A, src), dst, extra + ['-ac', '1', '-ar', str(RATE), '-f', 's16le', '-acodec', 'pcm_s16le'])
    md = os.path.join(ROOT, 'maxmod_data')
    os.makedirs(md, exist_ok=True)
    for nm in ('missnote1', 'missnote2', 'missnote3', 'intro3', 'intro2', 'intro1', 'introGo',
               'scrollMenu', 'confirmMenu', 'cancelMenu', 'fnf_loss_sfx'):
        ff(os.path.join(A, 'shared/sounds', nm + '.ogg'), os.path.join(md, nm.lower() + '.wav'),
           ['-ac', '1', '-ar', '22050', '-acodec', 'pcm_s16le'])
    ff(os.path.join(A, 'shared/music/gameOverEnd.ogg'), os.path.join(md, 'gameoverend.wav'),
       ['-ac', '1', '-ar', '22050', '-acodec', 'pcm_s16le'])
    H_OUT.append('#define AUDIO_RATE %d' % RATE)


def main():
    H_OUT.append('#pragma once')
    H_OUT.append('#include "assets.h"')
    C_OUT.append('#include "gen_assets.h"')
    convert_chart()
    convert_stage()
    convert_characters()
    convert_hud()
    convert_menu()
    convert_end()
    convert_fonts()
    convert_sub()
    if '--no-audio' not in sys.argv:
        convert_audio()
    else:
        H_OUT.append('#define AUDIO_RATE %d' % RATE)
    with open(os.path.join(ROOT, 'source/gen_assets.c'), 'w') as f:
        f.write('// Generated by tools/convert.py - do not edit\n' + '\n'.join(C_OUT) + '\n')
    with open(os.path.join(ROOT, 'include/gen_assets.h'), 'w') as f:
        f.write('// Generated by tools/convert.py - do not edit\n' + '\n'.join(H_OUT) + '\n')


if __name__ == '__main__':
    main()
