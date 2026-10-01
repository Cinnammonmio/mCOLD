#include "accel.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

#include "board.h"
#include "bus.h"
#include "health.h"

namespace {

const uint8_t WHO_AM_I = 0x0F;
const uint8_t WHO_AM_I_VALUE = 0x44;
const uint8_t CTRL1 = 0x20;
const uint8_t CTRL2 = 0x21;
const uint8_t CTRL3 = 0x22;
const uint8_t CTRL4_INT1 = 0x23;
const uint8_t CTRL5_INT2 = 0x24;
const uint8_t CTRL6 = 0x25;
const uint8_t OUT_X_L = 0x28;
const uint8_t WAKE_UP_THS = 0x34;
const uint8_t WAKE_UP_DUR = 0x35;
const uint8_t FREE_FALL = 0x36;
const uint8_t WAKE_UP_SRC = 0x38;
const uint8_t CTRL7 = 0x3F;

// CTRL1: ODR 100 Hz, low-power mode 1 (12-bit).
//
// Bring-up ran high-performance mode, which the datasheet puts near
// 90 uA at any rate; low-power mode 1 is a few microamps here. The wake
// detector works the same in either, and 100 Hz still sees an impact
// lasting ten milliseconds. The noise is higher in this mode, but the
// threshold sits around ten times above it. P7 measures what this
// actually draws on the board.
const uint8_t CTRL1_100HZ_LP1 = 0x50;

// CTRL2: block data update, so a sample is never read half old and half
// new; register auto-increment, so six output bytes come in one burst.
const uint8_t CTRL2_BDU_INC = 0x0C;

// CTRL3: LIR latches the interrupt until its source is read. Without
// it INT1 pulses for one sample period, and anything polling the pin
// steps straight over it -- which bring-up found looks exactly like an
// INT1 trace that was never connected. Push-pull, active high.
const uint8_t CTRL3_LIR = 0x10;

const uint8_t CTRL4_INT1_WU = 0x20;
const uint8_t CTRL6_FS_2G = 0x00;
const uint8_t CTRL7_INT_EN = 0x20;

// WAKE_UP_SRC
const uint8_t SRC_FF = 0x20;
const uint8_t SRC_WU = 0x08;
const uint8_t SRC_X = 0x04;
const uint8_t SRC_Y = 0x02;
const uint8_t SRC_Z = 0x01;

// At +-2 g the output is left-justified in 16 bits whatever the
// resolution, so one scale covers 12- and 14-bit modes alike: the low
// bits are simply zero in the coarser one.
const float MG_PER_LSB16 = 0.061f;

TaskHandle_t g_notify = nullptr;
volatile uint32_t g_events = 0;
volatile uint32_t g_last_ms = 0;
bool g_isr_attached = false;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void IRAM_ATTR on_int1(void *) {
  BaseType_t woke = pdFALSE;
  if (g_notify) vTaskNotifyGiveFromISR(g_notify, &woke);
  if (woke) portYIELD_FROM_ISR();
}

bool wr(uint8_t reg, uint8_t v) {
  return i2c_write_reg(Dev::Accel, ADDR_LIS2DW12, reg, v) == BusErr::Ok;
}

void decode(uint8_t src, AccelEvent *ev) {
  ev->raw_src = src;
  ev->wake = (src & SRC_WU) != 0;
  ev->free_fall = (src & SRC_FF) != 0;
  ev->x = (src & SRC_X) != 0;
  ev->y = (src & SRC_Y) != 0;
  ev->z = (src & SRC_Z) != 0;
}

void attach_isr(void) {
  if (g_isr_attached) return;
  gpio_config_t in = {};
  in.pin_bit_mask = 1ULL << PIN_ACC_INT1;
  in.mode = GPIO_MODE_INPUT;
  in.intr_type = GPIO_INTR_POSEDGE;
  gpio_config(&in);
  // Shared service: other drivers may already have installed it.
  const esp_err_t e = gpio_install_isr_service(0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return;
  if (gpio_isr_handler_add((gpio_num_t)PIN_ACC_INT1, on_int1, nullptr) == ESP_OK) {
    g_isr_attached = true;
  }
}

}  // namespace

bool accel_begin(uint8_t wake_threshold, AccelEvent *pending) {
  if (pending) memset(pending, 0, sizeof(*pending));

  uint8_t who = 0;
  if (i2c_read_reg(Dev::Accel, ADDR_LIS2DW12, WHO_AM_I, &who, 1) != BusErr::Ok) {
    return false;
  }
  if (who != WHO_AM_I_VALUE) {
    // Something answers at 0x18 and it is not this part. Reporting it
    // as working would mean every reading after this is fiction.
    health_fail(Dev::Accel, -300);
    return false;
  }

  // Whatever is latched now happened before this boot -- quite possibly
  // the tap that woke the chip. Collect it before reconfiguring.
  uint8_t src = 0;
  if (i2c_read_reg(Dev::Accel, ADDR_LIS2DW12, WAKE_UP_SRC, &src, 1) == BusErr::Ok &&
      pending && (src & (SRC_WU | SRC_FF))) {
    decode(src, pending);
  }

  // Interrupts off while the detector is rebuilt, so a half-written
  // configuration cannot fire.
  if (!wr(CTRL7, 0x00)) return false;
  if (!wr(CTRL2, CTRL2_BDU_INC)) return false;
  if (!wr(CTRL6, CTRL6_FS_2G)) return false;
  if (!wr(CTRL3, CTRL3_LIR)) return false;
  if (!wr(CTRL5_INT2, 0x00)) return false;     // INT2 is not connected
  if (!wr(FREE_FALL, 0x00)) return false;
  if (!wr(WAKE_UP_DUR, 0x00)) return false;    // one sample over is enough
  if (!wr(CTRL4_INT1, CTRL4_INT1_WU)) return false;
  if (!wr(CTRL1, CTRL1_100HZ_LP1)) return false;

  attach_isr();
  return accel_set_threshold(wake_threshold);
}

bool accel_set_threshold(uint8_t ths) {
  if (ths < 1) ths = 1;
  if (ths > 63) ths = 63;
  if (!wr(WAKE_UP_THS, ths)) return false;
  if (!wr(CTRL7, CTRL7_INT_EN)) return false;

  // Let the slope filter settle on the new configuration, then read the
  // source to drop anything that fired while it did. Otherwise the
  // first event after every reconfiguration is the reconfiguration.
  vTaskDelay(pdMS_TO_TICKS(30));
  uint8_t src = 0;
  return i2c_read_reg(Dev::Accel, ADDR_LIS2DW12, WAKE_UP_SRC, &src, 1) ==
         BusErr::Ok;
}

void accel_notify_task(TaskHandle_t t) { g_notify = t; }

bool accel_take_event(AccelEvent *ev) {
  uint8_t src = 0;
  if (i2c_read_reg(Dev::Accel, ADDR_LIS2DW12, WAKE_UP_SRC, &src, 1) != BusErr::Ok) {
    return false;
  }
  if (!(src & (SRC_WU | SRC_FF))) return false;
  if (ev) decode(src, ev);
  g_events++;
  g_last_ms = now_ms();
  return true;
}

bool accel_read(AccelSample *out) {
  uint8_t b[6];
  if (i2c_read_reg(Dev::Accel, ADDR_LIS2DW12, OUT_X_L, b, 6) != BusErr::Ok) {
    return false;
  }
  const int16_t rx = (int16_t)((uint16_t)b[1] << 8 | b[0]);
  const int16_t ry = (int16_t)((uint16_t)b[3] << 8 | b[2]);
  const int16_t rz = (int16_t)((uint16_t)b[5] << 8 | b[4]);
  if (out) {
    out->x_mg = rx * MG_PER_LSB16;
    out->y_mg = ry * MG_PER_LSB16;
    out->z_mg = rz * MG_PER_LSB16;
    out->magnitude_mg = sqrtf(out->x_mg * out->x_mg + out->y_mg * out->y_mg +
                              out->z_mg * out->z_mg);
  }
  return true;
}

uint32_t accel_event_count(void) { return g_events; }
uint32_t accel_last_event_ms(void) { return g_last_ms; }
int accel_int_level(void) { return gpio_get_level((gpio_num_t)PIN_ACC_INT1); }
