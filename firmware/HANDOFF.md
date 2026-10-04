# mCOLD firmware — handoff

Written 2026-10-02 for whoever picks this up next, including a Claude
session with no memory of how any of it was arrived at.

Everything below was measured on the real PCB unless it says otherwise.
Where a thing is still a guess, it says so. The expensive parts of this
document are the sections headed **traps** — each one cost real time to
find and will cost it again if it is rediscovered rather than read.

---

## 1. Where things are

| Path | What |
|---|---|
| `C:\mCOLD` | **checkout of github.com/Cinnammonmio/mCOLD** — on a space-free path because ESP-IDF requires it |
| `C:\mCOLD\firmware` | **the product firmware** |
| `C:\mCOLD\docs` | requirements, schematics, display and LED design |
| `C:\Arduino\Foam V.1\Firmware\mCOLD-main` | an older zip copy of the repo, **not a git checkout**. Its `bringup/` and `bench/` work is now on branch `bringup/import`; its stale `firmware/` (an abandoned first attempt) was left out |
| `bringup/` (branch `bringup/import`) | the bring-up tool that proved the hardware. Arduino/PlatformIO, still useful, not the product |

The board is on **COM7**, native USB, MAC `28:84:85:27:9A:74`.

Requirements live in `docs/mCOLD_Firmware_Requirements_2026-09-19.md`.
Section numbers below refer to it.

---

## 2. State right now

Firmware **0.6.0** released (tag `firmware/v0.6.0`, ELF sha256 `87a009c0a...`).
Running on the board:

```
RAM  13.8% (45,220 B)      Flash 41.8% (1,314,472 B of a 3 MiB OTA slot)
```

P0 to P5 complete, P6 in part: the box runs trips, records them, shows
them, talks to a phone over BLE with tap-to-authorize, and uploads to
the team's MQTT broker over Wi-Fi with application ACKs. **OTA and the
USB drive are deferred.** Next: **P7**, power.

**P7 in progress** (branch `firmware/p7-power`, `0.7.0-dev`): on battery
the box now deep-sleeps between jobs (`pm.*`, section 6). Verified on
the bench with `sleep_usb 1` (USB in, behaving as on battery): timer
wakes on the sample grid, 0.13 s boot + 1.2 s awake per wake, 3.6 s
with a panel refresh, motion wakes, upload sessions with backoff, the
trip carrying on through sleep with no RESUMED events.

First night on battery (2026-10-02/03): 99 % → 84 % in ~19 h, about
**12 mA** average — over the 7.1 mA budget. Found and fixed on
2026-10-03 (section 4 for the traps):

