# mCOLD device protocol

**Status: PROPOSED (protocol 1), 2026-10-02.** Written by the firmware
side for the iOS app team to accept or change. Nothing here is final
until they have; the UUIDs in particular are fixed only once the app
ships against them.

The same JSON requests and responses run over every transport. BLE is
the first (P5). USB serial carries them now for testing (`rpc` console
command), and later USB CDC and anything else can carry them unchanged.

## 1. Finding the device

1. The app reads the NFC tag: an NDEF record of MIME type
   `application/json`, `{"sn":"MCOLD-9A74","ble":"28:84:85:27:9A:76"}`.
   This is the record bring-up wrote and the firmware keeps on the tag
   (rewritten only when it differs). The app team owns this schema;
   firmware will add a URI record (for the iOS banner) once there is a
   URL to put in it.
2. Tapping the tag also wakes the device's BLE: it advertises for 60 s.
3. **iOS never sees a BLE MAC.** The app finds the device by its
   advertised name, which is the SN, and filters on the service UUID.
   The MAC in the NDEF record is a tiebreak only, as the app team
   confirmed.

Advertising: flags + the 128-bit service UUID in the advertisement,
the complete local name (`MCOLD-9A74`) in the scan response, 20-40 ms
interval while a window is open.

## 2. BLE GATT

One primary service. All UUIDs share the base
`d2a5xxxx-3818-4667-a205-b3ed9c37b8a5`.

| UUID | Name | Properties | Security |
|---|---|---|---|
| `d2a50000-…` | mCOLD service | | |
| `d2a50001-…` | INFO | read | open |
| `d2a50002-…` | STATUS | read, notify | open |
| `d2a50003-…` | COMMAND | write | **encrypted + authenticated** |
| `d2a50004-…` | RESPONSE | notify | sent only on an authenticated link |
| `d2a50005-…` | EVENT | notify | open |

- **INFO** is the `GET_INFO` result (section 4), so an app can check the
  protocol version before sending anything.
- **STATUS** is the `GET_STATUS` result, notified when the trip state or
  an alarm changes, and after each sample.
- **COMMAND** takes requests, **RESPONSE** returns their answers.
- **EVENT** notifies trip and alarm events as they happen:
  `{"ev":"ALARM_RAISE","alarm":"TEMP_HIGH"}`, `ALARM_CLEAR`, `ALARM_ACK`,
  `TRIP_START`, `TRIP_STOP`.

### Pairing

LE Secure Connections with MITM protection. The device's IO capability
is *display only*: when iOS pairs, the device shows a 6-digit passkey
on its e-paper and the user types it into the iOS prompt. So whoever
changes the device's settings or starts a trip was holding it. iOS
starts pairing by itself the first time the app writes COMMAND (the
write needs an authenticated link). The bond is kept on both sides.

### Fragments

A message is longer than one BLE write or notification, so both
directions use the same framing, independent of the MTU:

```
byte 0   flags: bit 7 FIRST, bit 6 LAST, bits 0-5 fragment index (mod 64)
byte 1.. a piece of the UTF-8 JSON message
```

A message is its FIRST fragment through its LAST, in order (BLE writes
with response and notifications are ordered). A single-fragment message
has both bits set. A fragment out of sequence, or more than 4,096 bytes
in all, discards the message; the device answers `BAD_REQUEST` with
`id` 0. A response is cut to fit the negotiated MTU (MTU - 3 bytes of
ATT header - 1 byte of flags).

## 3. Messages

Request:

```json
{"id": 17, "cmd": "START_TRIP", "low": 2.0, "high": 8.0}
```

Response:

```json
{"id": 17, "ok": true, "trip": 7}
{"id": 17, "ok": false, "err": "ALREADY_ACTIVE", "msg": "a trip is already running"}
```

- `id`: chosen by the app, 1 to 4294967295, unique per request. The
  response carries it back.
- **Idempotency.** Every command that changes something (marked ✎)
  remembers its `id`. Sending the same `id` again returns the original
  response and does nothing else -- so after a timeout or a dropped
  connection the app retries with the same `id`, never with a new one.
  The last 8 are kept in RAM; `START_TRIP`'s is also kept in flash, so a
  retry after the device has reset still returns the same trip.
