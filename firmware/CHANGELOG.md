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

The version is written in exactly one place, `PROJECT_VER` in
`firmware/CMakeLists.txt`, and ESP-IDF embeds it in the image header.

The firmware shares the mCOLD repo with the docs and design work, so
every git name it uses starts with `firmware/`:

- release tags `firmware/vX.Y.Z`
- `firmware/main` holds released firmware only, and reaches the repo's
  `main` by pull request
- phase work on `firmware/pN-...`, merged into `firmware/main` when the
  phase is verified on the board

To cut a release:

1. Set `PROJECT_VER` to the release number, without `-dev`
2. Move the `Unreleased` notes below under that number
3. Build, flash, and check that the boot banner prints the new number
4. Merge into `firmware/main`, then `git tag -a firmware/vX.Y.Z`
5. Set `PROJECT_VER` to the next `-dev` version

## Unreleased

## 0.1.0 — 2026-10-02

P1 complete: every on-board device has a driver, checked on the board.

### Drivers

- **LIS2DW12** accelerometer: XYZ in mg, 100 Hz low-power mode 1,
  wake-up detection latched onto INT1 (GPIO2) and delivered to the
  sensor task by interrupt. Checked: |a| 994 mg at rest; a tap raises an
  event and flashes the front-middle light
- **ATGM336H** GNSS: NMEA at 115200 with checksum validation, RMC, GGA
  and GSV, bounded sessions (2 min, every 10 min), RTC set from
  status-A satellite time. Checked: 1,077 sentences, 0 bad checksums.
  **Not checked: a fix.** Indoors it reported 0 satellites in view
- **ST25DV04KC** NFC: UID and IC_REF, phone detection from the latched
  field edges, verified NDEF writes that skip unchanged blocks.
  Checked: UID E002506962DC8136, a phone tap is detected
- **LEDs**: RMT, GRB on both part types, 20 % brightness cap, rail held
  only while a pixel is lit. Checked by eye, all four colours
- **Buzzer** (MLT-8530): LEDC 2.7 kHz, ends itself, capped at 3 s
- Firmware reports its version and image hash at boot and with
  `version`

### Side light: charge status

LED4, on the side of the case, follows the charger
(docs/led-design.md). It lights only while USB power is present -- read
from the charger's PG# pin, so it goes out the instant the cable is
pulled -- and therefore never costs the battery anything.

| Charger state | Side light |
|---|---|
| Pre-charge or fast charge, SOC below 80 % | breathes yellow (2 s cycle) |
| Fast charge, SOC 80 % or more | breathes green |
| Charge done | steady green |
| Power present, not charging | yellow blink every 2 s |
| Charge or battery fault (REG09) | red blink every 1 s, one beep |
| No external power, or charger silent | off |

The BQ25601 reports constant current and constant voltage alike as
"fast charge", so the yellow-to-green change comes from the fuel
gauge's SOC. Checked on the board: breathe yellow at 78 %, breathe
green from 80 %.

### Found on the board

- **The buzzer sets off the accelerometer**: 47 wake-up events for
  1.6 s of beeping, nobody touching the board. Motion during a beep and
  for 250 ms after is now counted separately and ignored
- **LED chain order differs from the design doc**: index 1 is front
  right, index 3 front left. Names follow position (left cargo, middle
  alive, right device), as the design asks

## 0.0.1 — 2026-10-02

Baseline as imported into git. P0 complete; P1 partial.

- Health registry, reference-counted rails, guarded I2C and SPI buses
- MAX6675, PCF8523, MAX17048 + INA226 + BQ25601
- Four tasks, a supervisor, and a `health` / `tasks` console
