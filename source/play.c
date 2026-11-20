// PlayState + the "twiddle" stage script (example_mods/stages/twiddle.hx)
#include "engine.h"
#include "character.h"

// ---------------------------------------------------------------------------
// constants
// ---------------------------------------------------------------------------
#define CROCHET (60000.0f / CHART_BPM)
#define STEP (CROCHET / 4)
#define SAFE_ZONE (10.0f / 60.0f * 1000.0f)
#define HIT_HEALTH 0.023f
#define MISS_HEALTH 0.0475f

#define BF_X 4200.0f
#define BF_Y 2850.0f
#define DAD_X 2600.0f
#define DAD_Y 2800.0f
#define BF_CAM_OFF_X (-200.0f)
#define BF_CAM_OFF_Y (-200.0f)
#define DEF_ZOOM 0.35f

// "position" from the character jsons (only the intro characters have one)
static const float chrPos[CHR_COUNT][2] = {
	[CHR_MAX] = { 30, 130 },
	[CHR_NUGGETOG] = { 40, 320 },
};

static const char *singAnims[4] = { "singLEFT", "singDOWN", "singUP", "singRIGHT" };
static const u32 laneKeys[4] = { KEY_LEFT | KEY_Y, KEY_DOWN | KEY_B, KEY_UP | KEY_X, KEY_RIGHT | KEY_A };

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static Sheet shSky, shStage, shTree, shOg, shHud, shEnd;
static int skyShade = -1;
static Character chars[CHR_COUNT];
static Character *bf, *dad;
static Cam cam;
static float followX, followY;
static float defaultZoom, hudZoom, defaultHudZoom = 0.8f;
static bool camZooming, forcedPos;
static float beatsPerZoom = 4;
static int zoomTween;                // FlxTween.num that also drives defaultCamZoom
static int camTween;                 // FlxTween on FlxG.camera.zoom only (defaultCamZoom untouched)
static float zoomTweenVal;
static float shakeTime;

// stage objects
static float skyA, stageA, treeA, fakeBgA;
static float shStrength, shDarkness;
static bool fireOn;

// DS-only fire effects: warm palette light, beat pulse, heat haze on the
// fire layer (HBlank DMA), rising embers and a Gouraud glow from the street
#define EMBERS 48
// fixed point (no FPU on the ARM9): positions in 1/16 px, speeds in 1/16 px per second,
// life in ms, phase as an index into a 64 entry sine table
typedef struct { s32 x, y; s16 vx, vy; u16 life, max; u8 phase, size; } Ember;
static Ember embers[EMBERS];
static float fireFx;                 // 0..1 intensity of the extra effects
static float firePulse;              // beat pulse, decays
static float fireTime;
static float lastCamX, lastCamY;
static u32 fireMs;
static const s8 sin64s[64] = {
	0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126,
	127, 126, 125, 122, 117, 112, 106, 98, 90, 81, 71, 60, 49, 37, 25, 12,
	0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112, -117, -122, -125, -126,
	-127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12 };


// hud
static float hudAlpha, uiAlpha, strumAlpha[4];
static bool uiVisible, hudVisible;
static float hudFadeA;               // camHUD.fade(BLACK)
static float gameFadeA;              // FlxG.camera.fade
static float gameFlashA, otherFlashA;
static u16 gameFlashColor, otherFlashColor;

// tiktok camera
static bool tiktokBlack;
static float helpBarTX, helpBarBX, helpBarA;
static bool helpBarsVisible;
static char subs[64];

// help me
static bool helpMode, succeeded, hereVisible, hereAllowScale;
static float hereScale;
static int helpCount;
static int lastTapFrame;

// end images
static float goodA, badA;
static int endImg;                   // 0 none, 1 good, 2 bad

// videos
enum { VID_NONE, VID_FIRE, VID_LYRICS, VID_FINALE_GOOD, VID_FINALE_BAD };
static int vidKind;
static float vidStart;

// conductor
static float songPos;
static bool countdownOn, songStarted, songEnded, paused, dead;
static int countdownTick;
static int lastBeat, lastStep;
static int evIdx;

// notes
typedef struct {
	float t, len;
	u8 dir, player, state;           // state: 0 pending, 1 holding, 2 done, 3 missed
	u8 nextPiece, pieces;
} Note;
static Note *notes;
static int nNotes;
static int firstActive;

// strums
static int strumAnim[4];             // 0 static, 1 pressed, 2 confirm
static float strumTime[4];
static float strumReset[4];

// score
static float health;
static int score, misses, combo, hits[4];
static float totalPlayed, totalHit;
static bool subDirty;
static int subTimer;
static int deaths;

// pause / game over
static int pauseSel;
static float pauseBgA, pauseMusicVol;
static float deadTime;
static bool deadConfirm;

static Img16 imgHere, imgBar, imgBarW, imgIcons[8];

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static inline float hud_x(float v) { return (v - 640.0f) * hudZoom * KDS + 128.0f; }
static inline float hud_y(float v) { return (v - 360.0f) * hudZoom * KDS + 96.0f; }
static inline float oth_x(float v) { return (v - 640.0f) * KDS + 128.0f; }
static inline float oth_y(float v) { return (v - 360.0f) * KDS + 96.0f; }
static inline int A31(float a) { return (int)(clampf(a, 0, 1) * 31 + 0.5f); }

static void flash_game(u16 color, float dur) {
	gameFlashColor = color;
	gameFlashA = 1;
	tween_cancel_target(&gameFlashA);
	tween_num(&gameFlashA, 1, 0, dur, EASE_LINEAR, 0, NULL, NULL);
}

static void flash_other(u16 color, float dur) {
	otherFlashColor = color;
	otherFlashA = 1;
	tween_cancel_target(&otherFlashA);
	tween_num(&otherFlashA, 1, 0, dur, EASE_LINEAR, 0, NULL, NULL);
}

static void snap_camera(void) {
	cam.camX = followX;
	cam.camY = followY;
}

static Character *get_char(int id) {
	return &chars[id];
}

// Psych 'Change Character' (characters are pre-created, keep their own state)
static void change_bf(int id) {
	Character *n = get_char(id);
	if (n == bf) return;
	n->alpha = bf->alpha;
	bf = n;
	chr_dance(bf);
	subDirty = true;
}

static void change_dad(int id) {
	Character *n = get_char(id);
	if (n == dad) return;
	n->alpha = dad->alpha;
	dad = n;
	chr_dance(dad);
}

static void move_camera(bool isDad) {
	Character *c = isDad ? dad : bf;
	float dx = 0, dy = 0;
	const char *an = chr_anim_name(c);
	const float camOffset = 25;
	if (!strncmp(an, "sing", 4)) {
		const char *d = an + 4;
		if (!strncmp(d, "UP", 2)) dy = -camOffset;
		else if (!strncmp(d, "DOWN", 4)) dy = camOffset;
		else if (!strncmp(d, "LEFT", 4)) dx = -camOffset;
		else if (!strncmp(d, "RIGHT", 5)) dx = camOffset;
	}
	if (isDad) {
		followX = chr_mid_x(dad) + 150 + dad->def->camx;
		followY = chr_mid_y(dad) - 100 + dad->def->camy;
	} else {
		followX = chr_mid_x(bf) - 100 - (bf->def->camx - BF_CAM_OFF_X);
		followY = chr_mid_y(bf) - 100 + bf->def->camy + BF_CAM_OFF_Y;
	}
	followX += dx;
	followY += dy;
}

