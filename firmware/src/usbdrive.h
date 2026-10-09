// The USB port as a serial port and a drive at once (phase F2, 2026-10-05).
//
// Out of reset the port is the chip's USB-Serial-JTAG: the console, and
// what esptool flashes through. With USB power, usbdrive_start() hands the
// port to TinyUSB, which shows the PC two things on the same cable:
//
//   a serial port   the console, as before -- a new COM port number
//   a drive         read-only, labelled from the SN (MC1L0169001, logrow.h):
//                   DEVICE.TXT now, the trips as CSV files in F3
//
// The drive is generated, not stored: every sector is built from the log
// and the settings when the PC asks for it, so nothing is written to flash
// and pulling the cable never damages anything.
//
// Flashing over USB needs the port back: `flash` calls usbdrive_stop()
// and restarts into the ROM's download mode on the USB-Serial-JTAG. This
// board has no BOOT button and SW1 sits inside the case, so that path,
// and OTA behind it, are the only ways to new firmware.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// With USB power, once: the port becomes TinyUSB's. Safe to call again.
void usbdrive_start(const char *sn);
bool usbdrive_active(void);

// One console byte, waiting at most `ms`: from the USB-Serial-JTAG until
// the drive starts, from its serial port after. False if none came.
bool usbdrive_console_read(uint8_t *c, uint32_t ms);

// The port back to the USB-Serial-JTAG (before `flash`).
void usbdrive_stop(void);

// The internal USB PHY to the USB-Serial-JTAG. TinyUSB routes it to the
// OTG controller with two RTC_CNTL bits that a software reset does NOT
// clear (found 2026-10-05): the ROM then came up in download mode on
// USB-OTG, where esptool cannot reset the chip, and the box sat there on
// battery until the cable was pulled. Called first thing at every boot
// and by usbdrive_stop(), so the ROM always finds the port it expects.
void usbdrive_phy_to_usj(void);
