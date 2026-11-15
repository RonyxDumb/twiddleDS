#!/usr/bin/env python3
"""Headless test driver (py-desmume). Usage:
   python3 tools/emutest.py ROM OUTDIR script
script: comma separated steps:  wNNN (run NNN frames), s<name> (screenshot),
        p<KEY>NN (hold key NN frames), t<x>:<y>:<frames> (touch)
"""
import os
import sys

os.environ.setdefault('SDL_VIDEODRIVER', 'dummy')
os.environ.setdefault('SDL_AUDIODRIVER', 'dummy')
from desmume.emulator import DeSmuME  # noqa
from desmume.controls import Keys, keymask  # noqa

rom, out, script = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
emu = DeSmuME()
emu.open(rom)
emu.volume_set(0)


def run(n):
    for _ in range(n):
        emu.cycle(with_joystick=False)


KEYS = {k[4:]: getattr(Keys, k) for k in dir(Keys) if k.startswith('KEY_')}
for step in script.split(','):
    if not step:
        continue
    c = step[0]
    if c == 'w':
        run(int(step[1:]))
    elif c == 's':
        emu.screenshot().save(os.path.join(out, step[1:] + '.png'))
    elif c == 'p':
        name = ''.join(ch for ch in step[1:] if ch.isalpha())
        n = int(''.join(ch for ch in step[1:] if ch.isdigit()) or 2)
        emu.input.keypad_add_key(keymask(KEYS[name]))
        run(n)
        emu.input.keypad_rm_key(keymask(KEYS[name]))
        run(1)
    elif c == 't':
        x, y, n = map(int, step[1:].split(':'))
        emu.input.touch_set_pos(x, y)
        run(n)
        emu.input.touch_release()
        run(1)
print('done')