static void move_camera_section(void) {
	int sec = (int)floorf(songPos / (CROCHET * 4));
	if (sec < 0) sec = 0;
	if (sec >= CHART_SECTIONS) return;
	move_camera(!chart_sections[sec]);
}

static int sky_shade_index(void);
static float health_percent(void) { return health / 2.0f * 100.0f; }

static u16 battery_color(void) {
	if (health >= 2) return RGB(0x21, 0xEF, 0x36);
	if (health >= 1.5f) return RGB(0x92, 0xEF, 0x58);
	if (health >= 1) return RGB(0xF1, 0xDB, 0x5C);
	if (health >= 0.5f) return RGB(0xCA, 0x54, 0x27);
	return RGB(0xCD, 0x33, 0x23);
}

static int sky_shade_index(void) { return shader_find(shSky.palAddr); }

// ---------------------------------------------------------------------------
// tween callbacks
// ---------------------------------------------------------------------------
static void cb_camzooming(void *ctx) { camZooming = true; zoomTween = 0; }
static void cb_helpbars_reset(void *ctx) {
	helpBarTX = 0;
	helpBarBX = 1280 - 1280 / 3.0f;
	helpBarA = 0;
}
static void cb_here_allow(void *ctx) { hereAllowScale = true; }
static void cb_here_hide(void *ctx) { hereAllowScale = false; }

static void zoom_tween(float to, float dur, int ease) {
	tween_cancel(zoomTween);
	zoomTweenVal = cam.zoom;
	zoomTween = tween_num(&zoomTweenVal, cam.zoom, to, dur, ease, 0, cb_camzooming, NULL);
	camZooming = false;
}

static void tween_shader(float strength, float darkness, float time, int ease) {
	tween_cancel_target(&shStrength);
	tween_cancel_target(&shDarkness);
	tween_num(&shDarkness, shDarkness, darkness, time, ease, 0, NULL, NULL);
	tween_num(&shStrength, shStrength, strength, time, ease, 0, NULL, NULL);
}

// ---------------------------------------------------------------------------
// videos
// ---------------------------------------------------------------------------
static void start_fire(void) {
	if (video_open("nitro:/vid/fire.vid", true, true)) {
		vidKind = VID_FIRE;
		vidStart = songPos;
		video_set_blend(8);     // mod: alpha 0.3 additive *before* the dark shader; on DS it is added after it
		fireOn = true;
		tween_cancel_target(&fireFx);
		tween_to(&fireFx, 1, 0.6f, EASE_SINEOUT, 0, NULL, NULL);
		firePulse = 1;
	}
}

static void ember_spawn(Ember *e, bool anywhere) {
	e->x = (rand() % 256) << 4;
	e->y = anywhere ? (rand() % 192) << 4 : (192 + rand() % 10) << 4;
	e->vx = (rand() % 320) - 160;
	e->vy = -(288 + rand() % 640);
	e->max = 2500 + rand() % 3000;
	e->life = anywhere ? rand() % e->max : 0;
	e->phase = rand() & 63;
	e->size = 1 + (rand() % 3 == 0);
}

static void fire_fx_update(float dt) {
	if ((!fireOn || vidKind != VID_FIRE) && fireFx > 0) {
		tween_cancel_target(&fireFx);
		fireFx = 0;
	}
	firePulse *= expf(-dt * 5);
	fireTime += dt;
	int dms = (int)(dt * 1000);
	fireMs += dms;
	// embers live in screen space: follow the camera with a bit of parallax
	s32 camDX = (s32)((cam.camX - lastCamX) * cam.zoom * KDS * 1.3f * 16);
	s32 camDY = (s32)((cam.camY - lastCamY) * cam.zoom * KDS * 1.3f * 16);
	lastCamX = cam.camX;
	lastCamY = cam.camY;
	if (fireFx <= 0) return;
	int boost = 256 + (int)(firePulse * 205);       // rise faster on the beat (8.8)
	int swayT = (fireMs * 23 / 1000) & 63;            // ~0.37 Hz sway
	for (int i = 0; i < EMBERS; i++) {
		Ember *e = &embers[i];
		e->life += dms;
		if (e->life >= e->max || e->y < -64) { ember_spawn(e, false); continue; }
		int sway = sin64s[(swayT + e->phase) & 63] * 224 / 127;   // +-14 px/s
		e->x += (e->vx + sway) * dms / 1000 - camDX;
		e->y += (e->vy * boost >> 8) * dms / 1000 - camDY;
		if (e->x < -64) e->x += 264 << 4;
		if (e->x > 260 << 4) e->x -= 264 << 4;
	}
	// heat haze on the fire layer, stronger on the beat
	video_set_wave((1.2f + firePulse * 1.6f) * fireFx, fireTime * 0.9f);
	video_set_blend(8 + (int)(firePulse * 6 * fireFx + 0.5f));
}

static void fire_fx_draw(void) {
	if (fireFx <= 0.01f) return;
	// warm glow rising from the street (vertex colours), pulsing with the beat
	int ga = (int)((6 + firePulse * 6) * fireFx);
	// two stacked gradients over the whole screen, so there is no visible edge
	gfx_rect_grad(0, 0, 256, 96, RGB(40, 0, 0), RGB(150, 30, 0), ga);
	gfx_rect_grad(0, 96, 256, 192, RGB(150, 30, 0), RGB(255, 110, 20), ga);
	// embers: white-hot -> orange -> deep red as they cool
	int fx = (int)(fireFx * 256);
	int flickT = (fireMs * 173 / 1000) & 63;          // ~2.7 Hz flicker
	for (int i = 0; i < EMBERS; i++) {
		Ember *e = &embers[i];
		int t = e->life * 256 / e->max;                // 0..255
		u16 c = t < 64 ? RGB(255, 240, 150) : t < 154 ? RGB(255, 150, 40) : RGB(220, 50, 10);
		int a = t < 26 ? t * 31 / 26 : 31 - (t - 26) * 31 / 230;
		a = a * (224 + sin64s[(flickT + e->phase * 5) & 63] / 4) >> 8;
		a = a * fx >> 8;
		int sz = (e->size * 16 * (256 - t * 2 / 5) >> 8) + 8;     // 1/16 px
		int x0 = e->x - sz / 2, y0 = e->y - sz / 2;
		gfx_quad_raw(x0, y0, x0 + sz, y0 + sz, 0, 0, 0, 0, 0, 0, c, a);
	}
}

static void stop_video(void) {
	video_close();
	vidKind = VID_NONE;
}

static void fire_fx_reset(void) {
	fireFx = firePulse = fireTime = 0;
	fireMs = 0;
	for (int i = 0; i < EMBERS; i++) ember_spawn(&embers[i], true);
	lastCamX = cam.camX;
	lastCamY = cam.camY;
}

static void red_mode(void);

static void on_video_end(void) {
	int k = vidKind;
	subDirty = true;
	stop_video();
	if (k == VID_LYRICS) {
		flash_other(RGB(255, 255, 255), 1);
		for (int i = 0; i < 4; i++) strumAlpha[i] = 1;
		uiAlpha = 1;
		tiktokBlack = false;
		red_mode();
	} else if (k == VID_FINALE_GOOD) {
		change_bf(CHR_NUGGET_HAT);
		change_dad(CHR_MAXPRO);
		forcedPos = false;
		move_camera_section();
		snap_camera();
		start_fire();
		tiktokBlack = false;
		dad->alpha = 1;
		skyA = stageA = treeA = 1;
		flash_game(RGB(255, 0, 0), 0.5f);
	} else if (k == VID_FINALE_BAD) {
		tiktokBlack = true;
	}
}

