// Factory mode: the line's rack finishes a box over Wi-Fi -> MQTT
// (flasher/docs/firmware-mqtt-rpc.md).
//
// The station sets it at the dock (`factory on` at the console, with the
// cable) and the box clears it itself at FACTORY_DONE, the rack's
// hand-over. While it is on, commands that change or test something are
// taken over MQTT (mcold/v1/<sn>/rpc) from the shared broker login, and
// the status goes out every 30 s. Off -- every shipped box -- the MQTT
// channel answers only commands that read.
#pragma once

#include <cJSON.h>
#include <stdbool.h>
#include <stdint.h>

bool factory_mode(void);
void factory_set(bool on);

// FACTORY_TEST {items:[i2c,temp,gnss,logtest], gnss_s}: the console's
// bench checks, each {ok, ...}; ok at the top when all of them are.
// Runs for as long as its items take (logtest ~2 min, gnss gnss_s).
void factory_test(cJSON *out, const cJSON *req);

// IDENTIFY {s}: every light blinks white and the buzzer beeps, for s
// seconds (1..30), so a person can tell which box on the rack this is.
void factory_identify(int seconds);

// GET_DEBUG: what a repair bench would ask the box first.
void factory_debug(cJSON *out);

// main.cpp: keep the GNSS module on (its task pumps the UART) for ms.
void gnss_hold_for(uint32_t ms);
