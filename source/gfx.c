// 3D renderer for the top screen: everything is drawn as textured quads.
//
// VRAM layout
//   A, B, D : texture slots 0, 1, 2 (384 KB). The first 64 KB of bank A holds
//             the character frame slots, streamed every frame in vblank.
//   C       : bottom screen 16bpp bitmap
//   E       : main engine BG2 (videos / fire overlay)
//   F       : texture palettes (16 KB)
#include "engine.h"

#define TEX_TOTAL   (384 * 1024)
#define CHR_REGION  (64 * 1024)
#define PAL_TOTAL   (16 * 1024)

typedef struct { u32 off, size; } Block;
static Block freeList[96];
static int nFree;
static u32 chrNext;
static u32 palNext;

// ---------------------------------------------------------------------------
// VRAM allocators
// ---------------------------------------------------------------------------
void gfx_reset_vram(void) {
	nFree = 1;
	freeList[0].off = CHR_REGION;
	freeList[0].size = TEX_TOTAL - CHR_REGION;
	chrNext = 0;
	palNext = 0;
	shader_clear();
}

u32 tex_alloc(u32 size) {
	size = (size + 7) & ~7;
	for (int i = 0; i < nFree; i++) {
		// never let a texture cross a 128 KB bank (slot) boundary
		u32 off = freeList[i].off;
		u32 end = freeList[i].off + freeList[i].size;
		u32 bankEnd = (off & ~0x1FFFF) + 0x20000;
		if (off + size > bankEnd) off = bankEnd;
		if (off + size <= end) {
			u32 before = off - freeList[i].off;
			u32 after = end - (off + size);
			if (before == 0) {
				freeList[i].off = off + size;
				freeList[i].size = after;
			} else {
				freeList[i].size = before;
				if (after) {
					memmove(&freeList[i + 2], &freeList[i + 1], (nFree - i - 1) * sizeof(Block));
					freeList[i + 1].off = off + size;
					freeList[i + 1].size = after;
					nFree++;
				}
			}
			if (freeList[i].size == 0) {
				memmove(&freeList[i], &freeList[i + 1], (nFree - i - 1) * sizeof(Block));
				nFree--;
			}
			return off;
		}
	}
	return 0xFFFFFFFF;
}

void tex_free(u32 off, u32 size) {
	size = (size + 7) & ~7;
	int i = 0;
	while (i < nFree && freeList[i].off < off) i++;
	memmove(&freeList[i + 1], &freeList[i], (nFree - i) * sizeof(Block));
	freeList[i].off = off;
	freeList[i].size = size;
	nFree++;
	// merge
	for (int j = 0; j < nFree - 1;) {
		if (freeList[j].off + freeList[j].size == freeList[j + 1].off) {
			freeList[j].size += freeList[j + 1].size;
			memmove(&freeList[j + 1], &freeList[j + 2], (nFree - j - 2) * sizeof(Block));
			nFree--;
		} else j++;
	}
}

static u32 chr_alloc(u32 size) {
	u32 o = chrNext;
	chrNext += (size + 7) & ~7;
	if (chrNext > CHR_REGION) return 0xFFFFFFFF;
	return o;
}

u32 pal_alloc(u32 bytes) {
	u32 o = (palNext + 15) & ~15;
	if (o + bytes > PAL_TOTAL) return 0xFFFFFFFF;
	palNext = o + bytes;
	return o;
}

static inline u32 *tex_lcd(u32 off) {
	if (off < 0x40000) return (u32 *)(0x06800000 + off);
	return (u32 *)(0x06860000 + (off - 0x40000));
}

void tex_write(u32 off, const void *src, u32 size) {
	u32 saved = vramSetPrimaryBanks(VRAM_A_LCD, VRAM_B_LCD, VRAM_C_SUB_BG, VRAM_D_LCD);
	u32 *d = tex_lcd(off);
	const u32 *s = src;
	for (u32 i = 0; i < size / 4; i++) d[i] = s[i];
	vramRestorePrimaryBanks(saved);
}

void pal_write(u32 addr, const u16 *src, u32 count) {
	vramSetBankF(VRAM_F_LCD);
	u16 *d = (u16 *)(0x06890000 + addr);
	for (u32 i = 0; i < count; i++) d[i] = src[i];
	vramSetBankF(VRAM_F_TEX_PALETTE_SLOT0);
}

// ---------------------------------------------------------------------------
// init / frame
// ---------------------------------------------------------------------------
static int g_z;
static int g_id;

