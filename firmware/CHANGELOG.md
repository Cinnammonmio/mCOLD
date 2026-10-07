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

- **Wi-Fi in five fixed slots** (decided 2026-10-07). A slot keeps its
  place when another is cleared. `GET_NETWORK` and the SETTINGS event's
  `network` always list all five (`{"slot":2,"ssid":null}` for an empty
  one); `SET_WIFI`/`APPLY_CONFIG` `add` take a `slot` (edit in place; the
  password may be left out only while the ssid stays; a changed ssid
  starts on DHCP), `DEL_WIFI` and `del` take a slot number or a name; one
  network in one slot only. Networks stored before sit in slots 1..n.
  Checked on box 002 over `rpc`. 0.7.0-dev.38/.39.

- **No `schema` in the MQTT batch** (decided 2026-10-07); `trip json` prints the batch head too. 0.7.0-dev.37.

- **A slimmer row on MQTT** (decided 2026-10-07): no `sn` (the batch and
  the topic say it), `timestamp` is Unix seconds (no `utc`, no local
  text), no `detail`. ~300 B a row, a part ~6 KB (was ~9). The CSV keeps
  every column. `trip json` prints a trip as the server gets it.
  0.7.0-dev.36.

- **A trip is named by a UUID, a date and a day number** (decided
  2026-10-07), not by the box's counter. Header format 5 carries
  `trip_id` (UUID v4, made at the start), `trip_date` (local YYYYMMDD,
  0 if the time was unknown) and `trip_number` (the day's running number
  from 1); the SN left the header to make room (the frame holds 112
  bytes). Every row in the rec JSON and the CSV has the three columns in
  place of `trip`; ACKs are `{"trip_id":"<uuid>","upto":N}` (the numeric
  form is refused); BLE `MARK_DELIVERED`, `GET_TRIP_SUMMARY`,
  `READ_LOG_CHUNK` take `trip_id`, `LIST_TRIPS` and the status name trips
  by the three. A trip without an identity (headers 3 and 4) is not
  uploaded, listed or put on the drive; a trip running across the update
  carries on, and stays home. Rows are ~440 B, a part ~9 KB: broker limit
  12 KB. 0.7.0-dev.35.
- **For testing, never in the product:** tap commands (`tap_test`, works
  asleep on battery too: INT1 carries the double tap alone and the
  motion rest is off), `rx_show` (BLE state in the header, what a phone
  sent below, NFC field edges folded into one tap), the BLE link lights
  (blue blink on connect/disconnect, two on AUTH, a tick on the right
  while connected), the `[rx]`/`[tx]` console log of every command, SHT-31
  detection (`i2c`). 0.7.0-dev.30 to .33.

- **What a phone sent, on the glass, for testing** (`rx_show`, console
  only, default off). Testing tap-then-connect with the app, the screen
  shows the last events -- NFC tap, BLE connected and gone, every command
  with what it carried and its answer (`START_TRIP low:20,... > ok`) --
  for a minute after the last, redrawn at most every 3 s. Passwords, the
  AUTH proof and the key show as ***. A command repeated with the same
  answer is one line with a count. The first build (dev.28) restarted the
  box at the second command: the page's 2.5 KB of buffers on the display
  task's 4 KB stack. They are static now, the task has 6 KB (3.4 KB left
  after a page), and `tasks` reports display and ble too. 0.7.0-dev.29.

- **Tap commands, for testing only** (`tap_test`, console only, default
  off). The box has no button: with `tap_test 1` a double tap is a
  command, chosen by how the box lies -- normal (z down): start a trip,
  or stop the one running; left side (-y): redraw the screen; right side
  (+y): GNSS held 10 minutes; 1/2/3/4 beeps, one long beep for anything
  else. The LIS2DW12 finds the double tap itself, asleep or not. It
  recognised none while wake-up shared INT1 (axis and sign in TAP_SRC,
  never TAP_IA, at 375 to 750 mg); with INT1 carrying the double tap
  alone, 400 Hz high-performance and 562 mg (ST AN5038) it works through
  the foam case. Motion is not counted while it is on. Commands within
  3 s of the last are ignored: four quick knocks started and stopped a
  trip. Checked on box 002, every position. Also `tap ths N`, and the
  tap source in every `[accel]` line. 0.7.0-dev.27.

