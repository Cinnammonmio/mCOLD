"""Finding a box on the USB bus, and finding it again after it restarts.

A box changes port every time it changes what it is running:

| What runs        | PID    | USB serial number                     |
|------------------|--------|---------------------------------------|
| ROM, or a firmware on the chip's USB-Serial-JTAG (bringup) | 0x1001 | the MAC, `28:84:85:27:9A:74` |
| ROM download mode on USB-OTG (older firmware) | 0x0009 | -- |
| mCOLD firmware (TinyUSB drive + console) | 0x4003 | the SN, or `MCOLD-9A74` before it has one |

So a box is followed by its MAC and its SN, never by "the newest port":
with six in a dock, the newest port is as likely to be another box's
(tools/usb_flash_all.py learned this first).
"""
from __future__ import annotations

import time
from dataclasses import dataclass

from serial.tools import list_ports


@dataclass
class Port:
    device: str
    vid: int | None
    pid: int | None
    serial: str
    location: str
    description: str

    def __str__(self) -> str:
        pid = f"{self.vid:04X}:{self.pid:04X}" if self.vid is not None else "----:----"
        return f"{self.device:<14} {pid}  {self.serial or '-':<20} {self.location or '-'}"


def scan() -> list[Port]:
    out = []
    for p in list_ports.comports():
        out.append(Port(p.device, p.vid, p.pid, p.serial_number or "", p.location or "",
                        p.description or ""))
    return sorted(out, key=lambda p: p.device)


def norm_mac(mac: str) -> str:
    h = "".join(c for c in mac.upper() if c in "0123456789ABCDEF")
    return ":".join(h[i:i + 2] for i in range(0, 12, 2)) if len(h) == 12 else mac.upper()


class UsbIds:
    """The product's USB identities, from its profile."""

    def __init__(self, usb: dict):
        self.vid = usb["vid"]
        self.rom_pids = set(usb.get("rom_pids", []))
        self.app_pids = set(usb.get("app_pids", []))
        self.usj_pid = usb.get("usj_pid")
        self.unset_serial = usb.get("app_serial_unset", "")
        # a USB-UART bridge: the port stays put whatever the chip runs
        self.bridge = bool(usb.get("bridge", False))

    def ours(self, p: Port) -> bool:
        return p.vid == self.vid and (p.pid in self.rom_pids or p.pid in self.app_pids)

    def is_app(self, p: Port) -> bool:
        return p.vid == self.vid and p.pid in self.app_pids

    def is_rom(self, p: Port) -> bool:
        return p.vid == self.vid and p.pid in self.rom_pids

    def is_usj(self, p: Port) -> bool:
        return p.vid == self.vid and p.pid == self.usj_pid

    def unset_name(self, mac: str) -> str:
        """The USB serial the firmware gives a box with no SN yet."""
        if not self.unset_serial or not mac:
            return ""
        h = norm_mac(mac).replace(":", "")
        return self.unset_serial.format(mac4=h[-4:], mac=h)

    def matches(self, p: Port, mac: str = "", sn: str = "", kind: str = "any") -> bool:
        """Is this port the box with this MAC / SN?"""
        if p.vid != self.vid:
            return False
        if kind in ("any", "rom") and p.pid in self.rom_pids:
            if p.pid == self.usj_pid:
                return bool(mac) and norm_mac(p.serial) == norm_mac(mac)
            # USB-OTG ROM port: no serial number to tell boxes apart
            return not p.serial
        if kind in ("any", "app") and p.pid in self.app_pids:
            if sn and p.serial == sn:
                return True
            return bool(mac) and p.serial == self.unset_name(mac)
        return False


def wait_for(pred, seconds: float, poll: float = 0.5) -> Port | None:
    end = time.monotonic() + seconds
    while True:
        for p in scan():
            if pred(p):
                return p
        if time.monotonic() >= end:
            return None
        time.sleep(poll)
