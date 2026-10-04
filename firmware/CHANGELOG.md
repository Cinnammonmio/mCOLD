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

P7, power: in progress.

- **Deep sleep between jobs on battery** (`pm.*`). Each wake runs as a
  short boot: every module does one pass and reports (`duties`), anything
  a person or a session is doing keeps the chip up (`holds`), and it
  sleeps until the earliest time any module asked for. USB power keeps
  it awake. Wake sources: timer; motion (accelerometer INT1, EXT0; one
  motion wake a minute); NFC GPO and PG# (EXT1, any low, each armed only
  while high).
- Pins held through sleep at safe levels: rails off, LED rail high, data
  and select lines low so nothing feeds an unpowered part.
- `mono_ms()`, a clock that runs through sleep. The boot counter now
  counts real resets only (a wake is the same boot carrying on), and the
  record stamp's tick uses this clock.
- Kept in RTC memory across a sleep: the trip's dwell timers and motion
  count, the sample schedule (samples stay on a fixed grid), the last
  GNSS fix and the GNSS plan, the display's last picture, the NFC auth
  key, the LED attention budget, the last Wi-Fi network, the upload plan.
- A wake is not a resume: no `RESUMED` event, no boot sweep, no
  rotation of the auth key, no redraw unless something changed.
- Wi-Fi only while wanted. On battery, one upload session per
  `upload_period_s`: join the last network directly (no scan), send,
  `"0"` on `online`, radio off. A session without an ACK backs off,
  doubling up to 4 hours. PROTOCOL.md section 6 updated for the server.
- BLE stack started on first need (a tap or USB power), not every boot.
- GNSS on battery: only during a trip (or with no valid time), every
  `gnss_period_s` (1800 s), 90 s with a recent fix and 180 s without,
  stretching x2/x4/x8 while sessions fail.
- INA226 powered down while asleep (330 µA otherwise, ~5 % of the
  budget); 16-sample averaging while awake.
- Flash log boot scan through a memory map: 168 ms → 18 ms.
- Faster boot: no PSRAM memory test, no image re-hash after deep sleep,
  bootloader logs at warning level, no 300 ms console wait after a wake.
- New settings: `sleep_en`, `idle_wake_s`, `gnss_period_s`,
  `upload_period_s`, and `sleep_usb` (bench: behave as on battery with
  the cable in). Console: `sleep`, `sleep clear`, `sleep test S`.

Measured on the bench (USB power, `sleep_usb 1`): a timer wake is 0.13 s
of ROM and bootloader plus 1.2 s of firmware; 3.6 s with a panel
refresh; an upload session ~15 s while the server sends no ACK.

After the first night on battery (~12 mA average, 2026-10-03):

- **VDD_SPI kept on in deep sleep.** GPIO47/48 are in its domain; with it
  off GPIO48 fell to 0 V and turned the LED rail on all night. Asleep:
  5.75 mA → ≤ 0.25 mA.
- Sleep entry: rails off, pins held, 40 ms settle, then the accelerometer
  latch is cleared before the motion wake is armed.
- Attention window 2 s on battery (30 s on USB).
- GNSS on battery gives up after 60 s with nothing heard; backoff to ×16.
- Upload sessions that get no ACK back off too, not only unreachable ones.
- **Low battery:** `batt_trip_mv` (3550) refuses START_TRIP with
  `BATTERY_LOW`; `batt_off_mv` (3400) logs `POWER_OFF`, draws BATTERY
  EMPTY and switches the box off through the charger's ship mode until
  USB is plugged in. Console `poweroff` runs the same path.
- Front lights on their own cap, `led_front_pct` (2 %); the alive blink on
  battery every `led_status_s` (900 s); a cargo alarm still at every wake.
- Fuel gauge: SOC clamped to 100 %; "no battery" above 4.28 V, with a
  quick start when a cell is connected.
- BATTERY EMPTY title in capitals (the title font has no lower case);
  `trip dump` names POWER OFF.
- `platformio.ini`: monitor DTR/RTS low (no reset on open), CRLF.
- ESP-IDF power management built in (`CONFIG_PM_ENABLE`, tickless idle);
  nothing uses it by default yet.
- Bench: `amps`, `hiz`, `cpu`, `pin`, `sleep_meas` (sleep current in the
  wake record), `sleep` shows the last sleep's current.

