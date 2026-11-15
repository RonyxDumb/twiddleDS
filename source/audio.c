// Audio: maxmod stream fed from NitroFS (inst + voices mixed in software),
// sound effects from a maxmod soundbank.
#include "engine.h"
#include <maxmod9.h>
#include "soundbank_bin.h"

#define RING 16384              // samples per track ring buffer
#define RING_MASK (RING - 1)
#define BUFLEN 1024             // maxmod stream buffer (samples)
#define TICK_HZ ((float)TICK_FREQ)

typedef struct {
	FILE *f;
	s16 *ring;
	volatile u32 w, r;
	bool loop, eof, open;
	u32 fileStart;
} Track;

enum { M_NONE, M_MUSIC, M_SONG, M_PAUSE };

static Track tr[3];             // 0: inst / music, 1: voices, 2: pause music
static volatile int mode = M_NONE;
static volatile int vol = 256;           // master (0..256)
static volatile int vvol = 256;          // voices
static volatile int pvol = 0;            // pause music
static volatile u32 songSamples;         // samples consumed from the song
static volatile u32 cbTicks;
static bool songPaused;
static float masterVol = 1.0f;

static inline u32 ticks_now(void) { return (u32)tickGetCount(); }

u32 time_ticks(void) { return ticks_now(); }

static inline s16 ring_get(Track *t) {
	s16 v = t->ring[t->r & RING_MASK];
	t->r++;
	return v;
}

static mm_word stream_cb(mm_word length, mm_addr dest, mm_stream_formats format) {
	s16 *out = dest;
	int m = mode;
	if (m == M_SONG) {
		Track *a = &tr[0], *b = &tr[1];
		u32 avail = a->w - a->r;
		u32 availb = b->w - b->r;
		if (availb < avail) avail = availb;
		u32 n = length < avail ? length : avail;
		int v = vol, vv = (vvol * vol) >> 8;
		for (u32 i = 0; i < n; i++) {
			int s = (ring_get(a) * v + ring_get(b) * vv) >> 8;
			if (s > 32767) s = 32767;
			if (s < -32768) s = -32768;
			out[i] = s;
		}
		for (u32 i = n; i < length; i++) out[i] = 0;
		songSamples += n;
		cbTicks = ticks_now();
	} else if (m == M_MUSIC || m == M_PAUSE) {
		Track *a = &tr[m == M_MUSIC ? 0 : 2];
		int v = m == M_MUSIC ? vol : pvol;
		u32 avail = a->w - a->r;
		u32 n = length < avail ? length : avail;
		for (u32 i = 0; i < n; i++) out[i] = (ring_get(a) * v) >> 8;
		for (u32 i = n; i < length; i++) out[i] = 0;
	} else {
		for (u32 i = 0; i < length; i++) out[i] = 0;
	}
	return length;
}

static void track_close(Track *t) {
	if (t->f) fclose(t->f);
	t->f = NULL;
	t->open = false;
	t->w = t->r = 0;
	t->eof = false;
}

static bool track_open(Track *t, const char *path, bool loop) {
	track_close(t);
	t->f = fopen(path, "rb");
	if (!t->f) return false;
	setvbuf(t->f, NULL, _IONBF, 0);
	t->loop = loop;
	t->eof = false;
	t->w = t->r = 0;
	t->open = true;
	return true;
}

static void track_fill(Track *t, int maxChunk) {
	if (!t->open || t->eof) return;
	u32 space = RING - (t->w - t->r);
	while (space >= 1024 && maxChunk > 0) {
		u32 pos = t->w & RING_MASK;
		u32 n = space;
		if (n > RING - pos) n = RING - pos;
		if (n > 4096) n = 4096;
		size_t got = fread(t->ring + pos, 2, n, t->f);
		if (got < n) {
			if (t->loop) {
				fseek(t->f, 0, SEEK_SET);
			} else {
				t->eof = true;
			}
		}
		t->w += got;
		space -= got;
		maxChunk--;
		if (t->eof) break;
	}
}

