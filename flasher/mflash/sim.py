"""Simulated mCOLD boxes on a simulated USB bus: the tests run on them, and
`mflash gui --demo` puts six of them in the window for training.

A box behaves the way the real one was measured to: it comes up on the
USB-Serial-JTAG (MAC as its USB serial) when blank, in the ROM or running
bringup; on the TinyUSB port (SN as its serial) when running the product
firmware; `flash` sends it to the ROM; `sn set` takes effect at reboot.
Each one keeps its own USB location, as a box in a dock slot does.
"""
from __future__ import annotations

import csv
import hashlib
import os
import struct
import threading
import time

from . import imageinfo
from .esp import EspResult
from .ports import Port

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

# seconds a command takes on the real box, roughly (scaled by the bus's speed)
DURATION = {"logtest": 4.0, "walk": 3.6, "ledtest": 3.0, "gnss": 3.0, "power": 0.5, "health": 0.6}


class SimBox:
    def __init__(self, mac: str = MAC, slot: int = 0, probe_reads_open: bool = True,
                 version: str = "0.7.0", elf16: str = "aabbccddeeff0011", gnss_dead: bool = False):
        self.mac = mac
        self.slot = slot
        self.state = "rom"            # rom | bringup | app
        self.flash: dict[int, str] = {}   # offset -> file name
        self.sn = ""
        self.pending_sn = ""
        self.probe = True
        self.probe_reads_open = probe_reads_open
        self.version, self.elf16 = version, elf16
        self.config: dict[str, int] = {}
        self.gnss_dead = gnss_dead    # a GNSS that sends nothing: wiring or the rail
        self.wifi: list[str] = []     # its known networks, in NVS: an app-only flash keeps them
        self.sent: list[str] = []

    # what is on the bus
    def port(self) -> Port:
        loc = f"1-1.{self.slot + 1}"
        if self.state in ("rom", "bringup"):
            return Port(f"COM{3 + 2 * self.slot}", 0x303A, 0x1001, self.mac, loc + ":1.0",
                        "USB JTAG/serial debug unit")
        serial = self.sn or "MCOLD-" + self.mac.replace(":", "")[-4:]
        return Port(f"COM{4 + 2 * self.slot}", 0x303A, 0x4003, serial, loc + ":1.2", "mCOLD console")

    def boot(self) -> None:
        if not (self.flash.get(0x20000) or self.flash.get(0x10000)):
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
                if self.gnss_dead:
                    return "  listening on GPIO18 at 115200 for 15 s\n\n  0 bytes\n  nothing received\n"
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
                rest = line[9:]
                ssid = rest[1:rest.index('"', 1)] if rest.startswith('"') else rest.split()[0]
                if ssid not in self.wifi:
                    self.wifi.append(ssid)
                return f"  {ssid} stored ({len(self.wifi)} known); joins the strongest in range\n"
            if line.startswith("wifi ip "):
                parts = line.split()
                if len(parts) >= 4 and parts[2] in self.wifi:
                    return f"  {parts[2]}: {'DHCP' if parts[3] == 'dhcp' else 'fixed address; used at the next join'}\n"
                return "  wifi ip SSID dhcp | wifi ip SSID IP GATEWAY SUBNET [DNS]  (a known SSID)\n"
            if line.startswith("wifi del "):
                ssid = line[9:].strip().strip('"')
                if ssid in self.wifi:
                    self.wifi.remove(ssid)
                    return "  removed\n"
                return "  not a known network\n"
            if line == "wifi":
                return ("\n  Wi-Fi      not connected (0 attempts)\n  known      "
                        + (", ".join(self.wifi) or "none") + "\n")
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


class SimBus:
    """Boxes on one bus; `speed` scales every delay (0 for tests, 1 for real time)."""

    def __init__(self, boxes: list[SimBox], speed: float = 0.0):
        self.boxes = boxes
        self.speed = speed
        self.lock = threading.Lock()

    def wait(self, s: float) -> None:
        if self.speed and s > 0:
            time.sleep(s * self.speed)

    def replace(self, slot: int) -> None:
        """A new blank board in this slot, as if the old one was taken out."""
        with self.lock:
            old = [b for b in self.boxes if b.slot == slot]
            if not old:
                return
            n = getattr(self, "_made", 0) + 1
            self._made = n
            mac = "28:84:85:%02X:%02X:%02X" % (0x30 + n, 0x10 + slot, 0x70 + slot)
            self.boxes[self.boxes.index(old[0])] = SimBox(mac=mac, slot=slot,
                                                          probe_reads_open=old[0].probe_reads_open,
                                                          gnss_dead=old[0].gnss_dead)

    def box_on(self, device: str) -> SimBox | None:
        for b in self.boxes:
            if b.port().device == device:
                return b
        return None


