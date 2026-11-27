/*
 * tools.c - the MCP tools of a Hatari VM.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <errno.h>
#include <regex.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mcp.h"
#include "machine.h"
#include "hatari_mcp.h"

/* ---- arguments ---- */

static bool arg_int(call *c, const json *a, const char *k, long def, long lo, long hi, long *out)
{
	json *v = json_get(a, k);
	if (!v || v->type == JSON_NULL) {
		*out = def;
		return true;
	}
	if (v->type != JSON_NUMBER || v->num != floor(v->num)) {
		call_error(c, "%s must be a whole number", k);
		return false;
	}
	if (v->num < lo || v->num > hi) {
		call_error(c, "%s must be from %ld to %ld", k, lo, hi);
		return false;
	}
	*out = (long)v->num;
	return true;
}

static bool arg_num(call *c, const json *a, const char *k, double def, double lo, double hi, double *out)
{
	json *v = json_get(a, k);
	if (!v || v->type == JSON_NULL) {
		*out = def;
		return true;
	}
	if (v->type != JSON_NUMBER || v->num < lo || v->num > hi) {
		call_error(c, "%s must be a number from %g to %g", k, lo, hi);
		return false;
	}
	*out = v->num;
	return true;
}

static bool arg_bool(call *c, const json *a, const char *k, bool def, bool *out)
{
	json *v = json_get(a, k);
	if (!v || v->type == JSON_NULL) {
		*out = def;
		return true;
	}
	if (v->type != JSON_BOOL) {
		call_error(c, "%s must be true or false", k);
		return false;
	}
	*out = v->b;
	return true;
}

static bool arg_str(call *c, const json *a, const char *k, bool required, const char **out)
{
	json *v = json_get(a, k);
	*out = NULL;
	if (!v || v->type == JSON_NULL) {
		if (required)
			call_error(c, "%s is missing", k);
		return !required;
	}
	if (v->type != JSON_STRING) {
		call_error(c, "%s must be a string", k);
		return false;
	}
	*out = v->str;
	return true;
}

// index into vals, def when missing
static bool arg_enum(call *c, const json *a, const char *k, const char *const *vals, int def, int *out)
{
	const char *s;
	if (!arg_str(c, a, k, def < 0, &s))
		return false;
	if (!s) {
		*out = def;
		return true;
	}
	for (int i = 0; vals[i]; i++) {
		if (strcmp(vals[i], s) == 0) {
			*out = i;
			return true;
		}
	}
	sbuf l;
	sb_init(&l);
	for (int i = 0; vals[i]; i++)
		sb_printf(&l, "%s%s", i ? ", " : "", vals[i]);
	call_error(c, "%s must be one of: %s", k, l.s);
	sb_free(&l);
	return false;
}

// the screen's size now (the window can change with the video mode)
static void screen_size(int *w, int *h)
{
	frame_t *f = frame_get(NULL);
	*w = f ? f->w : 640;
	*h = f ? f->h : 480;
	frame_put(f);
}

static bool get_rect(call *c, const json *v, const char *what, rect *r)
{
	long x, y, w, h;
	if (!v || v->type != JSON_OBJECT) {
		call_error(c, "%s must be an object with x, y, w and h", what);
		return false;
	}
	if (!json_get(v, "x") || !json_get(v, "y") || !json_get(v, "w") || !json_get(v, "h")) {
		call_error(c, "%s needs x, y, w and h", what);
		return false;
	}
	if (!arg_int(c, v, "x", 0, 0, SCR_MAX - 1, &x) || !arg_int(c, v, "y", 0, 0, SCR_MAX - 1, &y) ||
	    !arg_int(c, v, "w", 1, 1, SCR_MAX, &w) || !arg_int(c, v, "h", 1, 1, SCR_MAX, &h))
		return false;
	rect rr = { (int)x, (int)y, (int)w, (int)h };
	*r = rr;
	return true;
}

// a rectangle inside a frame; false when nothing of it is on the screen
static bool clip_rect(const frame_t *f, rect *r)
{
	if (r->x >= f->w || r->y >= f->h)
		return false;
	if (r->x + r->w > f->w)
		r->w = f->w - r->x;
	if (r->y + r->h > f->h)
		r->h = f->h - r->y;
	return r->w > 0 && r->h > 0;
}

