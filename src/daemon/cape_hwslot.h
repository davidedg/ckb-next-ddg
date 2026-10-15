#ifndef CAPE_HWSLOT_H
#define CAPE_HWSLOT_H

/*
 * Complete K95 RGB Platinum slot contract. Capability: hwslot. No internal format version or legacy endpoint.
 * GUI: @N get :hwslot:<m>:<p>     daemon: mode <m> hwslot <generation> <page>/<pages> <words...>
 * Cache errors: mode <m> hwslot <generation> error nocache|range. m is 1..3; pages start at 1.
 * Every page belongs to the same generation and identity; the GUI publishes only a complete record.
 * Record: id:<slot-id> st:empty|ok|none [ro:<encoded-reason>] [zip:<state>] [nm:<part>/<parts>:<text>]
 * [wl:<hh>] [ind:<24-hex>] <complete-lighting-body> <binding-words> end.
 * Empty: id:0 st:empty lights:known:0:0 end. Readonly slots have lights:unknown and no bindings.
 * Complete lighting grammar and typed parameters: cape_lightmodel.h. Binding grammar remains m:, a:, e:, im:, tx:.
 * Names/text use canonical percent-encoded UTF-8, with whole characters in fragments of at most 300 encoded bytes.
 * Bindings retain their existing byte/event limits and text/imitate metadata; lighting never uses an RGB picture.
 *
 * @N hwslot begin:<txn>:<m>:<base-id> [name:<txn>:<part>/<parts>:<text>]
 * light:<txn>:keep OR light:<txn>:model followed by the complete known lighting body (txn after every tag),
 * [wl:<txn>:<hh>] [ind:<txn>:<24-hex>] bind:<txn>:keep|model [map:/act:/ev:/im: binding body]
 * end:<txn>:<count>. count includes begin and every word before end. txn is eight nonzero lowercase hex digits.
 * One transaction per slot and notification owner. Any malformed, missing, duplicate or out-of-order field fails it.
 * Commands: check:<txn>:<m>, save:<txn>:<m>, abort:<txn>:<m>, read:<m>, erase:<m>:<base-id>.
 * Read always refreshes from PROFILE.ZIP; get only serves cache. A failure preserves the previous cache and GUI draft.
 * Check validates the whole recomposed profile and freezes its base, destination identity and exact files, including ZIP.
 * Repeated Check returns that plan. Save checks identity before writing and never recompiles it; explicit Retry retains it.
 * SAME needs no write. Enabled lighting layers are compiled from the complete model.
 * Check summary: verdict colours=<n> layers=<total> active=<enabled> and name/bindings/winlock/indicators/model changes,
 * lights=kept, kept=yes and reason as applicable. No image-replacement warnings or RGB staging words.
 * Abort, owner close, attach and Erase release the transaction; a failed Save retains it for explicit Retry.
 * Save/Erase preserve the qualified iCUE USB sequence, progress, read-back verification and error reports.
 * Each physical write or real daemon/GUI start requires independent authorization. hwsave writes no ZIP and is refused.
 */

#include "cape_slot.h"

#include <stddef.h>
#include <stdint.h>

#define CAPE_HWSLOT_TOKEN        "hwslot"
#define CAPE_HWSLOT_LINE_MAX     512   // bytes of a line, the newline included
#define CAPE_HWSLOT_WORD_MAX     400   // bytes of a word
#define CAPE_HWSLOT_TEXT_MAX     300   // encoded bytes of the text of one nm:/name: word (and of a reason)
#define CAPE_HWSLOT_EVENT_BYTES  128   // bytes of the events of one e:/ev: word (up to 256 hex digits; 64 events of 2 bytes)
#define CAPE_HWSLOT_NAME_WORDS   8     // nm:/name: words of one name at most
#define CAPE_HWSLOT_TX_WORDS     48    // tx: words of one string at most (1024 characters of 4 bytes, encoded: 41)
#define CAPE_HWSLOT_ID_MAX       52    // "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}:ffffffff" and a terminator

// Percent-encodes len bytes into out (NUL-terminated). Returns the encoded length, or CAPE_E_CAP if it does not fit.
int cape_hwslot_encode(const uint8_t* in, size_t len, char* out, size_t cap);
// Decodes text into out. Returns the number of bytes, CAPE_E_RANGE for a malformed text (a byte that should have been encoded, a
// bad %XX, lowercase hex), CAPE_E_CAP if it does not fit.
int cape_hwslot_decode(const char* text, size_t len, uint8_t* out, size_t cap);
// The id of a slot table entry (CAPE_SLOTID_SIZE bytes; NULL = an empty slot): "0" or "<guid>:<rev>" (see above).
void cape_hwslot_id(const uint8_t* entry, char out[CAPE_HWSLOT_ID_MAX]);
// The slot table entry of an id as cape_hwslot_id() writes it (and no other way). Returns 1, 0 for "0" (an empty slot: entry is
// zeroed) or CAPE_E_RANGE.
int cape_hwslot_parse_id(const char* text, size_t len, uint8_t entry[CAPE_SLOTID_SIZE]);
// UTF-16 to UTF-8 and back. Return the number of bytes/units, CAPE_E_RANGE for an unpaired surrogate, invalid UTF-8 (overlong,
// surrogates, beyond U+10FFFF) or a zero, CAPE_E_CAP if it does not fit.
int cape_hwslot_utf8(const uint16_t* in, size_t units, uint8_t* out, size_t cap);
int cape_hwslot_utf16(const uint8_t* in, size_t len, uint16_t* out, size_t cap);

#endif
