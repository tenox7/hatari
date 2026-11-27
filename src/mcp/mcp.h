/*
 * mcp.h - Hatari's MCP server, inside: what server.c, keys.c, tools.c and
 * bridge.c share.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef AMCP_MCP_H
#define AMCP_MCP_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "json.h"

/// VBLs a second: 50, 60 or 71 Hz, as the guest's video mode runs
extern volatile int mcp_tick_hz;
#define TICK_HZ		(mcp_tick_hz)
#define WAIT_MAX_MS	(25 * 60 * 1000)
#define SCR_MAX		4096	// the largest screen side we take

#define MCP_PROTOCOL_LATEST "2025-11-25"

typedef struct { int x, y, w, h; } rect;

/// A picture of the host window, 0x00RRGGBB pixels. Frames are shared and
/// never change: frame_get takes a reference, frame_put lets it go.
typedef struct frame {
	int refs;			// mcp_mu
	int w, h;
	uint64_t seq;		// counts changes
	uint64_t tick;		// guest tick of this change
	uint32_t px[];
} frame_t;

/// What the server knows of the emulator; guarded by mcp_mu.
typedef struct {
	frame_t *frame;			// the latest picture (NULL before the first)
	uint64_t ticks;			// guest VBLs so far
	double change_wall;		// wall time of the last change
	int calls_running;		// tool calls in progress
	bool paused_user;		// vm-power pause
	bool paused_break;		// debug-run or debug-step stopped the CPU
	bool quit;
	int reset;				// 1 warm, 2 cold
	bool quitting;			// hatari_mcp_done has begun
	bool pointer_known;		// a driver says where the pointer is
	int pointer_x, pointer_y;
	bool mouse_abs;			// the guest takes absolute positions (never on Hatari)
} mcp_state;

extern pthread_mutex_t mcp_mu;
extern pthread_cond_t mcp_cv;	// broadcast at every tick and state change
extern mcp_state mcp;

double wall_now(void);	// seconds, monotonic
/// Wait on mcp_cv (mcp_mu held) until woken or the wall time "until".
void mcp_wait_until(double until);
/// Run fn(arg) on the emulator's main thread and wait for it.
void mcp_main_call(void (*fn)(void *), void *arg);

/// Whether the CPU runs now (mcp_mu held).
bool mcp_cpu_running_locked(void);

/* ---- frames ---- */
/// The latest picture (a reference; NULL before the first), and the guest
/// tick now.
frame_t *frame_get(uint64_t *at);
void frame_put(frame_t *f);
/// Recent pictures (the last few changes), oldest first: those up to change
/// max_seq that appeared at or after guest tick min_tick. References.
int frame_history(uint64_t max_seq, uint64_t min_tick, frame_t **out, int max);

/* ---- keys (keys.c) ---- */
typedef struct {
	uint8_t code[6];	// scancodes, modifiers first
	int n;
	int hold, gap;		// ticks
} key_chord;

/// Parse a key or a combination ("ctrl+c", "shift-f1", "a").
bool keys_parse_combo(key_chord *k, const char *spec);
/// The keys for a character, in the US layout. False: it can't be typed.
bool keys_from_char(key_chord *k, unsigned char ch);
const char *keys_name_list(void);
/// Queue a chord; returns its sequence number (0: the queue is full).
uint64_t keys_push(const key_chord *k);
uint64_t keys_done(void);
int keys_pending(void);
void keys_clear(void);
/// The main thread, mcp_mu not held: one step of the key queue a tick.
void keys_tick(void);

/* ---- mouse packets, one a tick ---- */
/// abs: go to x, y (dx, dy) directly, where the guest can; else a relative packet.
uint64_t mcp_mouse_push(bool abs, int dx, int dy, int buttons, int wait_ticks);
uint64_t mcp_mouse_done(void);

/* ---- serial ---- */
#define SER_RING (1 << 20)
/// Bytes the guest sent: copy from absolute position pos, at most max.
/// Returns the count; *start is where they really start (old bytes are
/// overwritten after 1 MiB).
size_t mcp_serial_get(uint64_t pos, char *out, size_t max, uint64_t *start);
uint64_t mcp_serial_total(void);
/// Queue bytes for the guest to read.
bool mcp_serial_put(const char *data, size_t n);
/// Bytes queued for the guest and not read yet.
size_t mcp_serial_queued(void);
extern uint64_t mcp_serial_cursor;		// where the next read starts (mcp_mu)
/// The port the guest last wrote to (HATARI_MCP_SERIAL_*), 0 none yet.
int mcp_serial_port(void);

/* ---- debugger (the hook runs on the main thread) ---- */
#define DBG_MAX_BP 16
typedef struct {
	uint32_t bp[DBG_MAX_BP];
	int n_bp;
	int count, hits;	// stop at the count-th hit
	int super;			// -1 either, 0 user mode, 1 supervisor mode
	long steps;			// > 0: stop after this many instructions
	long stepped;
	uint32_t skip_pc;	// the first instruction after a resume runs
	bool skip_armed;
	bool stopped;
	uint32_t stop_pc;
} dbg_state;
extern dbg_state mcp_dbg;

/* ---- the registry of running VMs ---- */
const char *mcp_registry_dir(void);
/// Whether dir is ours and private (with create, make it first).
bool mcp_registry_ok(const char *dir, bool create);

/* ---- a tool call ---- */

typedef struct conn conn;
typedef struct call call;
struct call {
	conn *cn;
	char *id;				// the request's id, as JSON
	char *token;			// the progressToken, as JSON, or NULL
	volatile bool cancelled;
	double started, last_progress;
	int progress;
	sbuf text;				// the summary for the agent
	char *image;			// base64 PNG of the last screenshot, or NULL
	bool is_error;
	bool unmet;				// a wait did not get what it waited for
	call *parent;			// the steps call this step runs in
	call *next;
};

/// Whether the call, or the steps call it runs in, was cancelled.
static inline bool call_cancelled(const call *c)
{
	for (; c; c = c->parent)
		if (c->cancelled)
			return true;
	return false;
}

void call_printf(call *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void call_error(call *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/// Send a progress notification now and then, if the client asked.
void call_progress(call *c, const char *what);
/// Whether the call should stop (cancelled, VM quitting).
bool call_stopped(call *c);

typedef bool (*tool_fn)(call *c, const json *args);
tool_fn tool_find(const char *name);

/// The tool list (JSON array text) and the instructions.
const char *mcp_tools_json(void);
const char *mcp_instructions(void);
bool mcp_protocol_supported(const char *v);

/// PNG of 0x00RRGGBB pixels.
bool png_encode_rgb(sbuf *out, int w, int h, const uint32_t *px, int stride);

/// The VM's name (folder) and directory.
extern char *mcp_vm_name;
extern char *mcp_vm_dir;

#endif