static void play_video(const char *path, int kind) {
	stop_video();
	if (video_open(path, false, false)) {
		vidKind = kind;
		vidStart = songPos;
	}
}

// ---------------------------------------------------------------------------
// stage events
// ---------------------------------------------------------------------------
static void red_mode(void) {
	if (vidKind == VID_FIRE) stop_video();
	fireOn = false;
	skyA = 0.2f;
	stageA = 0;
	treeA = 0;
	uiVisible = false;
	change_bf(succeeded ? CHR_REDNUGHAT : CHR_REDNUGGET);
	change_dad(CHR_REDMAX);
	dad->x += 400;
	bf->x += -1100;
	bf->y += 500;
	forcedPos = true;
	followX = dad->x + 415;
	followY = dad->y + 300;
	snap_camera();
	if (succeeded) dad->alpha = 0;
	else {
		dad->alpha = 0.5f;
		bf->x += 75;
	}
}

static void start_helping(void) {
	helpMode = true;
	forcedPos = true;
	bf->stunned = true;
	chr_play(bf, "WHAT", true);
	tween_to(&uiAlpha, 0, 1, EASE_LINEAR, 0, NULL, NULL);
	for (int i = 0; i < 4; i++) tween_to(&strumAlpha[i], 0, 1, EASE_LINEAR, 0, NULL, NULL);
	tween_to(&helpBarA, 1, 2, EASE_LINEAR, 1, NULL, NULL);
	hereVisible = true;
	hereScale = 0;
	tween_num(&hereScale, 0, 0.35f, 0.7f, EASE_BACKOUT, 1, cb_here_allow, NULL);
	float x = chr_mid_x(bf) - 100 - (bf->def->camx - BF_CAM_OFF_X);
	float y = chr_mid_y(bf) - 100 + bf->def->camy + BF_CAM_OFF_Y;
	tween_cancel_target(&followX);
	tween_cancel_target(&followY);
	tween_to(&followX, x + 400, 0.25f, EASE_QUADOUT, 0, NULL, NULL);
	tween_to(&followY, y, 0.25f, EASE_QUADOUT, 0, NULL, NULL);
	tween_cancel(zoomTween);
	zoomTween = 0;
	subDirty = true;
}

static void judge_help(void) {
	helpMode = false;
	if (helpCount >= 75) succeeded = true;
	if (succeeded) {
		change_bf(CHR_NUGGET_HAT);
		chr_play(bf, "hatapear", true);
		bf->stunned = true;
	}
	hereAllowScale = false;
	tween_num(&hereScale, hereScale, 0, 0.7f, EASE_BACKIN, 0, cb_here_hide, NULL);
	subDirty = true;
}

static void run_event(const EventDef *e) {
	switch (e->type) {
	case EV_BEGIN:
		tween_cancel_target(&followX);
		tween_cancel_target(&followY);
		tween_cancel(camTween);
		camTween = 0;
		tween_cancel(zoomTween);
		zoomTween = 0;
		forcedPos = false;
		camZooming = true;
		break;
	case EV_TWEENIN: {
		float d = 22.27f - 21.03f;
		camZooming = false;
		tween_shader(3, 0.3f, d, EASE_CUBEINOUT);
		tween_to(&bf->x, bf->x + 700, d, EASE_CUBEOUT, 0, NULL, NULL);
		tween_cancel(zoomTween);
		zoomTween = 0;
		tween_cancel_target(&cam.zoom);
		camTween = tween_num(&cam.zoom, cam.zoom, cam.zoom + 0.3f, d, EASE_CUBEINOUT, 0, NULL, NULL);
		tween_to(&hudAlpha, 0.5f, d, EASE_CUBEINOUT, 0, NULL, NULL);
		tween_to(&fakeBgA, 0, d + 0.1f, EASE_LINEAR, 0, NULL, NULL);
		tween_to(&helpBarTX, -1280 / 3.0f, d + 0.1f, EASE_LINEAR, 0, NULL, NULL);
		tween_to(&helpBarBX, 1280, d + 0.1f, EASE_LINEAR, 0, cb_helpbars_reset, NULL);
		break;
	}
	case EV_TWEENOUT1:
		sheet_unload(&shOg);

		change_bf(CHR_NUGGET);
		change_dad(CHR_MAXPRO);
		skyA = stageA = treeA = 1;
		tween_shader(1, 0, 0.15f, EASE_LINEAR);
		defaultZoom = 0.4f;
		tween_cancel_target(&hudAlpha);
		tween_to(&hudAlpha, 1, 0.15f, EASE_CUBEOUT, 0, NULL, NULL);
		tween_cancel(zoomTween);
		zoomTween = 0;
		tween_cancel_target(&cam.zoom);
		camTween = tween_num(&cam.zoom, cam.zoom, defaultZoom, 0.15f, EASE_CUBEOUT, 0, NULL, NULL);
		break;
	case EV_BEATSPERZOOM:
		beatsPerZoom = e->a;
		break;
	case EV_INFRYZOOM:
		zoom_tween(e->a, e->b, EASE_CUBEOUT);
		break;
	case EV_ADDCAMZOOM:
		if (settings.camZooms && cam.zoom < 1.35f) {
			cam.zoom += e->a;
			hudZoom += e->b;
		}
		break;
	case EV_FIRE:
		shStrength = 4;
		shDarkness = 0.6f;
		start_fire();
		flash_game(RGB(255, 0, 0), 0.4f);
		cam.zoom += 0.2f;
		shakeTime = 0.35f;       // DS extra: impact shake when the street catches fire
		break;
	case EV_HELPME:
		start_helping();
		break;
	case EV_JUDGE:
		judge_help();
		break;
	case EV_CUTSCENE:
		tiktokBlack = true;
		bf->stunned = false;
		helpBarsVisible = false;
		hereVisible = false;
		forcedPos = false;
		subDirty = true;
		play_video(succeeded ? "nitro:/vid/lyrics_good.vid" : "nitro:/vid/lyrics_bad.vid", VID_LYRICS);
		break;
	case EV_SUBS:
		strncpy(subs, e->s, sizeof(subs) - 1);
		break;
	case EV_BRINGNOTES:
		for (int i = 0; i < 4; i++) tween_to(&strumAlpha[i], 1, 0.7f, EASE_LINEAR, 0, NULL, NULL);
		break;
	case EV_PLAYGOODVID:
		if (succeeded) play_video("nitro:/vid/good_finale.vid", VID_FINALE_GOOD), tiktokBlack = true;
		break;
	case EV_PLAYBADVID:
		if (!succeeded) play_video("nitro:/vid/bad_finale.vid", VID_FINALE_BAD), tiktokBlack = true;
		break;
	case EV_PANCENTER:
		forcedPos = true;
		tween_to(&followX, STAGE_SKY_W / 2, 1.2f, EASE_SINEINOUT, 0, NULL, NULL);
		zoom_tween(0.38f, 1.2f, EASE_SINEINOUT);
		break;
	case EV_PANUP:
		forcedPos = true;
		tween_to(&followY, followY - 500, 1.2f, EASE_SINEIN, 0, NULL, NULL);
		tween_num(&hudFadeA, 0, 1, 1, EASE_LINEAR, 0, NULL, NULL);
		break;
	case EV_FADEIMG:
		// the game camera is hidden from here on: free VRAM for the end image
		sheet_unload(&shOg);
		sheet_unload(&shStage);
		sheet_unload(&shTree);
		if (succeeded) {
			endImg = 1;
			sheet_load(&shEnd, &sheet_goodend, false);
			tween_num(&goodA, 0, 1, 4, EASE_LINEAR, 0, NULL, NULL);
		} else {
			endImg = 2;
			hudVisible = false;
			sheet_load(&shEnd, &sheet_badend, false);
			tween_num(&badA, 0, 1, 4, EASE_LINEAR, 0, NULL, NULL);
		}
		break;
	}
}

