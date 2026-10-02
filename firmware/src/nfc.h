// ST25DV04KC dynamic NFC tag: identity, phone detection, NDEF.
//
// One chip at two I2C addresses: 0x53 for user memory and the dynamic
// registers, 0x57 for the system area. Both are addressed with SIXTEEN
// bits -- probing it with one-byte register reads returns nothing at
// every address and looks exactly like a dead part.
//
// Phone detection uses IT_STS_Dyn, which latches the field's rising and
// falling edges and clears on read. FIELD_ON in EH_CTRL_Dyn is only true
// while the phone is actually there, so a quick tap falls between two
// polls; the latched edges cannot be missed that way. This works with
// the factory GPO configuration, so nothing in the system area has to
// be written -- which matters, because a wrong byte there can disable
// the RF interface, and some settings cannot be undone.
//
// While a phone is talking to the tag, the tag refuses I2C. That is the
// phone being there, not a bus fault, so these polls are made untracked
// and this driver keeps its own health: only a refusal that outlasts
// any plausible RF session counts against the part.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum class NfcField : uint8_t {
  Unknown = 0,
  Absent,
  Present,     // FIELD_ON read back set
  RfBusy,      // I2C refused: the RF side holds the tag
};

struct NfcPoll {
  NfcField field;
  bool arrived;    // a field rose since the last poll
  bool left;       // a field fell since the last poll
};

// Reads IC_REF and the UID. False if either half of the part is silent.
bool nfc_begin(void);

// UID as printed on readers, most significant byte first. False until
// nfc_begin() has read it.
bool nfc_uid(uint8_t out[8]);
uint8_t nfc_ic_ref(void);

bool nfc_poll(NfcPoll *out);
uint32_t nfc_tap_count(void);

// Writes a complete Type 5 tag image -- capability container, NDEF TLV,
// terminator -- from `ndef`, a serialised NDEF message. Blocks whose
// contents are already correct are not rewritten: EEPROM endurance is
// finite and most writes of an identity record change nothing. Every
// block written is read back and compared.
bool nfc_write_ndef(const uint8_t *ndef, size_t n);

bool nfc_read_user(uint16_t offset, uint8_t *buf, size_t n);

// The 04KC has 512 bytes of user memory.
static const size_t NFC_USER_BYTES = 512;
