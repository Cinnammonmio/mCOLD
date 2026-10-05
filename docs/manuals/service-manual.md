---
name: Service-Manual
lang: en
version: 0.4
status: draft
date: 2026-10-05
firmware: 0.7.0-dev.8
hardware: foam V1.0.1
---

# mCOLD Foam V.1 — Service Manual


For technicians, firmware and QA. Deeper background: `firmware/HANDOFF.md`
(state, traps, decisions), `firmware/PROTOCOL.md` (BLE/NFC/MQTT),
`firmware/CHANGELOG.md` (per version).

## 1. Hardware overview

| Block | Part | Notes |
|---|---|---|
| MCU | ESP32-S3-WROOM-1U-N16R8 | 16 MB flash, 8 MB octal PSRAM |
| Power path | BQ25601 charger → TPS63020 → 3V3_MAIN | SW3 switches 3V3_EN; PS/SYNC to GND (power save) |
| Fuel gauge | MAX17048 (0x36) | Powered from the cell; model not yet tuned to the cell |
| Current | INA226 (0x40), R15 10 mΩ | Battery branch; 2.5 µV = 0.25 mA per count |
| USB PD | HUSB238A (0x42) | PD 9 V not enabled |
| Temperature | MAX6675 + type-K probe | On the e-paper rail (GPIO42); T− tied to GND (rework, see 9) |
| Display | E-paper, mono panel for now (four-colour panel to come) | SSD1680; rotation 3 |
| Accelerometer | LIS2DW12 (0x18) | INT1 → GPIO2, wakes the box |
| NFC | ST25DV04KC (0x53/0x57) | GPO → GPIO1 |
| RTC | PCF8523 (0x68) + CR1220 on VBAT_3V | Keeps time with SW3 off |
| GNSS | ATGM336H on onboard antenna U3 | On the SD/GNSS rail (GPIO47) |
| Lights | 3× SK6812MINI-E front, 1× XL-4020 side | LED rail GPIO48, LOW = on |
| Buzzer | Magnetic, 2.7 kHz, via Q5 (GPIO6) | |

Rails, all off at boot: `3V3_EPD_TK` GPIO42 high = on · `3V3_SD_GNSS`
GPIO47 high = on · `3V3_LED` GPIO48 low = on.

## 2. Tools and connection

- PlatformIO (ESP-IDF 5.3.2), project `firmware/`, environment `mcold`.
- USB-C cable to a PC. The box appears as a USB serial port (e.g. COM7), 115200.
- Console: `pio device monitor`. DTR/RTS are held low by `platformio.ini`,
  so opening the monitor does **not** reset the box. Typed characters are
  echoed by the box; press Enter to run a command. `help` lists them.
- Only one program can hold the port: close the monitor before flashing.

## 3. Firmware

| Step | Command |
|---|---|
| Build | `pio run -e mcold` |
| Flash | `pio run -e mcold -t upload` |
| Check | boot banner `firmware X.Y.Z`; console `version` |

The version is set in one place: `PROJECT_VER` in `firmware/CMakeLists.txt`.
Releases are tagged `firmware/vX.Y.Z` (see `CHANGELOG.md`).

A box asleep on battery has no USB port and cannot be flashed: plug USB in
(it wakes and stays awake), then flash. If `sleep_usb` is 1 the box sleeps
even on USB — set it back to 0 first.

## 4. Console commands

| Command | What it does |
|---|---|
| `help` | List of commands |
| `health` | Every device, its state and readings; battery, charger, rails |
| `tasks` | Task heartbeats, heap |
| `version` | Firmware version and image hash |
| `sn` · `sn set S` | The serial number; factory sets it, then reboot (screen, BLE, NFC, MQTT, records) |
| `accel` / `gnss` / `nfc` | Sensor detail |
| `led I R G B` · `ledtest [S] [N]` · `beep MS` | Lights and buzzer by hand |
| `config` · `config set K V` · `config reset` | Settings (section 5) |
| `trip` · `trip start [L H [HYST DWELL]]` · `trip stop` · `trip ack` · `trip dump [N]` | Trips from the bench |
| `trip csv [ID]` | A trip as its CSV file (`TRIP_<SN>_<YYMMDDhhmm>.csv`) |
| `log` · `logtest` | Trip log status; power-cut self-test (14 cases) |
| `screen` · `screen N` · `screen rot 1\|3` | Redraw, design pages, orientation |
| `screen cal` | Six frames 3 px apart: count those visible per side, inset = (6 − count) × 3 |
| `wifi` / `sync` | Wi-Fi and upload status |
| `wifi add SSID PASS` · `wifi del SSID` | Known networks (up to 5) |
| `wifi ip SSID dhcp` · `wifi ip SSID IP GATEWAY SUBNET [DNS]` | A network's address: DHCP or fixed |
| `mqtt set HOST PORT [USER PASS]` | Broker and login (the console is the only way to set it) |
| `ota` · `ota base URL` · `ota FILE\|URL [force]` | Firmware update state, file server, install now |
| `flash` | Restart into download mode, to flash over USB (or run `python tools/usb_flash.py`) |
| `ble [on\|off]` | BLE status; `on` advertises 60 s |
| `rpc {json}` | Any PROTOCOL.md request, authorized |
| `sleep` · `sleep clear` | Power manager state, wake record |
| `soc` · `soc set P` | Counted state of charge, capacity, anchor; set it by hand |
| `reboot` | Restart |

