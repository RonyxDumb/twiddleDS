#pragma once
#include "engine.h"

typedef struct {
	float camX, camY;      // camera center (world)
	float zoom;
	float shakeX, shakeY;  // DS px
} Cam;

typedef struct {
	const ChrDef *def;
	int id;
	u8 *data;
	u16 *palOrig;
	u32 slotOff, texParam, palParam, palAddr;
	int slotW;
	float x, y, alpha;
	int anim;
	float animTime;
	int frameIdx;
	bool finished;
	float holdTimer;
	bool stunned;
	bool isPlayer;
	int uploaded;
	bool loaded;
} Character;

bool chr_load(Character *c, int id, bool isPlayer);
void chr_unload(Character *c);
int chr_find_anim(Character *c, const char *name);
void chr_play(Character *c, const char *name, bool force);
void chr_dance(Character *c);
void chr_update(Character *c, float dt, float stepCrochet);
void chr_draw(Character *c, const Cam *cam, u16 color);
const char *chr_anim_name(Character *c);
float chr_mid_x(Character *c);
float chr_mid_y(Character *c);

// world -> DS screen
static inline float cam_sx(const Cam *c, float wx, float sf) {
	return ((wx - c->camX * sf + 640.0f * sf - 640.0f) * c->zoom) * KDS + 128.0f + c->shakeX;
}
static inline float cam_sy(const Cam *c, float wy, float sf) {
	return ((wy - c->camY * sf + 360.0f * sf - 360.0f) * c->zoom) * KDS + 96.0f + c->shakeY;
}

u32 gfx_chr_slot(u32 size);
