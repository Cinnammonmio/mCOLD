// Settings from outside the box (decided 2026-10-05): the app over BLE
// (APPLY_CONFIG) and the server over MQTT (mcold/v1/<sn>/config) hand over
// the same document, applied by the same code:
//
//   {"rev": 3,                                   (MQTT: applied once per rev)
//    "config":   {"upload_period_s": 600, ...},  (settings.cpp: which keys)
//    "wifi":     {"add": [{"ssid": "WH-2", "pass": "..."}], "del": ["Old"]},
//    "mqtt":     {"host": "...", "port": 1883, "user": "...", "pass": "..."},
//    "ota_base": "https://..."}
//
// Every part is optional. Each key is checked before it is stored and
// reports on its own: one bad key does not stop the rest, nor a Wi-Fi
// network being added.
//
// Not everything is for outside hands. Bench switches (sleep_en, ...),
// the panel insets and the battery limits stay at the console; the
// calibration and the sample period cannot change while a trip runs --
// the trip's header says what they were for every row in it.
//
// A new broker is taken on trial (uplink.h): if the box cannot reach it
// within UPLINK_TRIAL_MS it goes back to the old one and says so, so a
// typo sent over MQTT cannot cut a box off for good.
#pragma once

#include <cJSON.h>
#include <stdbool.h>

// Applies `doc`; `report` (a JSON object) gets "applied": [what was set]
// and "errors": {what: why}. True if nothing was refused.
bool settings_apply(const cJSON *doc, cJSON *report);

// May the app or the server set this config key?
bool settings_remote_key(const char *key);
// Refused while a trip runs.
bool settings_trip_locked(const char *key);

// The network settings as they stand, without secrets: Wi-Fi names,
// broker host/port/user, OTA base URL.
void settings_network(cJSON *out);
