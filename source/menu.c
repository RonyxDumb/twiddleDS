// MaxMenu: "MAX DESIGN PRO" title, start / options buttons and the creepy
// face reveal before the song starts.
#include "engine.h"

static Sheet sh;
static Img16 subBg;
static bool optionNow;
static bool started;
static float faceA, eyeA, faceDummy;
static bool faceVisible;
static bool subDirty;

static void go_play(void *ctx) {
	faceVisible = false;
	g_fade = 1;
	state_switch(ST_PLAY);
}

static void step3(void *ctx) {
	// .then(FlxTween.tween(face, {alpha: 1}, 2)) -> just waits 2 seconds
	tween_num(&faceDummy, 0, 1, 2, EASE_LINEAR, 0, go_play, NULL);
}

static void step2(void *ctx) {
	tween_num(&eyeA, 0, 1, 0.7f, EASE_LINEAR, 2, step3, NULL);
}

static void step1(void *ctx) {
	tween_num(&faceA, 0, 1, 2, EASE_LINEAR, 0, step2, NULL);
}

static void draw_sub(void) {
	sub_clear(0);
	sub_blit(&subBg, 0, 0);
	sub_text_centered(FONT_VCRBIG, 128, 14, "TWIDDLEFINGER", RGB(255, 255, 255));
	sub_text_centered(FONT_VCR, 128, 36, "Nintendo DS port", RGB(200, 200, 200));
	if (!started) {
		int sx = 24, ox = 136, y = 120, w = 96, h = 32;
		sub_fill(sx, y, w, h, optionNow ? RGB(30, 30, 30) : RGB(0xF4, 0xA6, 0x7A));
		sub_fill(ox, y, w, h, optionNow ? RGB(0xF4, 0xA6, 0x7A) : RGB(30, 30, 30));
		sub_text_centered(FONT_VCRBIG, sx + w / 2, y + 8, "START", RGB(255, 255, 255));
		sub_text_centered(FONT_VCRBIG, ox + w / 2, y + 8, "OPTIONS", RGB(255, 255, 255));
		sub_text_centered(FONT_VCR, 128, 168, "LEFT/RIGHT: select   A: confirm", RGB(160, 160, 160));
	}
	subDirty = false;
}

void menu_enter(void) {
	sheet_load(&sh, &sheet_menu, false);
	font_load(FONT_VCR, false);
	font_load(FONT_VCRBIG, false);
	img_load(&subBg, "/sub/menubg.b16");
	optionNow = false;
	started = false;
	faceA = eyeA = 0;
	faceVisible = true;
	audio_play_music("nitro:/snd/menu.raw", true, 0.3f);
	g_fade = 1;
	tween_num(&g_fade, 1, 0, 0.4f, EASE_LINEAR, 0, NULL, NULL);
	subDirty = true;
}

void menu_leave(void) {
	sheet_unload(&sh);
	img_free(&subBg);
}

static void accept(void) {
	if (optionNow) {
		audio_sfx(SFX_CONFIRMMENU, 0.7f);
		state_switch(ST_OPTIONS);
		return;
	}
	started = true;
	subDirty = true;
	audio_set_volume(0);
	timer_after(2, step1, NULL);
}

void menu_update(float dt) {
	if (started) {
		if (subDirty) draw_sub();
		return;
	}
	if (kDown & (KEY_LEFT | KEY_RIGHT)) {
		optionNow = !optionNow;
		subDirty = true;
	}
	if (kDown & (KEY_A | KEY_START)) accept();
	if ((kDown & KEY_TOUCH)) {
		touchRead(&touch);
		if (touch.py >= 120 && touch.py < 152) {
			if (touch.px >= 24 && touch.px < 120) { optionNow = false; accept(); }
			else if (touch.px >= 136 && touch.px < 232) { optionNow = true; accept(); }
			subDirty = true;
		}
	}
	if (subDirty) draw_sub();
}

void menu_draw(void) {
	const float k = KDS;
	if (!started) {
		// dark title at y=20 + separator line, white title 200 px above center
		sheet_draw(&sh, MENU_TITLE_DARK, 128, (20 - 360) * k + 96, k, 0x7FFF, 31);
		float ly = (20 + MENU_TEXT_H - 360) * k + 96;
		gfx_rect(0, ly, 256, ly + 5 * k, RGB(0x24, 0x23, 0x20), 31);
		sheet_draw(&sh, MENU_TITLE, 128, ((720 - MENU_TEXT_H) / 2 - 200 - 360) * k + 96, k, 0x7FFF, 31);
		// buttons: 'i' = selected look
		float bx = (-190 - 640) * k + 128, by = (0 - 360) * k + 96;
		sheet_draw(&sh, optionNow ? MENU_START_S : MENU_START_I, bx, by, k, 0x7FFF, 31);
		sheet_draw(&sh, optionNow ? MENU_OPTIONS_I : MENU_OPTIONS_S, bx, by, k, 0x7FFF, 31);
	}
	if (faceVisible) {
		sheet_draw(&sh, MENU_HISFACE, 128, 96, k, 0x7FFF, (int)(faceA * 31));
		sheet_draw(&sh, MENU_EYE, 128, 96, k, 0x7FFF, (int)(eyeA * 31));
	}
}
