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

Firmware **0.2.0** released (tag `firmware/v0.2.0`, ELF sha256 `9f5ca65c3...`).
Running on the board:

```
RAM   6.5% (21,212 B)      Flash 13.6% (427,364 B of a 3 MiB OTA slot)
```

P0, P1 and P2 complete. Next: **P3**, the trip state machine and
sensing -- which is where the trip log first gets real records.

**Git.** The firmware lives in the shared mCOLD repo under `firmware/`,
with its full history (imported 2026-10-02; until then it was a repo of
its own, so commits before that have firmware files at the root of
their tree). Everything firmware is namespaced so it stays apart from
the rest of the repo:

| | |
|---|---|
| `firmware/vX.Y.Z` | release tags, annotated with the release notes |
| `firmware/main` | released firmware only; merged into `main` by pull request |
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
| `config.*` | NVS settings from one table of ranges and defaults; schema 2; over-long keys fail the build |
| `flashlog.*` | the trip log: CRC'd 128-byte frames, one trip per sector, power-cut safe; `logtest` runs 14 power-cut tests on the device |
| `main.cpp` | 7 tasks + supervisor; console: `help` lists the commands |

Checked on the board by a person, 2026-10-02: tap → motion event;
phone → NFC arrival; all four LED colours and positions (`ledtest`);
side light breathes yellow while charging below 80 %, green above.

| Still unverified | |
|---|---|
| **GNSS fix** | never seen on this board. Indoors it reported **0 satellites in view** -- not even a weak one. Either the room is that shielded or the antenna path does not work. Next time outdoors: leave it 10 min and read `gnss` |
| Side light going out on unplug | follows PG# directly; not yet watched |

| For P3 to pick up from the trip log | |
|---|---|
| When a trip may be deleted | the log only erases on request. Policy (§9.6): after server ACK, or oldest completed trip first when full, with a loss record first. Never the active trip |
| `log_meta` partition | still unused. Planned: trip table, ACK checkpoints, loss records -- a journal that can be rebuilt from `trip_log` |
| Sample record schema | frame payload is 112 bytes, little-endian fields, never a C struct (§9.4) |
| **Boot scan per wake** | the scan takes 168 ms. Once the box deep-sleeps between 5-minute samples, rescanning every wake costs ~5 mAh/day (~3 % of the budget). Keep the head position in RTC memory across sleep instead |
| Trip id `0xBE5C0001` | reserved for the console's bench records; real trip ids must never reach it |

**Do not run `nfc write` casually.** The tag currently holds a JSON
NDEF record from bring-up (`{"sn":"MCOLD-9A74","ble...`), possibly the
one the iOS team tested against. `nfc write` replaces it with a plain
Text record.

| Not written yet | |
|---|---|
| Door sensor (GPIO7) | trivial GPIO; belongs with the trip logic in P3 |
| HUSB238A | answers at 0x42; PD policy is §5.5 |
| Display | blocked: see §5 |
| Storage, trip state machine, BLE, Wi-Fi, USB MSC | phases 2 onward |

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

### Unresolved

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

### The RTC has no backup supply

`Control_3` reads `0xE0`: battery switch-over disabled, and the VBAT net
has no cell fitted. When the main rail drops, the time is gone — it was
found reading 2020-07-02 with the oscillator-stopped flag set.

So `rtc_time_valid()` exists, and **nothing may stamp a record with a
time the device cannot vouch for**. Re-sync from GNSS (`$GNRMC` carries
date and time) or from the app. Do not substitute a default date.

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

**Values and their validity are always separate.** A stale reading with
a flag beside it is honest; a stale reading on its own is a lie shaped
like data. No device failure is ever rendered as 0.

---

## 7. Phases

P0 skeleton · P1 drivers · P2 time/config/storage ·
**P3 trip state machine and sensing (next)** · P4 display, LED, buzzer ·
P5 BLE/NFC/protocol · P6 Wi-Fi sync and USB MSC/CSV ·
P7 power measurement · P8 hardening

P7 is where power is *measured*, not where it is designed: the event
loop and rail discipline have to be right from P1 or they get rebuilt.

## 8. Still open

- **T− grounding at the MAX6675** — blocks honest probe-fault detection
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
- **OTA over MQTT** is possible, two ways, and either needs the
  rollback that the `ota_0`/`ota_1` layout already allows:
  1. *Recommended:* MQTT carries only the command (version, URL,
     SHA-256, size); the image comes over HTTPS with `esp_https_ota`.
     Needs a file server.
  2. Image in chunks over MQTT itself, written with `esp_ota_write`,
     each chunk sequenced and acknowledged. Works with only the
     broker, but costs more code and is slower.
  On a plaintext broker with a shared login, anyone who has that login
  can push firmware to every box. So **signed images are not optional**:
  the device must check the signature before it boots a new image
  (Secure Boot v2 / signed app verification in ESP-IDF).
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
  judging
- `bringup/` and the `bench/epd29-s3` edits, recovered from the zip copy,
  are on branch `bringup/import` (pushed 2026-10-02), waiting for a pull
  request into `main`
