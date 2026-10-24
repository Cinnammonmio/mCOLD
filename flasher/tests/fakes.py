"""A simulated mCOLD box on a simulated USB bus, and image files to flash it with.

The box behaves the way the real one was measured to: it comes up on the
USB-Serial-JTAG (MAC as its serial) when blank, in the ROM or running
bringup; on the TinyUSB port (SN as its serial) when running the product
firmware; `flash` sends it to the ROM; `sn set` takes effect at reboot.
"""
from __future__ import annotations

import csv
import hashlib
import os
import struct

from mflash.esp import EspResult
from mflash.ports import Port

MAC = "28:84:85:27:9A:74"

BRINGUP = {
    "info": "  chip        ESP32-S3 rev 2, 2 cores\n  flash       16 MB\n  psram       8 MB \n",
    "scan": "  0x18  LIS2DW12 accelerometer     ack (write + read)\n"
            "  0x2D  unexpected: answers a read too, so something is really there\n"
            "\n  8 of 8 expected, 1 unexpected\n",
    "id": "  LIS2DW12  WHO_AM_I   0x44   correct\n  MAX17048  VERSION    0x0012\n"
          "  INA226    MANUF_ID   0x5449   'TI'\n  INA226    DIE_ID     0x2260   correct\n"
          "  HUSB238A  REG0x00    0x00   (no documented ID reg)\n"
          "  ST25DV    user 0x53 ack   system 0x57 ack\n"
          "  PCF8523   SECONDS    0x12   OS=0 running\n",
    "rail led on": "  led rail on\n",
    "rail led off": "  led rail off\n",
    "walk": "  index 0  LED4 side/charge\n  index 1  LED1 cargo\n",
    "buzz 200": "  buzzer 200 ms at 2.7 kHz\n",
    "chg on": "  CHG_CE_N LOW -- charging allowed\n",
    "power": "  charger     REG08 0x64  input: adapter  state: fast charging\n",
}


def nmea(body: str) -> str:
    ck = 0
    for c in body.encode():
        ck ^= c
    return f"${body}*{ck:02X}\r\n"


GNSS = ("  listening on GPIO18 at 115200 for 15 s\n" + nmea("GNRMC,,V,,,,,,,,,,N")
        + nmea("GNGGA,,,,,,0,00,25.5,,,,,,") + nmea("GPGSV,1,1,00") + "\n  312 bytes\n")

HEALTH = """
  device    state          ok    fail    last ok
  accel     ok             12       0         1s
  fuel      ok             12       0         1s
  current   ok             12       0         1s
  pd        unknown         0       0      never
  nfc       ok              3       0         4s
  rtc       ok             12       0         1s
  charger   ok             12       0         1s
  thermo    ok             12       0         1s
  gnss      ok              2       0        10s
  display   ok              1       0         9s
  leds      ok              2       0         5s
  buzzer    unknown         0       0      never
  sd        unknown         0       0      never

  temperature  {temp}
  clock        2026-10-24 10:00:00  source ntp
"""


class FakeBox:
    def __init__(self, mac: str = MAC, probe_reads_open: bool = True, version: str = "0.7.0",
                 elf16: str = "aabbccddeeff0011"):
        self.mac = mac
        self.state = "rom"            # rom | bringup | app
        self.flash = {}               # offset -> file name
        self.sn = ""
        self.pending_sn = ""
        self.probe = True
        self.probe_reads_open = probe_reads_open
        self.version, self.elf16 = version, elf16
        self.config = {}
        self.sent: list[str] = []

    # what is on the bus
    def port(self) -> Port:
        if self.state in ("rom", "bringup"):
            return Port("/dev/ttyACM0", 0x303A, 0x1001, self.mac, "1-1", "USB JTAG/serial debug unit")
        serial = self.sn or "MCOLD-" + self.mac.replace(":", "")[-4:]
        return Port("/dev/ttyACM1", 0x303A, 0x4003, serial, "1-1", "mCOLD console")

    def boot(self) -> None:
        app = self.flash.get(0x20000) or self.flash.get(0x10000)
        if not app:
            self.state = "rom"
        elif 0x10000 in self.flash:
            self.state = "bringup"
        else:
            self.state = "app"
            if self.pending_sn:
                self.sn, self.pending_sn = self.pending_sn, ""

    # its console
    def reply(self, line: str) -> str:
        self.sent.append(line)
        if self.state == "bringup":
            if line == "temp":
                # the profile reads it twice: probe in, then (after the operator
                # has pulled it) probe out
                self.probe, was = not self.probe, self.probe
                if was:
                    return "  raw         0x0BB0\n  temperature 23.50 C\n"
                return ("  raw         0x0B04\n  THERMOCOUPLE OPEN -- probe not connected\n"
                        if self.probe_reads_open else "  raw         0x0B00\n  temperature 44.00 C\n")
            if line.startswith("gnss"):
                return GNSS
            return BRINGUP.get(line, f"  unknown command '{line}'\n")
        if self.state == "app":
            if line.startswith("rpc "):
                return '  {"id":1,"ok":true,"net":{"connected":false,"mac":"%s"}}\n' % self.mac
            if line == "version":
                return f"\n  firmware   {self.version}\n  esp-idf    v5.3.2\n  elf sha256 {self.elf16}...\n"
            if line == "health":
                return HEALTH.format(temp="23.50 C  (MAX6675)")
            if line.startswith("sn set "):
                self.pending_sn = line[7:]
                return f"  SN {line[7:]} stored; reboot to use it everywhere\n"
            if line == "sn":
                return f"  {self.sn}   (USB drive X)\n"
            if line == "reboot":
                self.boot()
                return ""
            if line == "flash":
                self.state = "rom"
                return ""
            if line == "logtest":
                return "  ring wraps                                   pass\n\n  all passed\n"
            if line.startswith("wifi add "):
                return f"  {line.split()[2]} stored (1 known); joins the strongest in range\n"
            if line.startswith("mqtt set "):
                return "  stored; connecting\n"
            if line.startswith("ota base "):
                return "  stored\n"
            if line.startswith("config set "):
                k, v = line.split()[2:4]
                self.config[k] = int(v)
                return f"  {k} = {v}, stored\n"
            if line in ("ledtest", "beep 150"):
                return "  done\n"
            if line == "sync":
                return "\n  Wi-Fi      x\n  broker     siamatic.co.th:1883, connected\n"
            return f"  unknown command '{line}'\n"
        return ""