static bool parse_addr(call *c, const char *s, uint32_t *out)
{
	char *end;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 0);
	while (*end == ' ')
		end++;
	if (!*s || *end || errno || v > 0xffffffffULL) {
		call_error(c, "'%s' is not an address: write hex with 0x, or decimal", s);
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

/* ---- the screen ---- */

static bool in_rects(const rect *r, int n, int x, int y)
{
	for (int i = 0; i < n; i++)
		if (x >= r[i].x && x < r[i].x + r[i].w && y >= r[i].y && y < r[i].y + r[i].h)
			return true;
	return false;
}

/// Pixels that differ between a and b, inside region (or everywhere) and
/// outside the ignore rectangles; bbox gets their bounding box. A change of
/// size counts as all of the screen.
static int frame_diff(const frame_t *a, const frame_t *b, const rect *region,
		const rect *ign, int n_ign, rect *bbox)
{
	if (!a || !b || a->w != b->w || a->h != b->h) {
		const frame_t *f = b ? b : a;
		int w = f ? f->w : 0, h = f ? f->h : 0;
		if (bbox) {
			rect all = { 0, 0, w, h };
			*bbox = all;
		}
		return a == b ? 0 : w * h > 0 ? w * h : 1;
	}
	int n = 0, x0 = a->w, y0 = a->h, x1 = -1, y1 = -1;
	int ya = 0, yb = a->h, xa = 0, xb = a->w;
	if (region) {
		ya = region->y;
		yb = region->y + region->h < a->h ? region->y + region->h : a->h;
		xa = region->x;
		xb = region->x + region->w < a->w ? region->x + region->w : a->w;
	}
	for (int y = ya; y < yb; y++) {
		const uint32_t *pa = a->px + (size_t)y * a->w, *pb = b->px + (size_t)y * b->w;
		if (!region && !n_ign && memcmp(pa, pb, (size_t)a->w * 4) == 0)
			continue;
		for (int x = xa; x < xb; x++) {
			if (((pa[x] ^ pb[x]) & 0xffffff) == 0)
				continue;
			if (n_ign && in_rects(ign, n_ign, x, y))
				continue;
			n++;
			if (x < x0) x0 = x;
			if (y < y0) y0 = y;
			if (x > x1) x1 = x;
			if (y > y1) y1 = y;
		}
	}
	if (bbox) {
		rect r = { 0, 0, 0, 0 };
		if (n) {
			rect rr = { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
			r = rr;
		}
		*bbox = r;
	}
	return n;
}

/* the mouse tools: one at a time, and the buttons they hold down */
static pthread_mutex_t mouse_mu = PTHREAD_MUTEX_INITIALIZER;
static int m_buttons;

static bool pointer_get(int *x, int *y)
{
	pthread_mutex_lock(&mcp_mu);
	bool k = mcp.pointer_known;
	*x = mcp.pointer_x;
	*y = mcp.pointer_y;
	pthread_mutex_unlock(&mcp_mu);
	return k;
}

/// A screenshot of f into the call's result.
static void shot(call *c, const frame_t *f, const rect *region, double scale, bool pointer)
{
	if (!f) {
		call_printf(c, " (no picture yet: the screen has not been drawn)");
		return;
	}
	rect r = { 0, 0, f->w, f->h };
	if (region) {
		r = *region;
		if (!clip_rect(f, &r)) {
			call_printf(c, " (the region is outside the %dx%d screen)", f->w, f->h);
			return;
		}
	}
	int w = (int)lround(r.w * scale), h = (int)lround(r.h * scale);
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	uint32_t *img = malloc((size_t)w * h * 4);
	if (!img)
		return;
	if (scale >= 1) {
		for (int y = 0; y < h; y++) {
			int fy = r.y + (int)(y / scale);
			if (fy >= r.y + r.h) fy = r.y + r.h - 1;
			for (int x = 0; x < w; x++) {
				int fx = r.x + (int)(x / scale);
				if (fx >= r.x + r.w) fx = r.x + r.w - 1;
				img[(size_t)y * w + x] = f->px[(size_t)fy * f->w + fx];
			}
		}
	} else {
		// smaller: average the pixels under each one
		for (int y = 0; y < h; y++) {
			int fy0 = r.y + (int)(y / scale), fy1 = r.y + (int)((y + 1) / scale);
			if (fy1 <= fy0) fy1 = fy0 + 1;
			if (fy1 > r.y + r.h) fy1 = r.y + r.h;
			for (int x = 0; x < w; x++) {
				int fx0 = r.x + (int)(x / scale), fx1 = r.x + (int)((x + 1) / scale);
				if (fx1 <= fx0) fx1 = fx0 + 1;
				if (fx1 > r.x + r.w) fx1 = r.x + r.w;
				unsigned rs = 0, gs = 0, bs = 0, all = 0;
				for (int yy = fy0; yy < fy1; yy++) {
					for (int xx = fx0; xx < fx1; xx++, all++) {
						uint32_t v = f->px[(size_t)yy * f->w + xx];
						rs += (v >> 16) & 0xff;
						gs += (v >> 8) & 0xff;
						bs += v & 0xff;
					}
				}
				if (!all)
					all = 1;
				img[(size_t)y * w + x] = (rs / all) << 16 | (gs / all) << 8 | (bs / all);
			}
		}
	}
	int mx, my;
	if (pointer && pointer_get(&mx, &my)) {
		int px = (int)lround((mx - r.x) * scale), py = (int)lround((my - r.y) * scale);
		for (int d = -7; d <= 7; d++) {
			if (px + d >= 0 && px + d < w && py >= 0 && py < h)
				img[(size_t)py * w + px + d] = 0xff2020;
			if (py + d >= 0 && py + d < h && px >= 0 && px < w)
				img[(size_t)(py + d) * w + px] = 0xff2020;
		}
	}
	sbuf png, b64;
	sb_init(&png);
	sb_init(&b64);
	if (png_encode_rgb(&png, w, h, img, w)) {
		sb_base64(&b64, (const unsigned char *)png.s, png.len);
		free(c->image);
		c->image = sb_take(&b64);
	}
	sb_free(&png);
	sb_free(&b64);
	free(img);
}

/* ---- waiting ---- */

typedef struct {
	bool met, changed;
	rect box;		// the last change
	int pixels;
	double secs;
} wait_result;

static const char *const wait_names[] = { "none", "change", "stable", "change-then-stable", NULL };
enum { W_NONE, W_CHANGE, W_STABLE, W_CHANGE_STABLE };

#define WHIST 10	// pictures a wait remembers

static void put_all(frame_t **f, int n)
{
	for (int i = 0; i < n; i++)
		frame_put(f[i]);
}

static wait_result wait_screen_core(call *c, int until, long stable_ms, long timeout_ms, long min_px,
		const rect *region, const rect *ign, int n_ign, frame_t *before)
{
	wait_result res = { false, false, { 0, 0, 0, 0 }, 0, 0 };
	frame_t *seen[WHIST];
	double t0 = wall_now(), deadline = t0 + timeout_ms / 1000.0;
	bool need_change = until == W_CHANGE || until == W_CHANGE_STABLE;
	bool need_stable = until == W_STABLE || until == W_CHANGE_STABLE;
	uint64_t at;
	frame_t *ref;

	if (before) {
		pthread_mutex_lock(&mcp_mu);
		before->refs++;
		at = mcp.ticks;
		pthread_mutex_unlock(&mcp_mu);
		ref = before;
	} else {
		ref = frame_get(&at);
	}
	pthread_mutex_lock(&mcp_mu);
	uint64_t ref_tick = mcp.ticks;
	pthread_mutex_unlock(&mcp_mu);
	double ref_wall = t0;
	// what the screen showed in the seconds before: a change back to one of
	// these pictures (a blinking cursor) is not news
	uint64_t hist = 4 * (uint64_t)TICK_HZ;
	uint64_t since = at > hist ? at - hist : 0;
	int n_seen = ref ? frame_history(ref->seq, since, seen, WHIST - 1) : 0;
	if (ref) {
		pthread_mutex_lock(&mcp_mu);
		ref->refs++;
		pthread_mutex_unlock(&mcp_mu);
		seen[n_seen++] = ref;
	}

	for (;;) {
		pthread_mutex_lock(&mcp_mu);
		frame_t *cur = mcp.frame;
		if (cur)
			cur->refs++;
		uint64_t ticks = mcp.ticks;
		bool running = mcp_cpu_running_locked();
		pthread_mutex_unlock(&mcp_mu);
		double now = wall_now();
		rect box = {0, 0, 0, 0};
		int d = cur == ref ? 0 : frame_diff(ref, cur, region, ign, n_ign, &box);
		if (d >= min_px) {
			bool repeat = false;
			for (int i = 0; i < n_seen && !repeat; i++)
				repeat = frame_diff(seen[i], cur, region, ign, n_ign, NULL) < min_px;
			frame_put(ref);
			ref = cur;
			pthread_mutex_lock(&mcp_mu);
			cur->refs++;
			pthread_mutex_unlock(&mcp_mu);
			if (!repeat) {
				res.changed = true;
				res.box = box;
				res.pixels = d;
				ref_tick = ticks;
				ref_wall = now;
				if (n_seen == WHIST) {
					frame_put(seen[0]);
					memmove(seen, seen + 1, sizeof(frame_t *) * (WHIST - 1));
					n_seen--;
				}
				pthread_mutex_lock(&mcp_mu);
				cur->refs++;
				pthread_mutex_unlock(&mcp_mu);
				seen[n_seen++] = cur;
				if (until == W_CHANGE) {
					frame_put(cur);
					res.met = true;
					break;
				}
			}
		}
		frame_put(cur);
		if (need_stable && (!need_change || res.changed)) {
			double guest_ms = (ticks - ref_tick) * 1000.0 / TICK_HZ;
			double wall_ms = (now - ref_wall) * 1000.0;
			if (guest_ms >= stable_ms || (!running && wall_ms >= stable_ms)) {
				res.met = true;
				break;
			}
		}
		if (now >= deadline || call_stopped(c))
			break;
		call_progress(c, need_change && !res.changed ? "waiting for the screen to change" :
			"waiting for the screen to settle");
		pthread_mutex_lock(&mcp_mu);
		mcp_wait_until(deadline);
		pthread_mutex_unlock(&mcp_mu);
	}
	res.secs = wall_now() - t0;
	frame_put(ref);
	put_all(seen, n_seen);
	return res;
}

static void describe_wait(call *c, int until, const wait_result *w, long stable_ms)
{
	if (w->met) {
		if (until == W_CHANGE)
			call_printf(c, "The screen changed after %.1f s (%d pixels in %d,%d %dx%d).",
				w->secs, w->pixels, w->box.x, w->box.y, w->box.w, w->box.h);
		else
			call_printf(c, "The screen was stable for %ld ms after %.1f s%s.", stable_ms, w->secs,
				w->changed ? "" : " (it did not change)");
	} else {
		c->unmet = true;
		if (call_cancelled(c))
			call_printf(c, "The wait was cancelled after %.1f s.", w->secs);
		else if ((until == W_CHANGE || until == W_CHANGE_STABLE) && !w->changed)
			call_printf(c, "The screen did not change in %.1f s.", w->secs);
		else
			call_printf(c, "The screen did not settle in %.1f s; it still changes (last in %d,%d %dx%d).",
				w->secs, w->box.x, w->box.y, w->box.w, w->box.h);
	}
}

static bool wait_ticks(call *c, int n)
{
	pthread_mutex_lock(&mcp_mu);
	uint64_t until = mcp.ticks + n;
	double deadline = wall_now() + 30;
	while (mcp.ticks < until && mcp_cpu_running_locked() && !mcp.quitting && !call_cancelled(c) &&
	       wall_now() < deadline)
		mcp_wait_until(deadline);
	bool ok = mcp.ticks >= until;
	pthread_mutex_unlock(&mcp_mu);
	return ok;
}

// wait until the guest has the keys, mouse packets and serial bytes sent so far
static bool wait_delivered(call *c, uint64_t kseq, uint64_t mseq, bool serial)
{
	double deadline = wall_now() + WAIT_MAX_MS / 1000.0;
	for (;;) {
		bool done = keys_done() >= kseq && mcp_mouse_done() >= mseq &&
			(!serial || mcp_serial_queued() == 0);
		if (done)
			return true;
		pthread_mutex_lock(&mcp_mu);
		bool running = mcp_cpu_running_locked();
		pthread_mutex_unlock(&mcp_mu);
		if (!running && !serial) {
			call_printf(c, "The VM is paused: the input waits until it runs. ");
			return false;
		}
		if (call_stopped(c) || wall_now() > deadline)
			return false;
		call_progress(c, "sending the input");
		pthread_mutex_lock(&mcp_mu);
		mcp_wait_until(wall_now() + 0.1);
		pthread_mutex_unlock(&mcp_mu);
	}
}

/* "then": wait and look after an input */
typedef struct {
	int wait;
	long delay_ms, stable_ms, timeout_ms, min_px;
	bool shot, has_region;
	double scale;
	rect region;
} then_t;

static bool parse_shot(call *c, const json *v, bool def, bool *on, rect *region, bool *has_region,
		double *scale)
{
	*on = def;
	*has_region = false;
	*scale = 1;
	if (!v || v->type == JSON_NULL)
		return true;
	if (v->type == JSON_BOOL) {
		*on = v->b;
		return true;
	}
	if (v->type != JSON_OBJECT) {
		call_error(c, "screenshot must be true, false or an object");
		return false;
	}
	*on = true;
	json *r = json_get(v, "region");
	if (r && r->type != JSON_NULL) {
		if (!get_rect(c, r, "region", region))
			return false;
		*has_region = true;
	}
	return arg_num(c, v, "scale", 1, 0.25, 4, scale);
}

static bool parse_then(call *c, const json *args, then_t *t)
{
	json *v = json_get(args, "then");
	memset(t, 0, sizeof(*t));
	t->scale = 1;
	if (!v || v->type == JSON_NULL)
		return true;
	if (v->type != JSON_OBJECT) {
		call_error(c, "then must be an object");
		return false;
	}
	return arg_enum(c, v, "wait", wait_names, W_NONE, &t->wait) &&
		arg_int(c, v, "delay-ms", 0, 0, 600000, &t->delay_ms) &&
		arg_int(c, v, "stable-ms", 1000, 0, WAIT_MAX_MS, &t->stable_ms) &&
		arg_int(c, v, "timeout-ms", 10000, 0, WAIT_MAX_MS, &t->timeout_ms) &&
		arg_int(c, v, "min-pixels", 16, 1, SCR_MAX * SCR_MAX, &t->min_px) &&
		parse_shot(c, json_get(v, "screenshot"), false, &t->shot, &t->region, &t->has_region,
			&t->scale);
}

static void sleep_ms(call *c, long ms)
{
	double until = wall_now() + ms / 1000.0;
	pthread_mutex_lock(&mcp_mu);
	while (wall_now() < until && !call_cancelled(c) && !mcp.quitting)
		mcp_wait_until(until);
	pthread_mutex_unlock(&mcp_mu);
}

static void finish_then(call *c, const then_t *t, frame_t *before)
{
	if (t->delay_ms)
		sleep_ms(c, t->delay_ms);
	if (t->wait != W_NONE) {
		wait_result w = wait_screen_core(c, t->wait, t->stable_ms, t->timeout_ms, t->min_px,
			NULL, NULL, 0, before);
		call_printf(c, " ");
		describe_wait(c, t->wait, &w, t->stable_ms);
	}
	if (t->shot) {
		frame_t *f = frame_get(NULL);
		shot(c, f, t->has_region ? &t->region : NULL, t->scale, false);
		frame_put(f);
	}
}

/* ---- vm-info, vm-power ---- */

typedef struct {
	sbuf *out;
	mach_regs regs;
	char floppy[2][600];
} info_req;

static void mc_info(void *p)
{
	info_req *r = p;
	if (r->out) {
		mach_info(r->out);
		mach_serial_info(r->out);
	}
	mach_get_regs(&r->regs);
	for (int d = 0; d < 2; d++) {
		const char *f = mach_floppy_path(d);
		snprintf(r->floppy[d], sizeof(r->floppy[d]), "%s", f ? f : "");
	}
}

static bool tool_vm_info(call *c, const json *a)
{
	(void)a;
	sbuf m;
	sb_init(&m);
	info_req r;
	memset(&r, 0, sizeof(r));
	r.out = &m;
	mcp_main_call(mc_info, &r);
	pthread_mutex_lock(&mcp_mu);
	bool paused_break = mcp.paused_break, paused_user = mcp.paused_user;
	uint64_t ticks = mcp.ticks;
	bool known = mcp.pointer_known, abs = mcp.mouse_abs;
	int px = mcp.pointer_x, py = mcp.pointer_y;
	pthread_mutex_unlock(&mcp_mu);
	const char *run = paused_break ? "stopped by the debugger" : paused_user ? "paused" : "running";
	double secs = ticks / (double)TICK_HZ;
	call_printf(c, "VM %s (Hatari), pid %d, folder %s\n", mcp_vm_name, (int)getpid(), mcp_vm_dir);
	call_printf(c, "State: %s, VM time %d:%02d:%02d, PC %08x (%s mode)\n", run,
		(int)secs / 3600, (int)secs / 60 % 60, (int)secs % 60, r.regs.pc,
		(r.regs.sr & 0x2000) ? "supervisor" : "user");
	if (m.len)
		call_printf(c, "%s", m.s);
	sb_free(&m);
	call_printf(c, "Floppy A: %s\n", r.floppy[0][0] ? r.floppy[0] : "none (empty drive)");
	call_printf(c, "Floppy B: %s\n", r.floppy[1][0] ? r.floppy[1] : "none (empty drive)");
	frame_t *f = frame_get(NULL);
	if (f)
		call_printf(c, "Screen: %dx%d guest pixels (no borders, no statusbar)\n", f->w, f->h);
	else
		call_printf(c, "Screen: not drawn yet\n");
	frame_put(f);
	if (known)
		call_printf(c, "Mouse: the pointer is at %d,%d%s\n", px, py,
			abs ? " (the mouse tools put it exactly where they want)" :
			" (the guest's Line-A variables say so; the tools move it with IKBD packets)");
	else
		call_printf(c, "Mouse: nothing says where the pointer is yet (no Line-A variables found;"
			" IKBD packets only)\n");
	int sp = mcp_serial_port();
	call_printf(c, "Serial: the guest last wrote to %s\n",
		sp == 1 ? "Modem 1" : sp == 2 ? "Modem 2" : "neither yet");
	call_printf(c, "Keys waiting: %d.", keys_pending());
	return true;
}

static void mc_pause(void *p)
{
	(void)p;
	mach_pause(true);
}

static void mc_resume(void *p)
{
	(void)p;
	pthread_mutex_lock(&mcp_mu);
	bool in_break = mcp.paused_break;
	pthread_mutex_unlock(&mcp_mu);
	// stopped between instructions by a pause (not in the debugger's
	// hook): the next one runs first, even at a breakpoint
	if (!in_break && (mcp_dbg.n_bp || mcp_dbg.steps)) {
		mach_regs r;
		mach_get_regs(&r);
		mcp_dbg.skip_pc = r.pc;
		mcp_dbg.skip_armed = true;
	}
	mach_pause(false);
}

static bool tool_vm_power(call *c, const json *a)
{
	static const char *const acts[] = { "pause", "resume", "reset", "quit", NULL };
	int act;
	bool cold;
	if (!arg_enum(c, a, "action", acts, -1, &act) || !arg_bool(c, a, "cold", false, &cold))
		return false;
	switch (act) {
	case 0:
		mcp_main_call(mc_pause, NULL);
		pthread_mutex_lock(&mcp_mu);
		mcp.paused_user = true;
		pthread_cond_broadcast(&mcp_cv);
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "Paused. Screenshots, registers and memory still work; vm-power resume goes on.");
		break;
	case 1:
		mcp_main_call(mc_resume, NULL);
		pthread_mutex_lock(&mcp_mu);
		mcp.paused_user = false;
		mcp.paused_break = false;
		pthread_cond_broadcast(&mcp_cv);
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "Running.");
		break;
	case 2:
		pthread_mutex_lock(&mcp_mu);
		mcp.reset = cold ? 2 : 1;
		pthread_cond_broadcast(&mcp_cv);
		pthread_mutex_unlock(&mcp_mu);
		pthread_mutex_lock(&mouse_mu);
		m_buttons = 0;
		pthread_mutex_unlock(&mouse_mu);
		pthread_mutex_lock(&mcp_mu);
		bool running = mcp_cpu_running_locked();
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "%s reset: the machine starts again from its TOS (the disks stay).%s",
			cold ? "Cold" : "Warm", running ? "" : " It is paused: vm-power resume lets it run.");
		break;
	case 3:
		pthread_mutex_lock(&mcp_mu);
		mcp.quit = true;
		pthread_cond_broadcast(&mcp_cv);
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "Hatari is quitting.");
		break;
	}
	return true;
}

