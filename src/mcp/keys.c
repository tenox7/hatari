/*
 * keys.c - key names, the US layout and the key queue of the MCP tools.
 *
 * The keys go to the IKBD as Atari scancodes, one step a VBL: press the
 * modifiers and the key, hold, let go in reverse, then a gap.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "mcp.h"
#include "machine.h"

#define SC_LSHIFT	0x2a
#define SC_RSHIFT	0x36
#define SC_CTRL		0x1d
#define SC_ALT		0x38

static const struct { const char *name; uint8_t code; } names[] = {
	{ "esc", 0x01 }, { "escape", 0x01 },
	{ "backspace", 0x0e }, { "bs", 0x0e },
	{ "tab", 0x0f },
	{ "return", 0x1c }, { "ret", 0x1c }, { "enter", 0x1c },
	{ "ctrl", SC_CTRL }, { "control", SC_CTRL },
	{ "shift", SC_LSHIFT }, { "lshift", SC_LSHIFT }, { "rshift", SC_RSHIFT },
	{ "alt", SC_ALT }, { "alternate", SC_ALT },
	{ "space", 0x39 }, { "spc", 0x39 },
	{ "capslock", 0x3a }, { "caps", 0x3a },
	{ "f1", 0x3b }, { "f2", 0x3c }, { "f3", 0x3d }, { "f4", 0x3e }, { "f5", 0x3f },
	{ "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 }, { "f9", 0x43 }, { "f10", 0x44 },
	{ "home", 0x47 }, { "clrhome", 0x47 },
	{ "up", 0x48 }, { "down", 0x50 }, { "left", 0x4b }, { "right", 0x4d },
	{ "insert", 0x52 }, { "ins", 0x52 },
	{ "delete", 0x53 }, { "del", 0x53 },
	{ "undo", 0x61 }, { "help", 0x62 },
	{ "iso", 0x60 }, { "less", 0x60 },
	{ "kp-(", 0x63 }, { "kp-)", 0x64 }, { "kp-/", 0x65 }, { "kp-*", 0x66 },
	{ "kp-7", 0x67 }, { "kp-8", 0x68 }, { "kp-9", 0x69 }, { "kp-4", 0x6a },
	{ "kp-5", 0x6b }, { "kp-6", 0x6c }, { "kp-1", 0x6d }, { "kp-2", 0x6e },
	{ "kp-3", 0x6f }, { "kp-0", 0x70 }, { "kp-.", 0x71 }, { "kp-enter", 0x72 },
	{ "kp--", 0x4a }, { "kp-+", 0x4e },
	{ "minus", 0x0c }, { "equal", 0x0d }, { "grave", 0x29 }, { "backslash", 0x2b },
	{ "semicolon", 0x27 }, { "apostrophe", 0x28 }, { "comma", 0x33 }, { "dot", 0x34 },
	{ "slash", 0x35 }, { "bracketleft", 0x1a }, { "bracketright", 0x1b },
	{ NULL, 0 }
};

// the US layout: the scancode of each character, and whether it needs shift
static const char *const rows[] = {
	"\x02" "1!", "\x03" "2@", "\x04" "3#", "\x05" "4$", "\x06" "5%", "\x07" "6^",
	"\x08" "7&", "\x09" "8*", "\x0a" "9(", "\x0b" "0)", "\x0c" "-_", "\x0d" "=+",
	"\x10" "qQ", "\x11" "wW", "\x12" "eE", "\x13" "rR", "\x14" "tT", "\x15" "yY",
	"\x16" "uU", "\x17" "iI", "\x18" "oO", "\x19" "pP", "\x1a" "[{", "\x1b" "]}",
	"\x1e" "aA", "\x1f" "sS", "\x20" "dD", "\x21" "fF", "\x22" "gG", "\x23" "hH",
	"\x24" "jJ", "\x25" "kK", "\x26" "lL", "\x27" ";:", "\x28" "'\"", "\x29" "`~",
	"\x2b" "\\|", "\x2c" "zZ", "\x2d" "xX", "\x2e" "cC", "\x2f" "vV", "\x30" "bB",
	"\x31" "nN", "\x32" "mM", "\x33" ",<", "\x34" ".>", "\x35" "/?",
	NULL
};

static bool char_code(unsigned char ch, uint8_t *code, bool *shift)
{
	for (int i = 0; rows[i]; i++) {
		if ((unsigned char)rows[i][1] == ch || (unsigned char)rows[i][2] == ch) {
			*code = (uint8_t)rows[i][0];
			*shift = (unsigned char)rows[i][2] == ch;
			return true;
		}
	}
	return false;
}

bool keys_from_char(key_chord *k, unsigned char ch)
{
	uint8_t code = 0;
	bool shift = false;
	memset(k, 0, sizeof(*k));
	if (ch == '\n' || ch == '\r') {
		k->code[k->n++] = 0x1c;
	} else if (ch == '\t') {
		k->code[k->n++] = 0x0f;
	} else if (ch == ' ') {
		k->code[k->n++] = 0x39;
	} else if (ch == 0x1b) {
		k->code[k->n++] = 0x01;
	} else if (ch == 0x08 || ch == 0x7f) {
		k->code[k->n++] = ch == 0x08 ? 0x0e : 0x53;
	} else if (ch >= 1 && ch <= 26) {
		// a control character: ctrl and the letter
		char_code('a' + ch - 1, &code, &shift);
		k->code[k->n++] = SC_CTRL;
		k->code[k->n++] = code;
	} else if (char_code(ch, &code, &shift)) {
		if (shift)
			k->code[k->n++] = SC_LSHIFT;
		k->code[k->n++] = code;
	} else {
		return false;
	}
	return true;
}

static bool one_key(const char *s, uint8_t *code, bool *shift)
{
	*shift = false;
	for (int i = 0; names[i].name; i++) {
		if (strcasecmp(names[i].name, s) == 0) {
			*code = names[i].code;
			return true;
		}
	}
	if (strlen(s) == 1) {
		unsigned char ch = (unsigned char)tolower((unsigned char)s[0]);
		if (char_code(ch, code, shift)) {
			if (isupper((unsigned char)s[0]))
				*shift = true;
			return true;
		}
	}
	return false;
}

bool keys_parse_combo(key_chord *k, const char *spec)
{
	memset(k, 0, sizeof(*k));
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", spec);
	// split at + or - between parts, but "-" alone, "kp--" and "kp-+" are keys
	char *parts[6];
	int np = 0;
	char *p = buf;
	while (*p && np < 6) {
		char *start = p;
		if (strncasecmp(p, "kp-", 3) == 0)
			p += 3;
		p++;	// a part has at least one character ('+' or '-' too)
		while (*p && *p != '+' && *p != '-')
			p++;
		parts[np++] = start;
		if (*p) {
			*p++ = 0;
			if (!*p)
				return false;
		}
	}
	if (*p || np == 0)
		return false;
	bool any_shift = false;
	for (int i = 0; i < np; i++) {
		uint8_t code;
		bool shift;
		if (!one_key(parts[i], &code, &shift))
			return false;
		if (shift && !any_shift) {
			k->code[k->n++] = SC_LSHIFT;
			any_shift = true;
		}
		if (code == SC_LSHIFT) {
			if (any_shift)
				continue;
			any_shift = true;
		}
		if (k->n >= 6)
			return false;
		k->code[k->n++] = code;
	}
	return k->n > 0;
}

const char *keys_name_list(void)
{
	return "esc, backspace, tab, return, space, ctrl, shift, rshift, alt, capslock,"
		" f1-f10, up, down, left, right, home (clr/home), insert, delete,"
		" undo, help, iso, kp-0..kp-9, kp-(, kp-), kp-/, kp-*, kp--, kp-+, kp-., kp-enter,"
		" and single characters (US layout): a, A, 1, !, ...";
}

/* ---- the queue ---- */

