/*
 * machine.c - the MCP tools' way into Hatari: the screen, keys, mouse,
 * reset, pause and quit, the floppies, memory and the CPU's registers.
 *
 * Everything here runs on the emulator's main thread: from the VBL, from
 * Hatari's pause loop, from the debugger stop, or through mcp_main_call.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "main.h"
#include "configuration.h"
#include "audio.h"
#include "conv_gen.h"
#include "conv_st.h"
#include "debugcpu.h"
#include "floppy.h"
#include "gui_event.h"
#include "ikbd.h"
#include "m68000.h"
#include "options_cpu.h"
#include "reset.h"
#include "screen.h"
#include "sound.h"
#include "stMemory.h"
#include "tos.h"
#include "vdi.h"
#include "video.h"
#include "version.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "hatari_mcp.h"
#include "mcp.h"
#include "machine.h"

/* ---- the screen ---- */

static uint32_t *fbuf;
static size_t fbuf_n;
static int frame_w, frame_h;	// the last picture's size, in guest pixels

static bool fbuf_size(int w, int h)
{
	size_t n = (size_t)w * h;
	if (n > fbuf_n) {
		uint32_t *p = realloc(fbuf, n * sizeof(*p));
		if (!p)
			return false;
		fbuf = p;
		fbuf_n = n;
	}
	return true;
}

static inline uint16_t st_word(uint32_t a)
{
	return (uint16_t)(STRam[a] << 8 | STRam[a + 1]);
}

/*
 * ST, STE and Mega ST shifter modes: Hatari's converted screen, without the
 * borders and the statusbar, and back to one pixel per guest pixel (Hatari
 * doubles low resolution, and the lines of medium resolution).
 */
static bool frame_st(int *pw, int *ph)
{
	uint32_t *px;
	int sw, sh, pitch, w, h, sx, sy, x0, y0;

	Screen_GetDimension(&px, &sw, &sh, &pitch);
	if (!px || sw <= 0 || sh <= 0)
		return false;
	if (bUseHighRes) {
		w = 640;
		h = 400;
		sx = sy = 1;
		x0 = ((int)Screen_GetGenConvWidth() - w) / 2;
		y0 = ((int)Screen_GetGenConvHeight() - h) / 2;
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
	} else {
		h = 200;
		sy = nScreenZoomY > 0 ? nScreenZoomY : 1;
		if (STRes == ST_MEDIUM_RES) {
			w = 640;
			sx = 1;
			x0 = nBorderPixelsLeft * 2;	// the borders count low resolution pixels
		} else {
			w = 320;
			sx = nScreenZoomX > 0 ? nScreenZoomX : 1;
			x0 = nBorderPixelsLeft * sx;
		}
		y0 = (OVERSCAN_TOP - STScreenStartHorizLine) * sy;
		if (y0 < 0)
			y0 = 0;
	}
	if (x0 + (w - 1) * sx >= sw || y0 + (h - 1) * sy >= sh)
		return false;
	if (!fbuf_size(w, h))
		return false;

	uint32_t rm, gm, bm;
	int rs, gs, bs;
	Screen_GetPixelFormat(&rm, &gm, &bm, &rs, &gs, &bs);
	bool rgb = rm == 0xff0000 && gm == 0xff00 && bm == 0xff;
	int stride = pitch / 4;
	for (int y = 0; y < h; y++) {
		const uint32_t *row = px + (size_t)(y0 + y * sy) * stride + x0;
		uint32_t *out = fbuf + (size_t)y * w;
		for (int x = 0; x < w; x++) {
			uint32_t v = row[x * sx];
			if (rgb)
				out[x] = v & 0xffffff;
			else
				out[x] = (((v & rm) >> rs) & 0xff) << 16 | (((v & gm) >> gs) & 0xff) << 8 |
					(((v & bm) >> bs) & 0xff);
		}
	}
	*pw = w;
	*ph = h;
	return true;
}

/*
 * TT, Falcon and Hatari's extended VDI modes: the video memory that
 * Hatari converted last, in the guest's resolution, with its palette.
 */
