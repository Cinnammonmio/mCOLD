#include "bus.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <rom/ets_sys.h>
#include <string.h>

#include "board.h"

namespace {

i2c_master_bus_handle_t g_i2c_bus;
SemaphoreHandle_t g_i2c_lock;
SemaphoreHandle_t g_spi3_lock;
spi_device_handle_t g_tc;          // MAX6675 on SPI3
bool g_ready = false;

const int XFER_MS = 50;            // per transaction; nothing waits forever

// One device handle per address, created on first use. The IDF master
// driver wants a handle rather than an address on every call, and
// building them lazily keeps the address list in board.h instead of
// duplicating it here.
struct DevSlot {
  uint8_t addr;
  i2c_master_dev_handle_t h;
};
DevSlot g_slots[12];
int g_slot_count = 0;

i2c_master_dev_handle_t slot_for(uint8_t addr) {
  for (int i = 0; i < g_slot_count; i++) {
    if (g_slots[i].addr == addr) return g_slots[i].h;
  }
  if (g_slot_count >= (int)(sizeof(g_slots) / sizeof(g_slots[0]))) return nullptr;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = addr;
  cfg.scl_speed_hz = 100000;
  i2c_master_dev_handle_t h = nullptr;
  if (i2c_master_bus_add_device(g_i2c_bus, &cfg, &h) != ESP_OK) return nullptr;
  g_slots[g_slot_count].addr = addr;
  g_slots[g_slot_count].h = h;
  g_slot_count++;
  return h;
}

// Every path out of a bus call goes through here, so there is exactly
// one place where an attempt becomes a health record. A driver cannot
// forget to report, and cannot report twice.
inline BusErr done(Dev dev, BusErr e) {
  if (e == BusErr::Ok) {
    health_ok(dev);
  } else {
    health_fail(dev, (int32_t)e);
  }
  return e;
}

inline bool i2c_take(void) {
  return g_ready &&
         xSemaphoreTake(g_i2c_lock, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE;
}
inline void i2c_give(void) { xSemaphoreGive(g_i2c_lock); }

}  // namespace

void bus_init(void) {
  g_i2c_lock = xSemaphoreCreateMutex();
  g_spi3_lock = xSemaphoreCreateMutex();

  // Chip selects idle high before either bus exists. A CS that floats
  // low during init talks to a part by accident, and that presents as a
  // corrupt sensor rather than as an ordering problem.
  gpio_config_t out = {};
  out.pin_bit_mask =
      (1ULL << PIN_EPD_CS) | (1ULL << PIN_TC_CS) | (1ULL << PIN_EPD_DC);
  out.mode = GPIO_MODE_OUTPUT;
  gpio_config(&out);
  gpio_set_level((gpio_num_t)PIN_EPD_CS, 1);
  gpio_set_level((gpio_num_t)PIN_TC_CS, 1);
  gpio_set_level((gpio_num_t)PIN_EPD_DC, 1);

  // 100 kHz. Eight devices and a long flex share this bus: speed is
  // worth nothing here and margin is worth a great deal.
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = I2C_NUM_0;
  bc.sda_io_num = (gpio_num_t)PIN_SDA;
  bc.scl_io_num = (gpio_num_t)PIN_SCL;
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bc, &g_i2c_bus) != ESP_OK) return;

  spi_bus_config_t sb = {};
  sb.mosi_io_num = PIN_SPI3_MOSI;
  sb.miso_io_num = PIN_SPI3_MISO;
  sb.sclk_io_num = PIN_SPI3_SCK;
  sb.quadwp_io_num = -1;
  sb.quadhd_io_num = -1;
  sb.max_transfer_sz = 4096;
  if (spi_bus_initialize(SPI3_HOST, &sb, SPI_DMA_CH_AUTO) != ESP_OK) return;

  // CS is driven by hand rather than by the peripheral. The e-paper
  // driver will want this same bus on its own timing, and one owner per
  // CS line is easier to reason about than two.
  spi_device_interface_config_t dc = {};
  dc.clock_speed_hz = 2000000;
  dc.mode = 0;
  dc.spics_io_num = -1;
  dc.queue_size = 1;
  spi_bus_add_device(SPI3_HOST, &dc, &g_tc);

  g_ready = true;
}

BusErr i2c_write(Dev dev, uint8_t addr, const uint8_t *data, size_t n) {
  if (!i2c_take()) return done(dev, BusErr::Timeout);
  i2c_master_dev_handle_t h = slot_for(addr);
  BusErr e = BusErr::Nak;
  if (h) {
    static const uint8_t none = 0;
    e = (i2c_master_transmit(h, n ? data : &none, n, XFER_MS) == ESP_OK)
            ? BusErr::Ok
            : BusErr::Nak;
  }
  i2c_give();
  return done(dev, e);
}