Measured with the charger in HIZ: asleep ≤ 0.25 mA; awake ~130 mA
(~40 mA floor; the rest not found yet); BLE stack +25 mA; GNSS ~173 mA.

- **Counted state of charge** (`soc.*`): the INA226's current integrated
  while awake, `sleep_ua` (250 µA) for each sleep, 100 % at charge done,
  the gauge's percent only to start (or when the kept count disagrees
  with it by over 15 %), the capacity learned from a full-to-switch-off
  run. Kept in RTC memory and NVS (every 1 %). It feeds the screen, the
  app, the records and the low-battery alarm; the switch-off stays on
  the cell voltage. New settings `batt_mah` (1500), `sleep_ua` (250);
  console `soc`, `soc set P`; `health` shows counted and gauge side by
  side.
- The count is checked against the voltage (decided 2026-10-04, the cell's
  1500 mAh not being trusted): after a sleep of 4 min or more the rested
  voltage, plus what the wake's current takes off it (150 mOhm), reads an
  OCV table (generic LiPo, to be replaced by this cell's) and pulls the
  count towards it -- hard on the steep ends, gently in the flat middle,
  not within 2 h of charging. Two reliable points (charge done, a rested
  voltage on a steep part, the switch-off voltage) 30 % or more apart
  give the capacity from the charge counted between them. On the board:
  starts at 100 % "charge done" (gauge 95.6 %).
- The percent shown is the voltage's for now (`soc_source 0`, decided
  2026-10-04): the OCV table on the rested voltage plus the wake's load,
  smoothed; on a charger the count. The count goes on underneath to
  measure the capacity; `soc_source 1` shows it instead.
- Light sleep while awake, measured on the bench with BLE off and GNSS
  idle: 130-134 mA off, 126-131 mA on -- no real gain. The ~90 mA above
  the ~40 mA floor is not the CPU waiting, so it stays off.
- Review fixes: GNSS due but unavailable, and a dwell run out with no
  probe readings, no longer hold the chip awake; pm ignores wake times
  over 30 s in the past; the battery switch-off holds the chip up until
  done; a Wi-Fi refusal after a scan backs off instead of rescanning.
- **Light sleep while awake (trial, off by default):** config
  `light_sleep 1` lets the chip nap between tasks on battery and drop to
  40 MHz when idle (never on USB power). Every pin in use keeps its
  configuration through a nap (`CONFIG_PM_SLP_DISABLE_GPIO` would let go
  of them all); a GNSS session, a beep, a Wi-Fi session and a panel
  refresh hold the chip out of light sleep (`pm_no_light_sleep`). Aimed
  at the ~130 mA awake current, most of which is waiting. Not yet run on
  the board.

## 0.6.0 — 2026-10-02

P6 in part: the box reaches the server. **OTA and the USB drive are
deferred** (decided 2026-10-02) to a later release; both are planned in
PROTOCOL.md and HANDOFF.

- Wi-Fi station knowing up to 5 networks (console `wifi add`/`wifi del`, app
  `SET_WIFI`/`DEL_WIFI`): scans and joins the strongest known one, tries
  another after a refusal; credentials in NVS, names only ever reported;
  reconnect with backoff
- NTP once Wi-Fi is up: sets RTC and system clock (source `ntp`), logs a
  correction over 2 s in the trip
- RTC battery switch-over on, for the CR1220 hand-soldered on VBAT_3V:
  checked, the time survives SW3 off
- Display redraws at once on USB plug/unplug and footer icon changes
  (at most every 30 s); Wi-Fi and cloud icons show real state
- MQTT to the team broker (`mqtt set`), topics under `mcold/<sn>/`:
  record batches out, application ACKs in (contiguous high-water mark per
  trip, checked against the log, kept in NVS), retained status, online
  flag with last will; resend with backoff to 5 min while unanswered;
  the running trip gets every other batch
- Retention deletes server-acknowledged trips first, with no loss event
- `GET_SYNC_STATUS`, `SYNC_NOW`; PROTOCOL.md section 6 for the server team
- PSRAM takes NimBLE, Wi-Fi/LWIP buffers and the canvases: with Wi-Fi
  added, internal RAM had run out and the console task was never created
  (now checked and reported at boot)
