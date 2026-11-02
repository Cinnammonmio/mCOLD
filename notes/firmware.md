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