/* ---- screenshot, wait-screen ---- */

static bool tool_screenshot(call *c, const json *a)
{
	rect r;
	bool has_r = false, pointer;
	double scale;
	json *rv = json_get(a, "region");
	if (rv && rv->type != JSON_NULL) {
		if (!get_rect(c, rv, "region", &r))
			return false;
		has_r = true;
	}
	if (!arg_num(c, a, "scale", 1, 0.25, 4, &scale) || !arg_bool(c, a, "pointer", false, &pointer))
		return false;
	frame_t *f = frame_get(NULL);
	pthread_mutex_lock(&mcp_mu);
	bool running = mcp_cpu_running_locked();
	pthread_mutex_unlock(&mcp_mu);
	if (f)
		call_printf(c, "Screen %dx%d%s%s.", f->w, f->h, has_r ? ", a region" : "",
			running ? "" : " (the VM is paused)");
	else
		call_printf(c, "No picture yet.");
	shot(c, f, has_r ? &r : NULL, scale, pointer);
	frame_put(f);
	return true;
}

static bool tool_wait_screen(call *c, const json *a)
{
	static const char *const untils[] = { "change", "stable", "change-then-stable", NULL };
	int until;
	long stable_ms, timeout_ms, min_px;
	rect region, ign[16];
	bool has_region = false, sh, has_sr;
	rect sr;
	double scale;
	int n_ign = 0;
	if (!arg_enum(c, a, "until", untils, -1, &until) ||
	    !arg_int(c, a, "stable-ms", 2000, 0, WAIT_MAX_MS, &stable_ms) ||
	    !arg_int(c, a, "timeout-ms", 60000, 0, WAIT_MAX_MS, &timeout_ms) ||
	    !arg_int(c, a, "min-pixels", 16, 1, SCR_MAX * SCR_MAX, &min_px) ||
	    !parse_shot(c, json_get(a, "screenshot"), true, &sh, &sr, &has_sr, &scale))
		return false;
	until++;	// to W_*
	json *rv = json_get(a, "region");
	if (rv && rv->type != JSON_NULL) {
		if (!get_rect(c, rv, "region", &region))
			return false;
		has_region = true;
	}
	json *iv = json_get(a, "ignore");
	if (iv && iv->type != JSON_NULL) {
		if (iv->type != JSON_ARRAY || iv->n > 16) {
			call_error(c, "ignore must be a list of up to 16 rectangles");
			return false;
		}
		for (int i = 0; i < iv->n; i++)
			if (!get_rect(c, iv->items[i], "ignore", &ign[n_ign++]))
				return false;
	}
	wait_result w = wait_screen_core(c, until, stable_ms, timeout_ms, min_px,
		has_region ? &region : NULL, ign, n_ign, NULL);
	describe_wait(c, until, &w, stable_ms);
	if (sh) {
		frame_t *f = frame_get(NULL);
		shot(c, f, has_sr ? &sr : NULL, scale, false);
		frame_put(f);
	}
	return true;
}

/* ---- keyboard ---- */

static int ms_ticks(long ms)
{
	int t = (int)lround(ms * TICK_HZ / 1000.0);
	return t < 1 ? 1 : t > 255 ? 255 : t;
}