void audio_init(void) {
	mmInitDefaultMem((mm_addr)soundbank_bin);
	for (int i = 0; i < MSL_NSAMPS; i++) mmLoadEffect(i);
	for (int i = 0; i < 3; i++) tr[i].ring = malloc(RING * 2);


	static mm_stream st;
	st.sampling_rate = AUDIO_RATE;
	st.buffer_length = BUFLEN;
	st.callback = stream_cb;
	st.format = MM_STREAM_16BIT_MONO;
	st.timer = MM_TIMER0;
	st.manual = false;
	mmStreamOpen(&st);
}

void audio_update(void) {
	if (mode == M_SONG || mode == M_PAUSE || songPaused) {
		track_fill(&tr[0], 2);
		track_fill(&tr[1], 2);
	} else {
		track_fill(&tr[0], 2);
	}
	track_fill(&tr[2], 2);
}

void audio_set_volume(float v) {
	masterVol = clampf(v, 0, 1);
	vol = (int)(masterVol * 256);
}

float audio_get_volume(void) { return masterVol; }

void audio_set_voices(float v) { vvol = (int)(clampf(v, 0, 1) * 256); }

void audio_stop(void) {
	mode = M_NONE;
	songPaused = false;
	for (int i = 0; i < 3; i++) track_close(&tr[i]);
}

void audio_play_music(const char *path, bool loop, float volume) {
	audio_stop();
	audio_set_volume(volume);
	if (!track_open(&tr[0], path, loop)) return;
	track_fill(&tr[0], 8);
	mode = M_MUSIC;
}

void audio_play_song(void) {
	audio_stop();
	if (!track_open(&tr[0], "nitro:/snd/inst.raw", false)) return;
	if (!track_open(&tr[1], "nitro:/snd/voices.raw", false)) return;
	track_fill(&tr[0], 8);
	track_fill(&tr[1], 8);
	songSamples = 0;
	vvol = 256;
	cbTicks = ticks_now();
	songPaused = false;
	mode = M_SONG;
}

void audio_pause_song(bool paused) {
	if (paused) {
		if (mode == M_SONG) {
			mode = M_NONE;
			songPaused = true;
		}
	} else if (songPaused) {
		songPaused = false;
		cbTicks = ticks_now();
		mode = M_SONG;
	}
}

void audio_play_pause_music(bool on) {
	if (on) {
		if (track_open(&tr[2], "nitro:/snd/pause.raw", true)) {
			// start at a random spot of the first half like Psych
			fseek(tr[2].f, (rand() % (40 * AUDIO_RATE)) * 2, SEEK_SET);
			track_fill(&tr[2], 8);
			pvol = 0;
			mode = M_PAUSE;
		}
	} else {
		if (mode == M_PAUSE) mode = M_NONE;
		track_close(&tr[2]);
	}
}

void audio_pause_volume(float v) { pvol = (int)(clampf(v, 0, 1) * 256); }

bool audio_song_finished(void) {
	return tr[0].eof && tr[0].r >= tr[0].w && !songPaused;
}

float audio_song_time(void) {
	u32 s = songSamples;
	float base = ((float)s - BUFLEN) * 1000.0f / AUDIO_RATE;
	if (mode != M_SONG) return base;
	u32 dt = ticks_now() - cbTicks;
	float extra = dt * (1000.0f / TICK_HZ);
	float chunk = BUFLEN * 500.0f / AUDIO_RATE;   // callbacks happen every half buffer
	if (extra > chunk * 2) extra = chunk * 2;
	return base + extra;
}

void audio_sfx(int id, float v) {
	mm_sound_effect e;
	e.id = id;
	e.rate = 1024;
	e.handle = 0;
	e.volume = (mm_byte)(clampf(v, 0, 1) * 255);
	e.panning = 128;
	mmEffectEx(&e);
}
