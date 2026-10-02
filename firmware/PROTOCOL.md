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
   `application/json`:
   ```json
   {"sn":"MCOLD-9A74","ble":"28:84:85:27:9A:76","key":"571ab30e58225355ccfc9b010a935f65"}
   ```
   `sn` and `ble` are the record bring-up wrote; `key` is this box's
   current authorization key (section 2, *Authorization*). The device
   keeps the record up to date itself, rewriting only bytes that
   change. The app team owns this schema; firmware will add a URI
   record (for the iOS banner) once there is a URL to put in it.
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
| `d2a50003-…` | COMMAND | write | open; what a request may do depends on AUTH |
| `d2a50004-…` | RESPONSE | notify | open |
| `d2a50005-…` | EVENT | notify | open |

- **INFO** is the `GET_INFO` result (section 4), so an app can check the
  protocol version before sending anything.
- **STATUS** is the `GET_STATUS` result, notified when the trip state or
  an alarm changes, and after each sample.
- **COMMAND** takes requests, **RESPONSE** returns their answers.
- **EVENT** notifies trip and alarm events as they happen:
  `{"ev":"ALARM_RAISE","alarm":"TEMP_HIGH"}`, `ALARM_CLEAR`, `ALARM_ACK`,
  `TRIP_START`, `TRIP_STOP`.

### Authorization: tap to authorize

Decided 2026-10-02, instead of a passkey on the display. **No BLE
pairing**: the phone shows no system prompt, and nobody types anything.

Anyone in BLE range may read INFO and STATUS. Commands that change
something (marked ✎ in section 4) need an *authorized session*, and a
session is authorized by proving the app read the NFC tag -- which works
only from a few centimetres, so only with the box in hand.

1. Tap: read `key` (16 bytes, hex) from the tag record.
2. Connect, and read **INFO**. It carries `nonce`: 16 random bytes, hex,
   new for every connection.
3. Send `{"id":…,"cmd":"AUTH","proof":"<hex>"}` with
   `proof = HMAC-SHA256(key, nonce)`, both as raw bytes (not as hex text).
4. `{"ok":true}`: this connection may now send any command, until it
   closes. `NOT_AUTHORIZED`: tap again and retry.

The key never travels over BLE; someone listening sees a nonce and a
proof that are useless on any other connection.

**The key changes** at every reset of the device, and 2 minutes after
a session that used it closes. So one tap opens one session (and a
link that drops can come back within 2 minutes on the same tap). The
device writes the new key to the tag before it starts accepting it, so
the tag never offers a key the device would refuse.

Example, from the firmware's own test client:

```
key   7b3066da5096846781a0b5c1d0f6699b
nonce 951c0f73…  (from INFO)
proof = HMAC-SHA256(key, nonce) -> {"cmd":"AUTH","proof":"…"} -> ok
```

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
| `NOT_AUTHORIZED` | needs an authorized session (AUTH), or AUTH failed |
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
| `GET_INFO` | | | `proto`, `sn`, `fw`, `hw`, `idf`, `boot`, `uptime_s`; over BLE also `nonce` and `authorized` |
| `AUTH` | | `proof` (hex) | authorizes this connection (section 2) |
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
2. Tap-to-authorize (section 2): the `key` field in the tag record, AUTH
   with HMAC-SHA256 over the per-connection nonce.
3. Should STATUS be readable without AUTH? It carries temperature and
   position.
4. The NFC record: keep `{"sn","ble","key"}`, and which URL for the banner?
