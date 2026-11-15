// Bottom screen: 16bpp bitmap drawn in software
#include "engine.h"
#pragma GCC target("arm")

static u16 *fb;      // RAM back buffer
static u16 *vfb;     // VRAM
static volatile bool subDirtyFlag;

void sub_init(void) {
	videoSetModeSub(MODE_5_2D);
	int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	vfb = bgGetGfxPtr(bg);
	fb = malloc(256 * 192 * 2);
	sub_clear(0x8000);
}

u16 *sub_fb(void) { return fb; }

void sub_clear(u16 color) {
	u32 c = (color | 0x8000) | ((u32)(color | 0x8000) << 16);
	u32 *p = (u32 *)fb;
	for (int i = 0; i < 256 * 192 / 2; i++) p[i] = c;
	subDirtyFlag = true;
}

void sub_present(void) {
	if (!subDirtyFlag) return;
	DC_FlushRange(fb, 256 * 192 * 2);
	dmaCopyWords(3, fb, vfb, 256 * 192 * 2);
	subDirtyFlag = false;
}

void sub_mark(void) { subDirtyFlag = true; }

void sub_fill(int x, int y, int w, int h, u16 color) {
	subDirtyFlag = true;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > 256) w = 256 - x;
	if (y + h > 192) h = 192 - y;
	color |= 0x8000;
	for (int j = 0; j < h; j++) {
		u16 *p = fb + (y + j) * 256 + x;
		for (int i = 0; i < w; i++) p[i] = color;
	}
}

static inline u16 blend15(u16 d, u16 s, int a) {
	a += a >> 4;                 // 0..31 -> 0..32
	int r = ((s & 31) * a + (d & 31) * (32 - a)) >> 5;
	int g = (((s >> 5) & 31) * a + ((d >> 5) & 31) * (32 - a)) >> 5;
	int b = (((s >> 10) & 31) * a + ((d >> 10) & 31) * (32 - a)) >> 5;
	return r | (g << 5) | (b << 10) | 0x8000;
}

void sub_fill_blend(int x, int y, int w, int h, u16 color, int a) {
	subDirtyFlag = true;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > 256) w = 256 - x;
	if (y + h > 192) h = 192 - y;
	for (int j = 0; j < h; j++) {
		u16 *p = fb + (y + j) * 256 + x;
		for (int i = 0; i < w; i++) p[i] = blend15(p[i], color, a);
	}
}

bool img_load(Img16 *img, const char *path) {
	u32 size;
	u8 *b = file_load(path, &size);
	if (!b) { img->px = NULL; img->w = img->h = 0; return false; }
	img->w = b[0] | (b[1] << 8);
	img->h = b[2] | (b[3] << 8);
	img->px = malloc(img->w * img->h * 2);
	memcpy(img->px, b + 4, img->w * img->h * 2);
	free(b);
	return true;
}

void img_free(Img16 *img) {
	if (img->px) free(img->px);
	img->px = NULL;
}

void sub_blit(const Img16 *img, int x, int y) {
	subDirtyFlag = true;
	if (!img->px) return;
	for (int j = 0; j < img->h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= 192) continue;
		const u16 *s = img->px + j * img->w;
		u16 *d = fb + yy * 256;
		for (int i = 0; i < img->w; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= 256) continue;
			if (s[i] & 0x8000) d[xx] = s[i];
		}
	}
}

void sub_blit_flip(const Img16 *img, int x, int y) {
	subDirtyFlag = true;
	if (!img->px) return;
	for (int j = 0; j < img->h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= 192) continue;
		const u16 *s = img->px + j * img->w;
		u16 *d = fb + yy * 256;
		for (int i = 0; i < img->w; i++) {
			int xx = x + img->w - 1 - i;
			if (xx < 0 || xx >= 256) continue;
			if (s[i] & 0x8000) d[xx] = s[i];
		}
	}
}

// multiplies the image by a tint color, only the first clipw columns
void sub_blit_tint(const Img16 *img, int x, int y, int clipw, u16 tint) {
	subDirtyFlag = true;
	if (!img->px) return;
	int tr = (tint & 31) + 1, tg = ((tint >> 5) & 31) + 1, tb = ((tint >> 10) & 31) + 1;
	if (clipw > img->w) clipw = img->w;
	for (int j = 0; j < img->h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= 192) continue;
		const u16 *s = img->px + j * img->w;
		u16 *d = fb + yy * 256;
		for (int i = 0; i < clipw; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= 256) continue;
			u16 c = s[i];
			if (!(c & 0x8000)) continue;
			int r = ((c & 31) * tr) >> 5, g = (((c >> 5) & 31) * tg) >> 5, b = (((c >> 10) & 31) * tb) >> 5;
			d[xx] = r | (g << 5) | (b << 10) | 0x8000;
		}
	}
}

void sub_text(int fid, int x, int y, const char *s, u16 color) {
	subDirtyFlag = true;
	Font *f = font_get(fid);
	if (!f->alpha) font_load(fid, false);
	if (!f->alpha) return;
	const FontDef *d = font_defs[fid];
	int aw = 8 << d->wlog;
	for (; *s; s++) {
		int c = (u8)*s;
		if (c < 32 || c > 126) c = '?';
		const Glyph *g = &d->glyphs[c - 32];
		for (int j = 0; j < g->h; j++) {
			int yy = y + g->oy + j;
			if (yy < 0 || yy >= 192) continue;
			const u8 *a = f->alpha + (g->y + j) * aw + g->x;
			u16 *p = fb + yy * 256;
			for (int i = 0; i < g->w; i++) {
				int xx = x + g->ox + i;
				if (xx < 0 || xx >= 256 || !a[i]) continue;
				p[xx] = blend15(p[xx], color, a[i] >> 3);
			}
		}
		x += g->adv;
	}
}

void sub_text_centered(int fid, int cx, int y, const char *s, u16 color) {
	sub_text(fid, cx - font_width(fid, s) / 2, y, s, color);
}
