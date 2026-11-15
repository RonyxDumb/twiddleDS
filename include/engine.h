// Twiddlefinger DS - small engine shared declarations
#pragma once
#include <nds.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gen_assets.h"

// ---------------------------------------------------------------------------
// coordinate helpers
// ---------------------------------------------------------------------------
#define VW 1280.0f
#define VH 720.0f
#define KDS (192.0f / 720.0f)       // virtual px -> DS px

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ---------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------
void *file_load(const char *path, u32 *size);   // malloc'd, NULL on failure

// ---------------------------------------------------------------------------
// 3D graphics (top screen)
// ---------------------------------------------------------------------------
#define RGB(r, g, b) RGB15((r) >> 3, (g) >> 3, (b) >> 3)

typedef struct {
	const SheetDef *def;
	u32 texOff[24];
	u32 texParam[24];
	u32 palParam;
	u32 palAddr;
	u16 *palOrig;           // original palette (for shader LUT), may be NULL
	bool loaded;
	bool shaded;            // follows the "scary" shader palette
} Sheet;

typedef struct {
	const FontDef *def;
	u8 *alpha;              // glyph alpha atlas (RAM copy, for the bottom screen)
	u32 texOff, texParam, palParam;
	bool onTop;
} Font;

void gfx_init(void);
void gfx_reset_vram(void);              // free all textures/palettes
void gfx_frame_begin(void);
void gfx_frame_end(void);               // glFlush
void gfx_vblank(void);                  // pending VRAM uploads (call right after vblank)

u32 tex_alloc(u32 size);
void tex_free(u32 off, u32 size);
u32 pal_alloc(u32 bytes);
void tex_write(u32 off, const void *src, u32 size);     // maps banks to LCD (use outside gameplay)
void pal_write(u32 addr, const u16 *src, u32 count);

bool sheet_load(Sheet *s, const SheetDef *d, bool shaded);
void sheet_unload(Sheet *s);
// draw frame with origin at DS screen position (x,y), scale = DS px per sheet unit
void sheet_draw(Sheet *s, int frame, float x, float y, float scale, u16 color, int alpha);
void sheet_draw_ex(Sheet *s, int frame, float x, float y, float sx, float sy, u16 color, int alpha);
// stretched part of frame: (u0..u1, v0..v1 in [0,1]) to a screen rect
void sheet_draw_rect(Sheet *s, int frame, float x0, float y0, float x1, float y1, float fv0, float fv1, u16 color, int alpha);
void gfx_rect(float x0, float y0, float x1, float y1, u16 color, int alpha);
void gfx_quad_raw(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, u32 tex, u32 pal, u16 color, int alpha);

// shader palettes ("scary" shader: contrast strength + darkness)
void shader_set(float strength, float darkness);
void shader_register(u16 *orig, u32 palAddr, int count);
void shader_clear(void);
int shader_last(void);
int shader_find(u32 palAddr);                  // index of the last registered palette
void shader_set_alpha(int idx, float a);  // premultiplied alpha (layer over black)

// character texture streaming
void gfx_queue_upload(u32 texOff, const u8 *src, int w4, int h, int stride);

// fonts
void font_load(int id, bool top);
void font_unload_all(void);
Font *font_get(int id);
int font_width(int id, const char *s);
void font_draw(int id, float x, float y, const char *s, u16 color, int alpha);   // top screen
void font_draw_centered(int id, float cx, float y, const char *s, u16 color, int alpha);

// ---------------------------------------------------------------------------
// bottom screen (software rendered 16bpp bitmap)
// ---------------------------------------------------------------------------
typedef struct { u16 w, h; u16 *px; } Img16;
void sub_init(void);
void sub_present(void);                 // copy back buffer to VRAM (vblank)
u16 *sub_fb(void);
void sub_clear(u16 color);
void sub_fill(int x, int y, int w, int h, u16 color);
void sub_fill_blend(int x, int y, int w, int h, u16 color, int a32);
bool img_load(Img16 *img, const char *path);
void img_free(Img16 *img);
void sub_blit(const Img16 *img, int x, int y);
void sub_blit_tint(const Img16 *img, int x, int y, int clipw, u16 tint);
void sub_text(int font, int x, int y, const char *s, u16 color);
void sub_text_centered(int font, int cx, int y, const char *s, u16 color);

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void audio_init(void);
void audio_update(void);                // refill stream buffers (main loop)
void audio_play_music(const char *path, bool loop, float volume);
void audio_play_song(void);             // inst + voices
void audio_stop(void);
void audio_set_volume(float v);
float audio_get_volume(void);
void audio_set_voices(float v);
void audio_pause_song(bool paused);     // pause song, keep position
bool audio_song_finished(void);
float audio_song_time(void);            // ms, interpolated
void audio_sfx(int id, float vol);
void audio_play_pause_music(bool on);   // tea time loop while the song is paused
void audio_pause_volume(float v);
u32 time_ticks(void);                   // calico system ticks (TICK_FREQ Hz)

// ---------------------------------------------------------------------------
// video (main engine BG2 bitmap)
// ---------------------------------------------------------------------------
bool video_open(const char *path, bool loop, bool overlay);
void video_close(void);
bool video_active(void);
bool video_update(float ms);            // returns false when finished
void video_vblank(void);
void video_set_affine(float pa, float px, float py);
void video_set_blend(int eva);
void video_set_overlay_visible(bool on);
void video_set_window(int x0, int x1);   // x0 >= x1 disables
float video_duration(void);

// ---------------------------------------------------------------------------
// tweens
// ---------------------------------------------------------------------------
enum { EASE_LINEAR, EASE_CUBEOUT, EASE_CUBEIN, EASE_CUBEINOUT, EASE_CIRCIN, EASE_SINEOUT, EASE_SINEIN,
	EASE_SINEINOUT, EASE_QUADOUT, EASE_BACKOUT, EASE_BACKIN };
typedef void (*TweenCb)(void *ctx);
int tween_num(float *target, float from, float to, float dur, int ease, float delay, TweenCb cb, void *ctx);
int tween_to(float *target, float to, float dur, int ease, float delay, TweenCb cb, void *ctx);
void tween_cancel(int id);
void tween_cancel_target(float *target);
void tween_update(float dt);
void tween_clear(void);
int timer_after(float delay, TweenCb cb, void *ctx);
float ease_apply(int ease, float t);

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------
extern u32 kDown, kHeld, kUp;
extern touchPosition touch;
extern bool touchDown;

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------
typedef struct { bool downscroll; bool ghostTapping; int noteOffset; bool camZooms; bool botplay; } Settings;
extern Settings settings;
void settings_load(void);
void settings_save(void);

// ---------------------------------------------------------------------------
// states
// ---------------------------------------------------------------------------
enum { ST_MENU, ST_OPTIONS, ST_PLAY };
void state_switch(int st);
void menu_enter(void); void menu_update(float dt); void menu_draw(void); void menu_leave(void);
void options_enter(void); void options_update(float dt); void options_draw(void); void options_leave(void);
void play_enter(void); void play_update(float dt); void play_draw(void); void play_leave(void);

// main screen fade (0 = visible, 1 = black)
extern float g_fade;

#include "soundbank.h"
