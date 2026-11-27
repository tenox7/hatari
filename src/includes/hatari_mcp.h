/*
  Hatari - hatari_mcp.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The MCP server: what the rest of Hatari calls.

  Every Hatari built with ENABLE_MCP serves MCP (the Model Context
  Protocol) on a private Unix socket, /tmp/hatari-<uid>/<pid>.sock, and
  describes itself next to it in <pid>.json. "hatari --mcp-bridge" is the
  MCP server an agent starts: it finds the running VMs there and passes
  each tool call to one. See doc/mcp.txt.
*/

#ifndef HATARI_MCP_H
#define HATARI_MCP_H

#include <stdint.h>
#include <stdbool.h>

/* Start the server (after the emulation is set up). False if it is off. */
bool hatari_mcp_init(void);
/* Remove the socket and the registry file. */
void hatari_mcp_done(void);
/* Whether the server runs. */
bool hatari_mcp_on(void);

/* The VBL, on the emulator's main thread: the picture, input, calls from
 * the tools, reset and quit. */
void hatari_mcp_vbl(void);
/* While Hatari is paused (its main loop waits for events): calls from the
 * tools, reset and quit. */
void hatari_mcp_poll(void);

/* The serial ports the tools reach: the MFP's USART (Modem 1) and SCC
 * channel B (Modem 2). */
#define HATARI_MCP_SERIAL_MFP	1
#define HATARI_MCP_SERIAL_SCC	2
/* The guest sent a byte on a serial port. */
void hatari_mcp_serial_tx(int port, uint8_t b);
/* How many bytes the tools have queued for the guest on this port: the
 * input goes to the port the guest last wrote to (to both before it wrote
 * to either). */
int hatari_mcp_serial_pending(int port);
/* Take one queued byte (call only when hatari_mcp_serial_pending() > 0). */
uint8_t hatari_mcp_serial_rx(void);

/* The debugger's hook (DebugCpu_Check, before each instruction): nonzero
 * stops the CPU before the instruction at pc, until a tool lets it go on. */
extern volatile int hatari_mcp_dbg_active;
void hatari_mcp_dbg_check(uint32_t pc);

/* "hatari --mcp-bridge": the MCP server on stdin and stdout. */
int hatari_mcp_bridge_main(int argc, char **argv);

#endif /* HATARI_MCP_H */