// ---------------------------------------------------------------------------
// song flow
// ---------------------------------------------------------------------------
static void beat_hit(int beat) {
	if (fireOn) firePulse = beat % 2 == 0 ? 1 : 0.6f;
	if (camZooming && settings.camZooms && cam.zoom < 1.35f && beat % (int)beatsPerZoom == 0) {
		cam.zoom += 0.015f;
		hudZoom += 0.03f;
	}
	// characterBopper (danceEveryNumBeats = 2 for idle-only characters)
	if (beat % 2 == 0) {
		if (!bf->stunned && strncmp(chr_anim_name(bf), "sing", 4)) chr_dance(bf);
		if (!dad->stunned && strncmp(chr_anim_name(dad), "sing", 4)) chr_dance(dad);
	}
}

static void on_song_start(void) {
	float time = 1.5f;
	tween_num(&gameFadeA, 1, 0, time, EASE_LINEAR, 0, NULL, NULL);
	tween_num(&hudAlpha, 0, 1, time - 0.25f, EASE_SINEOUT, 0.25f, NULL, NULL);
	// FlxTween.tween(FlxG.camera, {zoom}): defaultCamZoom stays 0.35, so once 'begin'
	// enables camZooming the camera slowly pulls back to the wide stage shot
	camTween = tween_num(&cam.zoom, cam.zoom, cam.zoom - 0.1f, time, EASE_CIRCIN, 0, NULL, NULL);
	tween_to(&followX, followX - 700, time, EASE_CIRCIN, 0, NULL, NULL);
	tween_to(&followY, followY - 100, time, EASE_CIRCIN, 0, NULL, NULL);
}

static void strum_play(int d, int anim) {
	strumAnim[d] = anim;
	strumTime[d] = 0;
}

static void recalc(void) {
	subDirty = true;
}

static void good_note_hit(Note *n) {
	float diff = fabsf(n->t - songPos + settings.noteOffset);
	int r;
	if (diff <= 45) r = 0;
	else if (diff <= 90) r = 1;
	else if (diff <= 135) r = 2;
	else r = 3;
	static const int sc[4] = { 350, 200, 100, 50 };
	static const float acc[4] = { 1, 0.67f, 0.34f, 0 };
	score += sc[r];
	hits[r]++;
	combo++;
	totalPlayed += 1;
	totalHit += acc[r];
	health += HIT_HEALTH;
	if (health > 2) health = 2;
	if (!bf->stunned) {
		chr_play(bf, singAnims[n->dir], true);
		bf->holdTimer = 0;
	}
	strum_play(n->dir, 2);
	strumReset[n->dir] = 0;
	audio_set_voices(1);
	n->state = n->pieces > 0 ? 1 : 2;
	n->nextPiece = 1;
	recalc();
}

static void note_miss(Note *n, int extraPieces) {
	health -= MISS_HEALTH * (1 + extraPieces);
	combo = 0;
	score -= 10;
	misses++;
	totalPlayed += 1;
	audio_set_voices(0);
	recalc();
}

static void update_notes(void) {
	float pos = songPos - settings.noteOffset;
	// opponent notes + player misses
	while (firstActive < nNotes && notes[firstActive].state >= 2) firstActive++;
	for (int i = firstActive; i < nNotes; i++) {
		Note *n = &notes[i];
		if (n->t > pos + SAFE_ZONE + 50) break;
		if (n->state >= 2) continue;
		if (!n->player) {
			if (n->state == 0 && pos >= n->t) {
				camZooming = true;      // Psych: opponentNoteHit
				if (!dad->stunned) {
					chr_play(dad, singAnims[n->dir], true);
					dad->holdTimer = 0;
				}
				audio_set_voices(1);
				n->state = n->pieces > 0 ? 1 : 2;
				n->nextPiece = 1;
			}
			if (n->state == 1) {
				while (n->nextPiece <= n->pieces && pos >= n->t + STEP * n->nextPiece) {
					if (!dad->stunned) {
						chr_play(dad, singAnims[n->dir], true);
						dad->holdTimer = 0;
					}
					n->nextPiece++;
				}
				if (n->nextPiece > n->pieces) n->state = 2;
			}
			continue;
		}
		if (settings.botplay && n->state == 0 && pos >= n->t) {
			good_note_hit(n);
			continue;
		}
		if (n->state == 0 && pos > n->t + SAFE_ZONE) {
			n->state = 3;
			note_miss(n, n->pieces);
			continue;
		}
		if (n->state == 1) {
			bool held = settings.botplay || (kHeld & laneKeys[n->dir]) != 0;
			while (n->nextPiece <= n->pieces && pos >= n->t + STEP * n->nextPiece - SAFE_ZONE * 0.5f) {
				if (!held) break;
				health += HIT_HEALTH;
				if (health > 2) health = 2;
				if (!bf->stunned) {
					chr_play(bf, singAnims[n->dir], true);
					bf->holdTimer = 0;
				}
				strum_play(n->dir, 2);
				n->nextPiece++;
			}
			if (n->nextPiece > n->pieces) n->state = 2;
			else if (!held && pos > n->t + STEP * n->nextPiece + SAFE_ZONE) {
				n->state = 3;
				note_miss(n, 0);
			}
		}
	}
}

static void key_press(int d) {
	float pos = songPos - settings.noteOffset;
	Note *best = NULL;
	for (int i = firstActive; i < nNotes; i++) {
		Note *n = &notes[i];
		if (n->t > pos + SAFE_ZONE) break;
		if (!n->player || n->state != 0 || n->dir != d) continue;
		if (n->t > pos - SAFE_ZONE) {
			best = n;
			break;
		}
	}
	if (best) good_note_hit(best);
	else {
		if (strumAnim[d] != 2) strum_play(d, 1);
		if (!settings.ghostTapping) {
			health -= 0.05f;
			misses++;
			score -= 10;
			audio_set_voices(0);
			audio_sfx(SFX_MISSNOTE1 + (rand() % 3), 0.15f);
			recalc();
		}
	}
}

// ---------------------------------------------------------------------------
// bottom screen
// ---------------------------------------------------------------------------
static int icon_index(const Character *c, bool losing) {
	return c->def->icon * 2 + (losing ? 1 : 0);
}

// The bottom screen is redrawn in two levels: a full redraw when the layout
// changes (mode, icons, battery colour) and a cheap partial redraw of the
// score lines. A full redraw costs most of a frame of software rendering, so
// during gameplay it is split in stages over several frames; the back buffer
// is only copied to VRAM once the last stage is done (sub_hold), so no
// half-drawn screen is ever shown.
static int subKey = -1;
static int subStage = -1;            // next stage of a staged full redraw, -1 = idle
static int subMode;
static int tapShown;                 // help button drawn pressed (tap flash)

