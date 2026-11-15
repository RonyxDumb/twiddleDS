// Psych Engine Character, simplified (flixel animation semantics)
#include "character.h"

bool chr_load(Character *c, int id, bool isPlayer) {
	memset(c, 0, sizeof(*c));
	c->def = chr_defs[id];
	c->id = id;
	c->isPlayer = isPlayer;
	u32 size;
	c->data = file_load(c->def->file, &size);
	if (!c->data) return false;
	c->slotW = 8 << c->def->slotWlog;
	int slotH = 8 << c->def->slotHlog;
	c->slotOff = gfx_chr_slot(c->slotW * slotH);
	c->texParam = (c->slotOff >> 3) | (c->def->slotWlog << 20) | (c->def->slotHlog << 23) | (GL_RGB256 << 26) | GL_TEXTURE_COLOR0_TRANSPARENT;
	c->palAddr = pal_alloc(512);
	c->palParam = c->palAddr >> 4;
	pal_write(c->palAddr, (u16 *)c->data, 256);
	c->palOrig = (u16 *)c->data;      // palette is at the start of the blob
	shader_register(c->palOrig, c->palAddr, 256);
	c->alpha = 1;
	c->uploaded = -1;
	c->anim = -1;
	c->loaded = true;
	chr_dance(c);
	return true;
}

void chr_unload(Character *c) {
	if (c->data) free(c->data);
	c->data = NULL;
	c->loaded = false;
}

int chr_find_anim(Character *c, const char *name) {
	for (int i = 0; i < c->def->nAnims; i++)
		if (!strcmp(c->def->anims[i].name, name)) return i;
	return -1;
}

const char *chr_anim_name(Character *c) {
	if (c->anim < 0) return "";
	return c->def->anims[c->anim].name;
}

void chr_play(Character *c, const char *name, bool force) {
	int a = chr_find_anim(c, name);
	if (a < 0) return;
	if (!force && a == c->anim && !c->finished) return;
	c->anim = a;
	c->animTime = 0;
	c->frameIdx = 0;
	c->finished = false;
}

void chr_dance(Character *c) {
	chr_play(c, "idle", false);
}

void chr_update(Character *c, float dt, float stepCrochet) {
	if (!c->loaded || c->anim < 0) return;
	const ChrAnim *a = &c->def->anims[c->anim];
	if (!strncmp(a->name, "sing", 4)) c->holdTimer += dt;
	else if (c->isPlayer) c->holdTimer = 0;
	if (!c->isPlayer && c->holdTimer >= stepCrochet * 0.0011f * c->def->singDur) {
		chr_dance(c);
		c->holdTimer = 0;
		a = &c->def->anims[c->anim];
	}
	c->animTime += dt;
	int n = a->nFrames;
	int f = (int)(c->animTime * a->fps);
	if (f >= n) {
		if (a->loop) f %= n;
		else {
			f = n - 1;
			c->finished = true;
		}
	}
	c->frameIdx = f;
}

float chr_mid_x(Character *c) { return c->x + c->def->width / 2; }
float chr_mid_y(Character *c) { return c->y + c->def->height / 2; }

void chr_draw(Character *c, const Cam *cam, u16 color) {
	if (!c->loaded || c->anim < 0 || c->alpha <= 0.01f) return;
	const ChrAnim *a = &c->def->anims[c->anim];
	int uf = a->frames[c->frameIdx];
	const ChrFrame *f = &c->def->frames[uf];
	if (uf != c->uploaded) {
		gfx_queue_upload(c->slotOff, c->data + f->off, f->w, f->h, c->slotW);
		c->uploaded = uf;
	}
	float wx = c->x - a->offx + f->dx;
	float wy = c->y - a->offy + f->dy;
	float x0 = cam_sx(cam, wx, 1), y0 = cam_sy(cam, wy, 1);
	float x1 = cam_sx(cam, wx + f->ww, 1), y1 = cam_sy(cam, wy + f->wh, 1);
	gfx_quad_raw((int)(x0 * 16), (int)(y0 * 16), (int)(x1 * 16), (int)(y1 * 16), 0, 0, f->cw << 4, f->h << 4,
		c->texParam, c->palParam, color, (int)(c->alpha * 31 + 0.5f));
}