- Checked on the board: connected to Wi-Fi and to siamatic.co.th:1883,
  batches sent; injected ACKs (`ack`) for an unknown trip, past the log,
  and malformed were rejected; valid ones moved the mark and the next
  batch went at once; the mark survived a reboot. No real server ACK yet


## 0.5.0 — 2026-10-02

P5 complete: the app's way in -- NFC to find and authorize, BLE to talk,
one JSON protocol over both. Proposed to the iOS team; not yet
confirmed by them.

Not yet checked: a tap opening BLE while the box runs on battery (on
USB power it advertises all the time, so the bench cannot show it).

- `PROTOCOL.md`: protocol 1, proposed to the iOS team -- GATT service and
  UUIDs, MTU-independent fragments, JSON requests with ids, errors, the
  section 13 commands
- `rpc`: the commands, transport-independent; changing commands need an
  authorized session and are idempotent by request id (START_TRIP's
  survives a reset); unknown values are null, never 0
- `ble`: NimBLE peripheral advertising the SN; only after an NFC tap
  (60 s) or on external power
- **Tap to authorize** (decided 2026-10-02, instead of a passkey on the
  display): the NFC record carries a random key; each BLE connection
  gets a nonce in INFO and proves the key with AUTH =
  HMAC-SHA256(key, nonce). No pairing prompt, nothing to type; the key
  never goes over the air and changes at every reset and 2 min after a
  session ends. The tag is written only while no phone holds it, and a
  new key is used only after it reads back from the tag
- `tools/ble_client.py`: a reference client doing what the app must
- Checked over real BLE from a PC: command before AUTH refused, wrong
  proof refused, right proof accepted, a previous connection's proof
  refused, the old key refused after rotation and the new one accepted,
  395- and 467-byte answers reassembled from fragments, START_TRIP
  retried with one id gives one trip. A phone (nRF Connect) connected,
  read INFO and exchanged a command

## 0.4.0 — 2026-10-02

P4 complete: lights, sound and the e-paper -- on the mono panel in hand,
since the four-ink panel the board was designed for is a long way off.

### Lights and buzzer (`indicate`)

Per docs/led-design.md; each front light owns one meaning:

| Light | Means | Patterns |
|---|---|---|
| front left | the goods | DOUBLE red on a temperature alarm; BLINK blue on ack |
| front middle | trip running | TRIPLE green on start, TICK each round, BLINK on stop |
| front right | the box | TRIPLE amber on a device fault (unhealthy part, no time during a trip, probe or battery alarm) |
| whole front row | boot, phone tap | white sweep left to right at boot, then green row or amber fault; white row blink on a tap |
| side | charge | unchanged from 0.1.0 |

- Status repeats every second on external power or in a 30 s attention
  window (phone tap; motion, at most 4 an hour), otherwise once per
  sample period. Every pattern ends dark; lights and buzzer never
  sound together
- An alarm sounds three beeps unless acknowledged or `buzzer_enabled`
  is 0; `led_bright_pct` sets the brightness cap
- SWEEP runs by position, left to right (LED1 is on the right)

### E-paper

- Driver for the fitted 2.13" mono SSD1680 panel (GxEPD2_213_B74
  sequence, proven by bring-up); BUSY polarity and geometry in one
  table, so the four-ink panel is one entry away. Full refresh,
  2.3 s, deep sleep after; RST released so it cannot back-power the
  panel. **Rotation 3** is upright on this board
- The design's templates, ported from `display-mock/render.py` with the
  same IBM Plex fonts; three inks, accent printed black on mono
- Live screen chosen from state: A1 idle, A2 trip, A3 out of band,
  A4 alarm (frame), A5 no reading ("--"), A6 charging, B6 probe fault,
  C1 summary after a stop
- Refreshes only when the picture changes: at once for alarm and trip
  changes, otherwise at most every 5 minutes; the clock ticking is not
  a change. The header time is when it was drawn, local time
  (`tz_offset_min`, default UTC+7), "--:--" when the box has no time
- Console: `screen`, `screen N` (the 14 design pages), `screen rot`

### Checked on the board, by eye

- All LED patterns and the alarm beeps through start, alarm, ack, stop
- The display through boot, start, alarm and stop: four refreshes,
  exactly at the four changes; upright, readable

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
