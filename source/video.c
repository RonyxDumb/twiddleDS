// Video playback on the main engine BG2 (8bpp bitmap in VRAM bank E).
// File format (.vid):  "VID1" u16 w, u16 h, u16 fps, u16 n, u32 offs[n+1]
// each frame = 256 RGB15 palette entries + LZ77 (type 0x10) pixels.
#include "engine.h"

static FILE *vf;
static u32 *offs;
static int vw, vh, vfps, vn;
static int cur = -1;
static bool loopv, overlay, active, pending;
static u8 *frame;          // decoded pixels (vw*vh)
static u8 *cbuf;           // compressed buffer
static u32 cbufSize;
static u16 pal[256];
static int bgid = -1;
static int blendEva = 16;
static bool ovVisible = true;
static int winX0, winX1;

static void video_regs(void) {
	if (!active) {
		bgHide(bgid);
		REG_DISPCNT &= ~DISPLAY_WIN0_ON;
		BG_PALETTE[0] = 0;
		REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG0 | BLEND_DST_BACKDROP;
		REG_BLDALPHA = 16 | (0 << 8);
		return;
	}
	if (overlay) {
		bgSetPriority(bgid, 0);
		if (!ovVisible) bgHide(bgid);
		else if (!pending) bgShow(bgid);
		if (winX0 < winX1) {
			REG_DISPCNT |= DISPLAY_WIN0_ON;
			WIN0_X0 = winX0;
			WIN0_X1 = winX1 > 255 ? 255 : winX1;
			WIN0_Y0 = 0;
			WIN0_Y1 = 192;
			*(vu16 *)0x04000048 = (1 << 0) | (1 << 2) | (1 << 5);   // WININ: BG0, BG2, effects
			*(vu16 *)0x0400004A = (1 << 0) | (1 << 5);              // WINOUT: BG0, effects
		} else REG_DISPCNT &= ~DISPLAY_WIN0_ON;
		REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 | BLEND_DST_BG0 | BLEND_DST_BACKDROP;
		REG_BLDALPHA = blendEva | (16 << 8);
	} else {
		REG_DISPCNT &= ~DISPLAY_WIN0_ON;
		bgSetPriority(bgid, 2);
		REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG0 | BLEND_DST_BG2 | BLEND_DST_BACKDROP;
		REG_BLDALPHA = 16 | (0 << 8);
	}
}

static void video_hw_init(void) {
	if (bgid >= 0) return;
	bgid = bgInit(2, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
	bgHide(bgid);
	REG_BG2PA = 256; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = 256;
	REG_BG2X = 0; REG_BG2Y = 0;
}

bool video_open(const char *path, bool loop, bool ov) {
	video_hw_init();
	video_close();
	vf = fopen(path, "rb");
	if (!vf) return false;
	u8 hdr[12];
	fread(hdr, 1, 12, vf);
	vw = hdr[4] | (hdr[5] << 8);
	vh = hdr[6] | (hdr[7] << 8);
	vfps = hdr[8] | (hdr[9] << 8);
	vn = hdr[10] | (hdr[11] << 8);
	offs = malloc((vn + 1) * 4);
	fread(offs, 4, vn + 1, vf);
	frame = malloc(vw * vh);
	cbufSize = 0;
	for (int i = 0; i < vn; i++)
		if (offs[i + 1] - offs[i] > cbufSize) cbufSize = offs[i + 1] - offs[i];
	cbuf = malloc(cbufSize + 4);
	memset(frame, 0, vw * vh);
	loopv = loop;
	overlay = ov;
	cur = -1;
	active = true;
	pending = false;
	// clear the bitmap
	dmaFillWords(0, (void *)0x06000000, 256 * 256);
	REG_BG2PA = 256; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = 256;
	REG_BG2X = 0; REG_BG2Y = 0;
	return true;
}

void video_close(void) {
	if (vf) fclose(vf);
	vf = NULL;
	if (offs) free(offs);
	offs = NULL;
	if (frame) free(frame);
	frame = NULL;
	if (cbuf) free(cbuf);
	cbuf = NULL;
	active = false;
	pending = false;
	if (bgid >= 0) video_regs();
}

bool video_active(void) { return active; }
float video_duration(void) { return active ? vn * 1000.0f / vfps : 0; }

bool video_update(float ms) {
	if (!active) return false;
	int f = (int)(ms * vfps / 1000.0f);
	if (f < 0) f = 0;
	if (f >= vn) {
		if (!loopv) return false;
		f %= vn;
	}
	if (f == cur || pending) return true;
	u32 size = offs[f + 1] - offs[f];
	fseek(vf, offs[f], SEEK_SET);
	if (fread(cbuf, 1, size, vf) != size) return false;
	memcpy(pal, cbuf, 512);
	swiDecompressLZSSWram(cbuf + 512, frame);
	cur = f;
	pending = true;
	return true;
}

void video_vblank(void) {
	if (bgid < 0) return;
	if (!active) return;
	if (pending) {
		dmaCopy(pal, BG_PALETTE, 512);
		u8 *dst = (u8 *)0x06000000;
		if (vw == 256) {
			dmaCopy(frame, dst, vw * vh);
		} else {
			for (int y = 0; y < vh; y++) dmaCopy(frame + y * vw, dst + y * 256, vw);
		}
		pending = false;
		bgShow(bgid);
		video_regs();
	}
}

// affine mapping: texture = pa * screen + (px, py)
void video_set_affine(float pa, float px, float py) {
	int p = (int)(pa * 256.0f);
	REG_BG2PA = p; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = p;
	REG_BG2X = (int)(px * 256.0f);
	REG_BG2Y = (int)(py * 256.0f);
}

void video_set_blend(int eva) {
	blendEva = eva;
	if (active && overlay) REG_BLDALPHA = blendEva | (16 << 8);
}

void video_set_overlay_visible(bool on) {
	if (on == ovVisible) return;
	ovVisible = on;
	if (active && overlay) video_regs();
}

void video_set_window(int x0, int x1) {
	if (x0 < 0) x0 = 0;
	if (x1 > 256) x1 = 256;
	if (x0 == winX0 && x1 == winX1) return;
	winX0 = x0;
	winX1 = x1;
	if (active && overlay) video_regs();
}
