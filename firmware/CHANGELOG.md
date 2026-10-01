# Changelog

## Versioning

`MAJOR.MINOR.PATCH`, following the phases in HANDOFF.md §7 until the
first production release:

| Version | Meaning |
|---|---|
| `0.N.0` | phase N complete and verified on the board |
| `0.N.x` | fixes on top of phase N |
| `0.N.0-dev` | work in progress towards phase N; never shipped |
| `1.0.0` | first production release (after P8) |

The version is written in exactly one place, `PROJECT_VER` in the root
`CMakeLists.txt`, and ESP-IDF embeds it in the image header. Every
release is a git tag of the same name with a `v` in front, made on
`main`. Phase work happens on a `pN/...` branch and is merged when the
phase is verified.

To cut a release:

1. Set `PROJECT_VER` to the release number, without `-dev`
2. Move the `Unreleased` notes below under that number
3. Build, flash, and check that the boot banner prints the new number
4. Merge to `main`, then `git tag -a vX.Y.Z`
5. Set `PROJECT_VER` to the next `-dev` version

## Unreleased (0.1.0-dev)

P1, drivers.

- Firmware reports its version and image hash at boot and
  with the `version` command
- LIS2DW12 accelerometer: XYZ in mg, wake-up detection latched onto
  INT1 (GPIO2) and delivered to the sensor task by interrupt
- GNSS (ATGM336H): NMEA at 115200 with checksum validation, RMC and GGA,
  bounded acquisition sessions, and the RTC re-set from satellite time
  when its own time cannot be trusted
- ST25DV04KC: UID and IC reference, RF field detection from the latched
  dynamic register, verified NDEF writes that skip unchanged bytes
- Four addressable LEDs on RMT, with a brightness cap and the rail held
  only while a pixel is lit
- Buzzer on LEDC at 2.7 kHz, non-blocking and bounded in length
- Side light (LED4) shows charge state: breathe yellow while charging,
  breathe green from 80 %, steady green when full, yellow blink with
  input but no charging, red blink on a charge fault, off on battery
- LED index names follow physical position (front left is index 3,
  front right index 1), checked by eye
- Motion events while the buzzer sounds, and 250 ms after, are ignored:
  the buzzer trips the accelerometer by itself
- GNSS reports satellites in view, heard, and best SNR

## 0.0.1 — 2026-10-02

Baseline as imported into git. P0 complete; P1 partial.

- Health registry, reference-counted rails, guarded I2C and SPI buses
- MAX6675, PCF8523, MAX17048 + INA226 + BQ25601
- Four tasks, a supervisor, and a `health` / `tasks` console
