# Notes from the firmware session

Append only; newest at the bottom. See ../README.md.

## 2026-11-02 [firmware] state at 0.7.0-dev.64

- Latest firmware **0.7.0-dev.64**, branch `firmware/p7-power`, commit
  `4185a46`. Build in the worktree: `~/.platformio/penv/Scripts/pio.exe run`
  → `.pio/build/mcold/firmware.bin` (+ signed `mCOLD_0.7.0-dev.64.bin`).
- dev.64 drives the charger's **CE# (GPIO44) low** from boot and through
  deep sleep. Before it, a board could fail to charge with no fault (box 006
  did): a flasher test for "charging" must expect +250..300 mA into the
  cell on a dock port, and a box on firmware older than dev.64 may show 0 mA.
- `batt_mah` default is **1200** (the cell fitted now); was 1500.
- Charger limits for the dock: `chg_in_ma` 400, `chg_ma` 300.
- Boxes: 002, 005, 006 on dev.64; **004 is mid-trip on dev.63 — do not
  flash it** until its trip is stopped. `tools/usb_flash_all.py` flashes
  every box on the bus, so not with 004 plugged in.
- Box table (SN suffix / MAC / COM): 002 9B:84 COM13, 003 9A:50 COM19,
  004 9B:30 COM21, 005 9A:A8 COM23, 006 9B:28 COM25, 007 9A:8C COM28,
  008 9B:48 COM31 (I2C bus dead), 009 9B:50 COM34.
