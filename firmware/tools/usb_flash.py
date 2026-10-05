#!/usr/bin/env python3
"""Flash a box whose USB port is the drive + serial port (usbdrive.h).

    python tools/usb_flash.py                 # .pio/build/mcold/firmware.bin
    python tools/usb_flash.py path/to/app.bin

Once the firmware runs, the PC sees TinyUSB's serial port (VID 303A,
PID 4003), not the chip's USB-Serial-JTAG, and esptool cannot reset the
chip into its loader by itself. So, without a BOOT button and with SW1
inside the case:

  1. `flash` goes to the console: the firmware hands the port back and
     restarts into the ROM's download mode;
  2. the ROM's port appears -- the USB-Serial-JTAG (PID 1001) since
     0.7.0-dev.12, or USB-OTG (PID 0009) from older firmware;
  3. esptool writes the app (and an empty otadata, so it boots ota_0)
     without touching RTS/DTR: on the ROM's USB-OTG port an RTS "reset"
     does not reset and left the port hung once (2026-10-05);
  4. the chip is reset the way that port allows: RTC watchdog for USB-OTG,
     the usual RTS sequence for the USB-Serial-JTAG.

Needs pyserial and esptool (PlatformIO's Python has both).
"""
import os
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", ".pio", "build", "mcold")
ESPTOOL_DIR = os.path.expanduser(os.path.join("~", ".platformio", "packages", "tool-esptoolpy"))
VID = 0x303A
APP_PID, USJ_PID, OTG_ROM_PID = 0x4003, 0x1001, 0x0009


def ports(pid):
    return [p.device for p in list_ports.comports() if p.vid == VID and p.pid == pid]


def wait_port(pids, seconds):
    end = time.time() + seconds
    while time.time() < end:
        for pid in pids:
            found = ports(pid)
            if found:
                return pid, found[0]
        time.sleep(0.5)
    return None, None


def esptool(*args):
    cmd = [sys.executable, os.path.join(ESPTOOL_DIR, "esptool.py"), *args]
    print("  " + " ".join(args))
    return subprocess.call(cmd)


def main():
    app = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD, "firmware.bin")
    otadata = os.path.join(BUILD, "ota_data_initial.bin")

    pid, port = wait_port([APP_PID], 2)
    if port:
        print(f"firmware console on {port}: asking it into download mode")
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.2
        s.dtr = s.rts = False
        s.open()
        s.write(b"flash\r\n")
        time.sleep(0.5)
        s.close()
    pid, port = wait_port([USJ_PID, OTG_ROM_PID], 15)
    if not port:
        sys.exit("no download-mode port appeared")
    otg = pid == OTG_ROM_PID
    print(f"ROM download mode on {port} ({'USB-OTG' if otg else 'USB-Serial-JTAG'})")

    rc = esptool("--chip", "esp32s3", "-p", port, "--before", "no_reset", "--after", "no_reset",
                 "write_flash", "0xd000", otadata, "0x20000", app)
    if rc:
        sys.exit("write failed; the chip is still in download mode -- run this again")

    if otg:
        # The ROM's USB-OTG port: reset by the RTC watchdog, from a session
        # of its own, as esptool's hard_reset would when it can.
        sys.path.insert(0, ESPTOOL_DIR)
        from esptool.cmds import detect_chip
        time.sleep(1)
        esp = detect_chip(port, 115200, "no_reset")
        esp.write_reg(esp.RTC_CNTL_OPTION1_REG, 0, esp.RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK)
        print("  resetting with the RTC watchdog")
        esp.rtc_wdt_reset()
        try:
            esp._port.close()
        except Exception:
            pass
    else:
        esptool("--chip", "esp32s3", "-p", port, "--before", "no_reset", "--after", "hard_reset",
                "read_mac")
    pid, port = wait_port([APP_PID, USJ_PID], 20)
    print(f"back: {port}" if port else "the firmware has not come back yet: look at the box")


if __name__ == "__main__":
    main()
