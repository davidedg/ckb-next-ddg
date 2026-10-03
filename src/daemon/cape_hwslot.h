#ifndef CAPE_HWSLOT_H
#define CAPE_HWSLOT_H

/*
 * hwslot1: how the GUI reads, edits and saves the on-board slots of a K95 RGB Platinum through the daemon.
 * This comment is the specification, of the daemon (cape_hwbind.c, cape_stage.c) and of the GUI (hwbinding.cpp) alike.
 *
 * Capability. The daemon writes the word "hwslot1" in the features node of a K95 RGB Platinum whose firmware is a tested one.
 * A GUI that does not find it uses none of this (and keeps the hwsave save); a daemon without it answers none of this.
 *
 * Lines. Every line, both ways, is a single write() of at most CAPE_HWSLOT_LINE_MAX bytes including its newline (POSIX PIPE_BUF
 * is at least 512: a line that long is written whole or not at all, also on macOS). Words are separated by one space and never
 * contain a space. A slot is m = 1..3 (the firmware's index + 1, like "mode N"). A key is a key index of the K95P files in two
 * lowercase hex digits, 00..97 (the first CAPE_BIND_K95_KEYS entries of the daemon's keymap, also those without a name); a remap
 * may also send c8..cc (mouse1..mouse5). Every other number is lowercase hex where it says so and decimal otherwise. Text (names,
 * reasons) is UTF-8, percent-encoded: every byte other than A-Z a-z 0-9 - . _ ~ is %XX with uppercase hex.
 *
 * An id is the identity of a slot as the GUI knows it from "get :hwid": "0" for an empty slot, else "<guid>:<rev>", with the
 * GUID as :hwid prints it ("{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}") and <rev> the lowercase hex of the four bytes that follow
 * the GUID in the slot table (the cookie and 00 01), as :hwid prints them.
 *
 * The read of a slot on demand (section 3), the heartbeat of a check and the events of 4 bytes in e: and ev: are part of
 * "hwslot1": no daemon or GUI with an earlier version of it was ever published.
 *
 * 1. The record of a slot: what the daemon's cache holds for it (no USB traffic: the cache is what the last read found).
 *
 *    GUI:    @N get :hwbind:<m>:<p>                                  p = 1, 2, ... one page at a time, each after the last one came
 *    daemon: mode <m> hwbind <gen> <p>/<pages> <word>...
 *            mode <m> hwbind <gen> error nocache                     the slot was not read whole (at attach the daemon reads only
 *                                                                    PROFILE.I of each slot, as iCUE does: hwslot read:<m> reads the
 *                                                                    rest, section 3), or its read failed: no record. An empty slot
 *                                                                    has its record from the attach
 *            mode <m> hwbind <gen> error range                       p is not 1..pages (m not 1..3 gets no answer at all)
 *
 *    <gen> is the decimal generation of the slot's cache: it changes whenever the cache of that slot is replaced (an attach, the
 *    read-back after a save). Pages of different generations do not make a record. The words of pages 1..pages, in order, are the
 *    record; the daemon fills each page with as many whole words as fit in a line (prefix and newline included), in order:
 *
 *      v:1                                   the version of the record
 *      id:<id>
 *      st:ok | st:empty | st:raw:<text> | st:broken:<text>     the state of the bindings (cape_binding.h), with the reason
 *      ro:<text>                             the slot cannot be rewritten at all (PROFILE.I is missing or not valid), with the
 *                                            reason cut like a reason of st:; present only then (the bindings say nothing of it)
 *      nm:<i>/<n>:<text>                     the name, in n words (text cut as below), i = 1..n in order; not for an empty slot
 *      wl:<hh>                               the Win Lock options (byte 1 of PROFILE.DAT), if PROFILE.DAT was read
 *      ind:<24 hex>                          the indicator colours of PROFILE.I (profile, brightness, lock on, lock off: RGB each),
 *                                            if PROFILE.I was read
 *      lt:static | lt:empty | lt:effects | lt:unknown      what the lighting is: static layers, none, effects, not understood
 *      ly:<n>                                how many lighting layers
 *      m:<key>:<dst>                         a remap, one word each, in ascending order of key (st:ok only)
 *      a:<key>:<file>:<sub>:<start>:<run>:<rep>:<nev>:<flags>
 *                                            a macro action, in ascending order of key (st:ok only): the name of its file in the slot,
 *                                            subtype, start, run, repeat and flags (CAPE_BIND_F_*) in two hex digits, nev events
 *      e:<key>:<off>:<hex>                   the events of the action of key, as in the file (2 or 4 bytes each by the first byte,
 *                                            cape_format.h: four or eight hex digits), right after its a: word: each word as many
 *                                            whole events as fit in CAPE_HWSLOT_EVENT_BYTES bytes, off the index of its first event
 *                                            (so with events of 2 bytes only: off = 0, 64, 128...). A word that is not the last
 *                                            holds 126 or 128 bytes, and 126 only when the next word starts with an event of 4
 *                                            bytes; readers refuse any other cut (no e: word for an action with no events)
 *      end                                   the last word of the last page
 *
 *    A text (a name, a reason) is cut into words of at most CAPE_HWSLOT_TEXT_MAX encoded bytes each, each holding as many whole
 *    characters as fit; a reason longer than one word is cut short at a character (it is only shown). An empty slot is
 *    "v:1 id:0 st:empty lt:empty ly:0 end". A RAW or BROKEN slot has no m:, a: or e: words: its bindings are not a model.
 *
 * 2. The preparation of a save: what the slot is to become, sent before anything is checked or written (no USB traffic).
 *
 *    GUI:    @N hwslot <word> [<word>...]                            N = 1..9: the notification node that owns the transaction
 *
 *    The words of a transaction, in this order (the GUI makes them this way, the daemon accepts nothing else):
 *      begin:<txn>:<m>:<base>                <txn> 8 lowercase hex digits, not all zeros, chosen by the GUI; <base> the id of the
 *                                            slot the edit was made on (the id before a failed save, also for a new begin after
 *                                            it). A begin ends every open transaction of slot m and every one with the same txn.
 *      name:<txn>:<i>/<n>:<text>             the name, cut as in nm:, i = 1..n in order; required for an empty slot; none = keep
 *      light:<txn>:keep | light:<txn>:pic    once: keep the lighting files as they are, or make static layers (as hwsave) of the
 *                                            colours of the rgb: words, where a LED that is not listed is black (no layer)
 *      rgb:<txn>:<led>=<rrggbb>[,...]        after light:pic only: the LEDs that are not black, by the names of the daemon's
 *                                            keymap, in ascending byte order of name, each once, as many per word as fit
 *      wl:<txn>:<hh>                         at most once: the Win Lock options (byte 1 of PROFILE.DAT) to write; none = the slot's
 *                                            (01 for an empty slot, or when its PROFILE.DAT was not read). Whatever bind: says, the
 *                                            PROFILE.DAT written has them (a DAT kept as it is changes in that byte only)
 *      ind:<txn>:<24 hex>                    at most once: the indicator colours (profile, brightness, lock on, lock off: RGB each)
 *                                            for PROFILE.I; none = the slot's (iCUE's defaults for an empty slot)
 *      bind:<txn>:keep                       once, either this: keep PROFILE.MAP, PROFILE.DAT and the macro files as they are,
 *      bind:<txn>:model                      or this: write the bindings of the map:/act:/ev: words
 *      bind:<txn>:model:recreate             (the same, for a slot whose bindings are RAW or BROKEN, which are replaced: an error on
 *                                            an OK slot, and required for a RAW or BROKEN one)
 *      map:<txn>:<key>:<dst>                 after bind:model only: a remap, ascending order of key
 *      act:<txn>:<key>:<sub>:<start>:<run>:<rep>:<nev>     after bind:model only: a macro action (as in a:), ascending order of
 *                                            key, a key at most once across map: and act:
 *      ev:<txn>:<key>:<off>:<hex>            its events, cut as in e:, right after its act:
 *      end:<txn>:<words>                     the decimal count of the words of the transaction before this one, begin included
 *    and then only:
 *      check:<txn>:<m>                       what saving it would do (it reads the slot, which may take long: while it reads,
 *                                            "mode <m> hwsavecheck progress <done> <total> read" lines come as a heartbeat, the
 *                                            numbers as in "hwsave progress"): the "hwsavecheck" line of hwsave, with the words
 *                                            bindings=changed when the bindings (the keys: not the Win Lock options) differ from the
 *                                            slot's, winlock=changed and indicators=changed when wl: and ind: do, in this order, and,
 *                                            with light:keep, colours=0, layers = the layers kept and the word lights=kept; the
 *                                            verdict is same only when nothing differs (buttons=changed compares the picture with
 *                                            the indicator colours that would be written). A slot that changed since
 *                                            the base was read (compared with the copy kept by a failed save, if there is one) is
 *                                            "refused ... reason=..."; a preparation that is wrong or incomplete, or a transaction
 *                                            that is not open for this node and slot, is "hwsavecheck error err=staging <why>"
 *      save:<txn>:<m>                        the save, after a check: the "hwsave" lines of hwsave. It writes also when the check said
 *                                            same (the GUI decides what to skip). After a check that said refused it is
 *                                            "hwsave skipped refused reason=..." and sends nothing; without a check, or for a
 *                                            transaction that is not open for this node and slot, it is "hwsave fail stage=prepare
 *                                            file=- slot=unchanged kept=no err=staging <why>" and sends nothing. The transaction
 *                                            stays after a failure, for a Retry (the same save:), and ends with a success.
 *      abort:<txn>:<m>                       forget it (no answer)
 *    The answers to check: and save:, and the "hwsave progress" lines, go to node N only, as "mode <m> ...", m the slot of the
 *    word. A save made while the keyboard is in hardware mode switches it to software mode for the save and back (the daemon
 *    never writes in hardware mode): when switching back fails after a verified write the final line is "hwsave ok packets=<n>
 *    hwmode=failed" (on a fail line, hwmode=failed comes before err=). A save whose files do not fit in the free sectors of the
 *    flash is refused before anything is written: "hwsave fail stage=clear file=- slot=unchanged kept=no space=<need>/<free> err=...",
 *    need and free in sectors of 4 KiB (the sectors of the slot's old files count as used), after hwmode=failed if there is one;
 *    trying again gives the same. A transaction also ends when its node is closed and when the
 *    keyboard is attached again. Every word is at most CAPE_HWSLOT_WORD_MAX bytes. A word of a transaction that is not open for
 *    this node is ignored; a word that is wrong for an open one makes it fail, and check: or save: say why.
 *
 * 3. The read of a slot on demand: the daemon reads its bindings and lighting (it has only PROFILE.I of it since the attach), then the
 *    record of section 1 has it. Nothing is written and the mode of the keyboard is not changed (in hardware mode the daemon sends
 *    07 04 01 after the read, which leaves the keyboard in hardware mode and lights its active slot again; so after the reads of a
 *    check:, section 2).
 *
 *    GUI:    @N hwslot read:<m>                                     outside any transaction, from any node; m 1..3 (else ignored)
 *    daemon: mode <m> hwread progress <done> <total> read           while it reads, as "hwsave progress" (total 1000, done never
 *                                                                    goes down), also as a heartbeat during a slow status
 *            mode <m> hwread ok gen=<gen> packets=<n>               the cache of the slot is now all of it, generation <gen>;
 *                                                                    packets=0: it already was, nothing was sent
 *            mode <m> hwread fail err=<text>                        the cache of the slot is as it was (err is prose, last)
 *    The lines go to node N only. A read of a slot whose entry of the slot table has changed since the attach takes the new one:
 *    get :hwid and the record say it.
 */

#include "cape_slot.h"

#include <stddef.h>
#include <stdint.h>

#define CAPE_HWSLOT_TOKEN        "hwslot1"
#define CAPE_HWSLOT_LINE_MAX     512   // bytes of a line, the newline included
#define CAPE_HWSLOT_WORD_MAX     400   // bytes of a word
#define CAPE_HWSLOT_TEXT_MAX     300   // encoded bytes of the text of one nm:/name: word (and of a reason)
#define CAPE_HWSLOT_EVENT_BYTES  128   // bytes of the events of one e:/ev: word (up to 256 hex digits; 64 events of 2 bytes)
#define CAPE_HWSLOT_NAME_WORDS   8     // nm:/name: words of one name at most
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