static bool frame_genconv(int *pw, int *ph)
{
	uint32_t vaddr;
	int hscroll;
	int w = ConvertW, h = ConvertH, bpp = ConvertBPP, next = ConvertNextLine;

	ConvGen_GetFrameInfo(&vaddr, &hscroll);
	if (w < 16 || h < 1 || w > SCR_MAX || h > SCR_MAX || next <= 0)
		return false;
	if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16)
		return false;
	if (!fbuf_size(w, h))
		return false;

	uint32_t pal[256];
	if (bpp <= 8) {
		for (int i = 0; i < (1 << bpp); i++) {
			uint8_t r, g, b;
			ConvGen_GetPaletteColor(i, &r, &g, &b);
			pal[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
		}
	}
	// bytes a line reads: whole 16 pixel groups, and one more when scrolled
	uint32_t need = bpp == 16 ? (uint32_t)w * 2 : (uint32_t)((w + 15) / 16 + (hscroll ? 1 : 0)) * bpp * 2;
	for (int y = 0; y < h; y++) {
		uint32_t line = vaddr + (uint32_t)y * next;
		uint32_t *out = fbuf + (size_t)y * w;
		if (line + need > STRamEnd) {
			memset(out, 0, (size_t)w * 4);
			continue;
		}
		if (bpp == 16) {
			for (int x = 0; x < w; x++) {
				uint16_t s = st_word(line + x * 2);
				uint8_t r = ((s >> 8) & 0xf8) | (s >> 13);
				uint8_t g = ((s >> 3) & 0xfc) | ((s >> 9) & 0x3);
				uint8_t b = ((s << 3) & 0xf8) | ((s >> 2) & 0x07);
				out[x] = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
			}
			continue;
		}
		for (int x = 0; x < w; x++) {
			int p = x + hscroll;
			uint32_t a = line + (uint32_t)(p >> 4) * bpp * 2;
			int bit = 15 - (p & 15), idx = 0;
			for (int k = 0; k < bpp; k++)
				idx |= ((st_word(a + k * 2) >> bit) & 1) << k;
			out[x] = pal[idx];
		}
	}
	*pw = w;
	*ph = h;
	return true;
}

void mach_frame(void)
{
	int w = 0, h = 0;
	bool ok;

	if (!mcp_frame_wanted() || ConfigureParams.Screen.DisableVideo)
		return;
	if (Config_IsMachineFalcon() || Config_IsMachineTT() || bUseVDIRes)
		ok = frame_genconv(&w, &h);
	else
		ok = frame_st(&w, &h);
	if (!ok)
		return;
	frame_w = w;
	frame_h = h;
	mcp_frame_submit(fbuf, w, h);
}

int mach_tick_hz(void)
{
	return nScreenRefreshRate > 0 ? nScreenRefreshRate : 50;
}

/* ---- keys and the mouse ---- */

void mach_key(uint8_t code)
{
	IKBD_PressSTKey(code & 0x7f, !(code & 0x80));
}

bool mach_mouse_abs(int x, int y, int buttons)
{
	(void)x;
	(void)y;
	(void)buttons;
	return false;
}

void mach_mouse_rel(int dx, int dy, int buttons)
{
	KeyboardProcessor.Mouse.dx += dx;
	KeyboardProcessor.Mouse.dy += dy;
	if (buttons & MACH_BUTTON_LEFT)
		Keyboard.bLButtonDown |= BUTTON_MCP;
	else
		Keyboard.bLButtonDown &= ~BUTTON_MCP;
	if (buttons & MACH_BUTTON_RIGHT)
		Keyboard.bRButtonDown |= BUTTON_MCP;
	else
		Keyboard.bRButtonDown &= ~BUTTON_MCP;
}

bool mach_mouse_absolute(void)
{
	return false;
}

/*
 * Where the guest's pointer is: GCURX and GCURY in the Line-A variables of
 * TOS, which the VDI's mouse code keeps up to date. Nothing tells where
 * they are, so look for them: the block where the screen's size, planes
 * and bytes per line are those of the picture.
 */
#define LA_GCURX	(-0x25a)
#define LA_GCURY	(-0x258)
#define LA_V_REZ_HZ	(-0x00c)
#define LA_V_REZ_VT	(-0x004)
#define LA_BYTES_LIN	(-0x002)
#define LA_PLANES	0x000
#define LA_WIDTH	0x002

static uint32_t la_base;
static unsigned la_wait;	// VBLs until the next search

