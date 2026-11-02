// Debug over MQTT (2026-11-02): a second MQTT client, to a debug broker,
// only while debug is on. The production broker (uplink, client 1) is
// where it is switched on and off and where its state is seen:
//
//   mcold/v1/<sn>/debug/ctl    broker 1, in    {"id":N,"on":true,"min":60,
//                                              "channels":[...],"period_ms":100}
//                                              not retained: a retained one is ignored
//   mcold/<sn>/debug/state     broker 1, out   retained: on, until, channels, period, broker
//   mcold/<sn>/debug/<channel> broker 2, out   {"from":utc_ms|null,"mono":ms,
//                                              "period_ms":P,"rows":[...]}, QoS 0, 1 a second
//   mcold/<sn>/debug/log       broker 2, out   {"lines":[...]}: the console's output
//   mcold/v1/<sn>/debug/cmd    broker 2, in    rpc requests; DEBUG_SET here too
//   mcold/<sn>/debug/reply     broker 2, out   their answers, by id
//
// The debug broker comes from NVS ("dbg": host, port, user, pass), set
// only at the console or written by the station at flash time; a host
// left out comes from secrets.ini ([debug] host), and port and login left
// out are the production broker's. Nothing on MQTT can name a broker.
// Debug switches itself off: after 60 min on USB power by default (240 at
// most), after 15 on battery.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void debug_start(const char *sn);

// The switch, from the console or broker 1's debug/ctl. minutes 0: the
// default for the power source; channels 0: as they were (all four at
// first: power, sys, net, log);
// period_ms is held to 100..1000.
void debug_set(bool on, uint32_t minutes, uint32_t channels, uint32_t period_ms);
bool debug_on(void);

// uplink.cpp: the answer to a request that came on debug/cmd.
void debug_reply(const char *resp);

// uplink.cpp: a message on mcold/v1/<sn>/debug/ctl.
void debug_ctl(const char *json, int len, bool retained);

// uplink.cpp: the state for mcold/<sn>/debug/state, and a number that
// moves whenever it changes (so it is published once per change).
uint32_t debug_state_serial(void);
void debug_state_json(char *out, size_t n);

bool debug_set_server(const char *host, uint16_t port, const char *user, const char *pass);
bool debug_clear_server(void);
void debug_print(void);    // console `debug`
