#!/usr/bin/env python3
"""Converts the mod's mp4 videos to the DS .vid format (see source/video.c).

Usage: python3 tools/videos.py <path to fnf-TwiddleFinger checkout>
Needs ffmpeg and a host C compiler (for tools/lz10.c).
"""
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = sys.argv[1] if len(sys.argv) > 1 else '/home/user/fixeddata/fnf-twiddlefinger'
OUT = os.path.join(ROOT, 'nitrofiles', 'vid')

# name, width, height, fps, filter (16:9 sources)
VIDEOS = [
    # full screen: fill the 192 px height, crop the sides (like the game view)
    ('lyrics_good', 256, 192, 15, 'scale=-2:192:flags=lanczos,crop=256:192'),
    ('lyrics_bad', 256, 192, 15, 'scale=-2:192:flags=lanczos,crop=256:192'),
    ('bad_finale', 256, 192, 15, 'scale=-2:192:flags=lanczos,crop=256:192'),
    ('good_finale', 256, 192, 15, 'scale=-2:192:flags=lanczos,crop=256:192'),
    # fire overlay, stretched over the stage with the affine BG
    ('fire', 256, 144, 10, 'scale=256:144:flags=lanczos'),
]


def build_lz():
    exe = os.path.join(tempfile.gettempdir(), 'twiddle_lz10')
    subprocess.run(['cc', '-O2', '-o', exe, os.path.join(ROOT, 'tools', 'lz10.c')], check=True)
    return exe


def rgb15(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def convert(lz, name, w, h, fps, vf):
    src = os.path.join(MOD, 'assets', 'videos', name + '.mp4')
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', src, '-vf', 'fps=%d,%s' % (fps, vf),
                          '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'], check=True, stdout=subprocess.PIPE).stdout
    fsize = w * h * 3
    n = len(raw) // fsize
    frames = []
    tmpd = tempfile.mkdtemp()
    for i in range(n):
        im = Image.frombytes('RGB', (w, h), raw[i * fsize:(i + 1) * fsize])
        q = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        pal = q.getpalette()[:768]
        pal += [0] * (768 - len(pal))
        pb = b''.join(struct.pack('<H', rgb15(pal[k * 3], pal[k * 3 + 1], pal[k * 3 + 2])) for k in range(256))
        fi = os.path.join(tmpd, 'f.bin')
        fo = os.path.join(tmpd, 'f.lz')
        with open(fi, 'wb') as f:
            f.write(q.tobytes())
        subprocess.run([lz, fi, fo], check=True)
        frames.append(pb + open(fo, 'rb').read())
    hdr = b'VID1' + struct.pack('<HHHH', w, h, fps, n)
    base = len(hdr) + 4 * (n + 1)
    offs = []
    pos = base
    for fr in frames:
        offs.append(pos)
        pos += len(fr)
    offs.append(pos)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + '.vid'), 'wb') as f:
        f.write(hdr)
        f.write(struct.pack('<%dI' % (n + 1), *offs))
        for fr in frames:
            f.write(fr)
    print('%-12s %4d frames  %7.1f KB  (%.1f KB/frame)' % (name, n, pos / 1024, pos / 1024 / max(1, n)))


def main():
    lz = build_lz()
    only = sys.argv[2:]
    for v in VIDEOS:
        if only and v[0] not in only:
            continue
        convert(lz, *v)


if __name__ == '__main__':
    main()