static bool tool_key(call *c, const json *a)
{
	json *keys = json_get(a, "keys");
	long hold_ms, delay_ms;
	then_t t;
	if (!keys || keys->type != JSON_ARRAY || keys->n == 0 || keys->n > 500) {
		call_error(c, "keys must be a list of 1 to 500 keys or combinations");
		return false;
	}
	if (!arg_int(c, a, "hold-ms", 40, 1, 4000, &hold_ms) ||
	    !arg_int(c, a, "delay-ms", 40, 0, 4000, &delay_ms) || !parse_then(c, a, &t))
		return false;
	key_chord *ch = calloc(keys->n, sizeof(*ch));
	int *rep = calloc(keys->n, sizeof(*rep));
	for (int i = 0; i < keys->n; i++) {
		json *k = keys->items[i];
		if (k->type != JSON_STRING || !*k->str) {
			call_error(c, "keys must be strings");
			goto fail;
		}
		char spec[64];
		snprintf(spec, sizeof(spec), "%s", k->str);
		rep[i] = 1;
		char *star = strrchr(spec, '*');
		if (star && star != spec && star[1] && star[-1] != '-') {
			char *end;
			long n = strtol(star + 1, &end, 10);
			if (*end || n < 1 || n > 100) {
				call_error(c, "'%s': a repeat count is *1 to *100", k->str);
				goto fail;
			}
			*star = 0;
			rep[i] = n;
		}
		if (!keys_parse_combo(&ch[i], spec)) {
			call_error(c, "unknown key '%s'. Keys: %s; join a combination with - or +", k->str,
				keys_name_list());
			goto fail;
		}
		ch[i].hold = ms_ticks(hold_ms);
		ch[i].gap = delay_ms ? ms_ticks(delay_ms) : 1;
	}
	frame_t *before = frame_get(NULL);
	uint64_t seq = 0;
	for (int i = 0; i < keys->n; i++) {
		for (int r = 0; r < rep[i]; r++) {
			uint64_t s = keys_push(&ch[i]);
			if (!s) {
				call_error(c, "the key queue is full");
				frame_put(before);
				goto fail;
			}
			seq = s;
		}
	}
	bool ok = wait_delivered(c, seq, 0, false);
	call_printf(c, ok ? "Pressed" : "Queued");
	for (int i = 0; i < keys->n && i < 20; i++)
		call_printf(c, " %s", keys->items[i]->str);
	call_printf(c, keys->n > 20 ? " ...." : ".");
	if (ok)
		finish_then(c, &t, before);
	frame_put(before);
	free(ch);
	free(rep);
	return true;
fail:
	free(ch);
	free(rep);
	return false;
}

static bool tool_type_text(call *c, const json *a)
{
	const char *text, *layout;
	long delay_ms;
	then_t t;
	if (!arg_str(c, a, "text", true, &text) || !arg_str(c, a, "layout", false, &layout) ||
	    !arg_int(c, a, "char-delay-ms", 20, 0, 4000, &delay_ms) || !parse_then(c, a, &t))
		return false;
	if (layout && strcasecmp(layout, "us") && strcasecmp(layout, "en-us") && strcasecmp(layout, "en")) {
		call_error(c, "layout: only us is known; for another layout use key with scancode names,"
			" or set the guest to the US layout");
		return false;
	}
	size_t n = strlen(text);
	if (n == 0) {
		call_printf(c, "Nothing to type.");
		return true;
	}
	if (n > 100000) {
		call_error(c, "the text is too long (at most 100000 characters)");
		return false;
	}
	key_chord *ch = calloc(n, sizeof(*ch));
	for (size_t i = 0; i < n; i++) {
		unsigned char u = text[i];
		if (u >= 0x80 || !keys_from_char(&ch[i], u)) {
			size_t chno = 1;	// count characters, not UTF-8 bytes
			for (size_t k = 0; k < i; k++)
				chno += ((unsigned char)text[k] & 0xc0) != 0x80;
			if (u >= 0x80)
				call_error(c, "character %zu is not ASCII: the US layout cannot type it; nothing was typed", chno);
			else
				call_error(c, "character %zu (code %d) cannot be typed; nothing was typed", chno, u);
			free(ch);
			return false;
		}
		ch[i].hold = 1;
		ch[i].gap = delay_ms ? ms_ticks(delay_ms) : 1;
	}
	frame_t *before = frame_get(NULL);
	uint64_t seq = 0;
	for (size_t i = 0; i < n; i++) {
		uint64_t s = keys_push(&ch[i]);
		if (!s) {
			call_error(c, "the key queue is full");
			free(ch);
			frame_put(before);
			return false;
		}
		seq = s;
	}
	free(ch);
	bool ok = wait_delivered(c, seq, 0, false);
	call_printf(c, "%s %zu character%s.", ok ? "Typed" : "Queued", n, n == 1 ? "" : "s");
	if (ok)
		finish_then(c, &t, before);
	frame_put(before);
	return true;
}

/* ---- mouse ---- */

static const char *const button_names[] = { "left", "right", NULL };
static const int button_bits[] = { MACH_BUTTON_LEFT, MACH_BUTTON_RIGHT };

// relative IKBD packets for counts, in steps of at most step
static uint64_t send_counts(int cx, int cy, int step)
{
	uint64_t s = mcp_mouse_done();
	while (cx || cy) {
		int m = abs(cx) > abs(cy) ? abs(cx) : abs(cy);
		int k = m > step ? step : m;
		int dx = (int)lround((double)cx * k / m), dy = (int)lround((double)cy * k / m);
		if (!dx && !dy) {
			dx = cx ? (cx > 0 ? 1 : -1) : 0;
			dy = cy ? (cy > 0 ? 1 : -1) : 0;
		}
		uint64_t q = mcp_mouse_push(false, dx, dy, m_buttons, 0);
		if (q)
			s = q;
		cx -= dx;
		cy -= dy;
	}
	return s;
}

// mouse_mu held: put the pointer at x, y
static void move_to(call *c, int x, int y, int step)
{
	pthread_mutex_lock(&mcp_mu);
	bool abs_ok = mcp.mouse_abs;
	pthread_mutex_unlock(&mcp_mu);
	if (abs_ok) {
		wait_delivered(c, 0, mcp_mouse_push(true, x, y, m_buttons, 0), false);
		wait_ticks(c, 2);
		return;
	}
	int px, py;
	if (pointer_get(&px, &py)) {
		// the driver says where it is: move, look, and again (the guest
		// may speed the mouse up)
		for (int i = 0; i < 8; i++) {
			if (!pointer_get(&px, &py) || (px == x && py == y))
				break;
			wait_delivered(c, 0, send_counts(x - px, y - py, step), false);
			wait_ticks(c, 3);
		}
		return;
	}
	// nothing says where it is: push it into the top-left corner, then count
	uint64_t s = 0;
	for (int i = 0; i < 40; i++)
		s = mcp_mouse_push(false, -100, -100, m_buttons, 0);
	wait_delivered(c, 0, s, false);
	wait_ticks(c, 3);
	wait_delivered(c, 0, send_counts(x, y, step), false);
	wait_ticks(c, 3);
}

// "at X,Y" for the results
static const char *pointer_at(char *buf, size_t len)
{
	int x, y;
	if (pointer_get(&x, &y))
		snprintf(buf, len, "%d,%d", x, y);
	else
		snprintf(buf, len, "a place no driver tells");
	return buf;
}

static bool get_xy(call *c, const json *a, const char *kx, const char *ky, bool *has, long *x, long *y)
{
	*has = json_has(a, kx) || json_has(a, ky);
	if (!*has)
		return true;
	if (!json_has(a, kx) || !json_has(a, ky)) {
		call_error(c, "give both %s and %s", kx, ky);
		return false;
	}
	int w, h;
	screen_size(&w, &h);
	return arg_int(c, a, kx, 0, 0, w - 1, x) && arg_int(c, a, ky, 0, 0, h - 1, y);
}

static bool tool_mouse_move(call *c, const json *a)
{
	bool has_xy, has_d = json_has(a, "dx") || json_has(a, "dy");
	long x = 0, y = 0, dx, dy, step;
	then_t t;
	if (!get_xy(c, a, "x", "y", &has_xy, &x, &y) ||
	    !arg_int(c, a, "dx", 0, -SCR_MAX, SCR_MAX, &dx) || !arg_int(c, a, "dy", 0, -SCR_MAX, SCR_MAX, &dy) ||
	    !arg_int(c, a, "step-px", 16, 1, 127, &step) || !parse_then(c, a, &t))
		return false;
	if (has_xy == has_d) {
		call_error(c, "give x and y, or dx and dy");
		return false;
	}
	frame_t *before = frame_get(NULL);
	pthread_mutex_lock(&mouse_mu);
	if (has_xy) {
		move_to(c, x, y, step);
	} else {
		int px, py;
		pthread_mutex_lock(&mcp_mu);
		bool abs_ok = mcp.mouse_abs;
		pthread_mutex_unlock(&mcp_mu);
		if (abs_ok && pointer_get(&px, &py))
			move_to(c, px + dx, py + dy, step);
		else {
			wait_delivered(c, 0, send_counts(dx, dy, step), false);
			wait_ticks(c, 3);
		}
	}
	char at[48];
	call_printf(c, "The pointer is at %s.", pointer_at(at, sizeof(at)));
	pthread_mutex_unlock(&mouse_mu);
	finish_then(c, &t, before);
	frame_put(before);
	return true;
}

// mouse_mu held: the buttons now, through the same path as the moves
static uint64_t push_buttons(int wait)
{
	int px, py;
	pthread_mutex_lock(&mcp_mu);
	bool abs_ok = mcp.mouse_abs && mcp.pointer_known;
	px = mcp.pointer_x;
	py = mcp.pointer_y;
	pthread_mutex_unlock(&mcp_mu);
	return mcp_mouse_push(abs_ok, abs_ok ? px : 0, abs_ok ? py : 0, m_buttons, wait);
}

static bool tool_mouse_click(call *c, const json *a)
{
	bool has_xy;
	long x = 0, y = 0, count, hold_ms, interval_ms;
	int button;
	then_t t;
	if (!get_xy(c, a, "x", "y", &has_xy, &x, &y) ||
	    !arg_enum(c, a, "button", button_names, 0, &button) ||
	    !arg_int(c, a, "count", 1, 1, 3, &count) ||
	    !arg_int(c, a, "hold-ms", 80, 1, 4000, &hold_ms) ||
	    !arg_int(c, a, "interval-ms", 120, 1, 4000, &interval_ms) || !parse_then(c, a, &t))
		return false;
	frame_t *before = frame_get(NULL);
	pthread_mutex_lock(&mouse_mu);
	if (has_xy)
		move_to(c, x, y, 16);
	uint64_t s = 0;
	for (int i = 0; i < count; i++) {
		m_buttons |= button_bits[button];
		push_buttons(ms_ticks(hold_ms));
		m_buttons &= ~button_bits[button];
		s = push_buttons(i + 1 < count ? ms_ticks(interval_ms) : 1);
	}
	wait_delivered(c, 0, s, false);
	char at[48];
	call_printf(c, "Clicked %s%s at %s.", button_names[button], count == 2 ? " twice" :
		count == 3 ? " three times" : "", pointer_at(at, sizeof(at)));
	pthread_mutex_unlock(&mouse_mu);
	finish_then(c, &t, before);
	frame_put(before);
	return true;
}

