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
