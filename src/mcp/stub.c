/*
 * stub.c - the MCP hooks for the libretro core, which has no MCP server.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "config.h"
#include "hatari_mcp.h"

volatile int hatari_mcp_dbg_active;

bool hatari_mcp_init(void) { return false; }
void hatari_mcp_done(void) { }
bool hatari_mcp_on(void) { return false; }
void hatari_mcp_vbl(void) { }
void hatari_mcp_poll(void) { }
void hatari_mcp_serial_tx(int port, uint8_t b) { (void)port; (void)b; }
int hatari_mcp_serial_pending(int port) { (void)port; return 0; }
uint8_t hatari_mcp_serial_rx(void) { return 0; }
void hatari_mcp_dbg_check(uint32_t pc) { (void)pc; }
int hatari_mcp_bridge_main(int argc, char **argv) { (void)argc; (void)argv; return 1; }
