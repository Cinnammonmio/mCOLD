// A logged row (record.h) as the table everyone sees: the same columns,
// with the same names, in the server's JSON and in a CSV file.
//
//   trip_id, trip_date, trip_number, seq, sn, timestamp, utc, event, temp,
//   tempmin, tempmax, alarm, timeok, gnssstate, latitude, longitude,
//   motion, battery, internet, detail
//
// trip_id is a UUID the box makes at the start of the trip, so a server
// ties the trip to its sn; trip_date (YYYYMMDD, local) and trip_number
// (the day's running number) say which trip it was to a person. A trip
// from before they existed (header format 3, 4) has none: it is not
// uploaded.
//
// tempmin/tempmax come from the trip's header row and are repeated on
// every row, so each row reads on its own. `timestamp` is local time,
// hh:mm:ss DD/MM/YYYY (config tz_offset_min); `utc` is the same moment in
// Unix seconds, for machines. With timeok false neither can be trusted,
// and the row carries `boot` and `up_s` instead, to order it by.
#pragma once

#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "record.h"

struct LogRow {
  uint32_t utc;
  uint8_t time_q;
  uint16_t boot;
  uint32_t tick_ms;
  uint8_t event;          // RowEvent
  int16_t temp_c100;      // I16_NONE: none
  uint8_t alarms;         // bit per Alarm
  uint8_t gnss;           // GnssState
  int32_t lat_e7, lon_e7;
  uint16_t motion;
  uint8_t battery;        // %, 0xFF unknown
  uint8_t internet;       // Link
  int32_t detail;
};

struct RowHeader {
  bool valid;
  uint32_t trip;
  int16_t tempmin_c10, tempmax_c10;
  uint16_t hyst_c10, dwell_s, period_s, schema;
  int32_t cal_offset_c100, cal_gain_ppm;
  uint32_t cal_version;
  char fw[17];
  char sn[25];
  int32_t temp_adj_c100;    // format 4; 0 in format 3
  bool has_id;              // format 5: the three below are there
  uint8_t uuid[16];
  uint32_t date;            // YYYYMMDD, local; 0: the time was not known
  uint16_t number;          // the day's running number, from 1
};

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", 37 bytes with the NUL.
void uuid_str(const uint8_t u[16], char out[37]);
bool uuid_parse(const char *s, uint8_t u[16]);

struct RowSummary {
  bool valid;
  uint8_t reason;
  uint32_t samples;
  int16_t min_c100, max_c100;
  uint16_t alarms_raised;
  uint32_t motion;
};

// Encodes into `buf`; the header or summary only on the rows that carry
// one (TRIP_START, TRIP_STOP). Returns the length, 0 if it did not fit.
size_t row_encode(const LogRow &r, const RowHeader *h, const RowSummary *s, uint8_t *buf,
                  size_t cap);

// False if it is not a format 3 row. `h` and `s` (either may be null)
// are filled when the row carries them, and marked invalid otherwise.
bool row_decode(uint8_t type, const uint8_t *p, size_t len, LogRow *r, RowHeader *h,
                RowSummary *s);

const char *row_event_name(uint8_t e);
const char *row_alarm_name(uint8_t a);     // HIGH, LOW, PROBE, DOOR, BATTERY
const char *row_gnss_name(uint8_t g);      // none, last, fix
const char *row_link_name(uint8_t l);      // offline, wifi_only, online
// Active alarms as "HIGH|PROBE"; "" for none.
void row_alarm_str(uint8_t bits, char *out, size_t n);
// The detail column, in words ("TEMP_HIGH", "3412 mV", ...); "" if none.
void row_detail_str(const LogRow &r, char *out, size_t n);
// hh:mm:ss DD/MM/YYYY, local time; "" if the time is not known.
void row_time_str(uint32_t utc, int tz_min, char *out, size_t n);

// One row as a JSON object with the columns above.
cJSON *row_json(const LogRow &r, uint32_t seq, const char *sn, const RowHeader &h, int tz_min);

// CSV, the same columns in the same order.
void row_csv_header(char *out, size_t n);
void row_csv(const LogRow &r, uint32_t seq, const char *sn, const RowHeader &h, int tz_min,
             char *out, size_t n);

// The trip's file name: TRIP_<SN>_<YYMMDDhhmm>.csv, the whole SN as on
// the label and the local start time (decided 2026-10-05): long, but
// found by the SN on the box and dependent on no numbering rule. A trip
// started without a known time is TRIP_<SN>_<id>.
void row_file_name(const char *sn, const LogRow &start, uint32_t trip, int tz_min, char *out,
                   size_t n);

// The factory SN pattern, which does not change (2026-10-05):
//   PPPVv-LllYY-MMYY-NNN    e.g. mCDV1-L0169-1069-001
//   PPP product, v version, L lot ll of year YY, made month MM of year YY
//   (Buddhist era, last two digits), NNN unit 001-999.
bool sn_pattern_ok(const char *sn);

// The USB drive's volume label (FAT: 11 characters, capitals): the first
// two letters of the product, the version, the lot with its L, the unit
// -- mCDV1-L0169-1069-001 -> MC1L0169001 (decided 2026-10-05; the L stays
// so 0169 is not read as a month). An SN off the pattern gives its first
// 11 letters and digits.
void sn_usb_label(const char *sn, char *out, size_t n);