BusErr i2c_probe(Dev dev, uint8_t addr) {
  if (!i2c_take()) return done(dev, BusErr::Timeout);
  const BusErr e = (i2c_master_probe(g_i2c_bus, addr, XFER_MS) == ESP_OK)
                       ? BusErr::Ok
                       : BusErr::Nak;
  i2c_give();
  return done(dev, e);
}

BusErr i2c_read_reg(Dev dev, uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  if (!i2c_take()) return done(dev, BusErr::Timeout);
  i2c_master_dev_handle_t h = slot_for(addr);
  BusErr e = BusErr::Nak;
  if (h) {
    // transmit_receive issues a repeated start, not a stop. A stop here
    // would let another master, or the RF side of the NFC tag, slip in
    // between the pointer write and the read, and the value that came
    // back would belong to someone else.
    e = (i2c_master_transmit_receive(h, &reg, 1, buf, n, XFER_MS) == ESP_OK)
            ? BusErr::Ok
            : BusErr::Nak;
  }
  i2c_give();
  return done(dev, e);
}

BusErr i2c_write_reg(Dev dev, uint8_t addr, uint8_t reg, uint8_t v) {
  const uint8_t b[2] = {reg, v};
  return i2c_write(dev, addr, b, 2);
}

BusErr i2c_read_mem16(Dev dev, uint8_t addr, uint16_t mem, uint8_t *buf,
                      size_t n) {
  if (!i2c_take()) return done(dev, BusErr::Timeout);
  i2c_master_dev_handle_t h = slot_for(addr);
  BusErr e = BusErr::Nak;
  if (h) {
    const uint8_t a[2] = {(uint8_t)(mem >> 8), (uint8_t)(mem & 0xFF)};
    e = (i2c_master_transmit_receive(h, a, 2, buf, n, XFER_MS) == ESP_OK)
            ? BusErr::Ok
            : BusErr::Nak;
  }
  i2c_give();
  return done(dev, e);
}

BusErr i2c_write_mem16(Dev dev, uint8_t addr, uint16_t mem, const uint8_t *data,
                       size_t n) {
  if (n > 32) return done(dev, BusErr::Short);
  if (!i2c_take()) return done(dev, BusErr::Timeout);
  i2c_master_dev_handle_t h = slot_for(addr);
  BusErr e = BusErr::Nak;
  if (h) {
    uint8_t buf[34];
    buf[0] = (uint8_t)(mem >> 8);
    buf[1] = (uint8_t)(mem & 0xFF);
    memcpy(buf + 2, data, n);
    if (i2c_master_transmit(h, buf, n + 2, XFER_MS) == ESP_OK) {
      // EEPROM write cycle: the part stops acknowledging its own
      // address until it finishes. Polling for the ack to come back is
      // quicker than waiting out a worst case, and it is the truth
      // rather than an assumption about how long it took.
      e = BusErr::Timeout;
      for (int i = 0; i < 25; i++) {
        if (i2c_master_probe(g_i2c_bus, addr, 5) == ESP_OK) {
          e = BusErr::Ok;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }
  }
  i2c_give();
  return done(dev, e);
}

bool spi3_take(uint32_t wait_ms) {
  return g_ready &&
         xSemaphoreTake(g_spi3_lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void spi3_give(void) { xSemaphoreGive(g_spi3_lock); }

BusErr spi3_transfer16(Dev dev, int cs_pin, uint32_t hz, uint16_t *out) {
  (void)hz;
  if (!spi3_take(BUS_WAIT_MS)) return done(dev, BusErr::Timeout);

  uint8_t rx[2] = {0, 0};
  spi_transaction_t t = {};
  t.length = 16;
  t.rxlength = 16;
  t.flags = SPI_TRANS_USE_TXDATA;
  t.rx_buffer = rx;

  gpio_set_level((gpio_num_t)cs_pin, 0);
  ets_delay_us(2);
  const esp_err_t r = spi_device_polling_transmit(g_tc, &t);
  gpio_set_level((gpio_num_t)cs_pin, 1);
  spi3_give();

  if (r != ESP_OK) return done(dev, BusErr::Nak);
  const uint16_t v = (uint16_t)rx[0] << 8 | rx[1];

  // All-zeros and all-ones are what an unpowered part or a dead MISO
  // line look like. Neither is a reading, so neither is reported as one.
  if (v == 0x0000 || v == 0xFFFF) return done(dev, BusErr::Nak);
  if (out) *out = v;
  return done(dev, BusErr::Ok);
}

const char *bus_err_name(BusErr e) {
  switch (e) {
    case BusErr::Ok:       return "ok";
    case BusErr::Timeout:  return "timeout";
    case BusErr::Nak:      return "nak";
    case BusErr::Short:    return "short read";
    case BusErr::NotReady: return "not ready";
  }
  return "?";
}