Bench-only (power measurement and tests):

| Command | What it does |
|---|---|
| `amps [S]` | Battery current for S s: mean, min, max |
| `hiz [S]` · `hiz off` | Charger input off for S s: the box runs on its cell with USB in. Every boot switches it back on |
| `sleep test S` | Deep-sleep S s, timer wake only (even on USB) |
| `poweroff` | The battery-empty path: screen, ship mode. Unplug and replug USB to restart |
| `cpu MAX MIN LS` | CPU clock / light sleep until the next boot |
| `pin N 0\|1\|in` | Take a pin over as plain GPIO (until reboot) |
| `log write N` · `log read` · `log erase` | Bench trip records |
| `ack {json}` | Inject a server ACK |
| `cfgdoc {json}` | Inject a server settings document (`docs/device-settings.md`) |
| `ota rollback-test` | The next new image will not confirm itself |
| `config set usb_drive 0` | Turn the USB drive off: the box restarts with the port as before |
| `hang` | Hang the console task: the supervisor restarts the box after 5 min |

## 5. Settings

Stored in NVS, kept through firmware updates. `config` shows each with its
range and whether it is the default.

| Key | Range | Default | Meaning |
|---|---|---|---|
| `sample_period_s` | 60–3600 | 300 | Trip sample period |
| `cal_offset_c100` | −1000–1000 | 0 | Temperature offset, 0.01 °C |
| `cal_gain_ppm` | 900000–1100000 | 1000000 | Temperature gain |
| `cal_version` / `cal_date` | — | 0 | Calibration record |
| `temp_adj_c100` | −1000–1000 | 0 | User adjustment added after the calibration, 0.01 °C (eTEMP's tempAdj); settable from the app and the server |
| `accel_wake_ths` | 1–63 | 2 | Motion threshold, ×31 mg |
| `led_bright_pct` | 1–100 | 20 | Side light brightness cap |
| `led_front_pct` | 1–100 | 2 | Front lights brightness cap |
| `led_status_s` | 60–3600 | 900 | On battery, the alive blink this often |
| `batt_trip_mv` | 3000–4100 | 3550 | Below it, on battery, no new trip (`BATTERY_LOW`) |
| `batt_off_mv` | 3000–3700 | 3400 | Below it, on battery, the box switches itself off |
| `batt_mah` | 300–6000 | 1500 | Rated cell capacity, until one is learned |
| `sleep_ua` | 0–5000 | 250 | Current asleep, for counting charge through a sleep |
| `soc_source` | 0–1 | 0 | Percent shown: 0 voltage (OCV table), 1 counted |
| `tz_offset_min` | −720–840 | 420 | Display time zone (records are UTC) |
| `epd_inset_t/b/l/r` | 0–20 / 0–30 | 0 | Panel pixels the case hides on each side (`screen cal`) |
| `buzzer_enabled` | 0–1 | 1 | Alarm sound |
| `sleep_en` | 0–1 | 1 | Deep sleep on battery |
| `idle_wake_s` | 300–86400 | 3600 | Longest sleep with nothing due |
| `gnss_period_s` | 300–86400 | 1800 | On battery in a trip: one GNSS session this often |
| `upload_period_s` | 60–86400 | 300 | On battery: one upload session this often |
| `sleep_usb` | 0–1 | 0 | Bench: behave as on battery with USB in |
| `sleep_meas` | 0–1 | 0 | Bench: measure each sleep's current (+0.33 mA) |
| `light_sleep` | 0–1 | 0 | Trial: light sleep between tasks while awake, on battery |

Wi-Fi and broker credentials are separate (`wifi add`, `mqtt set`) and never
appear in documents or the repository. Which keys the app and the server may
set, and the settings document they use, are in `docs/device-settings.md`;
the calibration (`cal_*`), the battery limits and the bench switches are the
console's only.

## 6. Provisioning a new box

1. Flash the current release; `sn set <label SN>`, then `reboot`.
2. `health`: every fitted device `ok` (pd, buzzer, sd show `unknown` until used).
3. `logtest`: all 14 pass.
4. `ledtest` and `beep 150`: check each light and the buzzer by eye and ear.
5. `wifi add …` for the site networks; `mqtt set …` for the broker.
6. `sync`: Wi-Fi connected, broker connected.
7. Check the time: `health` → clock source `ntp`, `gnss` or `rtc`.

## 7. Power

| Measured (bench, 2026-10-03) | Value |
|---|---|
| Asleep | ≤ 0.25 mA (at the current monitor's floor) |
| Awake, radios off | ~130 mA (under investigation, see 10) |
| Awake, BLE stack on | ~155 mA |
| Awake, GNSS on | ~173 mA |
| Charging from a PC port | +330 to +400 mA into the cell |
| Timer wake | 0.13 s boot + 1.2 s awake; 3.6 s with a refresh |

Behaviour on battery: deep sleep between jobs; wakes on the sample timer,
motion, an NFC tap or USB power. USB power keeps the box awake.

Low battery: below `batt_trip_mv` no trip starts; below `batt_off_mv`
(three readings in a row) the box records `POWER OFF` in the trip, shows
BATTERY EMPTY and puts the charger in ship mode — the cell is disconnected
from everything but the fuel gauge. Plugging USB in powers it back on
(verified 2026-10-03).

State of charge is counted, not read from the voltage: the current
monitor's readings while awake, `sleep_ua` for each sleep, 100 % when the
charger reports charge done; the fuel gauge's percent only to start, or
when the kept count is more than 15 % from it (charged while switched
off). A run from full to the switch-off voltage teaches it the cell's
real capacity. `health` shows the counted and the gauge's percent side by
side. The switch-off itself is by voltage.

Wake record: `sleep` lists recent wakes (cause, boot ms, awake ms, mA,
slept s). It survives a reset but not a power cut; `sleep clear` restarts it.

## 8. Diagnostics

| `health` state | Meaning |
|---|---|
| `ok` | Answering |
| `degraded` | Worked before, failing now: something came loose |
| `failed` | Never worked this boot: not fitted or built wrong |
| `unknown` | Not tried yet |

Failed devices are retried once a minute and recover on their own.

Trip records (`trip dump N`): SAMPLE (temperature, motion, GNSS, battery,
alarms), EVENT (RESUMED, MOTION, PROBE FAULT/OK, ALARM, ALARM CLEAR,
ALARM ACK, TIME SET, LOSS, POWER OFF), START, STOP.

## 9. Known hardware rework (every board of this BOM)

| Item | Do |
|---|---|
| GNSS RF path | Leave off C_M1, C_M2 and R_SET_IPEX (onboard antenna). For the u.FL instead: off R_SET_PCB, C_M1, C_M2; keep R_SET_IPEX |
| RTC backup | CR1220 to VBAT_3V (pad hand-soldered on V1.0.1) |
| Thermocouple | MAX6675 T− to GND, or an open probe is not detected |
| Buzzer | Its pulse sets off the accelerometer; firmware ignores motion during a beep |

## 10. Troubleshooting

| Symptom | Check |
|---|---|
| No USB port | Box asleep on battery: plug in USB. Still none: SW3, then the battery |
| `pio` upload fails, port busy | Close the serial monitor |
| Box dark after `poweroff` or an empty battery | Unplug and replug USB. If not: disconnect and reconnect the battery |
| Temperature `--` | Probe plugged in? `health` → thermo |
| Clock `----------` | No source yet: Wi-Fi (NTP), sky (GNSS), or the app; check the CR1220 |
| No GNSS fix | Indoors is normal; sessions give up after 60 s with nothing heard |
| Records never leave the box | Server not sending ACKs (`sync` shows pending) |
| Battery % over 100 / "no battery" | No cell connected: the gauge sees the charger's output |
| Motion events with nobody near | Under investigation; buzzer and rail switching are already blanked |
| Awake current ~130 mA | Under investigation (bench floor ~40 mA) |

---

## Not yet included

- Battery: cell datasheet, charge current and limits, fuel-gauge model, runtime
- Root cause of the ~130 mA awake current
- Charger / PD policy, HUSB238A register map, PD 9 V
- USB drive with CSV (phases F2-F4), SD archive (deferred)
- Dock (6 boxes), factory provisioning tool, production logging, secure boot / flash encryption (P8)
- Temperature calibration procedure and accuracy
- Four-colour display, shock alarm, door sensor
- Disassembly, part replacement, schematics references by page
