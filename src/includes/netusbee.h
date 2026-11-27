/*
  Hatari - netusbee.h

  This file is distributed under the GNU General Public License, version 3
  or at your option any later version. Read the file gpl-3.0.txt for details.

  The EtherNEC and the NetUSBee on the cartridge port: what the rest of
  Hatari calls (src/netusbee/netusbee.cpp, around NeoST's devices).

  - EtherNEC: an NE2000 (RTL8019AS) network card. The cartridge port can
    only be read, so the card's register accesses are reads in
    $FA0000-$FBFFFF, the data in the address.
  - NetUSBee: the same NE2000, and an ISP1160 USB host with two ports: a
    keyboard, a mouse, or a disk image on each.
  - The card reaches the host's network through a built-in NAT (the guest
    is 10.0.2.15, the gateway 10.0.2.2, DNS 10.0.2.3).
*/

#ifndef HATARI_NETUSBEE_H
#define HATARI_NETUSBEE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the card up as the configuration says: at start, and at each reset
 * (only what changed changes). */
void NetUSBee_Apply(void);
void NetUSBee_UnInit(void);
/* Whether a card holds the cartridge port now. */
bool NetUSBee_OnCartPort(void);
/* Whether the configuration puts a card on the cartridge port. */
bool NetUSBee_Configured(void);
/* A reset: a cold one powers the card again (the cartridge port has no
 * reset line). */
void NetUSBee_Reset(bool bCold);
/* A CPU read in $FA0000-$FBFFFF. first: the first byte of the bus cycle
 * (a byte access, or the even byte of a word). */
uint8_t NetUSBee_ReadByte(uint32_t addr, bool first);
/* Each VBL: frames from the network into the card, USB frames. */
void NetUSBee_Vbl(void);
/* A key (ST scancode) or the mouse: true when a USB keyboard or mouse on
 * the NetUSBee takes it, instead of the IKBD. */
bool NetUSBee_Key(uint8_t scancode, bool down);
bool NetUSBee_UsbMouse(void);
void NetUSBee_Mouse(int dx, int dy, bool left, bool right);
void NetUSBee_MemorySnapShot_Capture(bool bSave);
/* One line about the card, for vm-info and the log ("" without one). */
void NetUSBee_Info(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HATARI_NETUSBEE_H */