class FakeConsole:
    def __init__(self, box: FakeBox, port: Port):
        self.box, self.port = box, port

    def open(self):
        return self

    def close(self):
        pass

    def command(self, line, quiet=1.5, timeout=10.0, until=None, may_vanish=False):
        return self.box.reply(line)

    def drain(self, quiet=0.3, limit=3.0):
        return ""


class FakeHardware:
    def __init__(self, box: FakeBox):
        self.box = box

    def scan(self):
        return [self.box.port()]

    def console(self, port, baud, eol):
        return FakeConsole(self.box, port)

    def sleep(self, s):
        pass


class FakeEsp:
    def __init__(self, box: FakeBox):
        self.box = box
        self.writes = []

    def _out(self):
        return f"Chip is ESP32-S3 (QFN56) (revision v0.2)\nMAC: {self.box.mac.lower()}\n" \
               "Detected flash size: 16MB\n"

    def chip_info(self, port, before="default_reset"):
        assert self.box.state == "rom", "chip_info outside the loader"
        out = self._out()
        return EspResult(True, out, []), {"mac": self.box.mac, "chip": "ESP32-S3 (QFN56)",
                                          "flash_size": "16MB", "revision": "0.2"}

    def write(self, port, images, erase_all=False, before="default_reset", after="hard_reset"):
        assert self.box.state in ("rom", "bringup"), "write outside the loader"
        self.writes.append(([os.path.basename(p) for _, p in images], erase_all))
        if erase_all:
            self.box.flash = {}
        for off, path in images:
            self.box.flash[off] = os.path.basename(path)
        if after == "hard_reset":
            self.box.boot()
        return EspResult(True, self._out() + "Hash of data verified.\n", [])

    def reset(self, port, otg):
        self.box.boot()
        return EspResult(True, "", [])


# ---- image files -----------------------------------------------------

def app_image(version: str, project: str = "mCOLD", size: int = 64 * 1024) -> tuple[bytes, str]:
    """An app image with a real esp_app_desc_t; returns (bytes, elf sha256 hex)."""
    elf_sha = hashlib.sha256(version.encode()).digest()
    desc = struct.pack("<II8x", 0xABCD5432, 0)
    for s, n in ((version, 32), (project, 32), ("10:00:00", 16), ("Oct 24 2026", 16),
                 ("v5.3.2", 32)):
        desc += s.encode().ljust(n, b"\0")
    desc += elf_sha + b"\0" * 20 * 4
    header = bytes([0xE9, 1, 2, 0x4F]) + struct.pack("<I", 0x40370000) + b"\0" * 16
    seg = struct.pack("<II", 0x3C000020, len(desc))
    body = header + seg + desc
    return body + b"\xff" * (size - len(body)), elf_sha.hex()


TYPES = {"app": 0x00, "data": 0x01}
SUBTYPES = {"ota_0": 0x10, "ota_1": 0x11, "factory": 0x00, "ota": 0x00, "phy": 0x01, "nvs": 0x02,
            "coredump": 0x03, "nvs_keys": 0x04}


def partition_table(csv_path: str) -> bytes:
    """What gen_esp32part.py makes of a CSV with explicit offsets."""
    out = b""
    with open(csv_path, encoding="utf-8") as f:
        lines = [ln for ln in f if ln.strip() and not ln.lstrip().startswith("#")]
    for r in csv.reader(lines):
        r = [c.strip() for c in r]
        t = TYPES.get(r[1], None)
        t = int(r[1], 0) if t is None else t
        st = SUBTYPES.get(r[2], None)
        st = int(r[2], 0) if st is None else st
        out += struct.pack("<HBBII16sI", 0x50AA, t, st, int(r[3], 0), int(r[4], 0),
                           r[0].encode(), 0)
    out += struct.pack("<H", 0xEBEB) + b"\xff" * 14 + hashlib.md5(out).digest()
    return out + b"\xff" * (0xC00 - len(out))


def build_dir(path: str, version: str, kind: str, repo: str) -> str:
    """A PlatformIO build folder for `kind` (test or production)."""
    os.makedirs(path, exist_ok=True)
    app, _ = app_image(version, "bringup" if kind == "test" else "mCOLD")
    with open(os.path.join(path, "firmware.bin"), "wb") as f:
        f.write(app)
    with open(os.path.join(path, "bootloader.bin"), "wb") as f:
        f.write(b"\xe9" + b"\0" * 4095)
    with open(os.path.join(path, "partitions.bin"), "wb") as f:
        f.write(partition_table(os.path.join(repo, "firmware", "partitions.csv")))
    with open(os.path.join(path, "ota_data_initial.bin"), "wb") as f:
        f.write(b"\xff" * 8192)
    return path
