#include "temp.h"

#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "bus.h"
#include "health.h"
#include "rails.h"

namespace {

// MAX6675 frame, MSB first:
//   D15      dummy, always 0
//   D14..D3  temperature, 0.25 C per count
//   D2       thermocouple input open
//   D1       device ID, always 0
//   D0       tri-state
//
// D15 and D1 are the structural check. If either is set, whatever is
// on the bus is not this part answering, and treating the rest of the
// word as a temperature would be inventing a reading.
const uint16_t MAX6675_DUMMY = 0x8000;
const uint16_t MAX6675_OPEN = 0x0004;
const uint16_t MAX6675_DEVID = 0x0002;

// SHT-31: single shot, high repeatability, no clock stretching. Up to
// 15.5 ms to convert; then T (2 bytes + CRC) and RH (2 + CRC), RH unused.
const uint8_t SHT31_MEASURE[2] = {0x24, 0x00};
const uint32_t SHT31_CONVERT_MS = 20;

// Kept through deep sleep: a box that had the SHT-31 at its cold boot
// keeps reading it, so a sensor that drops off the bus is a fault of that
// sensor (NoData), not a thermocouple that "came back open".
RTC_DATA_ATTR TempSensor g_sensor = TempSensor::Unknown;

uint8_t crc8(const uint8_t *d, int n) {
  uint8_t c = 0xFF;   // poly 0x31, init 0xFF (Sensirion)
  for (int i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x31 : c << 1);
  }
  return c;
}

TempStatus sht31_sample(float *celsius) {
  BusErr e = i2c_write(Dev::Thermo, ADDR_SHT31, SHT31_MEASURE, 2);
  if (e == BusErr::Timeout) return TempStatus::Busy;
  if (e != BusErr::Ok) return TempStatus::NoData;
  vTaskDelay(pdMS_TO_TICKS(SHT31_CONVERT_MS));
  uint8_t b[6];
  e = i2c_read(Dev::Thermo, ADDR_SHT31, b, sizeof(b));
  if (e != BusErr::Ok) {
    // Not done yet (it NAKs a read until it is): once more.
    vTaskDelay(pdMS_TO_TICKS(10));
    e = i2c_read(Dev::Thermo, ADDR_SHT31, b, sizeof(b));
  }
  if (e == BusErr::Timeout) return TempStatus::Busy;
  if (e != BusErr::Ok) return TempStatus::NoData;
  if (crc8(b, 2) != b[2]) {
    health_fail(Dev::Thermo, -100);
    return TempStatus::Garbled;
  }
  const uint16_t raw = (uint16_t)((b[0] << 8) | b[1]);
  if (celsius) *celsius = -45.0f + 175.0f * (float)raw / 65535.0f;
  return TempStatus::Ok;
}

TempStatus max6675_sample(float *celsius);

}  // namespace

TempSensor temp_sensor(void) {
  if (g_sensor == TempSensor::Unknown) {
    // Untracked: on a box with the thermocouple, no answer is the answer.
    g_sensor = i2c_probe(Dev::Untracked, ADDR_SHT31) == BusErr::Ok ? TempSensor::Sht31
                                                                   : TempSensor::Max6675;
  }
  return g_sensor;
}

const char *temp_sensor_name(TempSensor s) {
  switch (s) {
    case TempSensor::Max6675: return "type K (MAX6675)";
    case TempSensor::Sht31:   return "SHT-31";
    default:                  return "unknown";
  }
}

TempStatus temp_sample(float *celsius) {
  return temp_sensor() == TempSensor::Sht31 ? sht31_sample(celsius) : max6675_sample(celsius);
}

namespace {

TempStatus max6675_sample(float *celsius) {
  rail_acquire(Rail::Epd);     // shared with the display; reference counted

  uint16_t raw = 0;
  TempStatus st = TempStatus::NoData;

  // The conversion in flight when the part powered up belongs to
  // whatever state it was in before. Read it, drop it, wait for one
  // that was started under known conditions.
  vTaskDelay(pdMS_TO_TICKS(TEMP_CONVERT_MS));
  (void)spi3_transfer16(Dev::Thermo, PIN_TC_CS, 2000000, &raw);
  vTaskDelay(pdMS_TO_TICKS(TEMP_CONVERT_MS));

  const BusErr e = spi3_transfer16(Dev::Thermo, PIN_TC_CS, 2000000, &raw);
  rail_release(Rail::Epd);

  if (e == BusErr::Timeout) return TempStatus::Busy;
  if (e != BusErr::Ok) return TempStatus::NoData;   // all-ones or all-zeros

  if (raw & (MAX6675_DUMMY | MAX6675_DEVID)) {
    health_fail(Dev::Thermo, -100);
    return TempStatus::Garbled;
  }
  if (raw & MAX6675_OPEN) {
    // A disconnected probe is a fault with a name. The caller must not
    // be able to mistake it for a cold reading, so no value is written
    // out at all.
    health_fail(Dev::Thermo, -101);
    return TempStatus::Open;
  }

  if (celsius) *celsius = (float)(raw >> 3) * 0.25f;
  return TempStatus::Ok;
}

}  // namespace

const char *temp_status_name(TempStatus s) {
  switch (s) {
    case TempStatus::Ok:      return "ok";
    case TempStatus::Open:    return "probe open";   // the thermocouple only
    case TempStatus::NoData:  return "no data";
    case TempStatus::Garbled: return "garbled frame";
    case TempStatus::Busy:    return "bus busy";
  }
  return "?";
}
