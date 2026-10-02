#include "rtcclock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "board.h"
#include "bus.h"
#include "health.h"

namespace {

const uint8_t REG_CONTROL_1 = 0x00;
const uint8_t REG_CONTROL_3 = 0x02;
const uint8_t REG_SECONDS = 0x03;

// Control_1 bit 7. The schematic specifies the 12.5 pF crystal; the
// part powers up expecting 7 pF. Both run -- only one keeps time.
const uint8_t CAP_SEL_12PF = 0x80;

// Seconds register bit 7: set on power-on reset and whenever the
// oscillator stops. Clearing it and finding it back a moment later is
// how a dead crystal announces itself.
const uint8_t OS_FLAG = 0x80;

// Control_3: PM[2:0] in bits 7..5, BSF bit 3 (switched to the backup
// since cleared), BLF bit 2 (backup low; read only).
const uint8_t C3_SWITCHOVER_STANDARD = 0x00;   // PM 000
const uint8_t C3_BSF = 0x08;
const uint8_t C3_BLF = 0x04;

TimeSource g_source = TimeSource::None;
bool g_os = true;
bool g_ran_on_backup = false;
uint8_t g_c3_found = 0;

uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

}  // namespace

bool rtc_begin(void) {
  // 12.5 pF first, and on every boot. This is not a production-time
  // setting: the register returns to 7 pF whenever the part loses
  // power, which on this board is every time the battery runs down.
  if (i2c_write_reg(Dev::Rtc, ADDR_PCF8523, REG_CONTROL_1, CAP_SEL_12PF) !=
      BusErr::Ok) {
    return false;
  }

  // Backup supply. The board as drawn leaves VBAT_3V floating; this
  // prototype has a CR1220 hand-soldered onto it (2026-10-02). Control_3
  // came up 0xE0 -- switch-over disabled, the part running on VDD alone
  // -- which wastes the cell: with VDD gone the clock stops anyway. So,
  // every boot: PM = 000, switch-over in standard mode (to VBAT when VDD
  // falls below VBAT and below 2.5 V) and battery-low detection on.
  // Writing the byte also clears BSF, after reading whether the part ran
  // on the cell since the last boot. With no cell fitted this setting is
  // harmless: VBAT is never above VDD, so it never switches.
  uint8_t c3 = 0;
  if (i2c_read_reg(Dev::Rtc, ADDR_PCF8523, REG_CONTROL_3, &c3, 1) == BusErr::Ok) {
    g_ran_on_backup = (c3 & C3_BSF) != 0;
    g_c3_found = c3;
  }
  i2c_write_reg(Dev::Rtc, ADDR_PCF8523, REG_CONTROL_3, C3_SWITCHOVER_STANDARD);

  uint8_t sec = 0;
  if (i2c_read_reg(Dev::Rtc, ADDR_PCF8523, REG_SECONDS, &sec, 1) != BusErr::Ok) {
    return false;
  }
  g_os = (sec & OS_FLAG) != 0;

  // The oscillator-stopped flag is the verdict, with or without a cell:
  // clear means the time was carried across (on the cell, or because
  // VDD never dropped); set means it was not, and the time is unknown.
  g_source = g_os ? TimeSource::None : TimeSource::Rtc;
  return true;
}

bool rtc_backup_low(void) {
  uint8_t c3 = 0;
  if (i2c_read_reg(Dev::Rtc, ADDR_PCF8523, REG_CONTROL_3, &c3, 1) != BusErr::Ok) {
    return false;
  }
  return (c3 & C3_BLF) != 0;
}

bool rtc_ran_on_backup(void) { return g_ran_on_backup; }
uint8_t rtc_control3_at_boot(void) { return g_c3_found; }

bool rtc_time_valid(void) { return g_source != TimeSource::None; }
TimeSource rtc_source(void) { return g_source; }
bool rtc_os_flag(void) { return g_os; }

bool rtc_get(struct tm *out) {
  uint8_t b[7];
  if (i2c_read_reg(Dev::Rtc, ADDR_PCF8523, REG_SECONDS, b, 7) != BusErr::Ok) {
    return false;
  }
  // The flag is re-read on every access, not cached from boot: an
  // oscillator that stops mid-trip has to invalidate the time from
  // that moment, not at the next restart.
  g_os = (b[0] & OS_FLAG) != 0;
  if (g_os) g_source = TimeSource::None;

  if (!out) return true;
  memset(out, 0, sizeof(*out));
  out->tm_sec = bcd2bin(b[0] & 0x7F);
  out->tm_min = bcd2bin(b[1] & 0x7F);
  out->tm_hour = bcd2bin(b[2] & 0x3F);
  out->tm_mday = bcd2bin(b[3] & 0x3F);
  out->tm_mon = bcd2bin(b[5] & 0x1F) - 1;         // tm_mon is 0-based
  out->tm_year = bcd2bin(b[6]) + 100;             // tm_year is from 1900
  return true;
}

bool rtc_set(const struct tm *t, TimeSource src) {
  if (!t || src == TimeSource::None) return false;

  // Load capacitance before the time: changing it disturbs the
  // oscillator, and doing it afterwards would cost the seconds the
  // circuit takes to settle.
  if (i2c_write_reg(Dev::Rtc, ADDR_PCF8523, REG_CONTROL_1, CAP_SEL_12PF) !=
      BusErr::Ok) {
    return false;
  }

  uint8_t b[8];
  b[0] = REG_SECONDS;
  b[1] = bin2bcd((uint8_t)t->tm_sec);     // bit 7 written as 0: clears OS
  b[2] = bin2bcd((uint8_t)t->tm_min);
  b[3] = bin2bcd((uint8_t)t->tm_hour);
  b[4] = bin2bcd((uint8_t)t->tm_mday);
  b[5] = 0;                               // weekday, derived elsewhere
  b[6] = bin2bcd((uint8_t)(t->tm_mon + 1));
  b[7] = bin2bcd((uint8_t)(t->tm_year - 100));
  if (i2c_write(Dev::Rtc, ADDR_PCF8523, b, sizeof(b)) != BusErr::Ok) {
    return false;
  }

  // OS was just cleared. A crystal that is not oscillating sets it
  // again within a second, so reading it back is not a formality --
  // it is the only check that the 32.768 kHz circuit is alive.
  vTaskDelay(pdMS_TO_TICKS(1200));
  uint8_t sec = 0;
  if (i2c_read_reg(Dev::Rtc, ADDR_PCF8523, REG_SECONDS, &sec, 1) != BusErr::Ok) {
    return false;
  }
  g_os = (sec & OS_FLAG) != 0;
  if (g_os) {
    g_source = TimeSource::None;    // the crystal is not running
    health_fail(Dev::Rtc, -200);
    return false;
  }
  g_source = src;
  return true;
}

const char *rtc_source_name(TimeSource s) {
  switch (s) {
    case TimeSource::None: return "unknown";
    case TimeSource::Rtc:  return "rtc";
    case TimeSource::Gnss: return "gnss";
    case TimeSource::Host: return "host";
    case TimeSource::Ntp:  return "ntp";
  }
  return "?";
}
