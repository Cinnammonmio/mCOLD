#include "temp.h"

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

}  // namespace

TempStatus temp_sample(float *celsius) {
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

const char *temp_status_name(TempStatus s) {
  switch (s) {
    case TempStatus::Ok:      return "ok";
    case TempStatus::Open:    return "probe open";
    case TempStatus::NoData:  return "no data";
    case TempStatus::Garbled: return "garbled frame";
    case TempStatus::Busy:    return "bus busy";
  }
  return "?";
}
