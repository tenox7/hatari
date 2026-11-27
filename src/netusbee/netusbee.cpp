/*
  Hatari - netusbee.cpp

  This file is distributed under the GNU General Public License, version 3
  or at your option any later version. Read the file gpl-3.0.txt for details.

  The EtherNEC and the NetUSBee on the cartridge port: Hatari's side of
  NeoST's devices (io/Ne2000, io/Isp1160, io/UsbDevice, net/NatBackend and
  nat/, from https://github.com/habib256/neost, (c) 2026 VERHILLE Arnaud).
  See netusbee.h.
*/
#include <cstdio>
#include <cstring>
#include <string>

#include "io/Ne2000.hpp"
#include "io/Isp1160.hpp"
#include "io/UsbDevice.hpp"
#include "net/NatBackend.hpp"
#include "net/NetBackend.hpp"
#include "core/Pacing.hpp"
#include "core/StateArchive.hpp"

extern "C" {
#include "main.h"
#include "configuration.h"
#include "log.h"
#include "cycles.h"
#include "clocks_timings.h"
#include "netusbee.h"
}

namespace {

Ne2000 ne;
Isp1160 isp;
NatBackend nat;
NetBackendNull noNet;

int cart = CNF_NET_CART_NONE;		// the card now
std::string natKey;			// the NAT now ("" when closed)
std::string usbSpec[CNF_NET_USB_PORTS];	// what is on the USB ports now
uint8_t keyToUsb[128];			// keys down, and where they went (2: USB)
bool lastLeft, lastRight;

// HID usages (HID Usage Tables 1.12, chapter 10) of the ST scancodes; 0:
// none. From NeoST (Machine::keyEvent).
const uint8_t hidUsage[128] = {
	0,0x29,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x2D,0x2E,0x2A,0x2B,
	0x14,0x1A,0x08,0x15,0x17,0x1C,0x18,0x0C,0x12,0x13,0x2F,0x30,0x28,0xE0,0x04,0x16,
	0x07,0x09,0x0A,0x0B,0x0D,0x0E,0x0F,0x33,0x34,0x35,0xE1,0x31,0x1D,0x1B,0x06,0x19,
	0x05,0x11,0x10,0x36,0x37,0x38,0xE5,0,0xE2,0x2C,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,
	0x3F,0x40,0x41,0x42,0x43,0,0,0x4A,0x52,0,0x56,0x50,0,0x4F,0x57,0,
	0x51,0,0x49,0x4C,0,0,0,0,0,0,0,0,0,0,0,0,
	0x7A,0x75,0,0x54,0x55,0,0,0x5F,0x60,0x61,0x5C,0x5D,0x5E,0x59,0x5A,0x5B,
	0x62,0x63,0x58,0,0,0,0,0,0,0,0,0,0,0,0,0
};

bool usbHas(UsbDevice::Kind kind)
{
	return isp.enabled() && (isp.device(0).kind() == kind || isp.device(1).kind() == kind);
}

int64_t usbClock()
{
	return (int64_t)Cycles_GetClockCounterImmediate();
}

void applyNat(void)
{
	const CNF_NET *n = &ConfigureParams.Net;
	std::string key = n->bNat ? std::string("nat ") + n->szNatForwards : std::string();
	if (key == natKey)
		return;
	nat.close();
	natKey.clear();
	if (n->bNat) {
		NatBackend::Options o;
		o.forwards = n->szNatForwards;
		if (!NatBackend::available())
			Log_AlertDlg(LOG_ERROR, "This Hatari has no host networking: the network card is not connected.");
		else if (!nat.open(o))
			Log_AlertDlg(LOG_ERROR, "Network card: %s", nat.lastError().c_str());
		else {
			natKey = key;
			Log_Printf(LOG_INFO, "Network card: NAT, the guest is %s, gateway %s, DNS %s\n",
				   NatBackend::guestIp(), NatBackend::gatewayIp(), NatBackend::nameserver());
		}
	}
	ne.setBackend(nat.isOpen() ? static_cast<NetBackend*>(&nat) : &noNet);
}

void applyUsb(bool force)
{
	for (int port = 0; port < CNF_NET_USB_PORTS; port++) {
		std::string spec = ConfigureParams.Net.szUsb[port];
		if (spec.empty())
			spec = "none";
		if (!force && spec == usbSpec[port])
			continue;
		UsbDevice::Kind kind;
		std::string image;
		bool ro = false;
		if (!UsbDevice::parseSpec(spec, kind, image, ro)) {
			Log_AlertDlg(LOG_ERROR, "NetUSBee USB port %d: '%s' is not none, keyboard, mouse,"
				     " disk:FILE or disk-ro:FILE", port + 1, spec.c_str());
			kind = UsbDevice::Kind::None;
			image.clear();
		}
		if (!isp.attach(port, kind, image, ro))
			Log_AlertDlg(LOG_ERROR, "NetUSBee USB port %d: cannot use '%s'", port + 1, spec.c_str());
		usbSpec[port] = spec;
	}
}

}  // namespace

int64_t neost::pacing::cpuHzInt()
{
	return MachineClocks.CPU_Freq_Emul ? (int64_t)MachineClocks.CPU_Freq_Emul : 8021247;
}

