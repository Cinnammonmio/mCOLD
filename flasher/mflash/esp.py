"""esptool, and getting a box into the ROM's download mode to use it.

The board has no BOOT button (GPIO0 is not brought out), so download
mode is reached one of three ways, by what the box is running now:

1. On the chip's USB-Serial-JTAG (a blank chip, the ROM, bringup):
   esptool resets it into the loader itself (`--before default_reset`).
2. On the mCOLD firmware's own port (TinyUSB, PID 0x4003): esptool
   cannot reset it, so the console command `flash` hands the port back
   and restarts into download mode; the box comes back as (1).
3. On the ROM's USB-OTG port (PID 0x0009, firmware before 0.7.0-dev.12):
   no RTS/DTR at all -- an RTS "reset" hung the port once (2026-10-05) --
   and the reset afterwards is by the RTC watchdog.

This follows firmware/tools/usb_flash.py, which proved each of them on
the board. esptool runs as a separate process: several boxes are flashed
at once from threads, and esptool's own output is kept per box.
"""
from __future__ import annotations

import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass


def esptool_cmd() -> list[str]:
    """How to run esptool: MFLASH_ESPTOOL if set, else this Python's module."""
    env = os.environ.get("MFLASH_ESPTOOL")
    if env:
        return shlex.split(env, posix=os.name != "nt")
    return [sys.executable, "-m", "esptool"]


@dataclass
class EspResult:
    ok: bool
    output: str
    argv: list[str]

    @property
    def mac(self) -> str:
        m = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", self.output)
        return m.group(1).upper() if m else ""


class Esp:
    def __init__(self, chip: str, flash: dict, log=print):
        self.chip, self.flash, self.log = chip, flash, log

    def run(self, port: str, args: list[str], before: str = "default_reset",
            after: str = "hard_reset", timeout: float = 600) -> EspResult:
        argv = esptool_cmd() + ["--chip", self.chip, "-p", port, "--before", before,
                                "--after", after, *args]
        self.log("  esptool " + " ".join(argv[len(esptool_cmd()):]))
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as e:
            return EspResult(False, f"esptool timed out after {timeout:.0f} s\n{e.stdout or ''}", argv)
        return EspResult(r.returncode == 0, (r.stdout or "") + (r.stderr or ""), argv)

    def chip_info(self, port: str, before: str = "default_reset") -> tuple[EspResult, dict]:
        """MAC, chip revision, flash size, embedded PSRAM -- what the chip says it is."""
        r = self.run(port, ["flash_id"], before=before, after="no_reset")
        out = r.output
        info = {"mac": r.mac}
        m = re.search(r"Chip is (.+)", out) or re.search(r"Chip type:\s*(.+)", out)
        if m:
            info["chip"] = m.group(1).strip()
        m = re.search(r"Features:\s*(.+)", out)
        if m:
            info["features"] = m.group(1).strip()
        m = re.search(r"Detected flash size:\s*(\S+)", out)
        if m:
            info["flash_size"] = m.group(1)
        m = re.search(r"Embedded PSRAM (\d+MB)", out) or re.search(r"(\d+MB) PSRAM", out)
        if m:
            info["psram"] = m.group(1)
        m = re.search(r"revision v?(\d+\.\d+)", out)
        if m:
            info["revision"] = m.group(1)
        return r, info

    def write(self, port: str, images: list[tuple[int, str]], erase_all: bool = False,
              before: str = "default_reset", after: str = "hard_reset") -> EspResult:
        args = ["write_flash"]
        if erase_all:
            args.append("--erase-all")
        args += ["--flash_mode", self.flash["mode"], "--flash_freq", self.flash["freq"],
                 "--flash_size", self.flash["size"]]
        for off, path in images:
            args += [hex(off), path]
        return self.run(port, args, before=before, after=after)

    def read_flash(self, port: str, offset: int, size: int, path: str,
                   before: str = "default_reset") -> EspResult:
        return self.run(port, ["read_flash", hex(offset), hex(size), path], before=before,
                        after="no_reset", timeout=1800)

    def reset(self, port: str, otg: bool) -> EspResult:
        """Out of download mode into the firmware, the way the port allows."""
        if not otg:
            return self.run(port, ["read_mac"], before="no_reset", after="hard_reset")
        # The ROM's USB-OTG port: reset by the RTC watchdog, from a session of
        # its own, as esptool's hard_reset would when it can.
        code = (
            "import sys\n"
            "from esptool.cmds import detect_chip\n"
            "esp = detect_chip(sys.argv[1], 115200, 'no_reset')\n"
            "esp.write_reg(esp.RTC_CNTL_OPTION1_REG, 0, esp.RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK)\n"
            "esp.rtc_wdt_reset()\n"
        )
        r = subprocess.run([sys.executable, "-c", code, port], capture_output=True, text=True,
                           timeout=30)
        return EspResult(r.returncode == 0, r.stdout + r.stderr, [])