static bool tool_mouse_button(call *c, const json *a)
{
	static const char *const acts[] = { "down", "up", NULL };
	int act, button;
	then_t t;
	if (!arg_enum(c, a, "action", acts, -1, &act) ||
	    !arg_enum(c, a, "button", button_names, 0, &button) || !parse_then(c, a, &t))
		return false;
	frame_t *before = frame_get(NULL);
	pthread_mutex_lock(&mouse_mu);
	if (act == 0)
		m_buttons |= button_bits[button];
	else
		m_buttons &= ~button_bits[button];
	wait_delivered(c, 0, push_buttons(2), false);
	pthread_mutex_unlock(&mouse_mu);
	call_printf(c, "The %s button is %s.", button_names[button], act ? "up" : "down");
	finish_then(c, &t, before);
	frame_put(before);
	return true;
}

static bool tool_mouse_drag(call *c, const json *a)
{
	bool h1, h2;
	long x = 0, y = 0, tx = 0, ty = 0;
	int button;
	then_t t;
	if (!get_xy(c, a, "x", "y", &h1, &x, &y) || !get_xy(c, a, "to-x", "to-y", &h2, &tx, &ty) ||
	    !arg_enum(c, a, "button", button_names, 0, &button) || !parse_then(c, a, &t))
		return false;
	if (!h1 || !h2) {
		call_error(c, "give x, y, to-x and to-y");
		return false;
	}
	frame_t *before = frame_get(NULL);
	pthread_mutex_lock(&mouse_mu);
	move_to(c, x, y, 16);
	m_buttons |= button_bits[button];
	wait_delivered(c, 0, push_buttons(4), false);
	// on the way, so the guest sees the drag
	pthread_mutex_lock(&mcp_mu);
	bool abs_ok = mcp.mouse_abs;
	pthread_mutex_unlock(&mcp_mu);
	if (abs_ok) {
		uint64_t s = 0;
		for (int i = 1; i <= 8; i++)
			s = mcp_mouse_push(true, x + (tx - x) * i / 8, y + (ty - y) * i / 8, m_buttons, 1);
		wait_delivered(c, 0, s, false);
		wait_ticks(c, 2);
	} else {
		move_to(c, tx, ty, 8);
	}
	m_buttons &= ~button_bits[button];
	wait_delivered(c, 0, push_buttons(2), false);
	char at[48];
	call_printf(c, "Dragged from %ld,%ld to %s.", x, y, pointer_at(at, sizeof(at)));
	pthread_mutex_unlock(&mouse_mu);
	finish_then(c, &t, before);
	frame_put(before);
	return true;
}

typedef struct {
	int dir, n;
} wheel_req;

static void mc_wheel(void *p)
{
	wheel_req *r = p;
	for (int i = 0; i < r->n; i++)
		mach_wheel(r->dir);
}

static bool tool_mouse_wheel(call *c, const json *a)
{
	static const char *const dirs[] = { "up", "down", NULL };
	bool has_xy;
	long x = 0, y = 0, count;
	int dir;
	then_t t;
	if (!get_xy(c, a, "x", "y", &has_xy, &x, &y) || !arg_enum(c, a, "direction", dirs, -1, &dir) ||
	    !arg_int(c, a, "count", 1, 1, 50, &count) || !parse_then(c, a, &t))
		return false;
	frame_t *before = frame_get(NULL);
	pthread_mutex_lock(&mouse_mu);
	if (has_xy)
		move_to(c, x, y, 16);
	for (long i = 0; i < count; i++) {
		wheel_req r = { dir ? 1 : -1, 1 };
		mcp_main_call(mc_wheel, &r);
		wait_ticks(c, 2);
	}
	pthread_mutex_unlock(&mouse_mu);
	call_printf(c, "Turned the wheel %s %ld notch%s.", dir ? "down" : "up", count, count == 1 ? "" : "es");
	finish_then(c, &t, before);
	frame_put(before);
	return true;
}

// The changed areas between two frames: pixels with gaps of up to 2
// pixels between them make one area. Returns how many (at most max).
static int diff_blobs(const frame_t *a, const frame_t *b, rect *out, int max)
{
	if (!a || !b || a->w != b->w || a->h != b->h)
		return 0;
	int W = a->w, H = a->h;
	uint8_t *m = calloc((size_t)W * H, 1);
	int *stack = malloc(sizeof(int) * (size_t)W * H);
	int n = 0;
	for (int i = 0; i < W * H; i++)
		m[i] = ((a->px[i] ^ b->px[i]) & 0xffffff) != 0;
	for (int p0 = 0; p0 < W * H && n < max; p0++) {
		if (m[p0] != 1)
			continue;
		int sp = 0, x0 = W, y0 = H, x1 = -1, y1 = -1;
		stack[sp++] = p0;
		m[p0] = 2;
		while (sp) {
			int p = stack[--sp], px = p % W, py = p / W;
			if (px < x0) x0 = px;
			if (px > x1) x1 = px;
			if (py < y0) y0 = py;
			if (py > y1) y1 = py;
			for (int dy = -2; dy <= 2; dy++) {
				for (int dx = -2; dx <= 2; dx++) {
					int qx = px + dx, qy = py + dy;
					if (qx < 0 || qy < 0 || qx >= W || qy >= H)
						continue;
					int q = qy * W + qx;
					if (m[q] == 1) {
						m[q] = 2;
						stack[sp++] = q;
					}
				}
			}
		}
		rect r = { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
		out[n++] = r;
	}
	free(m);
	free(stack);
	return n;
}

static bool tool_pointer_locate(call *c, const json *a)
{
	(void)a;
	int px, py;
	if (pointer_get(&px, &py)) {
		frame_t *f = frame_get(NULL);
		call_printf(c, "The pointer is at %d,%d (the red cross; the guest's driver says so).", px, py);
		shot(c, f, NULL, 1, true);
		frame_put(f);
		return true;
	}
	// no driver tells: move it a little sideways and back, and look
	rect blobs[64];
	pthread_mutex_lock(&mouse_mu);
	wait_screen_core(c, W_STABLE, 300, 3000, 1, NULL, NULL, 0, NULL);
	frame_t *A = frame_get(NULL);
	wait_delivered(c, 0, send_counts(12, 0, 12), false);
	wait_screen_core(c, W_STABLE, 300, 3000, 1, NULL, NULL, 0, NULL);
	frame_t *B = frame_get(NULL);
	wait_delivered(c, 0, send_counts(-12, 0, 12), false);
	wait_screen_core(c, W_STABLE, 300, 3000, 1, NULL, NULL, 0, NULL);
	frame_t *C = frame_get(NULL);
	pthread_mutex_unlock(&mouse_mu);
	int n = diff_blobs(A, B, blobs, 64), best = -1;
	double best_area = 1e9;
	for (int i = 0; i < n; i++) {
		// the pointer, drawn twice 12 pixels apart: small
		if (blobs[i].w > 60 || blobs[i].h > 48 || blobs[i].w < 13)
			continue;
		double area = blobs[i].w * blobs[i].h;
		if (area < best_area) {
			best_area = area;
			best = i;
		}
	}
	if (best >= 0) {
		call_printf(c, "The pointer's tip is near %d,%d (the top-left corner of where it was drawn).",
			blobs[best].x, blobs[best].y);
	} else if (n) {
		call_printf(c, "The pointer was not found among %d changed areas (", n);
		for (int i = 0; i < n && i < 8; i++)
			call_printf(c, "%s%d,%d %dx%d", i ? "; " : "", blobs[i].x, blobs[i].y, blobs[i].w, blobs[i].h);
		call_printf(c, "); try again when the screen is still.");
		c->unmet = true;
	} else {
		call_printf(c, "Nothing moved on the screen: no pointer is drawn here.");
		c->unmet = true;
	}
	shot(c, C, NULL, 1, false);
	frame_put(A);
	frame_put(B);
	frame_put(C);
	return true;
}

/* ---- serial ---- */

static void sb_visible(sbuf *b, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = s[i];
		if (ch == '\r' || ch == 0x11 || ch == 0x13) {
			continue;	// and XON/XOFF: flow control, not output
		} else if (ch == '\n' || ch == '\t') {
			sb_putc(b, ch);
		} else if (ch == 0x1b) {
			sb_puts(b, "^[");
		} else if (ch < 0x20 || ch == 0x7f) {
			sb_printf(b, "^%c", ch == 0x7f ? '?' : ch + '@');
		} else if (ch >= 0x80) {
			sb_printf(b, "\\x%02x", ch);
		} else {
			sb_putc(b, ch);
		}
	}
}

#define SERIAL_NAME "the serial port (Modem 1 or Modem 2)"

static bool tool_serial_write(call *c, const json *a)
{
	const char *text;
	bool raw;
	if (!arg_str(c, a, "text", true, &text) || !arg_bool(c, a, "raw", false, &raw))
		return false;
	size_t n = strlen(text);
	char *buf = malloc(n + 1);
	for (size_t i = 0; i < n; i++)
		buf[i] = (!raw && text[i] == '\n') ? '\r' : text[i];
	bool ok = mcp_serial_put(buf, n);
	free(buf);
	if (!ok) {
		call_error(c, "the serial input queue is full; the guest is not reading " SERIAL_NAME);
		return false;
	}
	if (wait_delivered(c, 0, 0, true))
		call_printf(c, "Sent %zu bytes to " SERIAL_NAME ".", n);
	else
		call_printf(c, "%zu bytes wait to be read on " SERIAL_NAME ".", n);
	return true;
}

