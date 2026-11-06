# Firmware: commands over MQTT and factory mode

**Status: PROPOSED, 2026-10-29. Written against `firmware/p7-power`
(4185a46, 0.7.0-dev.64); not yet built or run on a board.** The patch is
[`firmware-mqtt-rpc.patch`](firmware-mqtt-rpc.patch) (`git apply` on that
branch). The station side (`mflash/remote.py`) and the simulated box
(`mflash/sim.py`, `SimNet`) implement exactly what is written here, and
`tests/test_rack.py` runs the whole line against the simulation.

## Why

The dock has six slots and a box sat in one for 5–10 minutes. The quick
route keeps a box on USB only for what needs the cable -- the firmware,
the SN, the line's Wi-Fi with a fixed address, the broker (console only,
by decision of 2026-10-05) -- about 1.5 minutes, then finishes it on a
rack of USB chargers over Wi-Fi -> MQTT, dozens at a time. The firmware
already has every topic the rack needs to *watch* a box (`status`,
`online`, `settings`, `config/state`, `ota/state`) and to *configure* it
(the settings document, OTA). What it lacks is a way to *test* a box and
*ask it questions* remotely. That is this change.

## 1. The rpc channel

| Topic | Direction | QoS | Retained | Payload |
|---|---|---|---|---|
| `mcold/v1/<sn>/rpc` | station → device | 1 | **no** | one request, as over BLE: `{"id":N,"cmd":"...",...}` |
| `mcold/<sn>/rpc/reply` | device → station | 1 | no | its answer, with the same `id` |

- `rpc_handle()` is the same as for BLE and the console (PROTOCOL.md §3–4),
  so every read command works here too: `GET_INFO`, `GET_STATUS`,
  `SELF_TEST`, `GET_SETTINGS`, ...
- Requests are queued (4 deep, 1 KB each) and run one at a time by a task
  of their own (`mqtt_rpc`), so `FACTORY_TEST` -- minutes -- never holds up
  batches, ACKs or the status. A request that finds the queue full is
  dropped; the station asks again after its timeout.
- The session is **authorized only in factory mode**. One broker login is
  shared by every box today (PROTOCOL.md §6, open question 2), so on a
  shipped box anyone with that login could otherwise write to it. Outside
  factory mode, commands that change something answer `NOT_AUTHORIZED`.

## 2. Factory mode

A flag in NVS (`factory`/`on`).

| How | |
|---|---|
| `factory on` at the console | the dock's quick stage, with the cable: `factory mode on` |
| `factory off` at the console | by hand, at a repair bench |
| `FACTORY_DONE` over MQTT | the box clears it itself at the rack's hand-over |
| `factory` | prints the state |

While it is on, the status goes out every **30 s** instead of every five
minutes, so the rack sees a box soon after it is put down.

## 3. New commands

`✎` = changes something: authorized link only, idempotent by `id`.

| Command | ✎ | Arguments | Result |
|---|---|---|---|
| `FACTORY_TEST` | ✎ | `items` (subset of `i2c`, `temp`, `gnss`, `logtest`; none = all), `gnss_s` (3–60, default 15) | one object per item, each with `ok`; `ok` at the top only if all are |
| `IDENTIFY` | ✎ | `s` (1–30, default 10) | every light blinks white and the buzzer beeps, for `s` seconds; answered after |
| `GET_DEBUG` | | | `reset`, `boot`, `uptime_s`, `coredump`, `heap`, `min_heap`, `psram`, `ip`, `ssid`, `wifi_fail_reason`, `fw`, `elf`, `factory_mode` |
| `FACTORY_DONE` | ✎ | | clears factory mode |

`FACTORY_TEST`, `IDENTIFY` and `FACTORY_DONE` also refuse with
`NOT_FACTORY` unless factory mode is on or the request came from the USB
console: a phone that has tapped the box can authorize BLE, but cannot
run them.

```json
{"id":7,"cmd":"FACTORY_TEST","items":["i2c","temp","gnss","logtest"],"gnss_s":15}
{"id":7,"ok":false,
 "i2c":{"sda":true,"scl":true,"missing":[],"found":8,"expected":8,"ok":true},
 "temp":{"state":"ok","sensor":"MAX6675","c":23.5,"ok":true},
 "gnss":{"good":0,"bad":0,"bytes":0,"ok":false},
 "logtest":{"failures":0,"ok":true}}
```