- **Rows keep the last known position.** After a fix, the module's next
  sentence without one marks the fix invalid but keeps its position; rows
  required a valid fix and went out with null latitude/longitude and
  gnssstate none, indoors, right after a fix (box 002, trip 1). They now
  carry the last fix, `fix` while it is current, `last` after; null only
  before the first fix since boot. Checked: trip 2 on box 002, every row
  with a position. 0.7.0-dev.22.

- **Every box starts with the broker** (decided 2026-10-05). The login
  lives in `secrets.ini` (gitignored); `tools/secrets_gen.py` writes it
  into `src/secrets_gen.h` (gitignored, always written, empty without
  secrets.ini, so the build tracks it). A box with no `mqtt set` of its
  own uses it; boxes that have one keep theirs. Every image built this
  way carries the login, OTA files included. Box 002 reached the broker
  with no setup but Wi-Fi. 0.7.0-dev.21.

- **SHT-31 instead of the thermocouple, found by itself.** Box 002 has an
  SHT-31 on the I2C bus (0x44, 3V3_MAIN) in place of the type K probe.
  At the cold boot the firmware looks for it and reads it if it answers,
  the MAX6675 if not: one firmware for both, the choice kept through deep
  sleep. Temperature only, no humidity. `health` and `DEVICE.TXT` name
  the sensor. New bench command `i2c`: line levels and every address that
  answers (box 002: 18 2D 36 40 42 44 53 57 68 6B). 0.7.0-dev.19.

- **F4: the drive checked with Windows.** Explorer and Excel open the
  files; unplug, plug in and eject are clean; plugged in on battery and
  with a trip running, the drive is there in 5 s. A 524-row trip (eight
  checkpoints), read first at jumps into the middle and the end, then
  whole: the same bytes as `trip csv`. `trip fill N` (console, test
  only) writes N sample rows at once for such tests. 0.7.0-dev.17.

- **DEVICE.TXT names the running trip**: its number, rows up to the
  snapshot and file name. `tasks` now prints the stack each task has
  never used: after a snapshot the console had 612 B left of 6 KB and
  the NFC task 876 B of 3 KB, so they get 8 KB and 4 KB (now 2.6 KB and
  1.9 KB to spare). `tools/usb_flash.py` resets the chip into the loader
  itself on the USB-Serial-JTAG: a write that died at 55 % left a loader
  that answered nothing until then. 0.7.0-dev.16.

- **F3: the trips as CSV files on the USB drive.** Each trip in the row
  format is `TRIP_<SN>_<YYMMDDhhmm>.csv` (long file names), the same bytes
  as `trip csv` (CRLF), built while the PC reads: checkpoints every 64
  rows let any sector be produced without reading the trip from the
  start. `DEVICE.TXT` says when the snapshot was taken, how many trips
  are shown, whether one is running and how many rows are not sent. The
  snapshot is taken when the PC mounts the drive; unplug and plug in for
  a newer one. Checked: both trips (7 and 22 rows) match `trip csv`
  byte for byte. 0.7.0-dev.14.
- **dev.13 hung the box on every USB mount**, and it was only got back by
  OTA: the snapshot ran inside `tud_mount_cb`, on TinyUSB's 4 KB task, and
  overflowed it. With the port gone, the cable pulled and the power
  switch cycled, the box booted on battery (no drive), checked in and
  took 0.7.0-dev.14 from the retained firmware message. In dev.14 the
  callback only flags the snapshot, the console task builds it, and the
  TinyUSB task has 8 KB.

- **F2: the USB port is a serial port and a drive at once.** With USB
  power, TinyUSB (vendored `components/esp_tinyusb`, its storage glue
  left out) shows the PC the console on a new COM port and a read-only
  FAT16 drive labelled `MC1L0169001` with `DEVICE.TXT`; the drive is
  generated on request, nothing is written. `usb_drive` (0/1, settable
  from the server) turns it off -- the way back if it ever misbehaves.
  Flashing: `flash` hands the port back and restarts into download mode;
  `tools/usb_flash.py` does the whole round. Found on the way: TinyUSB
  routes the internal USB PHY with RTC_CNTL bits a software reset keeps,
  so the ROM came up on USB-OTG, where esptool cannot reset it, and the
  box sat in download mode until the cable was pulled. Every boot and
  `flash` now give the PHY back to the USB-Serial-JTAG: checked, `flash`
  -> COM7 -> write -> RTS reset -> the firmware with its drive again.
  0.7.0-dev.12.

