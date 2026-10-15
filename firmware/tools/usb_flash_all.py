#!/usr/bin/env python3
"""Flash every box on the USB bus (a dock of six, say), one at a time, over its own port.

    python tools/usb_flash_all.py                 # .pio/build/mcold/firmware.bin
    python tools/usb_flash_all.py path/to/app.bin

usb_flash.py takes the first box it finds; with several on the bus that is a
guess, and the ROM port that comes up next may be another box's. Here each box
is followed by its MAC: read over its console, it names the ROM port it comes
back as (the USB-Serial-JTAG's serial number is the MAC), and the box is known
again afterwards by its serial number. App and otadata only, as usb_flash.py.

Needs pyserial and esptool (PlatformIO's Python has both).
"""
import json
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
VID, APP_PID, ROM_PID = 0x303A, 0x4003, 0x1001


def boxes():
    return sorted((p.serial_number, p.device) for p in list_ports.comports()
                  if p.vid == VID and p.pid == APP_PID)


def talk(port, line, wait=2.5):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = s.rts = False
    s.reset_input_buffer()
    s.write((line + "\r\n").encode())
    buf, last = b"", time.time()
    while time.time() - last < wait:
        b = s.read(4096)
        if b:
            buf += b
            last = time.time()
    s.close()
    return buf.decode("utf-8", "replace")


def find(pred, seconds):
    end = time.time() + seconds
    while time.time() < end:
        for p in list_ports.comports():
            if pred(p):
                return p.device
        time.sleep(0.5)
    return None


def flash(sn, port, app, otadata):
    out = talk(port, 'rpc {"id":1,"cmd":"GET_STATUS"}')
    m = re.search(r'"mac":"([0-9A-F:]{17})"', out)
    if not m:
        return f"{sn}: no MAC from its console; skipped"
    mac = m.group(1)
    try:
        talk(port, "flash", 0.5)
    except (serial.SerialException, OSError):
        pass                            # the port goes as the box restarts into its loader
    rom = find(lambda p: p.pid == ROM_PID and (p.serial_number or "").upper() == mac, 20)
    if not rom:
        return f"{sn}: its loader ({mac}) did not appear"
    r = subprocess.run([sys.executable, ESPTOOL, "--chip", "esp32s3", "-p", rom, "--before", "default_reset",
                        "--after", "hard_reset", "write_flash", "0xd000", otadata, "0x20000", app],
                       capture_output=True, text=True, timeout=300)
    if r.returncode:
        return f"{sn}: write failed on {rom} -- it is in download mode; run this again\n{r.stdout[-300:]}"
    back = find(lambda p: p.pid == APP_PID and p.serial_number == sn, 60)
    if not back:
        return f"{sn}: written, but it has not come back yet"
    time.sleep(2)
    v = re.search(r"firmware\s+(\S+)", talk(back, "version"))
    return f"{sn}: {mac} {back} firmware {v.group(1) if v else '?'}"


def main():
    app = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD, "firmware.bin")
    otadata = os.path.join(BUILD, "ota_data_initial.bin")
    todo = boxes()
    print(f"{len(todo)} boxes: " + ", ".join(sn for sn, _ in todo), flush=True)
    for sn, port in todo:
        print(time.strftime("%H:%M:%S ") + flash(sn, port, app, otadata), flush=True)


if __name__ == "__main__":
    main()