static void draw_score_top(void) {
	sub_fill(0, 108, 256, 68, RGB(12, 8, 10));
	char b[32];
	snprintf(b, sizeof(b), "Score: %d", score);
	sub_text_centered(FONT_VCRBIG, 128, 112, b, RGB(255, 255, 255));
}

static void draw_score_bottom(void) {
	char b[64];
	float accp = totalPlayed > 0 ? totalHit / totalPlayed * 100 : 0;
	snprintf(b, sizeof(b), "Misses: %d   Accuracy: %d.%02d%%", misses, (int)accp, (int)(accp * 100) % 100);
	sub_text_centered(FONT_VCR, 128, 138, b, RGB(220, 220, 220));
	snprintf(b, sizeof(b), "Sick %d  Good %d  Bad %d  Shit %d", hits[0], hits[1], hits[2], hits[3]);
	sub_text_centered(FONT_VCR, 128, 156, b, RGB(170, 170, 170));
#ifdef DEBUG_PERF
	{ extern int perf_drops, perf_frames; extern volatile u32 dbg_under, dbg_cb, dbg_len; snprintf(b, sizeof(b), "dr %d un %d cb %d len %d", perf_drops, (int)dbg_under, (int)dbg_cb, (int)dbg_len); sub_text(FONT_VCR, 2, 166, b, RGB(255, 255, 0)); }
#endif
}

static void draw_here(void) {
	int x = (256 - imgHere.w) / 2, y = 70;
	sub_fill(x, y, imgHere.w + 2, imgHere.h + 2, RGB(12, 8, 10));
	sub_blit(&imgHere, x + (tapShown ? 2 : 0), y + (tapShown ? 2 : 0));
}

// one stage of the full redraw; returns false when the screen is complete
static bool draw_sub_stage(int st) {
	float hp = health_percent();
	if (st == 0) { sub_clear(RGB(12, 8, 10)); return true; }
	switch (subMode) {
	case 1: {
		static const char *items[3] = { "Resume", "Restart Song", "Exit to menu" };
		if (st == 1) { sub_text_centered(FONT_VCRBIG, 128, 12, "PAUSED", RGB(255, 255, 255)); return true; }
		if (st <= 4) {
			int i = st - 2, y = 50 + i * 40;
			sub_fill(28, y, 200, 32, i == pauseSel ? RGB(0xF4, 0xA6, 0x7A) : RGB(40, 30, 34));
			sub_text_centered(FONT_VCRBIG, 128, y + 8, items[i], RGB(255, 255, 255));
			return true;
		}
		char b[48];
		snprintf(b, sizeof(b), "Blue balled: %d", deaths);
		sub_text_centered(FONT_VCR, 128, 176, b, RGB(180, 180, 180));
		return false;
	}
	case 2:
		sub_text_centered(FONT_VCRBIG, 128, 40, "GAME OVER", RGB(255, 80, 80));
		sub_text_centered(FONT_VCR, 128, 100, "A: retry", RGB(255, 255, 255));
		sub_text_centered(FONT_VCR, 128, 120, "B: exit to menu", RGB(255, 255, 255));
		return false;
	case 3:
		if (st == 1) { sub_text_centered(FONT_VCRBIG, 128, 16, "TAP TO HELP HIM!", RGB(255, 255, 255)); return true; }
		draw_here();
		return false;
	}
	int bx = (256 - imgBar.w) / 2, by = 46;
	switch (st) {
	case 1: sub_text_centered(FONT_VCRBIG, 128, 6, "TWIDDLEFINGER", RGB(255, 255, 255)); return true;
	case 2: if (uiVisible) sub_blit(&imgBar, bx, by); return true;
	case 3: if (uiVisible) sub_blit_tint(&imgBarW, bx + 11, by + 23, imgBarW.w, battery_color()); return true;
	case 4:
		if (uiVisible) {
			sub_blit(&imgIcons[icon_index(dad, hp > 80)], bx - 50, by + 2);
			sub_blit_flip(&imgIcons[icon_index(bf, hp < 20)], bx + imgBar.w - 6, by + 2);   // iconP1 is flipped
		}
		return true;
	case 5: draw_score_top(); return true;
	case 6: draw_score_bottom(); return true;
	default:
		sub_text_centered(FONT_VCR, 128, 178, settings.botplay ? "BOTPLAY - START: pause" : "START: pause", RGB(120, 120, 120));
		return false;
	}
}

// now = finish the whole redraw in this frame (pause / game over: nothing else is running)
static void draw_sub(bool now) {
	int mode = paused ? 1 : dead ? 2 : (helpMode || (hereVisible && hereScale > 0.05f)) ? 3 : 0;
	float hp = health_percent();
	int key = mode | (pauseSel << 2) | (uiVisible << 5) |
		(icon_index(dad, hp > 80) << 6) | (icon_index(bf, hp < 20) << 10) | ((int)battery_color() << 14);
	if (key != subKey) {
		subKey = key;
		subMode = mode;
		subStage = 0;
		tapShown = lastTapFrame > 0;
		sub_hold(true);
	}
	if (subStage < 0) {
		if (mode == 0) {
			draw_score_top();
			draw_score_bottom();
		} else if (mode == 3 && tapShown != (lastTapFrame > 0)) {
			tapShown = lastTapFrame > 0;
			draw_here();
		}
		return;
	}
	do {
		if (!draw_sub_stage(subStage++)) {
			subStage = -1;
			sub_hold(false);
			break;
		}
	} while (now);
}

// ---------------------------------------------------------------------------
// enter / leave
// ---------------------------------------------------------------------------
static void build_notes(void) {
	nNotes = CHART_NOTES;
	notes = malloc(sizeof(Note) * nNotes);
	for (int i = 0; i < nNotes; i++) {
		const NoteDef *d = &chart_notes[i];
		notes[i].t = d->time;
		notes[i].len = d->len;
		notes[i].dir = d->dir;
		notes[i].player = d->player;
		notes[i].state = 0;
		notes[i].nextPiece = 0;
		int fs = (int)floorf(d->len / STEP);
		notes[i].pieces = d->len > 0 ? fs : 0;
	}
	firstActive = 0;
}