static bool tool_serial_read(call *c, const json *a)
{
	bool all;
	long max;
	if (!arg_bool(c, a, "all", false, &all) || !arg_int(c, a, "max-bytes", 16384, 1, 262144, &max))
		return false;
	pthread_mutex_lock(&mcp_mu);
	uint64_t from = mcp_serial_cursor;
	pthread_mutex_unlock(&mcp_mu);
	uint64_t total = mcp_serial_total();
	if (all || total - from > (uint64_t)max)
		from = total > (uint64_t)max ? total - max : 0;
	char *buf = malloc(max);
	uint64_t start;
	size_t n = mcp_serial_get(from, buf, max, &start);
	pthread_mutex_lock(&mcp_mu);
	if (mcp_serial_cursor < start + n)
		mcp_serial_cursor = start + n;
	pthread_mutex_unlock(&mcp_mu);
	if (n == 0)
		call_printf(c, "(no new output on " SERIAL_NAME ")");
	else
		sb_visible(&c->text, buf, n);
	free(buf);
	return true;
}

// find text (or regex) in the output after from; wait for it
static bool serial_wait_for(call *c, const char *text, bool is_regex, long timeout_ms,
		uint64_t from, sbuf *out, uint64_t *match_end, char *sub, size_t subsz)
{
	regex_t re;
	if (is_regex) {
		int e = regcomp(&re, text, REG_EXTENDED);
		if (e) {
			char msg[200];
			regerror(e, &re, msg, sizeof(msg));
			call_error(c, "bad regular expression: %s", msg);
			return false;
		}
	}
	double deadline = wall_now() + timeout_ms / 1000.0;
	char *buf = malloc(SER_RING + 1);
	bool found = false;
	size_t tl = strlen(text);
	uint64_t seen = (uint64_t)-1;
	for (;;) {
		uint64_t total = mcp_serial_total();
		if (total == seen) {
			if (wall_now() >= deadline || call_stopped(c)) {
				uint64_t start;
				size_t n = mcp_serial_get(from, buf, SER_RING, &start);
				sb_add(out, buf, n);
				*match_end = start + n;
				break;
			}
			pthread_mutex_lock(&mcp_mu);
			mcp_wait_until(deadline);
			pthread_mutex_unlock(&mcp_mu);
			continue;
		}
		seen = total;
		uint64_t start;
		size_t n = mcp_serial_get(from, buf, SER_RING, &start);
		for (size_t i = 0; i < n; i++)
			if (!buf[i])
				buf[i] = ' ';
		buf[n] = 0;
		size_t end = 0;
		if (is_regex) {
			regmatch_t m[2];
			if (n && regexec(&re, buf, 2, m, 0) == 0) {
				found = true;
				end = m[0].rm_eo;
				if (sub && m[1].rm_so >= 0) {
					size_t l = m[1].rm_eo - m[1].rm_so;
					if (l >= subsz) l = subsz - 1;
					memcpy(sub, buf + m[1].rm_so, l);
					sub[l] = 0;
				}
			}
		} else if (tl) {
			char *p = n >= tl ? strstr(buf, text) : NULL;
			if (p) {
				found = true;
				end = p - buf + tl;
			}
		}
		if (found) {
			sb_add(out, buf, end);
			*match_end = start + end;
			break;
		}
		if (wall_now() >= deadline || call_stopped(c)) {
			sb_add(out, buf, n);
			*match_end = start + n;
			break;
		}
		call_progress(c, "waiting for serial output");
		pthread_mutex_lock(&mcp_mu);
		mcp_wait_until(deadline);
		pthread_mutex_unlock(&mcp_mu);
	}
	free(buf);
	if (is_regex)
		regfree(&re);
	return found;
}

static bool tool_serial_expect(call *c, const json *a)
{
	const char *text;
	bool is_regex;
	long timeout_ms;
	if (!arg_str(c, a, "text", true, &text) || !arg_bool(c, a, "regex", false, &is_regex) ||
	    !arg_int(c, a, "timeout-ms", 30000, 0, WAIT_MAX_MS, &timeout_ms))
		return false;
	pthread_mutex_lock(&mcp_mu);
	uint64_t from = mcp_serial_cursor;
	pthread_mutex_unlock(&mcp_mu);
	sbuf out;
	sb_init(&out);
	uint64_t end;
	double t0 = wall_now();
	if (!is_regex && !*text) {
		call_error(c, "text is empty");
		sb_free(&out);
		return false;
	}
	bool found = serial_wait_for(c, text, is_regex, timeout_ms, from, &out, &end, NULL, 0);
	if (c->is_error) {
		sb_free(&out);
		return false;
	}
	if (found) {
		pthread_mutex_lock(&mcp_mu);
		mcp_serial_cursor = end;
		pthread_mutex_unlock(&mcp_mu);
		call_printf(c, "Found after %.1f s. Output up to the match:\n", wall_now() - t0);
	} else {
		c->unmet = true;
		call_printf(c, "Not found in %.1f s. The output since the last read:\n", wall_now() - t0);
	}
	sb_visible(&c->text, out.s ? out.s : "", out.len);
	sb_free(&out);
	return true;
}

// queue text for the serial port and wait until the guest has it
static bool ser_send(call *c, const char *text)
{
	if (!mcp_serial_put(text, strlen(text))) {
		call_error(c, "the serial input queue is full; the guest is not reading " SERIAL_NAME);
		return false;
	}
	wait_delivered(c, 0, 0, true);
	return true;
}

static bool tool_serial_run(call *c, const json *a)
{
	const char *cmd;
	long timeout_ms;
	if (!arg_str(c, a, "command", true, &cmd) ||
	    !arg_int(c, a, "timeout-ms", 60000, 1000, WAIT_MAX_MS, &timeout_ms))
		return false;
	size_t n = strlen(cmd);
	while (n && (cmd[n - 1] == '\n' || cmd[n - 1] == ' '))
		n--;
	if (!n) {
		call_error(c, "command is empty");
		return false;
	}
	// the marker is typed split, so its echo does not match
	static const char marker_typed[] = "echo \"<<AR\"\"END $?>>\"";
	uint64_t start = mcp_serial_total();
	double t0 = wall_now();
	sbuf in;
	sb_init(&in);
	// line by line: each line goes as Return, then the marker
	for (size_t i = 0; i < n; i++)
		sb_putc(&in, cmd[i] == '\n' ? '\r' : cmd[i]);
	sb_puts(&in, cmd[n - 1] == '&' ? " " : "; ");
	sb_puts(&in, marker_typed);
	sb_putc(&in, '\r');
	if (!ser_send(c, in.s)) {
		sb_free(&in);
		return false;
	}
	sb_free(&in);

	sbuf out;
	sb_init(&out);
	uint64_t end;
	char status[16] = "";
	bool found = serial_wait_for(c, "<<AREND ([0-9]+)>>", true, timeout_ms, start, &out, &end,
		status, sizeof(status));
	pthread_mutex_lock(&mcp_mu);
	mcp_serial_cursor = end;
	pthread_mutex_unlock(&mcp_mu);
	// the output starts after the echo of the marker command, if the tty echoes
	const char *s = out.s ? out.s : "";
	size_t len = out.len;
	const char *p = s, *q;
	while ((q = memmem(p, s + len - p, "<<AR\"\"END", 9)) != NULL) {
		const char *nl = memchr(q, '\n', s + len - q);
		p = nl ? nl + 1 : s + len;
	}
	const char *stop = s + len;
	if (found) {
		const char *m = NULL, *r = p;
		while ((q = memmem(r, s + len - r, "<<AREND ", 8)) != NULL) {
			m = q;
			r = q + 8;
		}
		if (m)
			stop = m;
	}
	if (found) {
		call_printf(c, "Exit status %s after %.1f s. Output:\n", status, wall_now() - t0);
	} else {
		c->unmet = true;
		call_printf(c, "Still running after %.1f s (or no shell answered; is a shell reading "
			SERIAL_NAME "?). Output so far:\n", wall_now() - t0);
	}
	size_t olen = stop > p ? (size_t)(stop - p) : 0;
	if (olen > 65536) {
		call_printf(c, "[... %zu bytes left out ...]\n", olen - 65536);
		p = stop - 65536;
		olen = 65536;
	}
	sb_visible(&c->text, p, olen);
	sb_free(&out);
	return true;
}

/* ---- media ---- */

typedef struct {
	int action, drive;
	const char *file;
	bool ok;
	char floppy[2][600];
} media_req;

static void mc_media(void *p)
{
	media_req *r = p;
	if (r->action == 1)
		r->ok = mach_floppy_insert(r->drive, r->file);
	else if (r->action == 2) {
		mach_floppy_eject(r->drive);
		r->ok = true;
	}
	for (int d = 0; d < 2; d++) {
		const char *f = mach_floppy_path(d);
		snprintf(r->floppy[d], sizeof(r->floppy[d]), "%s", f ? f : "");
	}
}

static bool tool_media(call *c, const json *a)
{
	static const char *const acts[] = { "status", "insert", "eject", NULL };
	static const char *const drives[] = { "a", "b", NULL };
	int act, drive;
	const char *file;
	if (!arg_enum(c, a, "action", acts, 0, &act) || !arg_enum(c, a, "drive", drives, 0, &drive) ||
	    !arg_str(c, a, "file", act == 1, &file))
		return false;
	char path[1024] = "";
	if (file) {
		if (file[0] != '/' && mcp_vm_dir)
			snprintf(path, sizeof(path), "%s/%s", mcp_vm_dir, file);
		else
			snprintf(path, sizeof(path), "%s", file);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			call_error(c, "%s is not a file", path);
			return false;
		}
	}
	media_req r;
	memset(&r, 0, sizeof(r));
	r.action = act;
	r.drive = drive;
	r.file = path;
	mcp_main_call(mc_media, &r);
	if (act == 1 && !r.ok) {
		call_error(c, "could not open %s as a floppy image (.st, .msa, .dim, .stx, .ipf, or in a .zip)", path);
		return false;
	}
	call_printf(c, "%sFloppy A: %s. Floppy B: %s.", act == 1 ? "Inserted. " : act == 2 ? "Ejected. " : "",
		r.floppy[0][0] ? r.floppy[0] : "empty", r.floppy[1][0] ? r.floppy[1] : "empty");
	return true;
}

