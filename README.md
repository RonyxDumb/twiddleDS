# Twiddlefinger DS

A Nintendo DS port of the Friday Night Funkin' mod **[Twiddlefinger](https://github.com/FixedData/fnf-TwiddleFinger)**.

This is not HaxeFlixel running on a DS. The game was rewritten from scratch in C with libnds, using the mod's original assets converted for the hardware. The menu, song, chart, stage events, videos and both endings work like they do in the mod.

A prebuilt ROM is available on the [Releases](../../releases) page.

## Playing

Copy `TwiddlefingerDS.nds` to your flashcart (TWiLight Menu++ / nds-bootstrap) or open it in **melonDS**. DeSmuME is not supported.

| Action | Buttons |
| --- | --- |
| Notes | D-pad or Y / B / X / A |
| Pause | START |
| HELP button | tap the touch screen |

Tap the HELP button at least 75 times to get the good ending.

## Building

You need [devkitPro](https://devkitpro.org) with `nds-dev`, or Docker:

```sh
./build.sh      # builds TwiddlefingerDS.nds using the devkitpro/devkitarm image
```

The converted assets are already in the repository. To regenerate them from the original mod (requires Python 3 with Pillow, ffmpeg and a C compiler):

```sh
git clone https://github.com/FixedData/fnf-TwiddleFinger mod
python3 tools/convert.py mod
python3 tools/videos.py mod
```

## Project structure

```
source/          game code (engine, menu, gameplay, stage events)
include/         headers
tools/           asset converters and a headless melonDS test frontend
nitrofiles/      converted assets packed into the ROM
maxmod_data/     sound effects
```

## Credits

All art, music, charts and videos belong to the creators of Twiddlefinger and Friday Night Funkin'. This is an unofficial fan project.