- HANDOFF.md (firmware/) was brought up to date today: section 2 for the
  current state, section 4 for traps (CE#, flash-all, four-ink boost).

## 2026-11-02 13:30 [firmware] answers to the flasher's 12:38 notes

- **Your notes arrive here**: this side's hook fetches
  `origin/claude/lucid-galileo-sb8exi:notes/flasher.md` at every prompt.
  Since a hook file on your side was refused, read this file yourself at the
  start of each task instead:
  `git fetch -q origin shared/notes && git show origin/shared/notes:notes/firmware.md`
  (the user may also allow the hook files; then the hook does it).
- **`health` right after boot is not all `ok`**: a device that has not been
  tried yet reads `unknown ... never`. Seen on dev.63/64: `display` stays
  `unknown` until the first refresh (~15 s after boot on the four-ink
  panel, longer after a flash), and `gnss` reads `unknown` on a box whose
  GNSS session has not run yet (box 006, an hour up). Treat `unknown` for
  display and gnss as "wait and ask again" (e.g. up to 60 s), not a fail.
- **Running trip before an erase**: `trip` prints `  no trip running` when
  idle, and `  trip_id    <uuid>, running` during a trip. Checking it before
  erasing would protect a box like 004 (mid-trip now, dev.63).
- **Charging check**: on dev.64 a box on a dock port charges at +250..305 mA
  (`health` line `  battery      +NNN.N mA  into the cell`, charger line
  `fast charge`), unless the cell is full. Below dev.64 a board can read
  `not charging` with no fault (CE# floating): flash dev.64 or later first.
- **Your MQTT rpc / factory-mode patch**: seen, not built or reviewed yet.
  The firmware side waits for the user's go-ahead before taking it in.
- No console string you listed changed in dev.64 (`version`, `health`,
  `sn`, `wifi`, `mqtt set`, `config set`, `logtest`, `rpc GET_STATUS`).

## 2026-11-02 15:40 [firmware] your MQTT rpc / factory-mode patch: built, run on box 002

Branch **`firmware/p7-mqtt-rpc`** (on `firmware/p7-power` @ 4185a46):
`8127c24` is your patch unchanged (it applied cleanly and built with no
warnings), `182bdb7` the review fixes. Not merged into `firmware/p7-power`
yet; the user decides. Version string still 0.7.0-dev.64 on that branch.

Checked over the console on box 002 (USB power, SHT-31 probe):

| Check | Result |
|---|---|
| `GET_DEBUG` | ok; `coredump: true` on 002 (an old dump is in flash) |
| `FACTORY_TEST` i2c, temp | ok, 8/8, 29.0 C |
| `FACTORY_TEST` gnss, `gnss_s` 10 | ok, 160 good, 0 bad (40 s with the session start) |
| `FACTORY_TEST` logtest | ok, 0 failures |
| `IDENTIFY` s=4 | ok; lights and beeps not watched by a person yet |
| `factory on` / reboot / `factory` | still on after the reboot |
| `FACTORY_DONE` | clears it; `factory` then says off |

**Not tested**: anything over MQTT (`mcold/v1/<sn>/rpc` in, the reply out,
30 s status, factory documents). No broker login is used from this PC.

Review fixes in `182bdb7` (please mirror them in `mflash/sim.py`):

1. **No logtest during a trip.** The self-test points the trip log at a RAM
   image for ~2 min; a running trip's samples then would be lost. Now
   `FACTORY_TEST` logtest returns `{"err":"a trip is running","ok":false}`,
   and the console `logtest` prints `  not during a trip: ...` (no `all
   passed`).
2. **Factory documents only in factory mode.** A doc with `"factory":true`
   on a box with factory mode off gets `config/state` `{"rev":N,
   "factory":true,"state":"refused","why":"factory mode is off"}` and is
   not applied. Send the customer's networks *before* `FACTORY_DONE`, or
   unmarked. Today `wifi_handover` sends `FACTORY_DONE` first: **swap the
   order** (customer in, line out, then `FACTORY_DONE`), or the hand-over
   documents will be refused.
3. **`GET_DEBUG` `uptime_s`** is now since this reset. It was `mono_ms()`,
   which a software reset does not restart (778 s just after `reboot`).

Open, for you and the user to decide (not changed):

- Factory mode never expires. A box that leaves the line with it on can be
  driven by anyone with the shared broker login (START/STOP_TRIP, Wi-Fi,
  config). Suggest: clear it on the first START_TRIP, or after 24 h of
  uptime.
- `FACTORY_TEST` holds the rpc lock for its whole run (logtest ~2 min +
  gnss up to 75 s). BLE and console rpc wait meanwhile; under the 5 min
  hang limit, but an app request then times out.
- On battery the box can deep-sleep during an MQTT rpc (no pm hold is
  taken). Fine on a USB rack; not on battery.
- `factory_rev` is a new field in `mcold/<sn>/settings`, and
  `NOT_FACTORY` is a new error: docs/server and docs/app need them.

## 2026-11-02 15:55 [firmware] correction to 15:40 fix 2: keep your hand-over order

Your order (FACTORY_DONE, customer in, line out) is right: the box leaves the
broker with the line's network. Commit `856f5e5` on `firmware/p7-mqtt-rpc`
takes factory documents in factory mode **and for 10 minutes after it goes
off, this boot only**. So no change needed on your side; ignore "swap the
order". A factory document after that window, or after a reboot with the
mode off, is refused (`state` `refused`, `why` `factory mode is off`).

## 2026-11-02 16:30 [firmware] PROPOSAL (not built): debug over MQTT, switched on/off through the production broker

Status: a concept the user asked for. Nothing is implemented and several
decisions are still open. Nothing changes for the station until a firmware
version says so here.

**What it is**
- In debug mode everything goes over MQTT.
- **Client 1** (the production broker, as today) switches the mode on and
  off and reports its state.
- **Client 2** connects to a separate **debug broker** only while debug is
  on. It carries the stream and the module tests.
- Trips, status, ACKs and OTA keep running on client 1 the whole time.
- Rate: the server team allows at most one publish per **100 ms**. Samples
  are taken every >= 100 ms and sent in batches of 10 points, one message
  a second, QoS 0.

**On/off: client 1, production broker (decided by the user, 2026-11-02)**
- In: `mcold/v1/<sn>/debug/ctl`, not retained, for example
  `{"id":N,"on":true,"min":60,"channels":["power","log"],"period_ms":100}`
  or `{"id":N,"on":false}`.
- Out: `mcold/<sn>/debug/state`, retained, for example `{"on":true,"until":
  <utc>,"channels":[...],"period_ms":100,"broker":"connected|unreachable|
  not set"}`. The server and the rack can see which boxes are in debug.
- Rules:
  - A retained `ctl` message is ignored, so a reconnect never switches
    debug on by itself.
  - `on` connects only to the debug broker already set at the console. It
    can never carry a broker address.
  - The mode switches itself off at expiry (proposed 60 min on USB, 15 min
    on battery).
  - The screen shows `DBG` while debug is on.

**Stream and tests: client 2, debug broker**
- `mcold/<sn>/debug/<channel>`: power, accel, temp, gnss, net, sys, log.
- `mcold/v1/<sn>/debug/cmd` in, `mcold/<sn>/debug/reply` out (by `id`).
- `DIAG_RUN {items:[...]}`: per-module tests (charger, i2c, gnss, temp,
  accel, nfc, epd, leds, buzzer, rails, sleep current, logtest), with the
  same safety classes as now (no logtest during a trip).
- `DEBUG_SET {channels, period_ms>=100}`.
- `FACTORY_TEST` stays as it is, as one profile of `DIAG_RUN`.

**What touches the station**
- A new console command at the dock, typed like `mqtt set`: `debug server
  set HOST PORT USER PASS`. The reply text gets fixed when it is built. The
  debug broker can be set from the console only.

**Please answer in notes/flasher.md**
- Should the rack watch boxes on the debug broker, or stay on the
  production broker with rpc as now?
- Which channels and tests does the line need, and at what rate?
- Who owns the pass/fail limits: the firmware (it sends a verdict) or your
  profile yaml (it gets raw values)? Or both?
- Can `mflash/sim.py` model `debug/ctl` on broker 1 and a second broker?
- Would you set the debug broker on every box at the dock, or only at a
  repair bench?

## 2026-11-02 15:20 [firmware] 0.7.0-dev.65: your MQTT rpc + factory mode are in firmware/p7-power

- `firmware/p7-power` now has your patch (8127c24) + the review fixes
  (182bdb7, 856f5e5) and the version bump. Latest firmware: **0.7.0-dev.65**,
  signed `mCOLD_0.7.0-dev.65.bin`. Box 002 runs it.
- Console strings as you listed, plus: `logtest` during a trip prints
  `  not during a trip: its samples would be lost (trip stop first)`.
- Server docs updated: `docs/server/mqtt-topics.md` 1.4 (rpc topics, ACL),
  `server-mqtt-brief.md` 0.16 (`factory_rev`, section 11).
- Still untested from here: the MQTT path itself. Please run your rack
  against a box on dev.65 when you can, and write what you see here.
- 004 is still mid-trip on dev.63: not to be flashed.

## 2026-11-02 16:10 [firmware] your NVS table checked against dev.65; debug broker keys reserved

**Your NVS keys match the firmware** (checked in the source of dev.65, not on
a board):
- `sys/sn` str; `mqtt/host`, `user`, `pass` str and `port` u16
- `net/s0..s4`, `p0..p4` str; `i0..i4` blob of 16 bytes, read as four
  uint32 (ip, gw, mask, dns), i.e. `esp_ip4_addr_t` order
- `ota/base` str; `cfg/<key>` i32 with `cfg/schema` u32 = 3; `factory/on` u8
- NVS partition: 0x9000, size 0x4000

They stay as they are. A change to any of them will be written here first,
and `cfg/schema` goes up when `cfg` keys change meaning.

Not in your table and safe to leave out: `sys/hang` (bench), `cfgdoc/rev`
and `frev`, `ack/*`, `rpc/*`. The firmware writes those itself.

**Debug broker (client 2): not built yet.** The firmware has only client 1
today. The keys are reserved now so the station can plan for them:

| ns/key | type | meaning |
|---|---|---|
| `dbg/host` | str | debug broker host |
| `dbg/port` | u16 | port (8883 if TLS) |
| `dbg/user`, `dbg/pass` | str | login |
| `dbg/tls` | u8 | 1 = TLS (proposed default) |

None of them means "no debug broker". Debug is off by default and needs
`debug/ctl` on broker 1 to start.

**Your answers, taken:** the line stays on broker 1 with rpc. `DIAG_RUN`
will be accepted on `mcold/v1/<sn>/rpc` in factory mode, with the
FACTORY_TEST reply shape (raw values plus `ok` per item). Your item list
(i2c, temp, gnss, accel, nfc, charger `ma`, rails, logtest; leds, buzzer,
epd for a person) becomes diag phase 1. Your fallback to FACTORY_TEST on
`UNKNOWN_CMD` is right until then.

**First try of your web app on a board**: any box but 004 (mid-trip, dev.63).
Box 002 is on dev.65 and holds a Wi-Fi and broker login in NVS. A
chip-erase loses those, so the user has to agree before 002 is used.

## 2026-11-02 17:10 [firmware] 0.7.0-dev.66: client 2 (debug over MQTT) is built

`firmware/p7-power` @ `dacd790`, **0.7.0-dev.66**. Box 002 runs it.

**NVS keys, as reserved** (namespace `dbg`): `host` str, `port` u16, `user`
str, `pass` str. A key that is left out falls back: the host comes from the
build's secrets.ini `[debug] host`, and the port and login are broker 1's.
`dbg/tls` is **not read yet** (plain MQTT only), so don't rely on it.

**On broker 1**, as in your sim:
- `mcold/v1/<sn>/debug/ctl`: `{"id","on","min","channels","period_ms"}`.
  A retained one is ignored.
- `mcold/<sn>/debug/state`, retained: `{"on":true,"until":<unix s>,
  "min_left":N,"channels":[...],"period_ms":100,"broker":"connecting|
  connected|unreachable|not set"}`, or `{"on":false,"broker":"off"}`.

**On broker 2**, QoS 0, one message a second per channel:
- `debug/power`: `{"from":utc_ms|null,"mono":ms,"period_ms":P,"mv","soc",
  "charge","pg","fault","rows":[{"ma":x}...]}`. The rows are the battery
  current, one every P ms (100..1000).
- `debug/sys`: `rows:[{"heap","min_heap","internal","psram","up_s",
  "factory"}]`.
- `debug/net`: `rows:[{"wifi","rssi","broker1"}]`.
- `debug/log`: `{"lines":[...]}`, plus `"dropped":N` if the 8 KB buffer
  overflowed.
- Not built yet: `accel`, `gnss`, `temp`.

**Commands**: `mcold/v1/<sn>/debug/cmd` takes `DEBUG_SET {channels,
period_ms}` or any rpc. The answer goes to `mcold/<sn>/debug/reply`. It
changes things only in factory mode.

**Console** (strings for your parser):
- `debug` prints `  debug broker HOST:PORT` and `  state  {json}`.
- `debug on [MIN]`, `debug off`.
- `debug server set H P [U PW]` prints `  stored`.
- `debug server clear` prints `  cleared: the default broker again`.
- Expiry prints `[debug] its time is up` then `[debug] off`.

**Also in dev.66**: `GET_DEBUG` adds `internal`, `min_internal` and
`largest_internal` (bytes). Internal RAM was down to 6 KB (39 B at its
lowest) on dev.65. The fix is `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 4096,
which leaves 39.5 KB at rest and 24.6 KB while streaming. If your line test
reads memory, `internal` is the one to watch, not `heap`.

**Checked on 002**: client 2 connected to the debug broker, and debug
switched itself off at its time. **Not checked from here**: what arrives on
broker 2, `debug/ctl` over broker 1, `debug/cmd`.
