// ATGM336H GNSS: position as a measurement with an age, never as a fact.
//
// Three different things can be true of this module, and only the last
// is a position:
//
//   no valid sentences -- wrong baud, wrong wiring, no power. A device
//                         fault. Note that at the wrong baud the module
//                         still produces bytes (488 of them in twelve
//                         seconds at 9600, all nonsense), so "something
//                         arrived" proves nothing: only a sentence whose
//                         checksum matches counts as hearing the module.
//   sentences, no fix  -- the module is fine and cannot see the sky.
//                         Not a fault; reported as "no position".
//   a fix              -- RMC status A. Carries its own timestamp, and
//                         goes stale: the last known position is never
//                         to be passed off as the current one.
//
// The module runs at 115200, not the 9600 its documentation suggests.
// Measured on this board. It shares a rail with the SD card, which the
// rail manager reference-counts.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct GnssFix {
  bool valid;               // RMC status A at the time it was received
  double lat_deg, lon_deg;  // + north, + east
  float speed_kmh;
  float course_deg;

  uint8_t quality;          // GGA: 0 none, 1 GPS, 2 DGPS, 6 estimated
  uint8_t sats;             // satellites used
  float hdop;
  bool alt_valid;
  float alt_m;

  bool time_valid;          // date and time from a status-A RMC
  struct tm utc;

  uint32_t at_ms;           // uptime when received; 0 means never
};

// What the receiver can hear, from GSV. Indoors this is the useful
// number: "no fix" alone does not say whether the module heard nothing
// or was two satellites short.
struct GnssSky {
  uint8_t in_view;          // satellites the module expects overhead
  uint8_t heard;            // of those, how many it is receiving (SNR > 0)
  uint8_t best_snr;         // dB-Hz; a fix generally wants a few above ~30
  uint32_t at_ms;           // 0: no GSV seen yet
};

void gnss_sky(GnssSky *out);

struct GnssStats {
  uint32_t bytes;
  uint32_t sentences;       // checksum good
  uint32_t bad_checksum;    // the wrong-baud signature
  uint32_t overlong;        // lines that did not fit: also noise
  uint32_t sessions;
  uint32_t fixes;
};

// Powers the rail and opens the UART. The GPIOs are released again by
// gnss_power_off(), so the MCU does not back-power an unpowered module
// through its RX pin.
bool gnss_power_on(void);
void gnss_power_off(void);
bool gnss_is_on(void);

// Reads whatever the UART has for up to `wait_ms` and parses it.
// Returns the number of good sentences seen.
int gnss_pump(uint32_t wait_ms);

// The latest fix. `valid` and `at_ms` say whether and how recently it
// was a real one; a caller deciding whether to use it must look at
// both. Returns false if no fix has ever been received.
bool gnss_last_fix(GnssFix *out);

// Uptime of the last good sentence of any kind (0: never). The
// difference between this and the fix time is the difference between
// "module alive, no sky" and "module silent".
uint32_t gnss_last_sentence_ms(void);

void gnss_stats(GnssStats *out);

static const uint32_t GNSS_BAUD = 115200;

// How long after power-up a working module must have produced a
// checksum-valid sentence. It sends once a second; three seconds of
// nothing is a fault, not a slow start.
static const uint32_t GNSS_ALIVE_MS = 3000;