/* ---- memory and the debugger ---- */

typedef struct {
	uint32_t addr;
	bool write, check;
	int len;
	uint8_t *data;
	bool *ok;
	int bad;
} mem_req;

static void mc_mem(void *p)
{
	mem_req *r = p;
	for (int i = 0; i < r->len; i++) {
		uint8_t *b = mach_mem(r->addr + i, r->write || r->check);
		r->ok[i] = b != NULL;
		if (!b) {
			r->bad++;
			continue;
		}
		if (r->check)
			continue;
		if (r->write)
			*b = r->data[i];
		else
			r->data[i] = *b;
	}
	if (r->write)
		mach_mem_written(r->addr, r->len);
}

static bool tool_memory_read(call *c, const json *a)
{
	const char *as;
	long len;
	bool str;
	uint32_t addr;
	if (!arg_str(c, a, "address", true, &as) || !parse_addr(c, as, &addr) ||
	    !arg_int(c, a, "length", 128, 1, 4096, &len) || !arg_bool(c, a, "string", false, &str))
		return false;
	if (str && !json_has(a, "length"))
		len = 256;
	mem_req r = { addr, false, false, (int)len, calloc(len, 1), calloc(len, sizeof(bool)), 0 };
	mcp_main_call(mc_mem, &r);
	if (r.bad == len) {
		call_error(c, "nothing readable at 0x%08x (only ST-RAM, TT-RAM and ROM; not I/O)", addr);
	} else if (str) {
		call_printf(c, "0x%08x: \"", addr);
		int i;
		for (i = 0; i < len && r.ok[i] && r.data[i]; i++) {
			if (r.data[i] == '"' || r.data[i] == '\\')
				call_printf(c, "\\%c", r.data[i]);
			else if (r.data[i] >= 0x20 && r.data[i] < 0x7f)
				call_printf(c, "%c", r.data[i]);
			else
				call_printf(c, "\\x%02x", r.data[i]);
		}
		call_printf(c, "\"%s", i == len ? " (no zero byte yet)" : "");
	} else {
		for (int i = 0; i < len; i += 16) {
			call_printf(c, "%08x: ", addr + i);
			for (int k = 0; k < 16; k++) {
				if (i + k < len && r.ok[i + k])
					call_printf(c, "%02x", r.data[i + k]);
				else
					call_printf(c, i + k < len ? "--" : "  ");
				if (k & 1)
					call_printf(c, " ");
			}
			call_printf(c, " ");
			for (int k = 0; k < 16 && i + k < len; k++) {
				uint8_t b = r.data[i + k];
				call_printf(c, "%c", r.ok[i + k] && b >= 0x20 && b < 0x7f ? b : '.');
			}
			call_printf(c, "\n");
		}
		if (r.bad)
			call_printf(c, "(-- : not readable: I/O space, or no memory there)");
	}
	free(r.data);
	free(r.ok);
	return true;
}

static int hexv(char ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

static bool tool_memory_write(call *c, const json *a)
{
	const char *as, *hex, *text;
	uint32_t addr;
	if (!arg_str(c, a, "address", true, &as) || !parse_addr(c, as, &addr) ||
	    !arg_str(c, a, "hex", false, &hex) || !arg_str(c, a, "text", false, &text))
		return false;
	if (!!hex == !!text) {
		call_error(c, "give hex or text");
		return false;
	}
	uint8_t *data = malloc(4096);
	int len = 0;
	if (hex) {
		int hi = -1;
		for (const char *p = hex; *p; p++) {
			if (*p == ' ' || *p == ',' || *p == ':')
				continue;
			if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && hi < 0) {
				p++;
				continue;
			}
			int v = hexv(*p);
			if (v < 0 || len >= 4096) {
				call_error(c, v < 0 ? "hex has a character that is not a hex digit" : "at most 4096 bytes");
				free(data);
				return false;
			}
			if (hi < 0) {
				hi = v;
			} else {
				data[len++] = hi << 4 | v;
				hi = -1;
			}
		}
		if (hi >= 0) {
			call_error(c, "hex has an odd number of digits");
			free(data);
			return false;
		}
	} else {
		len = strlen(text);
		if (len > 4096) {
			call_error(c, "at most 4096 bytes");
			free(data);
			return false;
		}
		memcpy(data, text, len);
	}
	if (!len) {
		call_error(c, "nothing to write");
		free(data);
		return false;
	}
	bool *ok = calloc(len, sizeof(bool));
	// check first: write only if all of it is RAM
	mem_req chk = { addr, false, true, len, NULL, ok, 0 };
	mcp_main_call(mc_mem, &chk);
	for (int i = 0; i < len; i++) {
		if (!ok[i]) {
			call_error(c, "0x%08x is not RAM (ROM, I/O or nothing): nothing written", addr + i);
			free(ok);
			free(data);
			return false;
		}
	}
	mem_req r = { addr, true, false, len, data, ok, 0 };
	mcp_main_call(mc_mem, &r);
	call_printf(c, "Wrote %d bytes at 0x%08x.", len, addr);
	free(ok);
	free(data);
	return true;
}

typedef struct {
	uint32_t addr;
	int count;
	sbuf *out;
} dis_req;

static void dis_lines(sbuf *out, uint32_t pc, int count)
{
	char buf[256];
	if (!mach_disasm_available()) {
		sb_puts(out, "(no 68k disassembler)\n");
		return;
	}
	for (int i = 0; i < count; i++) {
		int n = mach_disasm(pc, buf, sizeof(buf));
		if (n <= 0) {
			sb_printf(out, "%08x  (not readable)\n", pc);
			break;
		}
		sb_printf(out, "%08x  %s\n", pc, buf);
		pc += n;
	}
}

static void mc_dis(void *p)
{
	dis_req *r = p;
	dis_lines(r->out, r->addr, r->count);
}

static void regs_text(sbuf *o, int ninstr)
{
	mach_regs r;
	mach_get_regs(&r);
	uint32_t sr = r.sr;
	sb_printf(o, "PC %08x  SR %04x (%s, T=%d, I=%d, X%d N%d Z%d V%d C%d)\n", r.pc, sr,
		sr & 0x2000 ? "supervisor" : "user", (sr >> 14) & 3, (sr >> 8) & 7,
		(sr >> 4) & 1, (sr >> 3) & 1, (sr >> 2) & 1, (sr >> 1) & 1, sr & 1);
	for (int i = 0; i < 8; i++)
		sb_printf(o, "D%d %08x%s", i, r.d[i], i == 3 || i == 7 ? "\n" : "  ");
	for (int i = 0; i < 8; i++)
		sb_printf(o, "A%d %08x%s", i, r.a[i], i == 3 || i == 7 ? "\n" : "  ");
	if (r.cpu >= 68020)
		sb_printf(o, "USP %08x  SSP %08x  MSP %08x  VBR %08x  SFC %d  DFC %d  CACR %08x  (%d)\n",
			r.usp, r.ssp, r.msp, r.vbr, r.sfc, r.dfc, r.cacr, r.cpu);
	else if (r.cpu == 68010)
		sb_printf(o, "USP %08x  SSP %08x  VBR %08x  SFC %d  DFC %d  (68010)\n",
			r.usp, r.ssp, r.vbr, r.sfc, r.dfc);
	else
		sb_printf(o, "USP %08x  SSP %08x  (68000)\n", r.usp, r.ssp);
	if (ninstr)
		dis_lines(o, r.pc, ninstr);
}

typedef struct {
	sbuf *out;
	int n;
} regs_req;

static void mc_regs(void *p)
{
	regs_req *r = p;
	regs_text(r->out, r->n);
}

static bool tool_debug_registers(call *c, const json *a)
{
	long n;
	if (!arg_int(c, a, "instructions", 4, 0, 32, &n))
		return false;
	regs_req r = { &c->text, (int)n };
	pthread_mutex_lock(&mcp_mu);
	bool running = mcp_cpu_running_locked();
	pthread_mutex_unlock(&mcp_mu);
	if (running)
		call_printf(c, "(the CPU runs: a snapshot at a VBL)\n");
	mcp_main_call(mc_regs, &r);
	return true;
}

typedef struct {
	const char *name;
	uint32_t value;
	bool ok;
} regw_req;

static void mc_regw(void *p)
{
	regw_req *r = p;
	r->ok = mach_set_reg(r->name, r->value);
}

static bool tool_register_write(call *c, const json *a)
{
	const char *name, *vs;
	uint32_t v;
	if (!arg_str(c, a, "register", true, &name) || !arg_str(c, a, "value", true, &vs) ||
	    !parse_addr(c, vs, &v))
		return false;
	pthread_mutex_lock(&mcp_mu);
	bool running = mcp_cpu_running_locked();
	pthread_mutex_unlock(&mcp_mu);
	if (running) {
		call_error(c, "the CPU runs: stop it first with vm-power pause or debug-run");
		return false;
	}
	regw_req r = { name, v, false };
	mcp_main_call(mc_regw, &r);
	if (!r.ok) {
		call_error(c, "unknown register '%s' (for this CPU): d0-d7, a0-a7, sp, pc, sr, usp, ssp;"
			" vbr, sfc, dfc from the 68010; msp, cacr from the 68020", name);
		return false;
	}
	call_printf(c, "%s = %08x.", name, v);
	return true;
}

