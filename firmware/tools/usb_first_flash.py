#!/usr/bin/env python3
"""First flash of boards that have never run this firmware, over USB, in order.

    python tools/usb_first_flash.py 003 007        # serial numbers ...-003 to ...-007

A chip that has never been flashed shows up on the PC as the ROM's
USB-Serial-JTAG (VID 303A, PID 1001). Each one that appears gets, one at a
time and in the order it appeared: the bootloader, the partition table, the
empty otadata and the app (what PlatformIO's upload writes, and what
usb_flash.py leaves alone), then a reset; the firmware's console port (PID
4003) comes up and the serial number is set and read back. The board needs
nothing else on it: no display, no sensors, no battery.

Ports that already run the firmware (PID 4003) are never touched, so a box
that is already flashed cannot be given a new serial number by this.

The result is a table, MAC -> serial number, in exports/first_flash.csv:
label the boards from it. Needs pyserial and esptool (PlatformIO's Python).
"""
import csv
import os
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", ".pio", "build", "mcold")
ESPTOOL = os.path.expanduser(os.path.join("~", ".platformio", "packages", "tool-esptoolpy", "esptool.py"))
OUT = os.path.join(HERE, "..", "..", "exports", "first_flash.csv")
VID, ROM_PIDS, APP_PID = 0x303A, (0x1001, 0x0009), 0x4003
SN_FORMAT = "mCDV1-L0169-1069-%s"


def rom_ports():
    return {p.device: p for p in list_ports.comports() if p.vid == VID and p.pid in ROM_PIDS}


def app_ports():
    return {p.device: p for p in list_ports.comports() if p.vid == VID and p.pid == APP_PID}


def esptool(*args):
    return subprocess.run([sys.executable, ESPTOOL, *args], capture_output=True, text=True, timeout=300)


def console(port, commands, wait=1.5):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = s.rts = False
    out = {}
    for c in commands:
        s.reset_input_buffer()
        s.write((c + "\r\n").encode())
        buf, last = b"", time.time()
        while time.time() - last < wait:
            b = s.read(4096)
            if b:
                buf += b
                last = time.time()
        out[c] = buf.decode("utf-8", "replace")
    s.close()
    return out


def flash_one(port, sn):
    files = [("0x0", "bootloader.bin"), ("0x8000", "partitions.bin"),
             ("0xd000", "ota_data_initial.bin"), ("0x20000", "firmware.bin")]
    args = ["--chip", "esp32s3", "-p", port, "--before", "default_reset", "--after", "hard_reset",
            "write_flash", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB"]
    for off, name in files:
        path = os.path.join(BUILD, name)
        if not os.path.exists(path):
            sys.exit(f"missing {path}: build first (pio run)")
        args += [off, path]
    r = esptool(*args)
    mac = re.search(r"MAC: ([0-9a-f:]{17})", r.stdout)
    return r.returncode == 0, (mac.group(1) if mac else "?"), r.stdout[-400:]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    first, last = int(sys.argv[1]), int(sys.argv[2])
    todo = [f"{n:03d}" for n in range(first, last + 1)]
    done = []
    seen_app = set(app_ports())
    print(f"waiting for {len(todo)} boards, serials {todo[0]}..{todo[-1]}; plug each one in", flush=True)
    idle_since = time.time()
    while todo and time.time() - idle_since < 3600:
        new = [p for p in sorted(rom_ports())]
        if not new:
            time.sleep(1)
            continue
        idle_since = time.time()
        port = new[0]
        n = todo[0]
        sn = SN_FORMAT % n
        print(f"[{time.strftime('%H:%M:%S')}] {port}: flashing for {sn}", flush=True)
        ok, mac, tail = flash_one(port, sn)
        if not ok:
            print(f"  write failed on {port}; leaving it, will try again\n{tail}", flush=True)
            time.sleep(5)
            continue
        print(f"  written, MAC {mac}; waiting for the firmware's port", flush=True)
        # Its own port, found by the name the firmware gives a box without a serial
        # number (MCOLD- and the last two bytes of the MAC): with several boards coming
        # up at once, "the newest port" is as likely to be another one's.
        temp = "MCOLD-" + mac.replace(":", "").upper()[-4:]
        end, got = time.time() + 40, None
        while time.time() < end and not got:
            hit = [d for d, p in app_ports().items() if p.serial_number == temp]
            got = hit[0] if hit else None
            time.sleep(0.5)
        if not got:
            print("  the firmware's port did not appear; look at the board", flush=True)
            todo_failed = True
            continue
        time.sleep(3)
        out = console(got, [f"sn set {sn}"])
        print("  " + out[f"sn set {sn}"].strip().replace("\n", " | ")[:200], flush=True)
        try:
            console(got, ["reboot"], wait=0.5)
        except (serial.SerialException, OSError):
            pass            # the port goes away with the reboot, as it should
        # the box comes back with its serial number as the USB serial
        back = []
        for _ in range(60):
            back = [p for p in app_ports().values() if p.serial_number == sn]
            if back:
                break
            time.sleep(1)
        seen_app.add(got)
        if back:
            res = console(back[0].device, ["version", "sn"])
            ver = re.search(r"firmware\s+(\S+)", res["version"])
            print(f"  {sn} on {back[0].device}, firmware {ver.group(1) if ver else '?'}", flush=True)
            done.append((mac, sn, back[0].device))
            todo.pop(0)
            new_file = not os.path.exists(OUT)
            os.makedirs(os.path.dirname(OUT), exist_ok=True)
            with open(OUT, "a", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                if new_file:
                    w.writerow(["time", "mac", "sn", "port"])
                w.writerow([time.strftime("%Y-%m-%d %H:%M:%S"), mac, sn, back[0].device])
        else:
            print(f"  {sn} did not come back with its serial; check it by hand", flush=True)
            todo.pop(0)
    print("done:", ", ".join(f"{sn} {mac}" for mac, sn, _ in done) or "none", flush=True)


if __name__ == "__main__":
    main()
