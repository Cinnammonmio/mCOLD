// Door contact on GPIO7.
//
// A magnetic reed contact to ground, a 1 MOhm pull-up and 47 nF across
// it (§4.2): about 47 ms of RC, which already filters contact bounce
// before the pin sees it. The internal pull-up stays off -- it would
// draw current through the contact for as long as the door is closed
// and change that RC.
//
// Which level means "closed" is a configuration item, not a constant:
// "normally closed" on a reed switch describes it with no magnet near,
// and whether the magnet sits by the switch when the lid is shut
// depends on the case. It has to be checked once with the real magnet
// in the real lid (config door_closed_lvl) [VERIFY].
//
// Two wires cannot tell "door open" from "wire cut": both read as the
// pull-up. This driver reports what it can see, not what it cannot.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

enum class DoorState : uint8_t { Unknown = 0, Closed = 1, Open = 2 };

void door_init(void);

// Every edge notifies this task; it should then call door_settle().
void door_notify_task(TaskHandle_t t);

// Reads the pin and returns true if the debounced state changed.
// `since_ms` gets how long the previous state lasted.
bool door_settle(DoorState *now, uint32_t *since_ms);

DoorState door_state(void);
const char *door_state_name(DoorState s);

// Settle time after an edge before the level is believed. Longer than
// the RC, short enough that a quick open-and-shut is still seen.
static const uint32_t DOOR_SETTLE_MS = 80;
