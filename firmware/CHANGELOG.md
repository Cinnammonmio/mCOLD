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
- `firmware/main` holds released firmware only, and is merged into the
  repo's `main` at each release (directly, no pull request: agreed
  2026-10-02)
- phase work on `firmware/pN-...`, merged into `firmware/main` when the
  phase is verified on the board

To cut a release:

1. Set `PROJECT_VER` to the release number, without `-dev`
2. Move the `Unreleased` notes below under that number
3. Build, flash, and check that the boot banner prints the new number
4. Merge into `firmware/main`, then `git tag -a firmware/vX.Y.Z`
5. Merge `firmware/main` into `main`; push both and the tag (never force)
6. Set `PROJECT_VER` to the next `-dev` version

## Unreleased

## 0.3.0 — 2026-10-02

P3 complete: the box runs trips and records them.

### Trips

- START takes that trip's own alarm thresholds (low, high, hysteresis,
  dwell) and writes a header; then a sample every sample period (5 min),
  events as they happen, and STOP writes a summary (samples, min/max
  valid temperature, alarms raised, motion)
- Started and stopped from the console for now (`trip start`, `trip
  stop`, `trip ack`, `trip dump`); from the app over BLE in P5
- **Resume after a reset**: the same trip carries on, an EV_RESUMED
  event records the reset reason, and totals and alarm state are
  rebuilt from the log -- an alarm acknowledged before the reset stays
  raised and acknowledged, never raised a second time
- The first sample after a start or resume waits (up to 10 s) for the
  first temperature reading since boot
- Trip ids come from a counter in NVS, checked against the log so an
  erased NVS can never reuse an id
- Records are stamped with UTC only when the clock can vouch for it;
  otherwise "no time", ordered by boot counter and tick

### Alarms

- Temperature high and low, raised after dwell, cleared only back
  inside by the hysteresis; no temperature for 60 s; battery under 15 %
  (clears at 20 %). Acknowledging is an event; it erases nothing
- Motion is recorded as events (one per burst, at most one a minute)
  and counted in samples; **no shock alarm** until a threshold is set

### Storage

- Record layout documented byte for byte in `src/record.h`:
  little-endian, never a C struct, an explicit "none" value for every
  field that can be unknown
- **Retention**: when the log is full, the oldest finished trip is
  deleted whole and a LOSS event records it; the running trip is never
  touched. A starting trip keeps its header as record 0

### Door

- **Switched off** (decided 2026-10-02): `MCOLD_DOOR 0` in
  `src/features.h` -- no GPIO7 setup, interrupt, setting, event or
  alarm. The record keeps its door fields, written as not fitted.
  Config schema 3 removes `door_closed_lvl`

### Checked on the board

- A trip with 5 s dwell raised the high-temperature alarm (47 C, no
  probe fitted), the ack was logged, and after two resets the alarm
  was still raised and acknowledged with no second raise
- Trips numbered 1, 2, 3, 4 across resets and stops
- The log filled to 77,376 records; starting a trip deleted trip 1
  (the oldest finished one) and wrote START, LOSS, SAMPLE in that order;
  every other trip untouched. `logtest`: 14 of 14 still pass

### Not in this release

- Shock alarm: needs a threshold from the product team
- `log_meta`: the trip list is rebuilt from the log itself for now;
  ACK checkpoints and an SD path arrive with P6

## 0.2.0 — 2026-10-02

P2 complete: time, configuration, and the trip log that everything in a
trip is written to first.

### Trip log (`flashlog`)

Append-only records in the 9.75 MiB `trip_log` partition (§9.2-9.4).

- 4 KiB sectors: a 64-byte header (sector sequence, trip, CRC32) and 31
  frames of 128 bytes; each frame carries type, length, trip, per-trip
  sequence, up to 112 bytes of payload and its own CRC32
- A frame commits by being written whole with a good CRC. A power cut
  leaves at most one torn frame, which is skipped and never reused;
  nothing committed is lost. All writes are 16-byte aligned, so this
  still holds with flash encryption on
- One trip per sector, so a trip can be deleted whole (§9.6); a trip
  written again after another resumes its own last sector
- Sectors are opened in rotation, spreading erases over the partition
- Capacity at the 5-minute sample period: about 269 days of samples
- Checked on the board, against a RAM image of NOR flash: power cut at
  every byte of a record and of a sector header, between header and
  first record, and in the middle of an erase; two interleaved trips;
  deleting a trip; full and reclaimed; wrap-around; a garbage sector;
  bad arguments. 14 of 14 pass (`logtest`)
- Checked on the real partition: 250 records written across a reboot,
  read back with 0 wrong and 0 torn; boot scan of 2,496 sectors 168 ms
- Console: `log`, `logtest`, and `log write` / `log read` / `log erase`
  against a bench trip id that no real trip can have

Not in this release, because each needs something that comes later:
when a trip may be deleted (P3 trip state, P6 server ACK), the
`log_meta` journal of trips, ACK checkpoints and loss records, and the
record schema of a sample (P3).

### Time and configuration

- Time: UTC with a quality (unknown / rtc / gnss / host), a boot counter
  in NVS and a per-boot tick, so events stay ordered across resets and
  clock corrections. The system clock follows the RTC; a lower-ranked
  source cannot overwrite a higher one within a boot
- Configuration in NVS, one key per setting from a single table of
  ranges and defaults; out-of-range values are refused on set and
  replaced by the default on load. Over-long NVS keys fail the build.
  Console: `config`, `config set`, `config reset`
- Config schema 2: temperature alarm thresholds are no longer device
  settings; they are set per trip when it starts (decided 2026-10-02).
  Keys left by schema 1 are erased on first boot
- Sample period default 300 s (5 min), decided 2026-10-02
- `reboot` command; motion events and GNSS sessions carry uptime stamps

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
