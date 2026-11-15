// Minimal FlxTween/FlxTimer replacement
#include "engine.h"

#define MAX_TWEENS 48

typedef struct {
	bool used;
	int id;
	float *target;
	float from, to, dur, t, delay;
	int ease;
	bool fromCurrent;
	TweenCb cb;
	void *ctx;
} Tween;

static Tween tw[MAX_TWEENS];
static int nextId = 1;

float ease_apply(int e, float t) {
	switch (e) {
	case EASE_CUBEOUT: { float u = t - 1; return u * u * u + 1; }
	case EASE_CUBEIN: return t * t * t;
	case EASE_CUBEINOUT:
		if (t < 0.5f) return 4 * t * t * t;
		{ float u = t - 1; return 4 * u * u * u + 1; }
	case EASE_CIRCIN: return -(sqrtf(1 - t * t) - 1);
	case EASE_SINEOUT: return sinf(t * (float)M_PI / 2);
	case EASE_SINEIN: return -cosf(t * (float)M_PI / 2) + 1;
	case EASE_SINEINOUT: return -cosf((float)M_PI * t) / 2 + 0.5f;
	case EASE_QUADOUT: return -t * (t - 2);
	case EASE_BACKOUT: { float u = t - 1; return u * u * (2.70158f * u + 1.70158f) + 1; }
	case EASE_BACKIN: return t * t * (2.70158f * t - 1.70158f);
	default: return t;
	}
}

static int add(float *target, float from, float to, float dur, int ease, float delay, TweenCb cb, void *ctx, bool fromCur) {
	for (int i = 0; i < MAX_TWEENS; i++) {
		if (!tw[i].used) {
			Tween *t = &tw[i];
			t->used = true;
			t->id = nextId++;
			t->target = target;
			t->from = from;
			t->to = to;
			t->dur = dur;
			t->t = 0;
			t->delay = delay;
			t->ease = ease;
			t->cb = cb;
			t->ctx = ctx;
			t->fromCurrent = fromCur;
			return t->id;
		}
	}
	return 0;
}

int tween_num(float *target, float from, float to, float dur, int ease, float delay, TweenCb cb, void *ctx) {
	return add(target, from, to, dur, ease, delay, cb, ctx, false);
}

// like FlxTween.tween: the start value is read when the tween starts
int tween_to(float *target, float to, float dur, int ease, float delay, TweenCb cb, void *ctx) {
	return add(target, target ? *target : 0, to, dur, ease, delay, cb, ctx, true);
}

int timer_after(float delay, TweenCb cb, void *ctx) {
	return add(NULL, 0, 0, 0, EASE_LINEAR, delay, cb, ctx, false);
}

void tween_cancel(int id) {
	if (!id) return;
	for (int i = 0; i < MAX_TWEENS; i++)
		if (tw[i].used && tw[i].id == id) tw[i].used = false;
}

void tween_cancel_target(float *target) {
	for (int i = 0; i < MAX_TWEENS; i++)
		if (tw[i].used && tw[i].target == target) tw[i].used = false;
}

void tween_clear(void) {
	for (int i = 0; i < MAX_TWEENS; i++) tw[i].used = false;
}

void tween_update(float dt0) {
	for (int i = 0; i < MAX_TWEENS; i++) {
		Tween *t = &tw[i];
		if (!t->used) continue;
		float dt = dt0;
		if (t->delay > 0) {
			t->delay -= dt;
			if (t->delay > 0) continue;
			if (t->fromCurrent && t->target) t->from = *t->target;
			dt = -t->delay;
			t->delay = 0;
		} else if (t->t == 0 && t->fromCurrent && t->target) {
			t->from = *t->target;
		}
		t->t += dt;
		float k = t->dur > 0 ? t->t / t->dur : 1;
		if (k >= 1) k = 1;
		if (t->target) *t->target = t->from + (t->to - t->from) * ease_apply(t->ease, k);
		if (k >= 1) {
			t->used = false;
			if (t->cb) t->cb(t->ctx);
		}
	}
}
