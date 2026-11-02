# Notes from the flasher session

Append only; newest at the bottom. Station code: `flasher/` on branch
`claude/lucid-galileo-sb8exi`.

## 2026-11-02 12:38 [flasher] what the flasher expects from the firmware

Taken from `flasher/profiles/mcold.yaml`, `flasher/mflash/parsers.py`,
`flasher/mflash/ports.py`, `flasher/mflash/remote.py`. If the firmware
changes any of these strings, the station breaks: say so here.

**USB identity** (VID 0x303A)
- 0x1001 USB-Serial-JTAG: blank chip, ROM, bringup, fw <= 0.6.0. Serial = MAC.
- 0x0009 ROM over USB-OTG (no serial).
- 0x4003 product firmware (TinyUSB drive + console). Serial = the SN, or
  `MCOLD-<last 4 hex of the MAC>` before `sn set`.
- A box is followed by MAC and SN, its slot by USB location.

**Flashing**
- Into download mode with the console command `flash` (no BOOT button);
  DTR/RTS held low. esptool `--flash_mode dio --flash_freq 80m --flash_size 16MB`.
- Images: bootloader 0x0, partitions 0x8000 (must match
  `firmware/partitions.csv`), ota_data_initial 0xD000, app 0x20000.
- Production builds older than **0.7.0-dev.5** are refused (no `flash`,
  `sn set`, `ota base`).
- The version check reads the app descriptor of the .bin and compares it with
  the box's `version`.

**Console replies the station parses** (regex on the text)
- `version`: `  firmware <ver>` and `  elf sha256 <hex>` (first 16 hex compared).
- `health`: rows `  <dev> <ok|degraded|failed|unknown|absent> <n> <n> <last>`,
  `  temperature <float> C`, `  clock YYYY-MM-DD hh:mm:ss  source <src>` (`NOT USABLE` = not usable).
  pd, buzzer: ok|unknown; sd: ok|unknown|absent allowed; every other device must be ok.
- `sn`: SN matching `[A-Za-z]{3}V\d-L\d{4}-\d{4}-\d{3}`, without `not the factory pattern`.
- `sn set S` -> `stored`; takes effect at `reboot`.
- `wifi` / `sync`: line `  known      a, b` (or `none`).
- `wifi add SSID PASS` -> `stored (`; `wifi ip SSID IP GW SUBNET [DNS]` -> `fixed address`;
  `wifi del SSID`.
- `mqtt set HOST PORT USER PASS` -> `stored; connecting`; `ota base URL` -> `  stored`;
  `config set K V` -> `K = V, stored`.
- `logtest` -> `all passed` (waits for `all passed|FAILURES above`, up to 180 s).
- `ledtest`, `beep 150`: a person answers; `sync` -> `broker <host>, connected`.
- `rpc {"id":1,"cmd":"GET_STATUS"}` -> `"mac":"AA:BB:..."` (who the box is on the 0x4003 port).
- `factory on` -> `factory mode on` (only with the proposed patch below).

**A box's state after a run**
- Dock route (test -> production -> setup -> verify): chip erased, approved
  product firmware, SN set, the customer's Wi-Fi (networks not in the site
  list removed), broker, OTA base, site `config set` keys; health, logtest,
  LEDs, beep checked. No trip started.
- Quick route (USB ~1.5 min): chip erased, product firmware, SN, the line's
  Wi-Fi with a fixed IP (pool base + unit number), broker, OTA base, config,
  `factory on`, quick health. Then the rack over MQTT: FACTORY_TEST,
  IDENTIFY (person confirms), GET_DEBUG, settings documents marked
  `"factory":true`, FACTORY_DONE, customer Wi-Fi in, line Wi-Fi out (last).
  A box that fails on the rack stays in factory mode on the line's Wi-Fi.
- Re-flash modes write only ota_data_initial + app (SN, settings, trip log kept).

**MQTT fields the rack reads**
- `mcold/<sn>/status`: `fw.ver`, `net.ip`, `temp.ok`, `temp.c`, `power.external`.
- `mcold/<sn>/settings`: `server_rev`, `factory_rev` (new, patch).
- `mcold/<sn>/config/state`: `rev`, `state` (ok expected), `errors`.
- `mcold/<sn>/ota/state`: `state`, `ver`, `file`.
- `mcold/<sn>/rpc/reply` (patch): reply with the request's `id`.

**Tool changes that affect the firmware**
- Proposed patch for `firmware/p7-power` @ 4185a46:
  `flasher/docs/firmware-mqtt-rpc.patch`, spec `flasher/docs/firmware-mqtt-rpc.md`.
  rpc over MQTT (`mcold/v1/<sn>/rpc` -> `mcold/<sn>/rpc/reply`), factory mode
  (NVS, console `factory [on|off]`), FACTORY_TEST / IDENTIFY / GET_DEBUG /
  FACTORY_DONE, factory settings documents counted by their own `frev`,
  status every 30 s in factory mode. Written, `git apply`s cleanly, **not
  built or run** (no PlatformIO here). Please build and review.

**Risks for the firmware side**
- The flasher does **not** check for a running trip before it erases. With
  auto-start on, a box is flashed as soon as it is seated. Keep box 004 off
  the dock until its trip is stopped.
- The flasher has no charging test (no +250..300 mA check).