static bool la_ok(uint32_t a)
{
	if (a < 0x400 + 0x25a || a + LA_WIDTH + 2 > STRamEnd)
		return false;
	unsigned planes = st_word(a + LA_PLANES);
	if (planes != 1 && planes != 2 && planes != 4 && planes != 8 && planes != 16)
		return false;
	unsigned hz = st_word(a + LA_V_REZ_HZ), vt = st_word(a + LA_V_REZ_VT);
	if (hz != (unsigned)frame_w || vt != (unsigned)frame_h)
		return false;
	unsigned bl = st_word(a + LA_BYTES_LIN);
	if (bl != st_word(a + LA_WIDTH) || bl != hz * planes / 8)
		return false;
	int16_t x = (int16_t)st_word(a + LA_GCURX), y = (int16_t)st_word(a + LA_GCURY);
	return x >= 0 && (unsigned)x < hz && y >= 0 && (unsigned)y < vt;
}

static void la_find(void)
{
	uint32_t end = STRamEnd < 0x100000 ? STRamEnd : 0x100000;
	la_base = 0;
	for (uint32_t a = 0x400 + 0x25a; a + 4 <= end; a += 2) {
		if (la_ok(a)) {
			la_base = a;
			return;
		}
	}
}

bool mach_pointer(int *x, int *y)
{
	if (!frame_w || !bTosImageLoaded)
		return false;
	if (la_base && !la_ok(la_base))
		la_base = 0;
	if (!la_base) {
		if (la_wait) {
			la_wait--;
			return false;
		}
		la_wait = 50;
		la_find();
		if (!la_base)
			return false;
	}
	*x = (int16_t)st_word(la_base + LA_GCURX);
	*y = (int16_t)st_word(la_base + LA_GCURY);
	return true;
}

void mach_wheel(int dir)
{
	uint8_t code = dir < 0 ? 0x48 : 0x50;	// cursor up or down, as for the host's wheel
	IKBD_PressSTKey(code, true);
	IKBD_PressSTKey(code, false);
}

/* ---- power ---- */

void mach_reset(bool cold)
{
	if (cold)
		Reset_Cold();
	else
		Reset_Warm();
	la_base = 0;
}

void mach_quit(void)
{
	// no "do you really want to quit?" dialog: the agent asked
	if (ConfigureParams.Memory.bAutoSave)
		Main_RequestQuit(0);
	else
		Main_SetQuitValue(0);
}

bool mach_service_events(void)
{
	GuiEvent_EventHandler();
	return bQuitProgram;
}

bool mach_paused(void)
{
	return !bEmulationActive;
}

void mach_pause(bool pause)
{
	if (pause)
		Main_PauseEmulation(true);
	else
		Main_UnPauseEmulation();
}

void mach_break(bool stopped)
{
	if (stopped) {
		Audio_EnableAudio(false);
	} else if (bEmulationActive) {
		Sound_BufferIndexNeedReset = true;
		Audio_EnableAudio(ConfigureParams.Sound.bEnableSound);
	}
}

/* ---- the floppies ---- */

const char *mach_floppy_path(int drive)
{
	if (drive < 0 || drive >= MAX_FLOPPYDRIVES || !EmulationDrives[drive].bDiskInserted)
		return NULL;
	return EmulationDrives[drive].sFileName;
}

bool mach_floppy_insert(int drive, const char *path)
{
	if (drive < 0 || drive >= MAX_FLOPPYDRIVES)
		return false;
	if (!Floppy_SetDiskFileName(drive, path, NULL))
		return false;
	if (Floppy_InsertDiskIntoDrive(drive) && EmulationDrives[drive].bDiskInserted)
		return true;
	Floppy_SetDiskFileNameNone(drive);
	return false;
}

void mach_floppy_eject(int drive)
{
	if (drive < 0 || drive >= MAX_FLOPPYDRIVES)
		return;
	Floppy_SetDiskFileNameNone(drive);
	Floppy_EjectDiskFromDrive(drive);
}

/* ---- memory ---- */

uint8_t *mach_mem(uint32_t addr, bool write)
{
	if (ConfigureParams.System.bAddressSpace24)
		addr &= 0x00ffffff;
	if (!STMemory_CheckAreaType(addr, 1, write ? ABFLAG_RAM : ABFLAG_RAM | ABFLAG_ROM))
		return NULL;
	return STMemory_STAddrToPointer(addr);
}

void mach_mem_written(uint32_t addr, int len)
{
	M68000_Flush_All_Caches(addr, len);
}

/* ---- registers ---- */

