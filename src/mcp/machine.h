/*
 * machine.h - what the MCP tools reach in Hatari (machine.c)
 *
 * These are the few things the tools do to the machine. Call them on the
 * emulator's main thread only: from the VBL (hatari_mcp_vbl), while it is
 * paused (hatari_mcp_poll) or stopped by the debugger, or through
 * mcp_main_call.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef AMCP_MACHINE_H
#define AMCP_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "json.h"

/* IKBD mouse button bits */
#define MACH_BUTTON_RIGHT	1
#define MACH_BUTTON_LEFT	2

/// An IKBD key event: the Atari scancode, | 0x80 to release.
void mach_key(uint8_t code);
/// Put the pointer at x, y with these buttons down. Hatari has no
/// absolute mouse driver: always false (the tools then use packets).
bool mach_mouse_abs(int x, int y, int buttons);
/// A relative IKBD mouse packet, with these buttons down (MACH_BUTTON_*).
void mach_mouse_rel(int dx, int dy, int buttons);
/// Where the guest's pointer is (its Line-A variables), when it says.
bool mach_pointer(int *x, int *y);
/// Turn the wheel one notch: up (-1) or down (1), as the cursor keys.
void mach_wheel(int dir);
/// Whether a driver takes the mouse as absolute positions (never).
bool mach_mouse_absolute(void);

void mach_reset(bool cold);
void mach_quit(void);
/// Keep the host window alive while the CPU is stopped; true when Hatari
/// is quitting.
bool mach_service_events(void);
/// Whether Hatari is paused (its own pause, or vm-power pause).
bool mach_paused(void);
/// Pause or resume Hatari's emulation.
void mach_pause(bool pause);
/// The debugger stopped the CPU (true) or lets it go on (false): sound off
/// and on.
void mach_break(bool stopped);

/// The floppy image in drive 0 (A) or 1 (B), or NULL when it is empty.
const char *mach_floppy_path(int drive);
bool mach_floppy_insert(int drive, const char *path);
void mach_floppy_eject(int drive);

/// The host address of a guest byte in RAM or ROM, or NULL (I/O space, or
/// nothing there). ROM can't be written.
uint8_t *mach_mem(uint32_t addr, bool write);
/// After a write to guest memory: the CPU caches forget it.
void mach_mem_written(uint32_t addr, int len);

typedef struct {
	uint32_t d[8], a[8];
	uint32_t pc, sr, usp, ssp, msp, vbr, sfc, dfc, cacr;
	int cpu;	// 68000 ... 68060
} mach_regs;

void mach_get_regs(mach_regs *r);
/// Set a register by name (d0-d7, a0-a7, sp, pc, sr, usp, ssp, msp, vbr,
/// sfc, dfc, cacr). False: no such register.
bool mach_set_reg(const char *name, uint32_t v);

/// Whether the 68k disassembler is there.
bool mach_disasm_available(void);
/// One instruction at addr as text; returns its length in bytes (0: none).
int mach_disasm(uint32_t addr, char *buf, size_t n);

/// Why debug-run and debug-step can't stop the CPU here, or NULL.
const char *mach_debug_unavailable(void);
/// The MCP debugger hook goes on or off (hatari_mcp_dbg_active changed).
void mach_debug_hook(void);

/// VBLs a second now (50, 60 or 71 Hz).
int mach_tick_hz(void);
/// The picture of the screen for the tools, at a VBL, when one is wanted.
void mach_frame(void);
/// The serial ports, for vm-info.
void mach_serial_info(sbuf *out);

/// The machine, for vm-info: CPU, RAM, TOS, disks, video mode.
void mach_info(sbuf *out);

/* the other way, server.c for machine.c */
/// Whether a picture is wanted at this VBL (all of them while the tools
/// are busy, fewer while idle).
bool mcp_frame_wanted(void);
/// A picture of the screen, 0x00RRGGBB pixels.
void mcp_frame_submit(const uint32_t *px, int w, int h);

#endif
