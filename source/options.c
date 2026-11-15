// Small options menu (subset of Psych's OptionsState)
#include "engine.h"

static int sel;
static bool dirty;
#define NOPT 6

static const char *label(int i, char *buf) {
	switch (i) {
	case 0: sprintf(buf, "Downscroll: %s", settings.downscroll ? "ON" : "OFF"); break;
	case 1: sprintf(buf, "Ghost Tapping: %s", settings.ghostTapping ? "ON" : "OFF"); break;
	case 2: sprintf(buf, "Camera Zooms: %s", settings.camZooms ? "ON" : "OFF"); break;
	case 3: sprintf(buf, "Note Offset: %d ms", settings.noteOffset); break;
	case 4: sprintf(buf, "Botplay: %s", settings.botplay ? "ON" : "OFF"); break;
	default: sprintf(buf, "Back"); break;
	}
	return buf;
}

static void draw_sub(void) {
	sub_clear(RGB(20, 12, 12));
	sub_text_centered(FONT_VCRBIG, 128, 10, "OPTIONS", RGB(255, 255, 255));
	char buf[48];
	for (int i = 0; i < NOPT; i++) {
		int y = 40 + i * 22;
		if (i == sel) sub_fill(16, y - 4, 224, 20, RGB(0xF4, 0xA6, 0x7A));
		sub_text_centered(FONT_VCR, 128, y + 2, label(i, buf), RGB(255, 255, 255));
	}
	sub_text_centered(FONT_VCR, 128, 178, "A: change  LEFT/RIGHT: adjust  B: back", RGB(170, 170, 170));
	dirty = false;
}

void options_enter(void) {
	sel = 0;
	font_load(FONT_VCR, true);
	font_load(FONT_VCRBIG, true);
	audio_play_music("nitro:/snd/menu.raw", true, 0.3f);
	g_fade = 0;
	g_subFade = 0;
	dirty = true;
}

void options_leave(void) { settings_save(); }

static void change(int dir) {
	switch (sel) {
	case 0: settings.downscroll = !settings.downscroll; break;
	case 1: settings.ghostTapping = !settings.ghostTapping; break;
	case 2: settings.camZooms = !settings.camZooms; break;
	case 3:
		settings.noteOffset += dir * 5;
		if (settings.noteOffset > 500) settings.noteOffset = 500;
		if (settings.noteOffset < -500) settings.noteOffset = -500;
		break;
	case 4: settings.botplay = !settings.botplay; break;
	case 5:
		if (dir == 0) {
			audio_sfx(SFX_CANCELMENU, 0.7f);
			state_switch(ST_MENU);
		}
		return;
	}
	audio_sfx(SFX_SCROLLMENU, 0.6f);
	dirty = true;
}

void options_update(float dt) {
	if (kDown & KEY_UP) { sel = (sel + NOPT - 1) % NOPT; audio_sfx(SFX_SCROLLMENU, 0.6f); dirty = true; }
	if (kDown & KEY_DOWN) { sel = (sel + 1) % NOPT; audio_sfx(SFX_SCROLLMENU, 0.6f); dirty = true; }
	if (kDown & KEY_A) change(sel == 3 ? 1 : 0);
	if (kDown & KEY_LEFT) change(-1);
	if (kDown & KEY_RIGHT) change(1);
	if (kDown & KEY_B) { audio_sfx(SFX_CANCELMENU, 0.7f); state_switch(ST_MENU); }
	if (kDown & KEY_TOUCH) {
		touchRead(&touch);
		for (int i = 0; i < NOPT; i++) {
			int y = 40 + i * 22;
			if (touch.py >= y - 4 && touch.py < y + 16) {
				sel = i;
				change(i == 3 ? (touch.px < 128 ? -1 : 1) : 0);
				dirty = true;
			}
		}
	}
	if (dirty) draw_sub();
}

void options_draw(void) {
	char buf[48];
	font_draw_centered(FONT_VCRBIG, 128, 16, "OPTIONS", RGB(255, 255, 255), 31);
	for (int i = 0; i < NOPT; i++) {
		int y = 44 + i * 22;
		if (i == sel) gfx_rect(20, y - 3, 236, y + 15, RGB(0xF4, 0xA6, 0x7A), 20);
		font_draw_centered(FONT_VCR, 128, y, label(i, buf), RGB(255, 255, 255), i == sel ? 31 : 18);
	}
}
