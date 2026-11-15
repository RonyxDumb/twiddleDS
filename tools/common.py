"""Shared helpers for the Twiddlefinger DS asset converter."""
import math
import xml.etree.ElementTree as ET
from PIL import Image

# virtual (HaxeFlixel) screen -> DS screen factor. The 1280x720 game view is
# scaled so that its height fills the 192 px DS screen (sides are cropped).
K = 192.0 / 720.0

FMT_A3I5 = 1
FMT_4COL = 2
FMT_16COL = 3
FMT_256COL = 4
FMT_A5I3 = 6


def log2size(n):
    """DS texture size code: 0 -> 8 px ... 7 -> 1024 px."""
    s = 0
    while (8 << s) < n:
        s += 1
    return s


def pow2_pieces(n, maxdim=256):
    """Split a length into power-of-two pieces (>=8) that cover it."""
    out = []
    while n > 0:
        if n <= 8:
            out.append(8)
            break
        p = 1 << int(math.floor(math.log2(n)))
        p = min(p, maxdim)
        # if remainder is tiny, round up instead of adding another piece
        if n - p > 0 and n - p <= 8 and p * 2 <= maxdim and (p * 2 - n) < 16:
            p = p * 2
        out.append(p)
        n -= p
    return out


def rgb15(r, g, b):
    return ((r >> 3) & 31) | (((g >> 3) & 31) << 5) | (((b >> 3) & 31) << 10)


def parse_sparrow(xml_path, png_path):
    """Returns (image, list of dict(name, x, y, w, h, fx, fy, fw, fh, rot))."""
    img = Image.open(png_path).convert('RGBA')
    root = ET.parse(xml_path).getroot()
    frames = []
    for t in root.iter('SubTexture'):
        a = t.attrib
        x, y, w, h = int(a['x']), int(a['y']), int(a['width']), int(a['height'])
        rot = a.get('rotated') == 'true'
        fx = int(a.get('frameX', 0))
        fy = int(a.get('frameY', 0))
        if rot:
            fw = int(a.get('frameWidth', h))
            fh = int(a.get('frameHeight', w))
        else:
            fw = int(a.get('frameWidth', w))
            fh = int(a.get('frameHeight', h))
        frames.append(dict(name=a['name'], x=x, y=y, w=w, h=h, fx=fx, fy=fy,
                           fw=fw, fh=fh, rot=rot))
    return img, frames


def crop_frame(img, f):
    c = img.crop((f['x'], f['y'], f['x'] + f['w'], f['y'] + f['h']))
    if f['rot']:
        c = c.rotate(90, expand=True)
    return c


def full_frame(img, f):
    """Frame pasted into its untrimmed source box."""
    c = crop_frame(img, f)
    box = Image.new('RGBA', (f['fw'], f['fh']), (0, 0, 0, 0))
    box.alpha_composite(c, (-f['fx'], -f['fy']))
    return box


def frames_by_prefix(frames, prefix):
    """Flixel addByPrefix: frames whose name starts with prefix, sorted by
    the numeric suffix (flixel sorts by the trailing digits)."""
    sel = [f for f in frames if f['name'].startswith(prefix)]

    def key(f):
        n = f['name'][len(prefix):]
        digits = ''
        for ch in reversed(n):
            if ch.isdigit():
                digits = ch + digits
            else:
                break
        return int(digits) if digits else 0
    sel.sort(key=key)
    return sel


def build_palette(images, ncolors=255, alpha_thr=100):
    """Shared palette for RGBA images. Index 0 is reserved for transparency.
    Returns a PIL 'P' palette image and list of (r,g,b) colors (len<=255)."""
    pixels = []
    for im in images:
        rgba = im.convert('RGBA')
        data = rgba.getdata()
        pixels.extend(p[:3] for p in data if p[3] >= alpha_thr)
    if not pixels:
        pixels = [(0, 0, 0)]
    # sample to keep it fast
    step = max(1, len(pixels) // 400000)
    pixels = pixels[::step]
    side = int(math.ceil(math.sqrt(len(pixels))))
    tmp = Image.new('RGB', (side, side))
    tmp.putdata(pixels + [pixels[-1]] * (side * side - len(pixels)))
    q = tmp.quantize(colors=ncolors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:ncolors * 3]
    colors = [tuple(pal[i * 3:i * 3 + 3]) for i in range(len(pal) // 3)]
    while len(colors) < ncolors:
        colors.append(colors[-1] if colors else (0, 0, 0))
    # rebuild a palette image with exactly the used colors
    q = Image.new('P', (1, 1))
    flat = [c for col in colors for c in col]
    q.putpalette(flat + list(colors[0]) * ((768 - len(flat)) // 3))
    q.info['ncolors'] = ncolors
    return q, colors


def index_image(im, palimg, alpha_thr=100, dither=False):
    """Map RGBA image to palette indices (1..255), 0 = transparent."""
    rgba = im.convert('RGBA')
    rgb = Image.new('RGB', rgba.size)
    rgb.paste(rgba.convert('RGB'))
    q = rgb.quantize(palette=palimg, dither=Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE)
    idx = q.tobytes()
    alpha = rgba.getchannel('A').tobytes()
    out = bytearray(len(idx))
    nmax = palimg.info.get('ncolors', 255) - 1
    for i in range(len(idx)):
        out[i] = 0 if alpha[i] < alpha_thr else min(idx[i], nmax) + 1
    return out


def pal_bytes(colors, count=256):
    """Palette as RGB15 little endian, entry 0 = transparent black."""
    out = bytearray()
    full = [(0, 0, 0)] + list(colors)
    full = full[:count] + [(0, 0, 0)] * max(0, count - len(full))
    for (r, g, b) in full:
        v = rgb15(r, g, b)
        out += bytes((v & 255, v >> 8))
    return out


def lz10_compress(data):
    """GBA/DS BIOS LZ77 (type 0x10) compressor with hash chains."""
    n = len(data)
    out = bytearray((0x10, n & 255, (n >> 8) & 255, (n >> 16) & 255))
    i = 0
    head = {}
    prev = [-1] * n
    while i < n:
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best_len = 0
            best_disp = 0
            if i + 3 <= n:
                key = data[i:i + 3]
                j = head.get(key, -1)
                tries = 0
                maxlen = min(18, n - i)
                while j >= 0 and i - j <= 4096 and tries < 64:
                    if i - j >= 1:
                        l = 0
                        while l < maxlen and data[j + l] == data[i + l]:
                            l += 1
                        if l > best_len:
                            best_len = l
                            best_disp = i - j
                            if l == maxlen:
                                break
                    j = prev[j]
                    tries += 1
            if best_len >= 3:
                flags |= 0x80 >> bit
                d = best_disp - 1
                out.append(((best_len - 3) << 4) | (d >> 8))
                out.append(d & 255)
                for k in range(best_len):
                    if i + 3 <= n:
                        key = data[i:i + 3]
                        prev[i] = head.get(key, -1)
                        head[key] = i
                    i += 1
            else:
                out.append(data[i])
                if i + 3 <= n:
                    key = data[i:i + 3]
                    prev[i] = head.get(key, -1)
                    head[key] = i
                i += 1
        out[flag_pos] = flags
    while len(out) % 4:
        out.append(0)
    return bytes(out)
