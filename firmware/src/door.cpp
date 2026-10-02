#include "door.h"

#include <driver/gpio.h>
#include <esp_timer.h>

#include "board.h"
#include "config.h"

namespace {

TaskHandle_t g_notify = nullptr;
DoorState g_state = DoorState::Unknown;
uint32_t g_since = 0;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

DoorState read_pin(void) {
  const int level = gpio_get_level((gpio_num_t)PIN_DOOR);
  return level == config().door_closed_lvl ? DoorState::Closed : DoorState::Open;
}

void IRAM_ATTR on_edge(void *) {
  BaseType_t woke = pdFALSE;
  if (g_notify) vTaskNotifyGiveFromISR(g_notify, &woke);
  if (woke) portYIELD_FROM_ISR();
}

}  // namespace

void door_init(void) {
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << PIN_DOOR;
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = GPIO_PULLUP_DISABLE;     // the board has 1 MOhm already
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  c.intr_type = GPIO_INTR_ANYEDGE;
  gpio_config(&c);
  const esp_err_t e = gpio_install_isr_service(0);
  if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) {
    gpio_isr_handler_add((gpio_num_t)PIN_DOOR, on_edge, nullptr);
  }
  g_state = read_pin();
  g_since = now_ms();
}

void door_notify_task(TaskHandle_t t) { g_notify = t; }

bool door_settle(DoorState *now, uint32_t *since_ms) {
  // Read twice across the settle time. Only a level that holds is a
  // door that moved; one that flips back was a knock on the contact.
  const DoorState a = read_pin();
  vTaskDelay(pdMS_TO_TICKS(DOOR_SETTLE_MS));
  const DoorState b = read_pin();
  if (a != b || b == g_state) {
    if (now) *now = g_state;
    return false;
  }
  const uint32_t t = now_ms();
  if (since_ms) *since_ms = t - g_since;
  g_state = b;
  g_since = t;
  if (now) *now = b;
  return true;
}

DoorState door_state(void) { return g_state; }

const char *door_state_name(DoorState s) {
  switch (s) {
    case DoorState::Closed: return "closed";
    case DoorState::Open:   return "open";
    default:                return "unknown";
  }
}