- **A hung task restarts the box** (decided 2026-10-05): the supervisor
  restarts after any watched job (now including the uplink, which takes
  OTA) has not run for 5 minutes, notes which in NVS and says so at the
  next boot. A box in its case has SW1 out of reach and no BOOT button,
  and only running firmware can take an OTA. `hang` at the console proves
  it: checked, the box came back by itself (boot count 217 -> 218).
  0.7.0-dev.9.

- **Wi-Fi with DHCP or a fixed address, a temperature adjustment, and no
  broker from outside** (decided 2026-10-05). Each known network has
  `dhcp` or `ip`/`gateway`/`subnet`/`dns`, as eTEMP has it -- in the
  settings document, `SET_WIFI` and `wifi ip` at the console; the fixed
  address is set once associated. `temp_adj_c100` (eTEMP's tempAdj) is
  added after the factory calibration, which is now the console's only;
  it is trip-locked and the trip header (format 4) records it. The MQTT
  broker can no longer be set from the app or the server, and the trial
  is gone with it. Checked on the board: a fixed 192.168.1.233 joined and
  reached the broker, back to DHCP; bad addresses refused; +5.00 C showed
  as +5.00 C. 0.7.0-dev.8.

- **The app gets the settings by itself** (decided 2026-10-05): a
  SETTINGS event on EVENT as soon as the app subscribes, again on AUTH
  and whenever a setting changes from anywhere -- every config key, which
  the app may edit, which are locked during a trip, the server's last
  rev, and after AUTH the network part (Wi-Fi names, broker, OTA base,
  never a password). `GET_SETTINGS` asks for the same; `GET_NETWORK` now
  needs AUTH. Checked over BLE from the PC (tools/ble_client.py's link):
  pushed on connect without the network part, again with it after AUTH,
  and after a SET_CONFIG. 0.7.0-dev.7.

- **Settings from the app and the server** (decided 2026-10-05,
  `docs/device-settings.md`): one settings document -- `config` keys,
  Wi-Fi add/del, MQTT broker, OTA base -- over BLE (`APPLY_CONFIG`) or
  MQTT (`mcold/v1/<sn>/config`, retained, applied once per `rev`, the
  result on `mcold/<sn>/config/state`). Only the user-facing keys; the
  sample period and calibration not while a trip runs; bench and factory
  keys stay at the console (`SET_CONFIG` over BLE follows the same list).
  A new broker is on trial for 15 minutes and the old one comes back if
  it is not reached. `GET_NETWORK` shows the network settings without
  passwords; `cfgdoc` at the console plays a server document. Checked on
  the board. 0.7.0-dev.6.

- **USB drive, F1 and the start of F2** (phases named 2026-10-05: F1 the
  device code, F2 `flash` + USB composite, F3 CSV files on the drive, F4
  Windows test). F1: no second code beside the SN; the drive label is
  `MC1L0169001` (product's first two letters, version, lot with its L,
  unit) and `sn` shows it. `sn set` takes only the label's pattern now
  (`PPPVv-LllYY-MMYY-NNN`, month 01-12, unit 001-999). F2: `flash`
  restarts into the ROM's download mode -- the way to flash once the USB
  port is ours, on a board with no BOOT button. Checked: esptool
  connected with no reset of its own, and a hard reset came back to the
  firmware.

- **Trip rows go up after the trip** (decided 2026-10-05,
  `docs/trip-data-flow.md`). While a trip runs its rows stay in the box
  and the server gets the status -- now with `alarms_raised` and
  `last_alarm`, sent at once when the broker answers again. A finished
  trip goes up in parts of 20 rows on a fixed grid (`part`, `parts`,
  `last`), oldest first, across sessions. The app's "stop and send" pulls
  the trip over BLE and gives it to the server itself, then
  `MARK_DELIVERED` tells the box not to upload it; `LIST_TRIPS` says
  which trips are `sent`, and the trip summary page says SENT / NOT SENT.
  On battery a status session every upload period while a trip runs.
  Fixed: going from USB to battery ended the first session at once (a
  batch from USB power looked overdue) and counted it as a miss.
  0.7.0-dev.5.

- **Topics as eTEMP has them** (decided 2026-10-05): what the box
  publishes is `mcold/<sn>/rec|status|online|ota/state`, what it
  subscribes to is `mcold/v1/<sn>/ack|firmware`. Summary for everyone in
  `docs/mqtt-topics.md`. 0.7.0-dev.4.

- **READY page with no trip** (A1, decided 2026-10-05): an arrow up to
  the NFC tag, a phone, "SCAN TO START TRIP". No clock, temperature or
  link icons, so the frame stays until a trip starts, power is plugged
  or pulled, or the battery moves 5 % from what the glass shows -- no
  more refresh every five minutes with no trip (watched 6.5 min on the
  board: none). Shown on the charger too, instead of the charge page.
  Battery too low for a trip: "Battery low. Charge first." in red. The
  charge page (now only a demo page) draws 100 % without running into
  its rows. New font: Bold 20 caps (`fonts_title20.h`).

- **One log row for everything** (record format 3, decided with the team
  2026-10-05). A sample and every event are the same row, carrying the
  state of the box at that moment: `trip, seq, sn, timestamp, utc, event,
  temp, tempmin, tempmax, alarm, timeok, gnssstate, latitude, longitude,
  motion, battery, internet, detail`. The server gets those columns as
  JSON (no more base64), `trip csv` prints the trip as the CSV it will be,
  named `TRIP_<SN>_<YYMMDDhhmm>.csv` (the whole SN, decided later the
  same day: a second short code would only confuse people). New rows: `USB_IN`/`USB_OUT`;
  ordinary motion is a count, not an event; the door is gone. Trips in
  the old format stay in the log but are not uploaded. 0.7.0-dev.3.

- **Topics move to `mcold/v1/<sn>/...`** (was `mcold/<sn>/...`): the
  product, then the version of the topic layout, as the team asked
  (2026-10-04). Same messages otherwise.

- **OTA** (`ota.*`), the eTEMP V2 way: the server publishes a file name
  on `mcold/v1/<sn>/firmware` (retained), the box downloads it from the
  file server (base URL in NVS, `ota base URL`) straight into the other
  slot. Checked before it is booted: same project and a newer version
  from the image header, the image's SHA-256, and an RSA-3072 signature
  against the key of the running image. Booted on probation: kept once
  it reaches the broker, rolled back by the bootloader if not within
  3 minutes. Not during a trip or under 30 % battery (it waits, then
  goes ahead). Results on `mcold/v1/<sn>/ota/state`, retained. Console:
  `ota`, `ota base URL`, `ota FILE|URL [force]`, `ota rollback-test`.
  Tested through MQTTX: a wrongly signed image refused, 0.7.0-dev to
  0.7.0-dev.1 and confirmed.
- Images are signed after the build by `tools/sign_app.py` (key in
  `keys/`, gitignored), which also leaves `mCOLD_<version>.bin` for the
  server. Signed apps without hardware secure boot: no eFuse burned.
- `status` carries `fw` (version, slot) and `net` (SSID, RSSI, IP, MAC).
- On battery with nothing to send, a check-in session every 6 hours, so
  a status and a firmware message still reach a box between trips.

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
- **Serial number from the factory** (`sn set S`, NVS "sys"/"sn"), e.g.
  `mCDV1-L0169-1069-001`: the BLE name, the NFC record, the MQTT client id
  and topics, the screen, the trip records. Buffers 32; the trip header
  is format 2 with a 24-byte SN (was 12). `MCOLD-xxxx` until set.
- Screen: the reading centred on its own, the unit hanging off its right;
  the header in a new bold 12 px face with lower case (tools/genfont.py)
  and the local time with the date (`22:50 04/10/26`, regular 11 px:
  bold and medium hinted the 1 heavier than the other digits); every
  crossed-out footer icon gets the same strike; everything drawn
  inside the area the case leaves visible, `epd_inset_t/b/l/r`, measured
  with `screen cal` (six frames 3 px apart).
- Lights in two families: warnings keep their place and colour (red left
  for the cargo, amber right for the box, the side light's amber and red);
  everything calm is blue, violet or cyan -- the boot sweep, the tap, trip
  start/running/stop, acknowledgement, charging. led-mock and the manuals
  follow. Nothing that is not an alarm is red or amber: the side light's
  "power in, not charging" blinks violet, and violet has little red in
  it (60/255) because at 2 % the first one read pink.
- The charge light is green again (breathing while charging, steady when
  full); the blue/cyan trial is dropped. "Not charging" stays violet.
- Bezel measured on the board with `screen cal`: left 3 px hidden, the
  other sides clear (`epd_inset_l 3`).
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
