// Firmware updates over the air: "controlled update, rollback" (§7).
//
// The server names a file and the box fetches it itself, the way eTEMP
// V2 does it: a message on mcold/<sn>/firmware carries just the file
// name (`mCOLD_0.7.1.bin`), the box joins it to the base URL it keeps in
// NVS and downloads the image over HTTP(S) straight into the other OTA
// slot. A whole http(s) URL is taken as it is.
//
// Nothing about the file is trusted because of where it came from. The
// box checks, in order:
//
//   1. the image header (esp_app_desc_t, in the first few KB): the same
//      project as the running firmware, and a newer version -- an older
//      or the same one is not installed (the console can force it);
//   2. while writing: the image's own SHA-256, appended at build time,
//      so a truncated or damaged download is never booted;
//   3. the signature (RSA-3072, the Secure Boot V2 scheme without
//      hardware secure boot): signed with the project key, kept off the
//      repository (tools/sign_app.py). The key to check it with is taken
//      from the RUNNING image's own signature block, so an unsigned or
//      foreign image cannot get in even through a broker or file server
//      someone else controls;
//   4. after the reboot: rollback. The new image starts "pending" and
//      has OTA_VERIFY_MS to reach the broker. If it does, it is kept; if
//      it does not, or it crashes first, the bootloader goes back to the
//      image it came from, and that one reports the rollback.
//
// Not during a trip, and not on a low battery: the request waits and
// runs once both allow it. What happened goes back on
// mcold/<sn>/ota/state (retained), so the server can see which boxes
// took an update.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Early, after NVS and before the uplink: settles the first boot of a
// new image, or notices a rollback.
void ota_start(void);

// A file to install: a name (joined to the base URL) or a whole URL.
// From the server, a name already handled is ignored, so a retained
// message seen at every session does nothing the second time. `force`
// (console only) skips the version and trip checks -- never the
// signature.
void ota_request(const char *name, bool from_server, bool force);

// Downloading, or a new image still to prove itself: keep the session up.
bool ota_busy(void);
// Wants the broker now, records or not: a new image to confirm. (A result
// the server has not heard yet waits for the next session as it comes.)
bool ota_needs_broker(void);
// The broker answered (uplink's MQTT task): what a new image proves itself by.
void ota_on_connected(void);

// For the uplink, which does all the publishing: the latest word for
// ota/state, numbered so each goes out once. False when there is none.
bool ota_outbox(char *buf, size_t n, uint32_t *serial);
// It went out (retained): a kept word need not be kept any longer.
void ota_outbox_sent(uint32_t serial);

bool ota_set_base(const char *url);
void ota_print(void);
// Bench: the next new image will not confirm itself, to watch a rollback.
void ota_test_rollback(void);

static const uint32_t OTA_VERIFY_MS = 180000;
static const int OTA_MIN_SOC = 30;       // % on battery to start a download