#define KQ 8192
static key_chord kq[KQ];
static int kq_head, kq_count;
static uint64_t kq_pushed, kq_done;
// the chord in progress: idle, down, held, then the gap after it
static int k_phase, k_wait;

uint64_t keys_push(const key_chord *k)
{
	uint64_t s = 0;
	pthread_mutex_lock(&mcp_mu);
	if (kq_count < KQ) {
		kq[(kq_head + kq_count) % KQ] = *k;
		kq_count++;
		s = ++kq_pushed;
	}
	pthread_mutex_unlock(&mcp_mu);
	return s;
}

uint64_t keys_done(void)
{
	pthread_mutex_lock(&mcp_mu);
	uint64_t s = kq_done;
	pthread_mutex_unlock(&mcp_mu);
	return s;
}

int keys_pending(void)
{
	pthread_mutex_lock(&mcp_mu);
	int n = kq_count;
	pthread_mutex_unlock(&mcp_mu);
	return n;
}

enum { K_IDLE, K_DOWN, K_HOLD, K_UP, K_GAP };

void keys_clear(void)
{
	pthread_mutex_lock(&mcp_mu);
	// a chord whose keys are down stays, so they are let go
	int keep = kq_count && k_phase == K_HOLD ? 1 : 0;
	kq_done += kq_count - keep;
	kq_count = keep;
	if (!keep) {
		k_phase = K_IDLE;
		k_wait = 0;
	}
	pthread_mutex_unlock(&mcp_mu);
}

void keys_tick(void)
{
	uint8_t send[6];
	int ns = 0;
	pthread_mutex_lock(&mcp_mu);
	if (k_wait > 0) {
		k_wait--;
		pthread_mutex_unlock(&mcp_mu);
		return;
	}
	if (!kq_count) {
		k_phase = K_IDLE;
		pthread_mutex_unlock(&mcp_mu);
		return;
	}
	key_chord *k = &kq[kq_head];
	switch (k_phase) {
	case K_IDLE:
		k_phase = K_DOWN;
		/* fall through */
	case K_DOWN:
		// all keys of the chord at once: the modifiers come first
		for (int i = 0; i < k->n; i++)
			send[ns++] = k->code[i];
		k_phase = K_HOLD;
		k_wait = k->hold > 1 ? k->hold - 1 : 0;
		break;
	case K_HOLD:
	case K_UP:
		for (int i = k->n - 1; i >= 0; i--)
			send[ns++] = k->code[i] | 0x80;
		k_phase = K_GAP;
		k_wait = k->gap > 1 ? k->gap - 1 : 0;
		break;
	case K_GAP:
		kq_head = (kq_head + 1) % KQ;
		kq_count--;
		kq_done++;
		k_phase = K_IDLE;
		break;
	}
	pthread_mutex_unlock(&mcp_mu);
	for (int i = 0; i < ns; i++)
		mach_key(send[i]);
}