extern "C" {

bool NetUSBee_Configured(void)
{
	return ConfigureParams.Net.nCartDevice == CNF_NET_CART_ETHERNEC ||
	       ConfigureParams.Net.nCartDevice == CNF_NET_CART_NETUSBEE;
}

bool NetUSBee_OnCartPort(void)
{
	return cart != CNF_NET_CART_NONE;
}

void NetUSBee_Apply(void)
{
	int want = NetUSBee_Configured() ? ConfigureParams.Net.nCartDevice : CNF_NET_CART_NONE;
	bool changed = want != cart;

	if (want == CNF_NET_CART_NONE) {
		if (changed) {
			ne.setEnabled(false);
			isp.setEnabled(false);
			nat.close();
			natKey.clear();
			for (auto &s : usbSpec)
				s.clear();
			cart = want;
		}
		return;
	}
	// both cards have the NE2000
	ne.setEnabled(true);
	applyNat();
	if (changed)
		ne.powerCycle();
	if (want == CNF_NET_CART_NETUSBEE) {
		isp.setClock(usbClock);
		if (!isp.enabled() || changed) {
			isp.setEnabled(true);
			isp.reset();
		}
		applyUsb(changed);
	} else if (isp.enabled()) {
		isp.setEnabled(false);
		for (auto &s : usbSpec)
			s.clear();
	}
	cart = want;
}

void NetUSBee_UnInit(void)
{
	ne.setEnabled(false);
	isp.setEnabled(false);
	nat.close();
	natKey.clear();
	cart = CNF_NET_CART_NONE;
}

void NetUSBee_Reset(bool bCold)
{
	NetUSBee_Apply();
	if (!bCold)
		return;
	if (ne.enabled())
		ne.powerCycle();
	if (isp.enabled())
		isp.reset();
	std::memset(keyToUsb, 0, sizeof(keyToUsb));
}

uint8_t NetUSBee_ReadByte(uint32_t addr, bool first)
{
	// as NeoST's bus: the ISP1160 first (its latch reads also read the
	// NE2000's CR), then the NE2000, which drives D8-D15 when it answers
	bool ispHit = false;
	uint8_t ispVal = 0xff, v;
	if (isp.enabled())
		ispHit = isp.cartRead(addr, ispVal, first);
	if (ne.enabled() && ne.cartRead(addr, v, first))
		return v;
	if (ispHit)
		return ispVal;
	return 0xff;		// the empty cartridge port
}

void NetUSBee_Vbl(void)
{
	if (ne.enabled())
		ne.poll();
	if (isp.enabled())
		isp.sync();
}

bool NetUSBee_Key(uint8_t scancode, bool down)
{
	unsigned scan = scancode & 0x7f;
	bool usb = usbHas(UsbDevice::Kind::Keyboard);
	// a key goes up where it went down
	if (down)
		keyToUsb[scan] = usb ? 2 : 1;
	else {
		if (keyToUsb[scan])
			usb = keyToUsb[scan] == 2;
		keyToUsb[scan] = 0;
	}
	if (!usb)
		return false;
	if (hidUsage[scan])
		isp.keyEvent(hidUsage[scan], down);
	return true;
}

bool NetUSBee_UsbMouse(void)
{
	return usbHas(UsbDevice::Kind::Mouse);
}

void NetUSBee_Mouse(int dx, int dy, bool left, bool right)
{
	if (!dx && !dy && left == lastLeft && right == lastRight)
		return;
	lastLeft = left;
	lastRight = right;
	isp.mouseEvent(dx, dy, left, right);
}

void NetUSBee_MemorySnapShot_Capture(bool bSave)
{
	// the card's registers and buffers, and the USB devices' state; not
	// the network connections
	int32_t kind = cart;
	MemorySnapShot_Store(&kind, sizeof(kind));
	if (!bSave)
		NetUSBee_Apply();	// the card of the restored configuration
	if (kind == CNF_NET_CART_NONE)
		return;
	StateArchive ar(!bSave);
	ne.serialize(ar);
	if (kind == CNF_NET_CART_NETUSBEE)
		isp.serialize(ar);
	if (!ar.ok())
		Log_AlertDlg(LOG_ERROR, "The network card's state in the snapshot is bad (%s).",
			     ar.failed() ? ar.failed() : "?");
	else if (!bSave && kind != cart)
		Log_Printf(LOG_WARN, "The snapshot has another network card than the configuration.\n");
	if (!bSave)
		isp.setClock(usbClock);
}

void NetUSBee_Info(char *buf, size_t len)
{
	if (!len)
		return;
	buf[0] = 0;
	if (cart == CNF_NET_CART_NONE)
		return;
	std::string s = cart == CNF_NET_CART_NETUSBEE ? "NetUSBee (NE2000 + USB)" : "EtherNEC (NE2000)";
	s += " on the cartridge port; ";
	if (nat.isOpen()) {
		s += "NAT: the guest is ";
		s += NatBackend::guestIp();
		s += ", gateway ";
		s += NatBackend::gatewayIp();
		s += ", DNS ";
		s += NatBackend::nameserver();
		if (ConfigureParams.Net.szNatForwards[0]) {
			s += ", forwards ";
			s += ConfigureParams.Net.szNatForwards;
		}
	} else {
		s += "not connected to a network";
	}
	if (cart == CNF_NET_CART_NETUSBEE) {
		for (int port = 0; port < CNF_NET_USB_PORTS; port++) {
			s += port ? ", USB 2: " : "; USB 1: ";
			s += usbSpec[port].empty() ? "none" : usbSpec[port];
		}
	}
	std::snprintf(buf, len, "%s", s.c_str());
}

}  // extern "C"