static bool tool_disassemble(call *c, const json *a)
{
	const char *as;
	long count;
	uint32_t addr = 0;
	if (!arg_str(c, a, "address", false, &as) || (as && !parse_addr(c, as, &addr)) ||
	    !arg_int(c, a, "count", 16, 1, 200, &count))
		return false;
	if (!as) {
		info_req r;
		memset(&r, 0, sizeof(r));
		mcp_main_call(mc_info, &r);
		addr = r.regs.pc;
	}
	dis_req r = { addr, (int)count, &c->text };
	mcp_main_call(mc_dis, &r);
	return true;
}

typedef struct {
	uint32_t bp[DBG_MAX_BP];
	int n, count, super;
	long steps;
	bool stopped_before;
} run_req;

static void mc_dbg_setup(void *p)
{
	run_req *r = p;
	dbg_state *d = &mcp_dbg;
	pthread_mutex_lock(&mcp_mu);
	bool in_break = mcp.paused_break;
	pthread_mutex_unlock(&mcp_mu);
	memcpy(d->bp, r->bp, sizeof(d->bp));
	d->n_bp = r->n;
	d->count = r->count;
	d->hits = 0;
	d->super = r->super;
	d->steps = r->steps;
	// stopped in the hook, the instruction at PC runs without another
	// call of the hook: it is the first step
	d->stepped = in_break && r->steps ? 1 : 0;
	d->stopped = false;
	// leaving a pause between instructions: the one at PC runs first,
	// breakpoint or not (leaving the hook, it runs anyway)
	mach_regs regs;
	mach_get_regs(&regs);
	d->skip_pc = regs.pc;
	d->skip_armed = r->stopped_before && !in_break && !r->steps;
	hatari_mcp_dbg_active = 1;
	mach_debug_hook();
	mach_pause(false);
}

static void mc_dbg_clear(void *p)
{
	(void)p;
	mcp_dbg.n_bp = 0;
	mcp_dbg.steps = 0;
	hatari_mcp_dbg_active = 0;
	mach_debug_hook();
}

static void mc_debug_check(void *p)
{
	*(const char **)p = mach_debug_unavailable();
}

static bool debug_ok(call *c)
{
	const char *why = NULL;
	mcp_main_call(mc_debug_check, &why);
	if (why)
		call_error(c, "%s", why);
	return !why;
}

static void report_stop(call *c)
{
	regs_req rr = { &c->text, 6 };
	mcp_main_call(mc_regs, &rr);
}

// resume the CPU and wait for a debugger stop
static bool dbg_go(call *c, run_req *r, long timeout_ms)
{
	pthread_mutex_lock(&mcp_mu);
	r->stopped_before = mcp.paused_break || mcp.paused_user;
	pthread_mutex_unlock(&mcp_mu);
	mcp_main_call(mc_dbg_setup, r);
	pthread_mutex_lock(&mcp_mu);
	mcp.paused_user = false;
	mcp.paused_break = false;
	pthread_cond_broadcast(&mcp_cv);
	double deadline = wall_now() + timeout_ms / 1000.0;
	while (!mcp.paused_break && !mcp.quitting && !call_cancelled(c) && wall_now() < deadline) {
		pthread_mutex_unlock(&mcp_mu);
		call_progress(c, r->steps ? "stepping" : "running to the breakpoint");
		pthread_mutex_lock(&mcp_mu);
		mcp_wait_until(deadline);
	}
	bool stopped = mcp.paused_break;
	pthread_mutex_unlock(&mcp_mu);
	if (!stopped)
		mcp_main_call(mc_dbg_clear, NULL);
	return stopped;
}

static bool tool_debug_run(call *c, const json *a)
{
	run_req r;
	memset(&r, 0, sizeof(r));
	long count, timeout_ms;
	json *av = json_get(a, "address");
	json *sv = json_get(a, "supervisor");
	if (!av) {
		call_error(c, "address is missing");
		return false;
	}
	if (av->type == JSON_STRING) {
		if (!parse_addr(c, av->str, &r.bp[0]))
			return false;
		r.n = 1;
	} else if (av->type == JSON_ARRAY && av->n >= 1 && av->n <= DBG_MAX_BP) {
		for (int i = 0; i < av->n; i++) {
			if (av->items[i]->type != JSON_STRING) {
				call_error(c, "address must be a string or a list of strings");
				return false;
			}
			if (!parse_addr(c, av->items[i]->str, &r.bp[i]))
				return false;
		}
		r.n = av->n;
	} else {
		call_error(c, "address must be an address or a list of 1 to %d", DBG_MAX_BP);
		return false;
	}
	r.super = -1;
	if (sv && sv->type != JSON_NULL) {
		if (sv->type != JSON_BOOL) {
			call_error(c, "supervisor must be true or false");
			return false;
		}
		r.super = sv->b;
	}
	if (!arg_int(c, a, "count", 1, 1, 1000000000, &count) ||
	    !arg_int(c, a, "timeout-ms", 30000, 0, WAIT_MAX_MS, &timeout_ms))
		return false;
	if (!debug_ok(c))
		return false;
	r.count = count;
	double t0 = wall_now();
	if (!dbg_go(c, &r, timeout_ms)) {
		c->unmet = true;
		call_printf(c, "Not reached in %.1f s; the breakpoints are gone and the CPU runs on.", wall_now() - t0);
		return true;
	}
	call_printf(c, "Stopped at %08x after %.1f s, before running the instruction there. vm-power"
		" resume lets it run; debug-step goes on one at a time.\n", mcp_dbg.stop_pc, wall_now() - t0);
	report_stop(c);
	return true;
}

static bool tool_debug_step(call *c, const json *a)
{
	long count;
	if (!arg_int(c, a, "count", 1, 1, 100000, &count))
		return false;
	pthread_mutex_lock(&mcp_mu);
	bool stopped = mcp.paused_break || mcp.paused_user;
	pthread_mutex_unlock(&mcp_mu);
	if (!stopped) {
		call_error(c, "the CPU runs: stop it first with debug-run or vm-power pause");
		return false;
	}
	if (!debug_ok(c))
		return false;
	run_req r;
	memset(&r, 0, sizeof(r));
	r.steps = count;
	r.super = -1;
	if (!dbg_go(c, &r, 60000)) {
		call_error(c, "the steps did not finish");
		return false;
	}
	call_printf(c, "Did %ld instruction%s.\n", count, count == 1 ? "" : "s");
	report_stop(c);
	return true;
}

/* ---- steps ---- */

static bool tool_steps(call *c, const json *a);

static const struct { const char *name; tool_fn fn; } tools[] = {
	{ "vm-info", tool_vm_info },
	{ "vm-power", tool_vm_power },
	{ "screenshot", tool_screenshot },
	{ "wait-screen", tool_wait_screen },
	{ "key", tool_key },
	{ "type-text", tool_type_text },
	{ "mouse-move", tool_mouse_move },
	{ "mouse-click", tool_mouse_click },
	{ "mouse-button", tool_mouse_button },
	{ "mouse-drag", tool_mouse_drag },
	{ "mouse-wheel", tool_mouse_wheel },
	{ "pointer-locate", tool_pointer_locate },
	{ "serial-write", tool_serial_write },
	{ "serial-read", tool_serial_read },
	{ "serial-expect", tool_serial_expect },
	{ "serial-run", tool_serial_run },
	{ "media", tool_media },
	{ "steps", tool_steps },
	{ "debug-registers", tool_debug_registers },
	{ "register-write", tool_register_write },
	{ "memory-read", tool_memory_read },
	{ "memory-write", tool_memory_write },
	{ "disassemble", tool_disassemble },
	{ "debug-run", tool_debug_run },
	{ "debug-step", tool_debug_step },
	{ NULL, NULL }
};

tool_fn tool_find(const char *name)
{
	for (int i = 0; tools[i].name; i++)
		if (strcmp(tools[i].name, name) == 0)
			return tools[i].fn;
	return NULL;
}

static bool tool_steps(call *c, const json *a)
{
	json *steps = json_get(a, "steps");
	if (!steps || steps->type != JSON_ARRAY || steps->n < 1 || steps->n > 50) {
		call_error(c, "steps must be a list of 1 to 50 steps");
		return false;
	}
	for (int i = 0; i < steps->n; i++) {
		json *s = steps->items[i];
		const char *name = json_get_str(s, "tool");
		json *args = json_get(s, "args");
		if (!name || !tool_find(name) || strcmp(name, "steps") == 0) {
			call_error(c, "step %d: %s", i + 1, name ? "no such tool (or steps inside steps)" : "tool is missing");
			return false;
		}
		if (args && args->type != JSON_OBJECT && args->type != JSON_NULL) {
			call_error(c, "step %d: args must be an object", i + 1);
			return false;
		}
	}
	json *empty = json_parse("{}", 2, NULL);
	for (int i = 0; i < steps->n; i++) {
		json *s = steps->items[i];
		const char *name = json_get_str(s, "tool");
		json *args = json_get(s, "args");
		if (!args || args->type != JSON_OBJECT)
			args = empty;
		call sub;
		memset(&sub, 0, sizeof(sub));
		sub.cn = c->cn;
		sub.id = c->id;
		sub.token = c->token;
		sub.started = c->started;
		sub.last_progress = c->last_progress;
		sub.parent = c;
		sb_init(&sub.text);
		tool_find(name)(&sub, args);
		c->last_progress = sub.last_progress;
		call_printf(c, "%d. %s: %s\n", i + 1, name, sub.text.len ? sub.text.s : "done");
		if (sub.image) {
			free(c->image);
			c->image = sub.image;
		}
		sb_free(&sub.text);
		if (sub.is_error || sub.unmet || call_stopped(c)) {
			call_printf(c, "Stopped at step %d of %d%s.", i + 1, steps->n,
				sub.is_error ? " (it failed)" : call_stopped(c) ? " (cancelled)" : " (its wait was not met)");
			c->unmet = !sub.is_error;
			c->is_error = sub.is_error;
			break;
		}
		if (i + 1 == steps->n)
			call_printf(c, "All %d steps done.", steps->n);
	}
	json_free(empty);
	return true;
}