void play_enter(void) {
	g_fade = 1;
	audio_stop();
	sheet_load(&shSky, &sheet_sky, true);
	skyShade = shader_last();
	sheet_load(&shStage, &sheet_stage, true);
	sheet_load(&shTree, &sheet_tree, true);
	sheet_load(&shOg, &sheet_og, true);
	sheet_load(&shHud, &sheet_hud, false);
	font_load(FONT_SUBS, true);
	font_load(FONT_VCR, true);
	font_load(FONT_VCRBIG, false);
	for (int i = 0; i < CHR_COUNT; i++) chr_load(&chars[i], i, i != CHR_MAXPRO && i != CHR_REDMAX && i != CHR_MAX);
	for (int i = 0; i < CHR_COUNT; i++) {
		bool player = chars[i].isPlayer;
		// stage position + the character json "position" offset (Psych startCharacterPos)
		chars[i].x = (player ? BF_X : DAD_X) + chrPos[i][0];
		chars[i].y = (player ? BF_Y : DAD_Y) + chrPos[i][1];
	}
	img_load(&imgHere, "/sub/here.b16");
	img_load(&imgBar, "/sub/hpbar.b16");
	img_load(&imgBarW, "/sub/barwhite.b16");
	static const char *icn[4] = { "nug", "nuggetcool", "max", "face" };
	for (int i = 0; i < 8; i++) {
		char p[48];
		snprintf(p, sizeof(p), "/sub/icon_%s_%d.b16", icn[i / 2], i % 2);
		img_load(&imgIcons[i], p);
	}
	// the song starts with the cartoon characters; tweenOut1 switches to nugget / maxpro
	bf = &chars[CHR_NUGGETOG];
	dad = &chars[CHR_MAX];
	build_notes();

	// state
	health = 1;
	score = misses = combo = 0;
	memset(hits, 0, sizeof(hits));
	totalPlayed = totalHit = 0;
	skyA = stageA = treeA = 1;
	fakeBgA = 1;
	shStrength = 1;
	shDarkness = 0;
	shader_set(1, 0);
	fireOn = false;
	fire_fx_reset();
	shader_set_tint(1, 1, 1);
	hudAlpha = 1;
	uiAlpha = 1;
	uiVisible = true;
	hudVisible = true;
	hudFadeA = 0;
	gameFlashA = otherFlashA = 0;
	tiktokBlack = false;
	helpBarTX = 0;
	helpBarBX = 1280 - 1280 / 3.0f;
	helpBarA = 1;
	helpBarsVisible = true;
	subs[0] = 0;
	helpMode = succeeded = hereVisible = hereAllowScale = false;
	hereScale = 0;
	helpCount = 0;
	goodA = badA = 0;
	endImg = 0;
	vidKind = VID_NONE;
	paused = dead = songEnded = songStarted = false;
	beatsPerZoom = 4;
	camZooming = false;
	forcedPos = false;
	zoomTween = 0;
	camTween = 0;
	defaultZoom = DEF_ZOOM;
	defaultHudZoom = 0.8f;
	hudZoom = 0.8f;
	cam.zoom = defaultZoom;
	cam.shakeX = cam.shakeY = 0;
	shakeTime = 0;
	for (int i = 0; i < 4; i++) {
		strumAnim[i] = 0;
		strumAlpha[i] = 0;
		strumReset[i] = 0;
		tween_num(&strumAlpha[i], 0, 1, 1, EASE_LINEAR, 0.5f + 0.2f * i, NULL, NULL);
	}

	// countdown
	songPos = -CROCHET * 5;
	countdownOn = true;
	countdownTick = 0;
	lastBeat = -100;
	lastStep = -100;
	evIdx = 0;

	// create(): camera on the first section, then onCreatePost / introCam
	move_camera_section();
	snap_camera();
	bf->x -= 700;
	skyA = stageA = treeA = 0.01f;
	hudAlpha = 0;
	forcedPos = true;
	cam.zoom += 0.5f;
	followY += 100;
	followX += 300 - 700;
	snap_camera();
	gameFadeA = 1;

	subDirty = true;
	subKey = -1;
	subStage = -1;
	tapShown = 0;
	sub_hold(false);
	g_fade = 0;
	g_subFade = 1;     // camHUD starts invisible (introCam) and fades in with the song
}

void play_leave(void) {
	audio_stop();
	stop_video();
	sheet_unload(&shSky);
	sheet_unload(&shStage);
	sheet_unload(&shTree);
	sheet_unload(&shOg);
	sheet_unload(&shHud);
	sheet_unload(&shEnd);
	for (int i = 0; i < CHR_COUNT; i++) chr_unload(&chars[i]);
	img_free(&imgHere);
	img_free(&imgBar);
	img_free(&imgBarW);
	for (int i = 0; i < 8; i++) img_free(&imgIcons[i]);
	free(notes);
	notes = NULL;
}

// ---------------------------------------------------------------------------
// pause / game over
// ---------------------------------------------------------------------------
static void open_pause(void) {
	paused = true;
	pauseSel = 0;
	pauseBgA = 0;
	pauseMusicVol = 0;
	audio_pause_song(true);
	audio_play_pause_music(true);
	tween_num(&pauseBgA, 0, 0.6f, 0.4f, EASE_QUADOUT, 0, NULL, NULL);
	subDirty = true;
}

static void close_pause(void) {
	paused = false;
	audio_play_pause_music(false);
	audio_pause_song(false);
	subDirty = true;
}

static void cb_to_menu(void *ctx) { state_switch(ST_MENU); }
static void cb_restart(void *ctx) { state_switch(ST_PLAY); }

static void pause_select(int i) {
	switch (i) {
	case 0: close_pause(); break;
	case 1: audio_play_pause_music(false); state_switch(ST_PLAY); break;
	case 2: audio_play_pause_music(false); deaths = 0; state_switch(ST_MENU); break;
	}
}

static void update_pause(float dt) {
	pauseMusicVol += 0.01f * dt;
	if (pauseMusicVol > 0.5f) pauseMusicVol = 0.5f;
	audio_pause_volume(pauseMusicVol);
	if (kDown & KEY_UP) { pauseSel = (pauseSel + 2) % 3; audio_sfx(SFX_SCROLLMENU, 0.4f); subDirty = true; }
	if (kDown & KEY_DOWN) { pauseSel = (pauseSel + 1) % 3; audio_sfx(SFX_SCROLLMENU, 0.4f); subDirty = true; }
	if (kDown & (KEY_A | KEY_START)) { pause_select(pauseSel); return; }
	if (kDown & KEY_B) { close_pause(); return; }
	if (kDown & KEY_TOUCH) {
		touchRead(&touch);
		for (int i = 0; i < 3; i++) {
			int y = 50 + i * 40;
			if (touch.py >= y && touch.py < y + 32 && touch.px >= 28 && touch.px < 228) {
				pauseSel = i;
				pause_select(i);
				return;
			}
		}
	}
}

static void die(void) {
	dead = true;
	g_subFade = 0;
	deaths++;
	deadTime = 0;
	deadConfirm = false;
	stop_video();
	audio_stop();
	audio_sfx(SFX_FNF_LOSS_SFX, 1);
	subDirty = true;
}

