// Twiddlefinger DS - entry point and state machine
#include "engine.h"
#include <filesystem.h>
#include <fat.h>

u32 kDown, kHeld, kUp;
touchPosition touch;
bool touchDown;
Settings settings = { false, true, 0, true, false };
float g_fade = 0;
int perf_drops, perf_frames;
u32 perf_t[6];
#define PT(i, x) do { u32 _t = time_ticks(); x; _t = time_ticks() - _t; if (_t > perf_t[i]) perf_t[i] = _t; } while (0)

static int curState = -1, nextState = -1;
static bool fatOk;

void *file_load(const char *path, u32 *size) {
	char full[96];
	snprintf(full, sizeof(full), "nitro:%s", path);
	FILE *f = fopen(full, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = malloc(n + 4);
	if (buf) fread(buf, 1, n, f);
	fclose(f);
	if (size) *size = n;
	return buf;
}

#define CFG_PATH "fat:/data/twiddleds.cfg"

void settings_load(void) {
	if (!fatOk) return;
	FILE *f = fopen(CFG_PATH, "rb");
	if (!f) return;
	u8 b[8] = {0};
	if (fread(b, 1, 8, f) == 8 && b[0] == 'T' && b[1] == 'W') {
		settings.downscroll = b[2];
		settings.ghostTapping = b[3];
		settings.camZooms = b[4];
		settings.noteOffset = (s16)(b[5] | (b[6] << 8));
		settings.botplay = b[7];
	}
	fclose(f);
}

void settings_save(void) {
	if (!fatOk) return;
	mkdir("fat:/data", 0777);
	FILE *f = fopen(CFG_PATH, "wb");
	if (!f) return;
	u8 b[8] = {'T', 'W', settings.downscroll, settings.ghostTapping, settings.camZooms,
		settings.noteOffset & 255, (settings.noteOffset >> 8) & 255, settings.botplay};
	fwrite(b, 1, 8, f);
	fclose(f);
}

void state_switch(int st) { nextState = st; }

static void do_switch(void) {
	switch (curState) {
	case ST_MENU: menu_leave(); break;
	case ST_OPTIONS: options_leave(); break;
	case ST_PLAY: play_leave(); break;
	}
	tween_clear();
	video_close();
	gfx_reset_vram();
	font_unload_all();
	curState = nextState;
	nextState = -1;
	switch (curState) {
	case ST_MENU: menu_enter(); break;
	case ST_OPTIONS: options_enter(); break;
	case ST_PLAY: play_enter(); break;
	}
}

static void fatal(const char *msg) {
	consoleDemoInit();
	printf("\n\n  Twiddlefinger DS\n\n  %s\n", msg);
	while (pmMainLoop()) swiWaitForVBlank();
	exit(0);
}

int main(int argc, char **argv) {
	defaultExceptionHandler();
	if (!nitroFSInit(NULL)) fatal("NitroFS init failed.\n  Run the .nds with a loader\n  that supports argv/NitroFS.");
	fatOk = fatInitDefault();
	srand(12345);

	gfx_init();
	sub_init();
	audio_init();
	settings_load();

	nextState = ST_MENU;
	u32 lastTicks = time_ticks();

	while (pmMainLoop()) {
		if (nextState >= 0) {
			do_switch();
			lastTicks = time_ticks();
		}
		scanKeys();
		kDown = keysDown();
		kHeld = keysHeld();
		kUp = keysUp();
		touchDown = (kHeld & KEY_TOUCH) != 0;
		if (touchDown) touchRead(&touch);

		u32 now = time_ticks();
		float dt = (now - lastTicks) / (float)TICK_FREQ;
		lastTicks = now;
		if (curState == ST_PLAY) { perf_frames++; if (dt > 1.5f / 59.83f) perf_drops += (int)(dt * 59.83f + 0.5f) - 1; }
		if (dt > 0.1f) dt = 0.1f;
		if (dt < 0.001f) dt = 1.0f / 60;

		switch (curState) {
		case ST_MENU: menu_update(dt); break;
		case ST_OPTIONS: options_update(dt); break;
		case ST_PLAY: PT(0, play_update(dt)); break;
		}
		tween_update(dt);

		gfx_frame_begin();
		switch (curState) {
		case ST_MENU: menu_draw(); break;
		case ST_OPTIONS: options_draw(); break;
		case ST_PLAY: PT(1, play_draw()); break;
		}
		PT(2, gfx_frame_end());

		swiWaitForVBlank();
		PT(3, gfx_vblank(); video_vblank(); sub_present());
		int b = (int)(clampf(g_fade, 0, 1) * 16);
		REG_MASTER_BRIGHT = b ? ((2 << 14) | b) : 0;
		REG_MASTER_BRIGHT_SUB = b ? ((2 << 14) | b) : 0;
		PT(4, audio_update());
	}
	return 0;
}
