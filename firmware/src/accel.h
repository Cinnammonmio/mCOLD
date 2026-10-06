// LIS2DW12 accelerometer: motion and shock as events, not as a stream.
//
// The part watches on its own. Its wake-up detector compares each
// sample with the one before it (a slope filter, so gravity drops out
// whichever way the box is lying) and raises INT1 when the difference
// crosses a threshold. That costs microamps, needs nothing from the
// MCU, and is the only way the seven-day power budget in §12 works
// out. Bring-up proved the whole path: a tap woke the ESP32 out of deep
// sleep through GPIO2, reset reason 2.
//
// Two things this driver deliberately does not claim:
//
//   The XYZ read after an event is NOT the peak of the shock. By the
//   time the MCU has woken and asked, the impact is long over; what
//   comes back is the box at rest afterwards. Events are reported as
//   events, with the axes that tripped, and the sample beside them is
//   marked as taken after the fact.
//
//   The threshold is not a calibrated shock limit. The only real data
//   point is that lifting the board by hand exceeds 1.45 g. Until the
//   product team sets one, the default is the bring-up value, which
//   detects motion -- handling, a tap -- rather than damage.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct AccelSample {
  float x_mg, y_mg, z_mg;
  float magnitude_mg;   // ~1000 at rest, whatever the orientation
};

struct AccelEvent {
  bool wake;            // the slope detector fired
  bool free_fall;
  bool x, y, z;         // which axes crossed the threshold
  uint8_t raw_src;      // WAKE_UP_SRC as read, for diagnostics
  bool double_tap;      // a double tap, with taps armed (tap_test, test only)
  uint8_t tap_src;      // TAP_SRC as read
};

// Checks WHO_AM_I, writes the complete configuration (every register
// that matters, so nothing depends on what state a previous run left
// the part in) and arms wake-up detection on INT1. Any event already
// latched -- the one that woke the chip, say -- is returned in
// `pending` rather than thrown away.
//
// `taps` (config tap_test, a bench aid, never in the product) also arms
// the part's double-tap detector on INT1 in place of wake-up, at 400 Hz
// high-performance: ~90 uA, and no motion counted, while it is on.
bool accel_begin(uint8_t wake_threshold, AccelEvent *pending, bool taps = false);

// Change the wake threshold, 1..63 in units of FS/64 (31.25 mg at
// +-2 g). Re-arms and clears anything latched under the old value.
bool accel_set_threshold(uint8_t wake_threshold);

// INT1 rising edge notifies this task (vTaskNotifyGive). One task, so
// that "who handles a motion event" has one answer.
void accel_notify_task(TaskHandle_t t);

// Reads and clears the latched source. Returns true with `ev` filled if
// anything had fired. Cheap enough to call on every sensor pass as
// well as on the interrupt: an edge that was missed -- the pin already
// high when the ISR was attached -- would otherwise leave the latch set
// and the pin stuck, and no further edge would ever come.
bool accel_take_event(AccelEvent *ev);

bool accel_read(AccelSample *out);

// Events counted since boot, and when the last one was seen (0: never).
uint32_t accel_event_count(void);
uint32_t accel_last_event_ms(void);

// Bring-up value: about 62 mg. Picks up a tap on the bench.
static const uint8_t ACCEL_WAKE_THS_DEFAULT = 2;

// The INT1 line as the MCU sees it, for diagnostics.
int accel_int_level(void);

// Bench (tap_test): the tap threshold, 1..31 x 62.5 mg (9 = 562 mg).
// Not kept: the next boot is back to 9.
bool accel_set_tap_threshold(uint8_t ths);
uint8_t accel_tap_threshold(void);