static void update_dead(float dt) {
	deadTime += dt;
	if (deadTime > 2.5f && deadTime - dt <= 2.5f && !deadConfirm)
		audio_play_music("nitro:/snd/gameover.raw", true, 1);
	if (deadConfirm) return;
	if (kDown & (KEY_A | KEY_START)) {
		deadConfirm = true;
		audio_stop();
		audio_sfx(SFX_GAMEOVEREND, 1);
		tween_num(&g_fade, 0, 1, 2, EASE_LINEAR, 0.7f, cb_restart, NULL);
	} else if (kDown & KEY_B) {
		deadConfirm = true;
		audio_stop();
		deaths = 0;
		tween_num(&g_fade, 0, 1, 0.5f, EASE_LINEAR, 0, cb_to_menu, NULL);
	}
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------
void play_update(float dt) {
	if (dead) {
		update_dead(dt);
		if (subDirty) { draw_sub(true); subDirty = false; }
		return;
	}
	if (paused) {
		update_pause(dt);
		if (subDirty) { draw_sub(true); subDirty = false; }
		return;
	}
	if ((kDown & KEY_START) && !songEnded && (songStarted || countdownOn)) {
		open_pause();
		draw_sub(true);
		subDirty = false;
		return;
	}

	// conductor
	if (countdownOn) {
		songPos += dt * 1000;
		int tick = (int)floorf((songPos + CROCHET * 5) / CROCHET) - 1;
		while (countdownTick <= tick && countdownTick <= 4) {
			static const int snd[4] = { SFX_INTRO3, SFX_INTRO2, SFX_INTRO1, SFX_INTROGO };
			if (countdownTick < 4) audio_sfx(snd[countdownTick], 0.6f);
			// characterBopper(tick)
			if (countdownTick % 2 == 0) {
				if (!bf->stunned && strncmp(chr_anim_name(bf), "sing", 4)) chr_dance(bf);
				if (!dad->stunned && strncmp(chr_anim_name(dad), "sing", 4)) chr_dance(dad);
			}
			countdownTick++;
		}
		if (songPos >= 0) {
			countdownOn = false;
			songStarted = true;
			audio_play_song();
			on_song_start();
			songPos = 0;
		}
	} else if (songStarted) {
		float t = audio_song_time();
		// keep the clock monotonic and smooth
		float predicted = songPos + dt * 1000;
		if (fabsf(t - predicted) > 30) songPos = t;
		else songPos = predicted + (t - predicted) * 0.1f;
		if (songPos < 0) songPos = 0;
		if (!songEnded && audio_song_finished()) {
			songEnded = true;
			tween_num(&g_fade, 0, 1, 1, EASE_LINEAR, 0, cb_to_menu, NULL);
		}
	}

	// beats / steps
	int beat = (int)floorf(songPos / CROCHET);
	if (songStarted && beat != lastBeat) {
		if (beat > lastBeat) beat_hit(beat);
		lastBeat = beat;
	}

	// events
	while (evIdx < CHART_EVENTS && chart_events[evIdx].time <= songPos) {
		if (songStarted) run_event(&chart_events[evIdx]);
		evIdx++;
	}

	// zoom tween drives defaultCamZoom
	if (zoomTween) {
		defaultZoom = zoomTweenVal;
		cam.zoom = zoomTweenVal;
		camZooming = false;
	}

	// input
	if (songStarted || countdownOn) {
		for (int d = 0; d < 4; d++) {
			if (kDown & laneKeys[d]) key_press(d);
			if ((kUp & laneKeys[d]) && !(kHeld & laneKeys[d])) {
				strum_play(d, 0);
			}
		}
	}
	if (songStarted) update_notes();

	// strum confirm -> static (opponent style reset is not used for the player)
	for (int d = 0; d < 4; d++) {
		strumTime[d] += dt;
		if (strumAnim[d] == 2 && (settings.botplay || !(kHeld & laneKeys[d])) && strumTime[d] > 0.15f) strumAnim[d] = 0;
	}

	// boyfriend returns to idle when not holding
	{
		bool holding = false;
		for (int d = 0; d < 4; d++) if (kHeld & laneKeys[d]) holding = true;
		if ((!holding || settings.botplay) && bf->holdTimer > STEP * 0.0011f * bf->def->singDur && !strncmp(chr_anim_name(bf), "sing", 4))
			chr_dance(bf);
	}

	// help me taps
	if (helpMode && (kDown & KEY_TOUCH)) {
		touchRead(&touch);
		int hx = (256 - imgHere.w) / 2;
		if (touch.px >= hx && touch.px < hx + imgHere.w && touch.py >= 70 && touch.py < 70 + imgHere.h) {
			helpCount++;
			hereScale = 0.375f;
			shakeTime = 0.5f;
			lastTapFrame = 4;
			subDirty = true;
		}
	}
	if (lastTapFrame > 0) {
		lastTapFrame--;
		if (lastTapFrame == 0) subDirty = true;
	}
	if (hereAllowScale) hereScale = lerpf(hereScale, 0.35f, 1 - expf(-dt * 6));

	// characters
	for (int i = 0; i < CHR_COUNT; i++) chr_update(&chars[i], dt, STEP);

	// camera
	if (!forcedPos && songStarted) move_camera_section();
	float lerp = 1 - expf(-dt * 2.4f);
	cam.camX += (followX - cam.camX) * lerp;
	cam.camY += (followY - cam.camY) * lerp;
	if (camZooming) {
		float k = expf(-dt * 3.125f);
		cam.zoom = defaultZoom + (cam.zoom - defaultZoom) * k;
		hudZoom = defaultHudZoom + (hudZoom - defaultHudZoom) * k;
	}
	if (shakeTime > 0) {
		shakeTime -= dt;
		float amp = 0.0025f * 1280 * cam.zoom * KDS;
		cam.shakeX = ((rand() % 2001) / 1000.0f - 1) * amp;
		cam.shakeY = ((rand() % 2001) / 1000.0f - 1) * amp * 0.5625f;
	} else cam.shakeX = cam.shakeY = 0;

	// shader palettes (the sky is drawn opaque with its alpha premultiplied)
	fire_fx_update(dt);
	shader_set(shStrength, shDarkness - floorf(firePulse * 4) / 4 * 0.12f * fireFx);
	// firelight: warm the shaded palettes (quantized so the LUTs rebuild rarely)
	{
		float w = floorf((fireFx * (0.7f + firePulse * 0.3f)) * 8 + 0.5f) / 8;
		shader_set_tint(1 + 0.45f * w, 1 - 0.05f * w, 1 - 0.45f * w);
	}

	shader_set_alpha(sky_shade_index(), skyA);
	// fire overlay belongs to camGame: hide it behind the tiktok camera / HUD fade
	video_set_overlay_visible(!tiktokBlack && hudFadeA < 0.95f && endImg == 0);
	if (helpBarsVisible && helpBarA > 0.5f)
		video_set_window((int)oth_x(helpBarTX + 1280 / 3.0f), (int)oth_x(helpBarBX));
	else video_set_window(0, 0);

	// videos
	if (vidKind != VID_NONE) {
		extern u32 perf_t[6];
		u32 _t0 = time_ticks();
		bool ok = video_update(songPos - vidStart);
		{ u32 _d = time_ticks() - _t0; if (_d > perf_t[5]) perf_t[5] = _d; }
		if (!ok) on_video_end();
		else if (vidKind == VID_FIRE) {
			// fire sprite: (0,0) world, width = sky width, scrollFactor 1.1
			float s = 256.0f / STAGE_SKY_W;
			float pa = s / (cam.zoom * KDS);
			float wx0 = (0 - 128 - cam.shakeX) / (cam.zoom * KDS) + 640 + (cam.camX - 640) * 1.1f;
			float wy0 = (0 - 96 - cam.shakeY) / (cam.zoom * KDS) + 360 + (cam.camY - 360) * 1.1f;
			video_set_affine(pa, wx0 * s, wy0 * s);
		}
	}

	// bottom screen = camHUD: follow its alpha / fade / visibility
	{
		float target = 1 - clampf(hudAlpha, 0, 1);
		if (hudFadeA > target) target = hudFadeA;
		if (!hudVisible) target = 1;
		float step = dt / 0.6f;
		if (g_subFade < target) g_subFade = g_subFade + step > target ? target : g_subFade + step;
		else if (g_subFade > target) g_subFade = g_subFade - step < target ? target : g_subFade - step;
	}

	// health / death
	if (health <= 0 && songStarted) {
		health = 0;
		die();
		return;
	}

	if (subStage >= 0) draw_sub(false);             // staged full redraw in progress
	else if (subMode == 3 && subKey >= 0 && tapShown != (lastTapFrame > 0)) draw_sub(false);   // tap flash, cheap
	else if (subDirty && ++subTimer >= 12) {
		draw_sub(false);
		subDirty = false;
		subTimer = 0;
	}
}

// ---------------------------------------------------------------------------
// draw
// ---------------------------------------------------------------------------
static void draw_note_sprite(int frame, float vx, float vy, int alpha, u16 color) {
	sheet_draw(&shHud, frame, hud_x(vx), hud_y(vy), hudZoom * KDS, color, alpha);
}

static void draw_hud(void) {
	if (!hudVisible) return;
	float ha = hudAlpha;
	float strumY = settings.downscroll ? (720 - 150 + 100) : (50 - 100);
	strumY += 55;  // center of the strum hitbox
	float speed = CHART_SPEED;
	float pos = songPos - settings.noteOffset;
	// strums
	for (int d = 0; d < 4; d++) {
		float cx = 732 + 112 * d + 55;
		int fr;
		if (strumAnim[d] == 2) {
			int f = (int)(strumTime[d] * 24);
			if (f > 3) f = 3;
			fr = HUD_CONFIRM0_0 + d * 10 + f;
		} else if (strumAnim[d] == 1) {
			int f = (int)(strumTime[d] * 24);
			if (f > 3) f = 3;
			fr = HUD_PRESS0_0 + d * 10 + f;
		} else fr = HUD_STATIC0 + d * 10;
		draw_note_sprite(fr, cx, strumY, A31(strumAlpha[d] * ha), 0x7FFF);
	}
	// notes
	float pxPerMs = 0.45f * speed;
	float dir = settings.downscroll ? -1 : 1;
	for (int i = firstActive; i < nNotes; i++) {
		Note *n = &notes[i];
		float dy = (n->t - pos) * pxPerMs;
		if (dy > 900) break;
		if (!n->player || n->state == 2) continue;
		float cx = 732 + 112 * n->dir + 55;
		float a = strumAlpha[n->dir] * ha;
		if (n->state == 3) a *= 0.3f;
		// sustain
		if (n->len > 0) {
			float yStart = n->state == 1 ? strumY : strumY + dy * dir;
			float yEnd = strumY + (n->t + n->len - pos) * pxPerMs * dir;
			int hold = HUD_HOLD0 + n->dir * 2, end = hold + 1;
			const PartDef *ep = &shHud.def->parts[shHud.def->frames[end].first];
			const PartDef *hp = &shHud.def->parts[shHud.def->frames[hold].first];
			float s = hudZoom * KDS / shHud.def->scale;
			float hw = hp->w * s, ew = ep->w * s, eh = ep->h * s;
			float x = hud_x(cx);
			float y0 = hud_y(yStart), y1 = hud_y(yEnd);
			int sa = A31(a * 0.6f);
			if (!settings.downscroll) {
				if (y1 - eh > y0) sheet_draw_rect(&shHud, hold, x - hw / 2, y0, x + hw / 2, y1 - eh, 0, 1, 0x7FFF, sa);
				if (y1 > y0) {
					float top = y1 - eh < y0 ? y0 : y1 - eh;
					float fv = (top - (y1 - eh)) / eh;
					sheet_draw_rect(&shHud, end, x - ew / 2, top, x + ew / 2, y1, fv, 1, 0x7FFF, sa);
				}
			} else {
				// flipped: end cap at the top
				if (y1 + eh < y0) sheet_draw_rect(&shHud, hold, x - hw / 2, y1 + eh, x + hw / 2, y0, 0, 1, 0x7FFF, sa);
				if (y1 < y0) {
					float bot = y1 + eh > y0 ? y0 : y1 + eh;
					float fv = (bot - y1) / eh;
					sheet_draw_rect(&shHud, end, x - ew / 2, bot, x + ew / 2, y1, 1 - fv, 1, 0x7FFF, sa);
				}
			}
		}
		if (n->state == 0 || n->state == 3)
			draw_note_sprite(HUD_NOTE0 + n->dir * 10, cx, strumY + dy * dir, A31(a), 0x7FFF);
	}
	// battery + icons are drawn only on the bottom screen
	// help button
	if (hereVisible && hereScale > 0.01f) {
		float s = hereScale / 0.35f * hudZoom * KDS;
		sheet_draw(&shHud, HUD_HERE, hud_x(640), hud_y(50 + HERE_H / 2), s, 0x7FFF, A31(ha));
	}
}

void play_draw(void) {
	// camGame
	if (!tiktokBlack) {
		u16 white = 0x7FFF;
		// sky
		float k = cam.zoom * KDS;
		if (skyA > 0.004f) sheet_draw(&shSky, SKY_MAIN, cam_sx(&cam, 0, 1), cam_sy(&cam, 0, 1), k, white, 31);
		sheet_draw(&shStage, STAGE_MAIN, cam_sx(&cam, 0, 1), cam_sy(&cam, 0, 1), k, white, A31(stageA));
		if (fakeBgA > 0.01f) sheet_draw(&shOg, OG_MAIN, cam_sx(&cam, 2000, 1), cam_sy(&cam, 1200, 1), k, white, A31(fakeBgA));
		chr_draw(dad, &cam, white);
		chr_draw(bf, &cam, white);
		sheet_draw(&shTree, TREE_MAIN, cam_sx(&cam, -1200, 1.2f), cam_sy(&cam, 400, 1.2f), k, white, A31(treeA * 0.9f));
		sheet_draw(&shTree, TREE_MAIN, cam_sx(&cam, 4000, 1.2f), cam_sy(&cam, 400, 1.2f), k, white, A31(treeA * 0.9f));
		fire_fx_draw();
		if (gameFlashA > 0) gfx_rect(0, 0, 256, 192, gameFlashColor, A31(gameFlashA));
		if (gameFadeA > 0) gfx_rect(0, 0, 256, 192, 0, A31(gameFadeA));
	}
	// tiktokCam
	if (helpBarsVisible && helpBarA > 0.01f) {
		gfx_rect(oth_x(helpBarTX), 0, oth_x(helpBarTX + 1280 / 3.0f), 192, 0, A31(helpBarA));
		gfx_rect(oth_x(helpBarBX), 0, oth_x(helpBarBX + 1280 / 3.0f), 192, 0, A31(helpBarA));
	}
	if (subs[0]) {
		Font *f = font_get(FONT_SUBS);
		font_draw_centered(FONT_SUBS, 128, 192 - f->def->lineh - 4, subs, 0x7FFF, 31);
	}
	// camHUD
	draw_hud();
	if (hudFadeA > 0 && hudVisible) gfx_rect(0, 0, 256, 192, 0, A31(hudFadeA));
	// camOther
	if (endImg == 1) sheet_draw(&shEnd, GOODEND_MAIN, 128, 96, KDS, 0x7FFF, A31(goodA));
	if (endImg == 2) sheet_draw(&shEnd, BADEND_MAIN, 128, 96, KDS, 0x7FFF, A31(badA));
	if (otherFlashA > 0) gfx_rect(0, 0, 256, 192, otherFlashColor, A31(otherFlashA));
	// pause
	if (paused) {
		gfx_rect(0, 0, 256, 192, 0, A31(pauseBgA));
		static const char *items[3] = { "Resume", "Restart Song", "Exit to menu" };
		font_draw(FONT_VCR, 250 - font_width(FONT_VCR, "twiddlefinger"), 6, "twiddlefinger", 0x7FFF, 31);
		for (int i = 0; i < 3; i++) {
			int y = 70 + (i - pauseSel) * 30;
			font_draw(FONT_VCR, 30 + (i - pauseSel) * 6, y, items[i], 0x7FFF, i == pauseSel ? 31 : 18);
		}
	}
	if (dead) {
		gfx_rect(0, 0, 256, 192, 0, 31);
		font_draw_centered(FONT_VCR, 128, 90, "GAME OVER", RGB(255, 80, 80), 31);
	}
}
