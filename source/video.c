// Video playback on the main engine BG2 (8bpp bitmap in VRAM bank E).
// File format (.vid):  "VID1" u16 w, u16 h, u16 fps, u16 n, u32 offs[n+1]
// each frame = 256 RGB15 palette entries + LZ77 (type 0x10) pixels.
//
// Decoding is spread over several main loop iterations: every frame we read
// a bounded chunk of the next video frame and decompress a bounded amount of
// pixels, so a video frame never causes a big spike.
#include "engine.h"

#define READ_BUDGET   8192      // bytes read from the file per update
#define LZ_BUDGET     16384     // bytes decompressed per step

static FILE *vf;
static u32 *offs;
static int vw, vh, vfps, vn;
static bool loopv, overlay, active, pending;
static u8 *fbuf[2];        // decoded pixels: [front] shown, other = decoding
static int front;
static u8 *cbuf;           // compressed frame
static u32 cbufSize;
static u16 palFront[256];
static int bgid = -1;
static int blendEva = 16;
static bool ovVisible = true;
static int winX0, winX1;

// affine parameters (latched in vblank) + heat haze: an HBlank DMA rewrites
// BG2X before every scanline so the fire layer ripples like hot air
#define HDMA_CH 1
static s32 affPA = 256, affX, affY;
static float waveAmp, wavePhase;
static s32 waveTab[193];
static bool hdmaOn;

// decoder state
enum { D_IDLE, D_READ, D_LZ, D_READY };
static int dstate;
static int dframe;         // frame being decoded
static int shown;          // frame currently shown (-1 = none yet)
static u32 rpos, rsize;    // read progress
static u32 lzSrc, lzDst, lzTotal;
static u8 lzFlags;
static int lzBits;

// resumable LZ77 decoder (runs from ITCM in ARM mode)
ITCM_CODE __attribute__((target("arm"), noinline))
static void lz_step(const u8 *src, u8 *dst, u32 budget) {
	u32 s = lzSrc, d = lzDst, end = lzTotal;
	u32 stop = d + budget;
	if (stop > end) stop = end;
	u32 flags = lzFlags;
	int bits = lzBits;
	while (d < stop) {
		if (bits == 0) {
			flags = src[s++];
			bits = 8;
		}
		if (flags & 0x80) {
			u32 b0 = src[s], b1 = src[s + 1];
			s += 2;
			u32 len = (b0 >> 4) + 3;
			u32 disp = (((b0 & 15) << 8) | b1) + 1;
			if (d + len > end) len = end - d;
			const u8 *p = dst + d - disp;
			u8 *q = dst + d;
			for (u32 i = 0; i < len; i++) q[i] = p[i];
			d += len;
		} else {
			dst[d++] = src[s++];
		}
		flags = (flags << 1) & 0xFF;
		bits--;
	}
	lzSrc = s;
	lzDst = d;
	lzFlags = flags;
	lzBits = bits;
}

static void hdma_stop(void) {
	if (!hdmaOn) return;
	DMA_CR(HDMA_CH) = 0;
	hdmaOn = false;
}