class SimConsole:
    def __init__(self, bus: SimBus, port: Port):
        self.bus, self.port = bus, port

    def open(self):
        return self

    def close(self):
        pass

    def command(self, line, quiet=1.5, timeout=10.0, until=None, may_vanish=False):
        box = self.bus.box_on(self.port.device)
        if not box:
            if may_vanish:
                return ""
            from .console import ConsoleError
            raise ConsoleError(f"{self.port.device}: gone")
        self.bus.wait(DURATION.get(line.split()[0] if line else "", 0.3))
        with self.bus.lock:
            return box.reply(line)

    def drain(self, quiet=0.3, limit=3.0):
        return ""


class SimEsp:
    def __init__(self, bus: SimBus):
        self.bus = bus
        self.writes: list[tuple[list[str], bool]] = []

    def _box(self, port: str) -> SimBox:
        b = self.bus.box_on(port)
        assert b is not None, f"no box on {port}"
        return b

    @staticmethod
    def _out(box: SimBox) -> str:
        return (f"Chip is ESP32-S3 (QFN56) (revision v0.2)\nMAC: {box.mac.lower()}\n"
                "Detected flash size: 16MB\n")

    def chip_info(self, port, before="default_reset"):
        box = self._box(port)
        assert box.state in ("rom", "bringup"), "chip_info outside the loader"
        self.bus.wait(1.0)
        return EspResult(True, self._out(box), []), {
            "mac": box.mac, "chip": "ESP32-S3 (QFN56)", "flash_size": "16MB", "revision": "0.2"}

    def write(self, port, images, erase_all=False, before="default_reset", after="hard_reset"):
        box = self._box(port)
        assert box.state in ("rom", "bringup"), "write outside the loader"
        self.writes.append(([os.path.basename(p) for _, p in images], erase_all))
        self.bus.wait(4.0 + (3.0 if erase_all else 0))
        with self.bus.lock:
            if erase_all:
                box.flash = {}
            for off, path in images:
                box.flash[off] = os.path.basename(path)
                info = imageinfo.read_app_info_file(path)
                if info and off == 0x20000:
                    box.version, box.elf16 = info.version, info.elf_sha256[:16]
            if after == "hard_reset":
                box.boot()
        return EspResult(True, self._out(box) + "Hash of data verified.\n", [])

    def read_flash(self, port, offset, size, path, before="default_reset"):
        with open(path, "wb") as f:
            f.write(b"\xff" * size)
        return EspResult(True, "", [])

    def reset(self, port, otg):
        box = self._box(port)
        with self.bus.lock:
            box.boot()
        return EspResult(True, "", [])


class SimHardware:
    def __init__(self, bus: SimBus):
        self.bus = bus
        self._esp = SimEsp(bus)

    def scan(self) -> list[Port]:
        with self.bus.lock:
            return [b.port() for b in self.bus.boxes]

    def console(self, port, baud, eol):
        return SimConsole(self.bus, port)

    def sleep(self, s):
        self.bus.wait(s)

    def esp(self, chip, flash):
        return self._esp


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


def demo_setup(root: str, speed: float = 0.4) -> SimBus:
    """A library with approved demo images in `root`, and five boxes in six slots:
    slot 4 empty, slot 6 a box whose GNSS sends nothing (it fails the test stage)."""
    from . import firmware as fwlib
    from . import profile as proflib
    os.environ["MFLASH_LIBRARY"] = os.path.join(root, "library")
    p = proflib.load("mcold")
    # two production builds, so the dropdown has a choice; the newer is imported last
    for kind, ver in (("test", "bringup-demo"), ("production", "0.7.0-dev.61"),
                      ("production", "0.7.0-demo")):
        d = build_dir(os.path.join(root, f"build-{kind}-{ver}"), ver, kind, proflib.REPO)
        m = fwlib.import_build(p, kind, d, {"build_dir": "demo"})
        if not m.get("approved"):
            fwlib.approve("mcold", kind, m["id"], "demo")
    boxes = []
    for slot in (0, 1, 2, 4, 5):
        mac = "28:84:85:27:%02X:%02X" % (0x10 + slot, 0x70 + slot)
        boxes.append(SimBox(mac=mac, slot=slot, gnss_dead=slot == 5))
    return SimBus(boxes, speed=speed)