- Unknown keys in a request are ignored, so fields can be added.
- `proto` may be sent in a request; a value the device does not support
  is answered `UNSUPPORTED_PROTO`.

Errors:

| `err` | Meaning |
|---|---|
| `BAD_REQUEST` | not JSON, no `id`, or no `cmd` |
| `UNKNOWN_CMD` | `cmd` is not one this firmware knows |
| `BAD_ARGS` | an argument missing or out of range |
| `NOT_AUTHORIZED` | needs an authenticated link |
| `UNSUPPORTED_PROTO` | `proto` too new |
| `ALREADY_ACTIVE` / `NOT_ACTIVE` | trip state does not allow it |
| `LOG_FULL`, `NO_LOG`, `FLASH` | storage could not do it |
| `NOT_SUPPORTED` | defined, but not in this firmware yet |

Units: temperatures in °C as numbers, times as Unix seconds UTC with a
separate quality (`none`, `rtc`, `gnss`, `host`), durations in seconds.
A value the device does not have is **absent or `null`, never 0**.

## 4. Commands

| Command | ✎ | Arguments | Result |
|---|---|---|---|
| `GET_INFO` | | | `proto`, `sn`, `fw`, `hw`, `idf`, `boot`, `uptime_s` |
| `GET_STATUS` | | | see below |
| `GET_CONFIG` | | | `config`: every setting with its value |
| `SET_CONFIG` | ✎ | `key`, `value` | |
| `SET_TIME` | ✎ | `utc` (Unix s) | `quality` |
| `START_TRIP` | ✎ | `low`, `high` (°C); `hyst` (°C, 0.5), `dwell_s` (300) | `trip` |
| `STOP_TRIP` | ✎ | | `trip` |
| `ACK_ALARM` | ✎ | | `alarms` still active |
| `LIST_TRIPS` | | | `trips`: `[{"trip", "last_seq"}]`, oldest first |
| `GET_TRIP_SUMMARY` | | `trip` | thresholds, `samples`, `min`, `max`, `alarms`, `stopped` |
| `READ_LOG_CHUNK` | | `trip`, `from` (seq), `max` (1-16) | `records`, `next` |
| `GET_STORAGE_STATUS` | | | `sectors`, `used`, `free`, `trips`, `days_left` |
| `SELF_TEST` | | | `health`: each device and its state |
| `REBOOT` | ✎ | | (answered, then the device restarts) |
| `GET_SYNC_STATUS`, `SYNC_NOW`, `GET_USB_SNAPSHOT_STATUS` | | | `NOT_SUPPORTED` until P6 |

`GET_STATUS`:

```json
{"id":3,"ok":true,
 "time":{"utc":1790000000,"quality":"rtc","boot":42},
 "temp":{"ok":true,"c":4.25},
 "trip":{"active":true,"id":7,"samples":12,"min":3.5,"max":5.25,
         "alarms":["TEMP_HIGH"],"acked":false},
 "power":{"soc":78,"mv":3987,"ma":-12,"charge":"none","external":false},
 "gnss":{"fix":false,"age_s":null},
 "storage":{"used_pct":1}}
```

`READ_LOG_CHUNK` returns records exactly as the device stored them, one
JSON object each, `{"seq":5,"type":2,"data":"<base64>"}`, where `data` is
the record payload laid out as in `src/record.h` (little-endian, the
11-byte stamp first). `next` is the sequence to ask for next, or `null`
when there is no more. The app keeps the byte layout, not the device's
interpretation of it -- so a record from a newer firmware is not lost
by an older app, only not yet understood.

## 5. Open questions for the app team

1. Accept or change the UUIDs, the fragment format and JSON.
2. Is the passkey on the e-paper the pairing they want? (The display
   refreshes in about 2.5 s; the four-ink panel will take far longer.)
3. Should STATUS be readable without pairing? It carries temperature
   and position.
4. The NFC record: keep `{"sn","ble"}`, and which URL for the banner?