static void video_regs(void) {
	if (!active) {
		hdma_stop();
		bgHide(bgid);
		REG_DISPCNT &= ~DISPLAY_WIN0_ON;
		BG_PALETTE[0] = 0;
		REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG0 | BLEND_DST_BACKDROP;
		REG_BLDALPHA = 16 | (0 << 8);
		return;
	}
	if (overlay) {
		bgSetPriority(bgid, 0);
		if (!ovVisible || shown < 0) bgHide(bgid);
		else bgShow(bgid);
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
		if (shown < 0) bgHide(bgid);
		else bgShow(bgid);
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

static void start_decode(int f) {
	dframe = f;
	rpos = 0;
	rsize = offs[f + 1] - offs[f];
	fseek(vf, offs[f], SEEK_SET);
	dstate = D_READ;
}

bool video_open(const char *path, bool loop, bool ov) {
	video_hw_init();
	video_close();
	vf = fopen(path, "rb");
	if (!vf) return false;
	setvbuf(vf, NULL, _IONBF, 0);
	u8 hdr[12];
	fread(hdr, 1, 12, vf);
	vw = hdr[4] | (hdr[5] << 8);
	vh = hdr[6] | (hdr[7] << 8);
	vfps = hdr[8] | (hdr[9] << 8);
	vn = hdr[10] | (hdr[11] << 8);
	offs = malloc((vn + 1) * 4);
	fread(offs, 4, vn + 1, vf);
	fbuf[0] = malloc(vw * vh);
	fbuf[1] = malloc(vw * vh);
	cbufSize = 0;
	for (int i = 0; i < vn; i++)
		if (offs[i + 1] - offs[i] > cbufSize) cbufSize = offs[i + 1] - offs[i];
	cbuf = malloc(cbufSize + 4);
	loopv = loop;
	overlay = ov;
	front = 0;
	shown = -1;
	active = true;
	pending = false;
	dmaFillWords(0, (void *)0x06000000, 256 * 256);
	REG_BG2PA = 256; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = 256;
	REG_BG2X = 0; REG_BG2Y = 0;
	start_decode(0);
	video_regs();
	return true;
}

void video_close(void) {
	hdma_stop();
	affPA = 256;
	affX = affY = 0;
	waveAmp = 0;
	if (vf) fclose(vf);
	vf = NULL;
	if (offs) free(offs);
	offs = NULL;
	for (int i = 0; i < 2; i++) {
		if (fbuf[i]) free(fbuf[i]);
		fbuf[i] = NULL;
	}
	if (cbuf) free(cbuf);
	cbuf = NULL;
	active = false;
	pending = false;
	dstate = D_IDLE;
	if (bgid >= 0) video_regs();
}

bool video_active(void) { return active; }
float video_duration(void) { return active ? vn * 1000.0f / vfps : 0; }

static void decode_work(void) {
	if (dstate == D_READ) {
		u32 n = rsize - rpos;
		if (n > READ_BUDGET) n = READ_BUDGET;
		u32 got = fread(cbuf + rpos, 1, n, vf);
		rpos += got;
		if (got < n || rpos >= rsize) {
			const u8 *h = cbuf + 512;
			lzTotal = h[1] | (h[2] << 8) | (h[3] << 16);
			if (lzTotal > (u32)(vw * vh)) lzTotal = vw * vh;
			lzSrc = 512 + 4;
			lzDst = 0;
			lzBits = 0;
			lzFlags = 0;
			dstate = D_LZ;
		}
	} else if (dstate == D_LZ) {
		lz_step(cbuf, fbuf[front ^ 1], LZ_BUDGET);
		if (lzDst >= lzTotal) dstate = D_READY;
	}
}

// signed distance (in frames) from the decoded frame to the target frame
static int frame_diff(int target, int f) {
	int d = target - f;
	if (loopv) {
		d %= vn;
		if (d < 0) d += vn;
		if (d > vn / 2) d -= vn;
	}
	return d;
}

bool video_update(float ms) {
	if (!active) return false;
	int f = (int)(ms * vfps / 1000.0f);
	if (f < 0) f = 0;
	if (f >= vn) {
		if (!loopv) return false;
		f %= vn;
	}
	if (pending) return true;      // previous frame not copied to VRAM yet
	if (dstate == D_IDLE) return true;
	int diff = frame_diff(f, dframe);
	if (dstate != D_READY && diff > 3) {
		start_decode(f);            // way behind: jump
		diff = 0;
	}
	decode_work();
	if (dstate == D_LZ) decode_work();
	if (dstate == D_READY && diff >= 0) {
		front ^= 1;
		memcpy(palFront, cbuf, 512);
		shown = dframe;
		pending = true;
		int next = diff > 1 ? f + 1 : dframe + 1;
		if (next >= vn) next = loopv ? next % vn : -1;
		if (next >= 0) start_decode(next);
		else dstate = D_IDLE;
	}
	return true;
}

// sine in 1/4096, 64 entries per turn
static const s16 sin64[64] = {
	0, 401, 799, 1189, 1567, 1931, 2276, 2598, 2896, 3166, 3406, 3612, 3784, 3920, 4017, 4076,
	4096, 4076, 4017, 3920, 3784, 3612, 3406, 3166, 2896, 2598, 2276, 1931, 1567, 1189, 799, 401,
	0, -401, -799, -1189, -1567, -1931, -2276, -2598, -2896, -3166, -3406, -3612, -3784, -3920, -4017, -4076,
	-4096, -4076, -4017, -3920, -3784, -3612, -3406, -3166, -2896, -2598, -2276, -1931, -1567, -1189, -799, -401 };

static void affine_vblank(void) {
	REG_BG2PA = affPA; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = affPA;
	REG_BG2Y = affY;
	bool wave = waveAmp > 0.05f && overlay && ovVisible && shown >= 0;
	if (!wave) {
		hdma_stop();
		REG_BG2X = affX;
		return;
	}
	// offset in texture units (20.8): screen px * pa
	s32 amp = (s32)(waveAmp * affPA);
	int ph = (int)(wavePhase * 64.0f) & 63;
	for (int y = 0; y < 193; y++) {
		// two octaves: a slow swell and a faster shimmer
		int a = sin64[(y + ph) & 63];
		int b = sin64[((y * 3) + ph * 2) & 63];
		waveTab[y] = affX + ((amp * (a * 3 + b) / 4) >> 12);
	}
	DC_FlushRange(waveTab, sizeof(waveTab));
	DMA_CR(HDMA_CH) = 0;
	REG_BG2X = waveTab[0];
	DMA_SRC(HDMA_CH) = (u32)&waveTab[1];
	DMA_DEST(HDMA_CH) = (u32)&REG_BG2X;
	DMA_CR(HDMA_CH) = DMA_ENABLE | DMA_REPEAT | DMA_START_HBL | DMA_32_BIT | DMA_DST_RESET | 1;
	hdmaOn = true;
}

void video_vblank(void) {
	if (bgid < 0 || !active) return;
	affine_vblank();
	if (!pending) return;
	DC_FlushRange(fbuf[front], vw * vh);
	dmaCopy(palFront, BG_PALETTE, 512);
	u8 *dst = (u8 *)0x06000000;
	if (vw == 256) {
		dmaCopyWords(3, fbuf[front], dst, vw * vh);
	} else {
		for (int y = 0; y < vh; y++) dmaCopyWords(3, fbuf[front] + y * vw, dst + y * 256, vw);
	}
	pending = false;
	video_regs();
}

// affine mapping: texture = pa * screen + (px, py)
void video_set_affine(float pa, float px, float py) {
	affPA = (int)(pa * 256.0f);
	affX = (int)(px * 256.0f);
	affY = (int)(py * 256.0f);
}

void video_set_wave(float amp, float phase) {
	waveAmp = amp;
	wavePhase = phase - floorf(phase);
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