void gfx_init(void) {
	videoSetMode(MODE_5_3D);
	vramSetPrimaryBanks(VRAM_A_TEXTURE_SLOT0, VRAM_B_TEXTURE_SLOT1, VRAM_C_SUB_BG, VRAM_D_TEXTURE_SLOT2);
	vramSetBankE(VRAM_E_MAIN_BG);
	vramSetBankF(VRAM_F_TEX_PALETTE_SLOT0);
	lcdMainOnTop();

	glInit();
	glEnable(GL_TEXTURE_2D | GL_BLEND);
	glDisable(GL_ANTIALIAS | GL_OUTLINE | GL_FOG);
	glClearColor(0, 0, 0, 0);
	glClearPolyID(63);
	glClearDepth(GL_MAX_DEPTH);
	glViewport(0, 0, 255, 191);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	// vertex units are 1/16 DS pixel
	glOrthof32(0, 4096, 3072, 0, -4096, 4096);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	bgSetPriority(0, 1);
	gfx_reset_vram();
}

void gfx_frame_begin(void) {
	g_z = -4000;
	g_id = 0;
}

void gfx_frame_end(void) {
	glFlush(GL_TRANS_MANUALSORT);
}

// ---------------------------------------------------------------------------
// quads
// ---------------------------------------------------------------------------
void gfx_quad_raw(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, u32 tex, u32 pal, u16 color, int alpha) {
	if (alpha <= 0) return;
	if (alpha > 31) alpha = 31;
	if (x1 <= x0 || y1 <= y0) return;
	if (x1 <= 0 || y1 <= 0 || x0 >= 4096 || y0 >= 3072) return;
	// clip to the screen (keeps v16 coordinates in range)
	if (x0 < 0) { u0 += (int)((s64)(u1 - u0) * (0 - x0) / (x1 - x0)); x0 = 0; }
	if (x1 > 4096) { u1 -= (int)((s64)(u1 - u0) * (x1 - 4096) / (x1 - x0)); x1 = 4096; }
	if (y0 < 0) { v0 += (int)((s64)(v1 - v0) * (0 - y0) / (y1 - y0)); y0 = 0; }
	if (y1 > 3072) { v1 -= (int)((s64)(v1 - v0) * (y1 - 3072) / (y1 - y0)); y1 = 3072; }
	g_id = (g_id % 62) + 1;
	int z = g_z;
	g_z += 2;
	GFX_POLY_FORMAT = POLY_ALPHA(alpha) | POLY_CULL_NONE | POLY_ID(g_id);
	GFX_TEX_FORMAT = tex;
	GFX_PAL_FORMAT = pal;
	GFX_BEGIN = GL_QUADS;
	GFX_COLOR = color;
	GFX_TEX_COORD = TEXTURE_PACK(u0, v0);
	GFX_VERTEX16 = ((u32)(y0 & 0xFFFF) << 16) | (x0 & 0xFFFF);
	GFX_VERTEX16 = z & 0xFFFF;
	GFX_TEX_COORD = TEXTURE_PACK(u0, v1);
	GFX_VERTEX16 = ((u32)(y1 & 0xFFFF) << 16) | (x0 & 0xFFFF);
	GFX_VERTEX16 = z & 0xFFFF;
	GFX_TEX_COORD = TEXTURE_PACK(u1, v1);
	GFX_VERTEX16 = ((u32)(y1 & 0xFFFF) << 16) | (x1 & 0xFFFF);
	GFX_VERTEX16 = z & 0xFFFF;
	GFX_TEX_COORD = TEXTURE_PACK(u1, v0);
	GFX_VERTEX16 = ((u32)(y0 & 0xFFFF) << 16) | (x1 & 0xFFFF);
	GFX_VERTEX16 = z & 0xFFFF;
	GFX_END = 0;
}

static inline int fx16(float v) {
	if (v > 60000.0f) v = 60000.0f;
	if (v < -60000.0f) v = -60000.0f;
	return (int)(v * 16.0f);
}

void gfx_rect(float x0, float y0, float x1, float y1, u16 color, int alpha) {
	gfx_quad_raw(fx16(x0), fx16(y0), fx16(x1), fx16(y1), 0, 0, 0, 0, 0, 0, color, alpha);
}