void mach_get_regs(mach_regs *r)
{
	MakeSR();
	for (int i = 0; i < 8; i++) {
		r->d[i] = m68k_dreg(regs, i);
		r->a[i] = m68k_areg(regs, i);
	}
	r->pc = M68000_GetPC();
	r->sr = regs.sr;
	uint32_t sp = m68k_areg(regs, 7);
	r->usp = regs.s ? regs.usp : sp;
	r->ssp = (regs.s && !regs.m) ? sp : regs.isp;
	r->msp = (regs.s && regs.m) ? sp : regs.msp;
	r->vbr = regs.vbr;
	r->sfc = regs.sfc;
	r->dfc = regs.dfc;
	r->cacr = regs.cacr;
	r->cpu = currprefs.cpu_model;
}

bool mach_set_reg(const char *name, uint32_t v)
{
	int cpu = currprefs.cpu_model;

	if ((name[0] == 'd' || name[0] == 'D' || name[0] == 'a' || name[0] == 'A') &&
	    name[1] >= '0' && name[1] <= '7' && !name[2]) {
		if (name[0] == 'd' || name[0] == 'D')
			m68k_dreg(regs, name[1] - '0') = v;
		else
			m68k_areg(regs, name[1] - '0') = v;
		return true;
	}
	if (!strcasecmp(name, "sp")) {
		m68k_areg(regs, 7) = v;
	} else if (!strcasecmp(name, "pc")) {
		M68000_SetPC(v);
	} else if (!strcasecmp(name, "sr")) {
		M68000_SetSR(v);
	} else if (!strcasecmp(name, "usp")) {
		if (regs.s)
			regs.usp = v;
		else
			m68k_areg(regs, 7) = v;
	} else if (!strcasecmp(name, "ssp") || !strcasecmp(name, "isp")) {
		if (regs.s && !regs.m)
			m68k_areg(regs, 7) = v;
		else
			regs.isp = v;
	} else if (!strcasecmp(name, "msp") && cpu >= 68020) {
		if (regs.s && regs.m)
			m68k_areg(regs, 7) = v;
		else
			regs.msp = v;
	} else if (!strcasecmp(name, "vbr") && cpu >= 68010) {
		regs.vbr = v;
	} else if (!strcasecmp(name, "sfc") && cpu >= 68010) {
		regs.sfc = v & 7;
	} else if (!strcasecmp(name, "dfc") && cpu >= 68010) {
		regs.dfc = v & 7;
	} else if (!strcasecmp(name, "cacr") && cpu >= 68020) {
		regs.cacr = v;
	} else {
		return false;
	}
	return true;
}

/* ---- the disassembler ---- */

bool mach_disasm_available(void)
{
	return true;
}

int mach_disasm(uint32_t addr, char *buf, size_t n)
{
	// only where there is memory: the disassembler reads it
	if (!mach_mem(addr, false) || !mach_mem(addr + 21, false))
		return 0;
	char line[256];
	uaecptr next = addr;
	line[0] = 0;
	m68k_disasm_2(line, sizeof(line), addr, NULL, 0, &next, 1, NULL, NULL, 0xffffffff, 1);
	if (next <= addr)
		return 0;
	/* the line is "ADDRESS hex words  INSTRUCTION": keep the instruction */
	const char *p = line;
	while (*p && *p != ' ')
		p++;		// the address
	while (*p == ' ')
		p++;
	for (;;) {	// the instruction's words, in hex
		const char *q = p;
		int digits = 0;
		while ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F')) {
			q++;
			digits++;
		}
		if (digits != 4 || *q != ' ')
			break;
		p = q;
		while (*p == ' ')
			p++;
	}
	snprintf(buf, n, "%s", p);
	size_t l = strlen(buf);
	while (l && (buf[l - 1] == '\n' || buf[l - 1] == ' '))
		buf[--l] = 0;
	return (int)(next - addr);
}

const char *mach_debug_unavailable(void)
{
	return NULL;	// Hatari's CPU cores all stop between instructions
}

void mach_debug_hook(void)
{
	DebugCpu_UpdateDebugging();
}

/* ---- vm-info ---- */

static const char *machine_name(void)
{
	switch (ConfigureParams.System.nMachineType) {
	case MACHINE_ST: return "ST";
	case MACHINE_MEGA_ST: return "Mega ST";
	case MACHINE_STE: return "STE";
	case MACHINE_MEGA_STE: return "Mega STE";
	case MACHINE_TT: return "TT";
	case MACHINE_FALCON: return "Falcon";
	}
	return "?";
}