What each item checks is what the bench commands check, with the same code:

| Item | Same as | Passes when |
|---|---|---|
| `i2c` | `i2c` | all of 18 36 40 42 53 57 68 6B answer |
| `temp` | `temp` / `health` | the probe reads, between −40 and 85 °C |
| `gnss` | `gnss` | ≥ 3 sentences with a good checksum in `gnss_s` (bytes alone do not count). The GNSS task keeps the UART; the test holds its session open (`gnss_hold_for`, like `gnss hold`) and reads its counters |
| `logtest` | `logtest` | `flashlog_selftest()` reports no failure (~2 min) |

## 4. The line's settings documents

The rack sends settings documents on `mcold/v1/<sn>/config` as the server
does (`docs/app/device-settings.md`), marked `"factory": true`:

```json
{"rev":2,"factory":true,"wifi":{"add":[{"ssid":"Customer-AP","pass":"...","dhcp":true}]}}
{"rev":3,"factory":true,"wifi":{"del":["mCOLD-Line"]}}
```

They are counted by their own revision (`frev`, NVS `cfgdoc`/`frev`;
reported as `factory_rev` in `settings`), never the server's (`rev`,
`server_rev`). Otherwise the line's revisions would be the box's
`server_rev` when it ships, and the customer's server, starting at 1,
would be ignored. `config/state` carries `"factory":true` for them. The
station clears the retained message after the hand-over.

Without this patch the rack still works, but sends documents without the
marker and with a rev above `server_rev`; the server must then start above
that.

## 5. What the rack does with it (profile `rack:`)

1. **online**: `online` = "1" and a `status`; `net.ip` = the pool's address
   for the unit, `fw.ver` = the version flashed at the dock
2. **status**: `temp.ok`, `power.external`, temperature in range
3. **rpc** `FACTORY_TEST`
4. **identify_confirm**: `IDENTIFY`, then a person answers "box 007 blinking
   and beeping?" (yes / no / again)
5. **debug_pull**: `GET_DEBUG` and every retained message of the box to
   `records/debug/<sn>/<time>.json`; `coredump` must be false
6. **config_doc**: the site's `config` keys and OTA base
7. **ota** (off by default: the dock flashed the chosen build)
8. **wifi_handover**, always last: `FACTORY_DONE`, then the customer's
   networks in, then the line's network out. The box leaves the line's
   Wi-Fi when it applies the second document; its `config/state` or its
   last will confirms it

A box that fails stays in factory mode, on the line's Wi-Fi, at its
address: the repair bench can still reach it.

## 6. To test on a board

```sh
# after the dock's quick stage (or by hand: `factory on` at the console)
mosquitto_sub -h BROKER -u USER -P PASS -v -t 'mcold/mCDV1-L0169-1069-001/#' &
mosquitto_pub -h BROKER -u USER -P PASS -t mcold/v1/mCDV1-L0169-1069-001/rpc \
  -m '{"id":101,"cmd":"GET_DEBUG"}'
mosquitto_pub ... -m '{"id":102,"cmd":"IDENTIFY","s":5}'
mosquitto_pub ... -m '{"id":103,"cmd":"FACTORY_TEST","items":["i2c","temp"]}'
mosquitto_pub ... -m '{"id":104,"cmd":"FACTORY_DONE"}'
mosquitto_pub ... -m '{"id":105,"cmd":"IDENTIFY"}'      # now NOT_AUTHORIZED
```

Things to check that the simulation cannot:

- the stack of `mqtt_rpc` (6 KB) through `FACTORY_TEST` with `logtest`
- `rpc_handle` holds its mutex for the whole of `FACTORY_TEST`: BLE and
  console commands wait meanwhile (acceptable on the line; confirm nothing
  times out badly)
- `IDENTIFY`'s lights and buzzer against `indicate.cpp`, which also drives
  the lights every second on USB power
- `esp_core_dump_image_check()` with this build's core dump settings
  (the `espcoredump` component was added to `CMakeLists.txt`)