// ---------------------------------------------------------------------------
// sheets
// ---------------------------------------------------------------------------
bool sheet_load(Sheet *s, const SheetDef *d, bool shaded) {
	memset(s, 0, sizeof(*s));
	s->def = d;
	u32 size;
	u8 *buf = file_load(d->file, &size);
	if (!buf) return false;
	u32 palBytes = d->palCount * 2;
	s->palAddr = pal_alloc(palBytes);
	pal_write(s->palAddr, (u16 *)buf, d->palCount);
	s->palParam = s->palAddr >> 4;
	if (shaded) {
		s->palOrig = malloc(palBytes);
		memcpy(s->palOrig, buf, palBytes);
		shader_register(s->palOrig, s->palAddr, d->palCount);
		s->shaded = true;
	}
	u32 saved = vramSetPrimaryBanks(VRAM_A_LCD, VRAM_B_LCD, VRAM_C_SUB_BG, VRAM_D_LCD);
	for (int i = 0; i < d->nTex; i++) {
		const TexDef *t = &d->tex[i];
		u32 off = tex_alloc(t->size);
		if (off == 0xFFFFFFFF) {
			vramRestorePrimaryBanks(saved);
			free(buf);
			return false;
		}
		u32 *dst = tex_lcd(off);
		const u32 *src = (const u32 *)(buf + t->off);
		for (u32 k = 0; k < t->size / 4; k++) dst[k] = src[k];
		s->texOff[i] = off;
		s->texParam[i] = (off >> 3) | (t->wlog << 20) | (t->hlog << 23) | (t->fmt << 26) | GL_TEXTURE_COLOR0_TRANSPARENT;
	}
	vramRestorePrimaryBanks(saved);
	free(buf);
	s->loaded = true;
	return true;
}

void sheet_unload(Sheet *s) {
	if (!s->loaded) return;
	for (int i = 0; i < s->def->nTex; i++) tex_free(s->texOff[i], s->def->tex[i].size);
	if (s->palOrig) free(s->palOrig);
	s->palOrig = NULL;
	s->loaded = false;
}

void sheet_draw_ex(Sheet *s, int frame, float x, float y, float sx, float sy, u16 color, int alpha) {
	if (!s->loaded || alpha <= 0) return;
	const SheetDef *d = s->def;
	const FrameDef *f = &d->frames[frame];
	float ks = sx / d->scale, kt = sy / d->scale;
	for (int i = 0; i < f->count; i++) {
		const PartDef *p = &d->parts[f->first + i];
		float px = x + p->dx * ks, py = y + p->dy * kt;
		gfx_quad_raw(fx16(px), fx16(py), fx16(px + p->w * ks), fx16(py + p->h * kt),
			p->u << 4, p->v << 4, (p->u + p->w) << 4, (p->v + p->h) << 4,
			s->texParam[p->tex], s->palParam, color, alpha);
	}
}

void sheet_draw(Sheet *s, int frame, float x, float y, float scale, u16 color, int alpha) {
	sheet_draw_ex(s, frame, x, y, scale, scale, color, alpha);
}

void sheet_draw_rect(Sheet *s, int frame, float x0, float y0, float x1, float y1, float fv0, float fv1, u16 color, int alpha) {
	if (!s->loaded || alpha <= 0) return;
	const SheetDef *d = s->def;
	const PartDef *p = &d->parts[d->frames[frame].first];
	int v0 = (p->v << 4) + (int)(p->h * 16 * fv0);
	int v1 = (p->v << 4) + (int)(p->h * 16 * fv1);
	gfx_quad_raw(fx16(x0), fx16(y0), fx16(x1), fx16(y1), p->u << 4, v0, (p->u + p->w) << 4, v1,
		s->texParam[p->tex], s->palParam, color, alpha);
}

// ---------------------------------------------------------------------------
// "scary" shader emulated through palettes:
//   rgb = ((rgb - 0.5) * strength + 0.5) * (1 - darkness)
// ---------------------------------------------------------------------------
typedef struct { u16 *orig; u32 addr; int count; } ShadePal;
static ShadePal shadePals[16];
static int nShade;
static u16 shadeBuf[16][256];
static bool shadeDirty;
static float shStrength = 1, shDarkness = 0;
static u8 lut[32];

void shader_clear(void) { nShade = 0; shStrength = 1; shDarkness = 0; }

static void shader_build(int idx) {
	ShadePal *p = &shadePals[idx];
	for (int i = 0; i < p->count; i++) {
		u16 c = p->orig[i];
		shadeBuf[idx][i] = lut[c & 31] | (lut[(c >> 5) & 31] << 5) | (lut[(c >> 10) & 31] << 10);
	}
}

static void shader_lut(void) {
	for (int i = 0; i < 32; i++) {
		float v = i / 31.0f;
		v = ((v - 0.5f) * (shStrength > 0 ? shStrength : 0) + 0.5f);
		v = clampf(v, 0, 1) * (1 - shDarkness);
		lut[i] = (u8)clampf(v * 31.0f + 0.5f, 0, 31);
	}
}

void shader_register(u16 *orig, u32 palAddr, int count) {
	if (nShade >= 16) return;
	shadePals[nShade].orig = orig;
	shadePals[nShade].addr = palAddr;
	shadePals[nShade].count = count > 256 ? 256 : count;
	nShade++;
	shader_lut();
	shader_build(nShade - 1);
	shadeDirty = true;
}