void mach_serial_info(sbuf *out)
{
	bool scc = Config_IsMachineMegaSTE() || Config_IsMachineTT() || Config_IsMachineFalcon();
	const CNF_RS232 *s = &ConfigureParams.RS232;

	if (Config_IsMachineFalcon())
		sb_puts(out, "Serial: the tools reach the Falcon's serial port, SCC channel B (Modem 2:"
			" AUX, u:/dev/modem2 under MiNT), and the MFP's USART, which has no port on"
			" a real Falcon.\n");
	else if (scc)
		sb_puts(out, "Serial: the tools reach Modem 1, the MFP's USART (BIOS device 6 and AUX,"
			" u:/dev/modem1 under MiNT), and Modem 2, SCC channel B (BIOS device 7,"
			" u:/dev/modem2).\n");
	else
		sb_puts(out, "Serial: the tools reach the serial port, the MFP's USART (Modem 1: BIOS"
			" device 6 and AUX, u:/dev/modem1 under MiNT).\n");
	if (s->bEnableRS232 && (s->szInFileName[0] || s->szOutFileName[0]))
		sb_printf(out, "Modem 1 is also connected to %s%s%s: the tools see what the guest sends"
			" there, and their input comes first\n", s->szInFileName,
			s->szInFileName[0] && s->szOutFileName[0] ? " / " : "", s->szOutFileName);
	if (scc && s->EnableScc[CNF_SCC_CHANNELS_B])
		sb_printf(out, "Modem 2 is also connected to %s%s%s\n",
			s->SccInFileName[CNF_SCC_CHANNELS_B],
			s->SccInFileName[CNF_SCC_CHANNELS_B][0] && s->SccOutFileName[CNF_SCC_CHANNELS_B][0] ? " / " : "",
			s->SccOutFileName[CNF_SCC_CHANNELS_B]);
}

void mach_info(sbuf *out)
{
	const CNF_SYSTEM *s = &ConfigureParams.System;

	sb_printf(out, "Machine: %s, %s, %d MHz %d%s%s, ST-RAM %d KB, TT-RAM %d KB\n", PROG_NAME,
		machine_name(), s->nCpuFreq, currprefs.cpu_model,
		s->bMMU && currprefs.cpu_model >= 68030 ? " with MMU" : "",
		s->bCycleExactCpu ? " (cycle exact)" : "",
		ConfigureParams.Memory.STRamSize_KB, ConfigureParams.Memory.TTRamSize_KB);
	if (bIsEmuTOS) {
		if (EmuTosVersion)
			sb_printf(out, "ROM: EmuTOS %u.%u.%u", EmuTosVersion >> 24, (EmuTosVersion >> 16) & 0xff,
				(EmuTosVersion >> 8) & 0xff);
		else
			sb_puts(out, "ROM: EmuTOS");
	} else {
		sb_printf(out, "ROM: TOS %x.%02x", TosVersion >> 8, TosVersion & 0xff);
	}
	sb_printf(out, " (%s) at 0x%06x\n", ConfigureParams.Rom.szTosImageFileName, TosAddress);
	for (int i = 0; i < MAX_ACSI_DEVS; i++)
		if (ConfigureParams.Acsi[i].bUseDevice)
			sb_printf(out, "ACSI %d: %s\n", i, ConfigureParams.Acsi[i].sDeviceFile);
	for (int i = 0; i < MAX_SCSI_DEVS; i++)
		if (ConfigureParams.Scsi[i].bUseDevice)
			sb_printf(out, "SCSI %d: %s\n", i, ConfigureParams.Scsi[i].sDeviceFile);
	for (int i = 0; i < MAX_IDE_DEVS; i++)
		if (ConfigureParams.Ide[i].bUseDevice)
			sb_printf(out, "IDE %d: %s\n", i, ConfigureParams.Ide[i].sDeviceFile);
	if (ConfigureParams.HardDisk.bUseHardDiskDirectories)
		sb_printf(out, "GEMDOS drive: %s\n", ConfigureParams.HardDisk.szHardDiskDirectories[0]);
	if (bUseVDIRes)
		sb_printf(out, "Video: Hatari's extended VDI mode, %dx%d, %d planes\n", VDIWidth, VDIHeight, VDIPlanes);
	else if (Config_IsMachineFalcon() || Config_IsMachineTT())
		sb_printf(out, "Video: %dx%d, %d bits a pixel, %d Hz\n", ConvertW, ConvertH, ConvertBPP,
			mach_tick_hz());
	else
		sb_printf(out, "Video: %s resolution, %d Hz\n", bUseHighRes ? "ST high" :
			STRes == ST_MEDIUM_RES ? "ST medium" : "ST low", mach_tick_hz());
}
