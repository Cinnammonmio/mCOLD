#include "power.h"

#include <string.h>

#include "board.h"
#include "bus.h"
#include "health.h"

namespace {

// MAX17048
const uint8_t MAX_VCELL = 0x02;     // 78.125 uV per count
const uint8_t MAX_SOC = 0x04;       // high byte percent, low byte /256
const uint8_t MAX_CRATE = 0x16;     // signed, 0.208 %/hr per count

// INA226 across R15
const uint8_t INA_CONFIG = 0x00;
// 16 averages of 1.1 ms shunt and bus conversions, continuous: a reading
// every 35 ms that is not one instant of a current that pulses with the
// radio. MODE 000 is power-down.
const uint16_t INA_RUN = 0x4000 | (2 << 9) | (4 << 6) | (4 << 3) | 7;
const uint16_t INA_OFF = INA_RUN & ~7;
const uint8_t INA_SHUNT = 0x01;     // signed, 2.5 uV per count
const uint8_t INA_BUS = 0x02;       // 1.25 mV per count
const float SHUNT_OHMS = 0.010f;

// BQ25601
const uint8_t BQ_REG01 = 0x01;      // power-on config, holds the kick bit
const uint8_t BQ_REG08 = 0x08;      // system status
const uint8_t BQ_REG09 = 0x09;      // faults
const uint8_t BQ_WD_RST = 0x40;     // REG01 bit 6
const uint8_t BQ_FAULT_WATCHDOG = 0x80;

bool read16(Dev d, uint8_t addr, uint8_t reg, uint16_t *v) {
  uint8_t b[2];
  if (i2c_read_reg(d, addr, reg, b, 2) != BusErr::Ok) return false;
  *v = (uint16_t)b[0] << 8 | b[1];
  return true;
}

}  // namespace

bool write16(Dev d, uint8_t addr, uint8_t reg, uint16_t v) {
  const uint8_t b[3] = {reg, (uint8_t)(v >> 8), (uint8_t)v};
  return i2c_write(d, addr, b, 3) == BusErr::Ok;
}

void power_init(void) {
  // The charger is not configured here on purpose. Its limits depend on
  // a battery datasheet that is still listed as unconfirmed, and writing
  // a charge current chosen from a guess is worse than leaving the part
  // at the defaults it was designed to be safe at.
  //
  // The current monitor is: it comes out of power-down (power_sleep).
  write16(Dev::Current, ADDR_INA226, INA_CONFIG, INA_RUN);
}

void power_sleep(void) {
  // Left converting, the INA226 draws 330 uA -- about 5 % of the whole
  // 7-day budget, spent measuring a current nobody reads while the chip
  // sleeps. Power-down is 0.5 uA.
  write16(Dev::Current, ADDR_INA226, INA_CONFIG, INA_OFF);
}

void power_read(PowerStatus *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  uint16_t v;

  if (read16(Dev::Fuel, ADDR_MAX17048, MAX_VCELL, &v)) {
    out->cell_volts = v * 78.125e-6f;
    out->cell_valid = true;
    if (read16(Dev::Fuel, ADDR_MAX17048, MAX_SOC, &v)) {
      out->soc_percent = (v >> 8) + (v & 0xFF) / 256.0f;
    }
    if (read16(Dev::Fuel, ADDR_MAX17048, MAX_CRATE, &v)) {
      out->rate_percent_hr = (int16_t)v * 0.208f;
    }
  }

  if (read16(Dev::Current, ADDR_INA226, INA_SHUNT, &v)) {
    // The sign is the whole point of this part: it says which way
    // charge is moving, which the voltage alone cannot.
    const float shunt_v = (int16_t)v * 2.5e-6f;
    out->battery_ma = shunt_v / SHUNT_OHMS * 1000.0f;
    out->current_valid = true;
    if (read16(Dev::Current, ADDR_INA226, INA_BUS, &v)) {
      out->bus_volts = v * 1.25e-3f;
    }
  }

  uint8_t s = 0;
  if (i2c_read_reg(Dev::Charger, ADDR_BQ25601, BQ_REG08, &s, 1) == BusErr::Ok) {
    out->charger_valid = true;
    switch ((s >> 3) & 3) {
      case 0: out->charge = ChargeState::NotCharging; break;
      case 1: out->charge = ChargeState::PreCharge; break;
      case 2: out->charge = ChargeState::FastCharge; break;
      default: out->charge = ChargeState::Done; break;
    }
    switch ((s >> 5) & 7) {
      case 0: out->vbus = VbusType::None; break;
      case 1: out->vbus = VbusType::UsbHost; break;
      case 2: out->vbus = VbusType::Adapter; break;
      case 7: out->vbus = VbusType::Otg; break;
      default: out->vbus = VbusType::Unknown; break;
    }
    out->power_good = (s >> 2) & 1;

    uint8_t f = 0;
    if (i2c_read_reg(Dev::Charger, ADDR_BQ25601, BQ_REG09, &f, 1) == BusErr::Ok) {
      out->fault_reg = f;
      out->watchdog_expired = (f & BQ_FAULT_WATCHDOG) != 0;
    }
  }
}

void power_kick_watchdog(void) {
  // Read, set the one bit, write back. The first version of this wrote
  // 0x40 to REG01 outright, which looks like "set the kick bit" and is
  // actually "set the kick bit and clear everything else in the
  // register" -- including CHG_CONFIG, the charge enable. It would
  // have turned charging off every twenty seconds while reporting that
  // it was keeping the charger alive.
  uint8_t r = 0;
  if (i2c_read_reg(Dev::Charger, ADDR_BQ25601, BQ_REG01, &r, 1) != BusErr::Ok) {
    return;
  }
  i2c_write_reg(Dev::Charger, ADDR_BQ25601, BQ_REG01, (uint8_t)(r | BQ_WD_RST));
}

const char *charge_state_name(ChargeState s) {
  switch (s) {
    case ChargeState::NotCharging: return "not charging";
    case ChargeState::PreCharge:   return "pre-charge";
    case ChargeState::FastCharge:  return "fast charge";
    case ChargeState::Done:        return "charge done";
    default:                       return "unknown";
  }
}

const char *vbus_type_name(VbusType v) {
  switch (v) {
    case VbusType::None:    return "no input";
    case VbusType::UsbHost: return "usb host";
    case VbusType::Adapter: return "adapter";
    case VbusType::Otg:     return "otg";
    default:                return "unknown";
  }
}