void shader_set(float strength, float darkness) {
	if (fabsf(strength - shStrength) < 0.01f && fabsf(darkness - shDarkness) < 0.005f) return;
	shStrength = strength;
	shDarkness = darkness;
	shader_lut();
	for (int i = 0; i < nShade; i++) shader_build(i);
	shadeDirty = true;
}

// ---------------------------------------------------------------------------
// vblank uploads (character frames + shaded palettes)
// ---------------------------------------------------------------------------
typedef struct { u32 off; const u8 *src; int w4, h, stride; } Upload;
static Upload upq[8];
static int nUp;

void gfx_queue_upload(u32 texOff, const u8 *src, int w4, int h, int stride) {
	if (nUp >= 8) return;
	upq[nUp].off = texOff;
	upq[nUp].src = src;
	upq[nUp].w4 = w4;
	upq[nUp].h = h;
	upq[nUp].stride = stride;
	nUp++;
}

void gfx_vblank(void) {
	if (nUp) {
		VRAM_A_CR = VRAM_ENABLE | VRAM_A_LCD;
		for (int i = 0; i < nUp; i++) {
			Upload *u = &upq[i];
			u32 *dst = tex_lcd(u->off);
			const u32 *src = (const u32 *)u->src;
			int wq = u->w4 >> 2, sq = u->stride >> 2;
			for (int y = 0; y < u->h; y++) {
				for (int x = 0; x < wq; x++) dst[x] = src[x];
				dst += sq;
				src += wq;
			}
		}
		VRAM_A_CR = VRAM_ENABLE | VRAM_A_TEXTURE_SLOT0;
		nUp = 0;
	}
	if (shadeDirty) {
		VRAM_F_CR = VRAM_ENABLE | VRAM_F_LCD;
		for (int i = 0; i < nShade; i++) {
			u16 *d = (u16 *)(0x06890000 + shadePals[i].addr);
			for (int k = 0; k < shadePals[i].count; k++) d[k] = shadeBuf[i][k];
		}
		VRAM_F_CR = VRAM_ENABLE | VRAM_F_TEX_PALETTE_SLOT0;
		shadeDirty = false;
	}
}

u32 gfx_chr_slot(u32 size) { return chr_alloc(size); }

// ---------------------------------------------------------------------------
// fonts
// ---------------------------------------------------------------------------
static Font fonts[FONT_COUNT];

void font_load(int id, bool top) {
	Font *f = &fonts[id];
	const FontDef *d = font_defs[id];
	if (!f->alpha) {
		u32 size;
		f->def = d;
		f->alpha = file_load(d->file, &size);
	}
	if (top && !f->onTop && f->alpha) {
		int w = 8 << d->wlog, h = 8 << d->hlog;
		u8 *tmp = malloc(w * h);
		for (int i = 0; i < w * h; i++) {
			u8 a = f->alpha[i] >> 3;
			tmp[i] = a ? ((a << 3) | 1) : 0;
		}
		f->texOff = tex_alloc(w * h);
		tex_write(f->texOff, tmp, w * h);
		free(tmp);
		static const u16 pal[8] = {0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF};
		u32 pa = pal_alloc(16);
		pal_write(pa, pal, 8);
		f->palParam = pa >> 4;
		f->texParam = (f->texOff >> 3) | (d->wlog << 20) | (d->hlog << 23) | (GL_RGB8_A5 << 26);
		f->onTop = true;
	}
}

void font_unload_all(void) {
	for (int i = 0; i < FONT_COUNT; i++) fonts[i].onTop = false;
}

Font *font_get(int id) { return &fonts[id]; }

int font_width(int id, const char *s) {
	const FontDef *d = font_defs[id];
	int w = 0;
	for (; *s; s++) {
		int c = (u8)*s;
		if (c < 32 || c > 126) c = '?';
		w += d->glyphs[c - 32].adv;
	}
	return w;
}

void font_draw(int id, float x, float y, const char *s, u16 color, int alpha) {
	Font *f = &fonts[id];
	if (!f->onTop) return;
	const FontDef *d = f->def;
	for (; *s; s++) {
		int c = (u8)*s;
		if (c < 32 || c > 126) c = '?';
		const Glyph *g = &d->glyphs[c - 32];
		if (g->w) {
			int x0 = fx16(x + g->ox), y0 = fx16(y + g->oy);
			gfx_quad_raw(x0, y0, x0 + g->w * 16, y0 + g->h * 16, g->x << 4, g->y << 4, (g->x + g->w) << 4,
				(g->y + g->h) << 4, f->texParam, f->palParam, color, alpha);
		}
		x += g->adv;
	}
}

void font_draw_centered(int id, float cx, float y, const char *s, u16 color, int alpha) {
	font_draw(id, cx - font_width(id, s) / 2.0f, y, s, color, alpha);
}