| Cause | Was | Now |
|---|---|---|
| GPIO48 lost its hold in deep sleep (VDD_SPI off): LED rail on all night | 5.75 mA asleep | ≤ 0.25 mA asleep (the INA226's floor) |
| Motion → 30 s attention window, 4 an hour, mostly false motion | ~5 mA | 2 s window on battery |
| GNSS indoors: full 180 s sessions | ~3 mA | gives up after 60 s with nothing heard; backoff to ×16 |
| Rail switching at sleep entry latched an accelerometer event | motion wake 0 s after sleeping | 40 ms settle, latch cleared before arming |

Estimate after the fixes: ~1.5 mA average without uploads, 3–4 mA with
an ACKing server — **to be confirmed by the second night (2026-10-03/04)**.

Also new on 2026-10-03: low-battery switch-off (`batt_off_mv`, charger
ship mode, verified: replug USB restarts it) and no new trip below
`batt_trip_mv`; front lights 2 %, alive blink every 15 min on battery;
fuel gauge without a battery reads "no battery" instead of 116 %.

**Not yet done:** the awake current (~130 mA, floor ~40 mA, ~90 mA
unexplained — see section 8); NFC tap wake checked with a phone;
charger/PD policy and the fuel-gauge model (battery datasheet); a
state of charge that counts current, not voltage alone.

Bench state: Wi-Fi networks `mio` and `Mio_2.4G` and the broker login
are in this board's NVS (set over USB, never in git). The server does
not ACK yet, so nothing is reclaimed (`sync` shows what is pending). The door is switched off (`MCOLD_DOOR 0` in
`src/features.h`, decided 2026-10-02). The e-paper runs on the mono
panel in hand; the four-ink panel is a long way off.

**Git.** The firmware lives in the shared mCOLD repo under `firmware/`,
with its full history (imported 2026-10-02; until then it was a repo of
its own, so commits before that have firmware files at the root of
their tree). Everything firmware is namespaced so it stays apart from
the rest of the repo:

| | |
|---|---|
| `firmware/vX.Y.Z` | release tags, annotated with the release notes |
| `firmware/main` | released firmware only; merged into `main` at each release |
| `firmware/pN-...` | phase work, merged into `firmware/main` when verified |

Version scheme, release steps and what each release contains:
`CHANGELOG.md`. The version is `PROJECT_VER` in `firmware/CMakeLists.txt`
and nowhere else.

| Done | |
|---|---|
| `partitions.csv` | verified with Espressif's `gen_esp32part.py` |
| `health.*` | device registry, 5 states, backoff and recovery; `Dev::Untracked` for NAKs that are not faults |
| `rails.*` | reference-counted rail manager |
| `bus.*` | I2C/SPI behind mutexes with timeouts; reports health automatically |
| `temp.*` | MAX6675 |
| `rtcclock.*` | PCF8523 |
| `power.*` | MAX17048 + INA226 + BQ25601 |
| `accel.*` | LIS2DW12: 100 Hz low-power mode 1, wake-up latched on INT1, interrupt-driven |
| `gnss.*` | ATGM336H: 115200 NMEA, checksums, RMC/GGA/GSV, sessions, sets the RTC from status-A time |
| `nfc.*` | ST25DV04KC: UID, phone detection, verified NDEF writes |
| `leds.*` | RMT, GRB, 20% cap, rail held only while lit; names follow physical position |
| `buzzer.*` | MLT-8530, LEDC 2.7 kHz, self-terminating, 3 s cap |
| `chargeled.*` | side light shows charge state (table in CHANGELOG 0.1.0), only on external power |
| `timekeep.*` | UTC + quality, boot counter in NVS, per-boot tick; system clock follows the RTC |
| `config.*` | NVS settings from one table of ranges and defaults; schema 3; over-long keys fail the build |
| `flashlog.*` | the trip log: CRC'd 128-byte frames, one trip per sector, power-cut safe; `logtest` runs 14 power-cut tests on the device |
| `record.h` | the byte layout of every trip record, with its "none" values |
| `trip.*` | trips: start/stop, samples, events, alarm engine, resume after reset, retention when full |
| `door.*` | GPIO7 door contact, compiled but **off** (`features.h`) |
| `indicate.*` | LED and buzzer patterns per docs/led-design.md |
| `epd.*` | SSD1680 driver for the mono 2.13" panel; panel specifics in one `PanelDef` |
| `canvas.*`, `gfxfont.h`, `fonts_mcold.h` | 250x122 three-ink framebuffer, Adafruit GFX algorithms and fonts |
| `screens.*` | the design's templates, ported from `display-mock/render.py` |
| `display.*` | chooses the screen from state, refreshes per display-design.md; rotation 3 |
| `rpc.*` | the JSON protocol, transport-independent; sessions; idempotent by request id |
| `ble.*` | NimBLE GATT transport, MTU-independent fragments; advertises on tap or external power |
| `auth.*` | tap to authorize: key in the NFC record, AUTH = HMAC-SHA256(key, nonce), rotation |
| `tools/ble_client.py` | reference client for the app team; runs from a PC with Bluetooth |
| `net.*` | Wi-Fi: up to 5 networks, joins the strongest; SNTP sets the clock |
| `uplink.*` | MQTT: record batches out, application ACKs in (PROTOCOL.md section 6) |
| `logrow.*` | the row (record.h) as the table everyone sees: the server's JSON, the CSV, the trip's file name |
| `ota.*` | firmware updates: file name from the server, download, checks, rollback (PROTOCOL.md section 6) |
| `main.cpp` | 9 tasks + supervisor; console: `help` lists the commands |

Checked on the board by a person, 2026-10-02: tap → motion event;
phone → NFC arrival; all four LED colours and positions (`ledtest`);
side light breathes yellow while charging below 80 %, green above.

| Still unverified | |
|---|---|
| Side light going out on unplug | follows PG# directly; not yet watched |

| Open from P3 | |
|---|---|
| Shock alarm | motion is logged as events and counted per sample; no alarm until a threshold is chosen |
| Deleting after server ACK | only "oldest finished trip when full" exists. ACK-driven reclaim is P6 |
| `log_meta` partition | still unused: the trip list is rebuilt from `trip_log`. ACK checkpoints and loss records can live there in P6 |
| **Boot scan per wake** | the scan takes 168 ms. Once the box deep-sleeps between 5-minute samples, rescanning every wake costs ~5 mAh/day (~3 % of the budget). Keep the head position in RTC memory across sleep (P7) |
| Trip id `0xBE5C0001` | reserved for the console's bench records; real trip ids must never reach it |
| Test trips on the board | trips 2, 3, 4 in the log are bench tests from 2026-10-02 |

**The NFC tag is the device's to write.** It keeps the JSON record
`{"sn","ble","key"}` up to date itself (bring-up's `{"sn","ble"}`
plus the authorization key) and rewrites only bytes that change.

| Not written yet | |
|---|---|
| HUSB238A | answers at 0x42; PD policy is §5.5 |
| USB MSC, SD | deferred from P6 |

---

## 3. Hardware facts, measured

### Verified working

ESP32-S3 rev 2, **16 MB flash and 8 MB octal PSRAM** — N16R8 confirmed by
the chip itself.

All eight I2C devices answer and return correct identity registers:

| Addr | Part | Evidence |
|---|---|---|
| 0x18 | LIS2DW12 | `WHO_AM_I = 0x44` |
| 0x36 | MAX17048 | `VERSION = 0x0012` (needs the battery connected — it is powered from CELL_PLUS, not 3V3_MAIN) |
| 0x40 | INA226 | `MANUF_ID = 0x5449`, `DIE_ID = 0x2260` |
| 0x42 | HUSB238A | answers; no documented ID register |
| 0x53 / 0x57 | ST25DV04KC | UID `E002506962DC8136`, `IC_REF 0x50` |
| 0x68 | PCF8523 | keeps time to within a second over 4.5 h |
| 0x6B | BQ25601 | `REG08` tracks charging correctly |

Also proven: GNSS (right module, right wiring), buzzer, all four LEDs,
the e-paper flex and its boost circuit, the accelerometer across all
three axes, NFC read **and write**, and charging at ~474 mA.

Every interrupt pin — GPIO 1, 2, 4, 5, 7, 21 — is RTC-capable, so all of
them can wake the chip from deep sleep. The board was laid out correctly
for the low-power architecture §12 asks for.

TPS63020 PS/SYNC is tied to GND (checked in the layout 2026-10-03): the
3.3 V converter runs in power-save mode at light load.

**Power, measured 2026-10-03** (INA226 on the cell, 0.25 mA per count;
awake figures with the charger in HIZ so the box runs from its cell):

| State | Battery current |
|---|---|
| Deep sleep, VDD_SPI kept on | ≤ 0.25 mA |
| Awake, radios off, rails off | ~130 mA mean, ~40 mA floor |
| Awake, BLE stack up | ~155 mA |
| Awake, GNSS session | ~173 mA |
| Awake, CPU at 80 MHz / 40 MHz | 158 / 93 mA |
| Charging from a PC port | +330 to +400 mA into the cell |

The CPU clock barely matters between 160 and 80 MHz, so the ~90 mA above
the floor is not the CPU. Signal pins of the unpowered panel/MAX6675/GNSS
pulled low account for 15–25 mA of it; the rest is not found yet.

### Unresolved

**GNSS RF path: every DNP part was fitted -- FIXED 2026-10-02.** The board was
built with all antenna options populated, for easy rework. Two of them
kill the signal on their own:

- **C_M1 and C_M2, 100 pF shunt to ground** at the onboard antenna's
  matching network. At 1.575 GHz 100 pF is about 1 ohm -- a short across
  a 50-ohm line. Those footprints are for matching parts of a few pF,
  per the antenna's datasheet, or nothing.
- **R_SET_PCB and R_SET_IPEX both fitted**, so RF_IN also drives the
  empty u.FL connector: an open stub on the line.

Before: 0 satellites in view, not even weak ones. **Rework done: C_M1,
C_M2 and R_SET_IPEX removed** (onboard antenna U3 GPS1003 in use). After,
by a window: 9 to 17 satellites heard, best SNR 43 dB-Hz, a fix with 6-8
satellites and HDOP down to 1.7 within minutes, and the RTC set from
satellite time -- the first fix this board has ever had. **Every board
built from this BOM needs the same three parts left off.**

For the u.FL instead: remove R_SET_PCB, C_M1, C_M2, keep R_SET_IPEX --
and VCC_RF (pin 14) is not connected, so only a passive antenna works
there.

`$GPTXT,…,ANTENNA OPEN` from the module is not a fault here: the
ATGM336H detects an antenna by DC current into RF_IN, and C_DC blocks DC
by design. It says OPEN with this working antenna too.

**An unidentified I2C device at 0x2D.** It acknowledges both a
zero-length write and a one-byte read, so it is not a scan artefact, but
no register in 0x00–0x1F reads and a plain read returns `FF` eight times.
Not in the schematic's device list. Worth a look at the schematic for
anything at 0x2D (0x5A as an 8-bit address).

---

## 4. Traps

Each of these looked like a hardware failure and was not — or looked
fine and was not.

### The MAX6675 reports a temperature with no probe connected

With the K-type unplugged the part returns a well-formed frame, open-bit
clear, reading ~44 °C. Measured repeatedly. The driver cannot tell this
from a real reading, because by every structural check it *is* one.

Open detection on this part needs **T− grounded**. If the probe input
floats, the detector does not fire and the input reads as a plausible
temperature.

**This is a product-level risk, not a firmware bug.** A box whose probe
comes loose in transit keeps logging convincing numbers, which is worse
than logging nothing: the customer believes them. §4.1 requires probe
disconnection to be a fault, and today it cannot be detected.

Next step is a hardware measurement: is T− at the MAX6675 actually
connected to GND? Firmware cannot compensate — it has no information
that separates the two cases.

### The first MAX6675 conversion after power-up is not a measurement

175.5 °C, then 47.25, 46.75, 46.00. The conversion in flight when the
rail comes up belongs to whatever state the part was in before.
`temp_sample()` discards one frame and waits a fresh conversion. Do not
remove this.

### The PCF8523 forgets its crystal setting on every power loss

`CAP_SEL` returns to 7 pF; the schematic specifies 12.5 pF. Set wrong,
**the clock still runs, just at the wrong rate** — minutes a week, no
fault reported anywhere. `rtc_begin()` writes it on every boot. This is
not a production-time setting.

### The RTC has no backup supply (as drawn)

The RTC's VDD is 3V3_MAIN, which SW3 cuts; the VBAT_3V net has no cell
in the schematic, and `Control_3` comes up `0xE0`: switch-over disabled.
So switching the box off loses the time -- it was found reading
2020-07-02 with the oscillator-stopped flag set.

**This prototype has a CR1220 hand-soldered onto VBAT_3V (2026-10-02)**,
and `rtc_begin()` now sets `Control_3` to `0x00` on every boot:
switch-over in standard mode, battery-low detection on. `health` shows
the cell's state and whether it carried the clock through a power-off.
**Checked 2026-10-02:** SW3 off for a minute or two, then on: `Control_3`
read `0x08` (BSF: it ran on the cell), source `rtc`, time still right.
The README's suggested rework -- VBAT tied to VSS -- is for a board with
no backup at all; **do not do it on a board with the cell.** The next
board revision should have a cell or supercap on VBAT_3V by design.

Either way `rtc_time_valid()` stays the rule: **nothing may stamp a
record with a time the device cannot vouch for.** Without the cell, the
time comes back from NTP (seconds after Wi-Fi), GNSS (`$GNRMC`), or the
app. Never a default date.

### The BQ25601 silently undoes its own configuration

It runs a ~40 s I2C watchdog. Let it expire and every register returns
to its reset value with no indication except `REG09` bit 7. A charge
current set at boot quietly becomes the factory default minutes later.

And the kick bit shares `REG01` with the charge enable, so:

```c
i2c_write_reg(..., BQ_REG01, 0x40);           // WRONG
i2c_read_reg(..., BQ_REG01, &r, 1);           // right
i2c_write_reg(..., BQ_REG01, r | BQ_WD_RST);
```

The wrong version reads as "keep the charger alive" and actually turns
charging off every twenty seconds. It was in this codebase for a while.

### The GNSS module runs at 115200, not 9600

And at 9600 it still emits bytes — 488 of them in twelve seconds, all
nonsense. A test that only asks "did anything arrive" passes.

### The e-paper driver class decides the BUSY polarity, and they differ

| Panel | GxEPD2 class | busy level | full refresh |
|---|---|---|---|
| 2.13" (G), 4 ink — **what the board is designed for** | `GxEPD2_213c_GDEY0213F51` | **LOW** | 25 s |
| 2.13" (B), 3 ink | `GxEPD2_213_Z98c` | HIGH | 15 s |
| 2.13" mono — **what is currently plugged in** | `GxEPD2_213_B74` | HIGH | 3.6 s |

Run a mono panel under the (G) driver and it waits out a 50-second
timeout on a panel that was ready the whole time. It reads exactly like
dead hardware, and it sent this bring-up looking for a fault in the
boost circuit that does not exist.

The flex, the rail and the boost all work: images render.

### Opening *and closing* the serial port resets the board

So any state set by a console command is gone before anyone can act on
it. Three motion-detection tests in a row reported zero events on a
sensor that works perfectly, because the thing being tested was switched
off by the script that set it up. Anything that must be live while a
human does something physical has to be **armed in `setup()`**, not by a
command.

Scripts can avoid it: open the port with DTR and RTS held low
(pyserial: set `dtr = rts = False` *before* `open()`) and the board is
not reset. `platformio.ini` now does the same for `pio device monitor`
(`monitor_dtr = 0`, `monitor_rts = 0`, 2026-10-03), so opening the
monitor no longer resets the box. That is the only way to watch a box cycle through deep
sleep: the USB port disappears every time the chip sleeps and the
watcher has to reopen it without resetting what it is watching.

### GPIO47 and GPIO48 cannot be held in deep sleep unless VDD_SPI stays on

They are powered from VDD_SPI, which deep sleep switches off by default.
`gpio_hold_en` on a pad with no power holds nothing: GPIO48 falls to 0 V,
and 0 V is *LED rail on* through Q1 — the four pixels' idle current, all
night. It was the 5.75 mA asleep of the first battery night. `pm.cpp`
keeps `ESP_PD_DOMAIN_VDDSDIO` on through sleep (≤ 0.25 mA measured after).
Any new pin to hold in sleep: check its power domain first.

### The takeover title font has no lower case

`mColdTitle26` is cut to ' '..'Z' (`fonts_mcold.h`). A lower-case letter
is not drawn at all: "Battery empty" came out as "B". Titles in capitals.

### With no battery the fuel gauge reads the charger

MAX17048 is powered from CELL_PLUS, which the charger holds up with no
cell connected; the gauge models that as a cell and read 116 %. Above
4.28 V the firmware now reports "no battery", and quick-starts the gauge
when a cell is connected again.

### A box asleep cannot be flashed

With `sleep_usb 1` (or on battery) the chip is awake about a second in
every sample period and the USB port is gone the rest of the time;
`pio run -t upload` fails. Catch a wake and send `config set sleep_usb
0` first (it needs to arrive within that second -- the bench script
writes the moment the port appears), or hold BOOT while plugging in. On
real battery, plugging USB in wakes the box (PG#) and keeps it awake.

### Motion events on the bench with nobody touching it

During the sleep tests the accelerometer reported single Z (sometimes X
or Y) events every few minutes, often near a Wi-Fi start or stop. Three
panel refreshes in a row on USB produced none, so it is not the panel.
Not yet known whether it is the radio's current step on 3V3_MAIN (as
with the buzzer, see below) or the desk. Each one is a motion wake on
battery (at most one a minute). Worth a controlled test before P8.

One cause is known and fixed: switching the rails off at sleep entry
latched an event that woke the box 0 s later. The latch is now cleared
40 ms after the rails go off and the pins are held, before the motion
wake is armed (`pm_on_quiet`).

### A zero result from a test that needs human timing is not data

Related, and the more general lesson. Several results of "nothing
happened" were treated as evidence about the hardware when they were
evidence about the test. Prefer a signal the person can see directly —
an LED on the board answered the motion question immediately, with no
coordination at all.

### The buzzer sets off the accelerometer

Nobody touching the board: every beep fires the LIS2DW12 wake-up
detector on all three axes at once (`WAKE_UP_SRC 0x0F`). Two to four
events for a short beep, 47 for 1.6 s of beeping. The LEDs cause none.
Whether it is the coil shaking the board or its ~200 mA pulse on the
supply was not separated; the cure is the same.

So `main.cpp` ignores motion events while the buzzer sounds and for
250 ms after (`buzzer_recent()`), and counts them separately. Without
that, every alarm beep would log itself as a shock. A real knock inside
that window is lost too. P3 should know this when it sets shock alarms.

### PlatformIO does not sign: tools/sign_app.py does

`idf.py` pads (`--secure-pad-v2`) and signs the app when signed apps are
on; PlatformIO runs `elf2image` itself and does neither. Without the
padding the signature block is not where the checker looks; without the
signature the running image has no key to check an update against, and
every OTA fails with "No signatures were found for the running app".
`tools/sign_app.py` (an `extra_scripts` post-script) adds both, and
refuses to finish the build without the key. espsecure needs
`cryptography` and `ecdsa` in `~/.platformio/penv` (installed
2026-10-04; a fresh PlatformIO install needs them again).

### A new image must not deep-sleep before it is confirmed

With rollback on, a new image boots "pending verify"; any reset before
`esp_ota_mark_app_valid_cancel_rollback()` -- and a deep-sleep wake is a
reset -- makes the bootloader treat it as failed and go back. `ota.cpp`
holds `Hold::Ota` until the broker answers (or 3 minutes pass), and the
uplink opens a session at once for it, records or not.

### The USB isolator browns the board out

Any load step — the SD/GNSS rail, the buzzer, an e-paper refresh —
resets the board through it (`reset reason 9`). On a normal port all of
it passes. Bench infrastructure, not the product, but it costs an hour
every time it is rediscovered.

---

## 5. Decisions, and why

**ESP-IDF, not Arduino.** §13 asks for it, and `sdkconfig` is what makes
`CONFIG_PM_ENABLE`, the task watchdog and core dumps reachable — none of
which exist under the Arduino framework alone.

**No Arduino component either.** It was tried, to keep GxEPD2 for the
display. It drags ESP RainMaker and ESP Insights in behind it, a cloud
SDK this product has no use for, and the build fails looking for a TLS
certificate to embed. The display is phase 4 and blocked on the real
panel, so the dependency bought nothing today and cost every build.
Revisit at phase 4: either port the one panel's command sequence out of
GxEPD2, or bring Arduino back as a separate component then.

**The project lives at `C:\mCOLD`.** ESP-IDF refuses a project path
containing a space and the repo is under `Foam V.1`. Not configurable.

**Low battery, by voltage (decided 2026-10-03).** Below `batt_trip_mv`
(3.55 V, ~10 %) on battery no new trip starts (`BATTERY_LOW`); below
`batt_off_mv` (3.40 V, ~5 %), three readings in a row, the box logs
`POWER_OFF`, draws BATTERY EMPTY and puts the BQ25601 in ship mode, so
the cell feeds nothing but the fuel gauge until USB comes back. Voltage,
not the gauge's percent: the gauge has no model of this cell yet.

**Lights cost the chip being awake, not the LEDs (2026-10-03).** Front
lights 2 %, the alive blink every `led_status_s` (15 min) on battery, a
cargo alarm at every wake, the motion attention window 2 s on battery
(30 s on USB). The 30 s window at four an hour was ~5 mA on its own.

**60 s is the fastest sampling this partition layout supports.** At
300 s the log holds 269 days, at 60 s it holds 54, at 30 s only 27 —
short of the one-month requirement. Anything faster needs the layout
revisited, not a config change.

---

## 6. Architecture

`app_main()` creates every task deliberately; there is no `loop()`.

The rule the whole shape serves: **a box that stops logging temperature
because the GNSS went quiet has failed; a box that logs for a week and
reports "no position" has not.** So no task waits on another, no task
holds a bus across a long operation, and no device failure propagates
past the task that owns it.

- **`health`** — every device has a state. `Unknown` (never tried),
  `Ok`, `Degraded` (worked before, failing now), `Failed` (never
  worked), `Absent` (not fitted, e.g. no SD card). Degraded and Failed
  are separate because they answer different questions: *built wrong*
  versus *something came loose*. Failed devices back off to one retry a
  minute and recover on their own if the fault clears.

- **`bus`** — one gate per bus, every call timed out, every outcome
  turned into a health record in exactly one place so a driver can
  neither forget to report nor report twice. Register reads use a
  repeated start: a stop would let the NFC tag's RF side interleave and
  return another device's data.

- **`rails`** — reference counted. The display shares a rail with the
  thermocouple and the SD card shares one with the GNSS, so "I am done
  with the display, turning it off" is wrong whenever a temperature
  read is in flight. Settle times are measured, not guessed: the LED
  rail needs 50 ms or the pixels never latch their first frame.

- **`supervisor`** — watches heartbeats and reports stalls. It does not
  restart anything. A box that reboots loses its state and its time, and
  a reboot loop costs more than a sensor that limps.

- **`pm`** (P7) — deep sleep on battery. A wake from deep sleep is a
  reset to the CPU, so a wake runs as a short boot (`pm_warm()` tells a
  module it is one). Three inputs, kept apart: **duties** (each module's
  one pass for this wake, reported with `pm_done`), **holds** (something
  sleep must not cut: BLE, a phone on the tag, a GNSS or Wi-Fi session,
  a refresh, a light pattern, the console), and **next** (when each
  module next needs the chip, `pm_next`). All duties done and no holds:
  sleep until the earliest next. USB power never sleeps. A duty missing
  after 60 s, or holds that are not a person's after 10 minutes, do not
  keep the box up -- an awake box on battery is empty in a day and a
  half. State that must cross a sleep is `RTC_DATA_ATTR` (reloaded on
  any other reset, so it means exactly "this boot, through its
  sleeps"); the wake record (`sleep` on the console) is `RTC_NOINIT`
  so it survives the console reset too. Intervals that cross a sleep
  use `mono_ms()`, never `esp_timer`, which restarts at every wake.

**Values and their validity are always separate.** A stale reading with
a flag beside it is honest; a stale reading on its own is a lie shaped
like data. No device failure is ever rendered as 0.

---

## 7. Phases

Each phase ends in a release `0.N.0`, verified on the board. The repo
README once carried an older 14-step plan; this is the one in use.

| Phase | Release | Scope | Waiting on |
|---|---|---|---|
| P0 skeleton | 0.0.1 ✅ | ESP-IDF project, partition table, health registry, rails, guarded buses, tasks + supervisor, console | -- |
| P1 drivers | 0.1.0 ✅ | every on-board device: MAX6675, PCF8523, power chain, LIS2DW12, GNSS, ST25DV, LEDs, buzzer; side light shows charge state | a GNSS fix has never been seen |
| P2 time/config/storage | 0.2.0 ✅ | UTC with quality + boot counter; NVS config; power-cut-safe trip log | -- |
| P3 trip and sensing | 0.3.0 ✅ | trip state machine (§8, state axes kept separate); START/STOP with a trip header carrying that trip's alarm thresholds; a sample every 5 min into the log; events (motion, probe fault, alarms, reset/data gap); alarm engine (thresholds, hysteresis, dwell; ack never erases history); resume after a reset mid-trip; retention when full (§9.6). Driven from the console until BLE exists. Door switched off | shock threshold |
| P4 display, LED, buzzer | 0.4.0 ✅ | e-paper driver and screens from `display-mock`, live from state, on the mono panel in hand; LED and buzzer patterns per `docs/led-design.md` (never together); attention window | the four-ink panel (a driver table entry when it comes) |
| P5 BLE/NFC/protocol | 0.5.0 ✅ | NimBLE GATT advertising the SN; the §13 command set with request IDs and idempotency; NDEF record; tap brings BLE up; tap to authorize (decided 2026-10-02). NFC GPO wake moves to P7 | the iOS team's answer to PROTOCOL.md |
| P6 sync, USB, OTA | 0.6.0 ✅ in part | Done: Wi-Fi (5 networks), NTP, MQTT to the team's broker, live and backlog upload, application ACK then reclaim. Deferred: USB mass storage with CSV, SD archive, OTA (signed, rollback) | the server team: ACK, TLS, per-device logins; a file server for OTA |
| **P7 power** | 0.7.0 | deep sleep between samples; every wake source (timer, accel, door, NFC, PG#); GPIO holds; log head kept in RTC memory; charger and PD policy; current measured until 7 days on 1500 mAh is shown | battery datasheet (RCOMP, charge limits) |
| P8 hardening | 1.0.0 | watchdog and core-dump retrieval; quiet production logging; flash encryption / secure boot decision; the §14 acceptance cases; 31-day offline simulation; 6-device Dock; factory provisioning | -- |

P7 is where power is *measured*, not where it is designed: the event
loop and rail discipline have to be right from P1 or they get rebuilt.

## 8. Still open

- **T− grounded at the MAX6675** (hand-soldered 2026-10-03). Probe-fault
  detection not yet tested; the panel showed `--` that evening with the
  probe plugged in -- check the trip for PROBE_FAULT events
- **Awake current ~130 mA** with a ~40 mA floor: ~90 mA not yet found
  (not the CPU clock, not the radios; idle signal pins only 15–25 mA)
- **NFC tap wake** from deep sleep: armed (GPO, EXT1) but not yet tried
  with a phone
- The 4-colour panel is not in hand; busy polarity and 25 s refresh
  cannot be verified without it
- **Server is MQTT** (decided by the team, 2026-10-02): broker
  `siamatic.co.th:1883`, run by the team themselves. Login is in the
  team's hands; it goes in `secrets.ini` (gitignored, template in
  `secrets.example.ini`), never in the repo. Not yet checked from here.
  Still open, and all of it is P6:
  - **1883 is plaintext** and the login is shared. §10.4 asks for TLS
    with server certificate checking. Ask the team for 8883 + TLS and
    per-device credentials before anything ships.
  - **Topic layout and the application ACK.** A PUBACK only proves the
    broker got the message, not that the server stored it (§9.5). The
    server must publish an ACK naming device, trip and sequence range,
    on a topic the device subscribes to, before the device may reclaim.
- **OTA works** (2026-10-04, `ota.*`): the server publishes a file name
  on `mcold/v1/<sn>/firmware` (retained), the box fetches it from the team's
  file server (the base URL eTEMP uses, in NVS) and checks header,
  SHA-256 and signature; rollback if the new image does not reach the
  broker in 3 minutes. Tested on the bench through MQTTX: a wrongly
  signed image refused ("signature bad"), 0.7.0-dev -> 0.7.0-dev.1 in
  about 15 s of download, confirmed. Not yet tried: a real rollback
  (`ota rollback-test`), and an update on battery.
- **The OTA signing key** is `firmware/keys/ota_signing.pem`, gitignored.
  A box accepts only images signed with the key its *running* image was
  signed with, so losing the key means every box in the field has to be
  reflashed over USB. **Keep a copy off this PC.** No eFuse is burned:
  USB flashing always works, and hardware secure boot stays a P8
  decision.
- BLE UUIDs and the NDEF schema need the iOS app team. Note: **iOS
  cannot see a BLE MAC**, so the device must advertise its SN and the
  app must match on that. The app team has confirmed the MAC is only a
  tiebreak when device IDs collide
- **Temperature alarm thresholds are set per trip**, by the app at
  START_TRIP (decided 2026-10-02) -- not device config. P3 puts them in
  the trip header. **Sample period: 5 min** (300 s, the config default)
- Shock threshold — the only real data point is that lifting the board
  by hand exceeds 1.45 g, which is not enough to set one
- Battery datasheet, so MAX17048 `RCOMP` and the charger limits can be
  set rather than left at defaults. SOC currently reads low after a deep
  discharge; ModelGauge needs a full charge cycle before it is worth
  judging. The user sees the percent fall fast near 90 % (surface
  charge relaxing after a full charge, and the gauge reading voltage
  under the 130 mA of a wake): a state of charge that counts current
  is being added (P7)
- `bringup/` and the `bench/epd29-s3` edits, recovered from the zip copy,
  are on branch `bringup/import` (pushed 2026-10-02), waiting for a pull
  request into `main`
