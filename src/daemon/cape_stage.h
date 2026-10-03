#ifndef CAPE_STAGE_H
#define CAPE_STAGE_H

/*
 * The preparation of a save of a K95 RGB Platinum slot (hwslot1, section 2 of cape_hwslot.h): the words of "hwslot" as they come,
 * checked against the grammar and gathered into one transaction per slot. What the words mean for the slot (the base against the
 * cache, a name for an empty slot, recreate against the state of the bindings, the limits of an edit) is judged by check: and save:,
 * not here. Pure code: nothing here talks to the device.
 */

#include "cape_hwslot.h"

#define CAPE_STAGE_SLOTS     3
#define CAPE_STAGE_OWNERS    10     // notification nodes 1..9 (OUTFIFO_MAX)
#define CAPE_STAGE_LED_NAME  16

typedef enum {
    CAPE_STAGE_LIGHT_UNSET = 0,
    CAPE_STAGE_LIGHT_KEEP,
    CAPE_STAGE_LIGHT_PIC
} cape_stage_light;

typedef enum {
    CAPE_STAGE_BIND_UNSET = 0,
    CAPE_STAGE_BIND_KEEP,
    CAPE_STAGE_BIND_MODEL
} cape_stage_bind;

typedef struct {
    char led[CAPE_STAGE_LED_NAME];
    uint8_t rgb[3];
} cape_stage_rgb;

typedef struct {
    int open;                         // there is a transaction for this slot
    int complete;                     // end: came, with the right count, and nothing was wrong
    int failed;                       // a word was wrong: why says which
    char why[CAPE_SLOT_WHY];
    uint32_t txn;
    int owner;
    int base_empty;                   // the edit was made on an empty slot
    uint8_t base[CAPE_SLOTID_SIZE];   // else the slot table entry it was made on
    unsigned words;                   // the words so far, begin included
    int phase;                        // where in the order of words the transaction is
    int has_name;
    uint16_t name[CAPE_INFO_NAME_MAX];
    size_t name_units;
    cape_stage_light light;
    cape_stage_rgb* rgb;              // the LEDs that are not black, in ascending order of name
    size_t nrgb;
    int has_wl;                       // wl: came: the Win Lock options to write
    uint8_t wl;
    int has_ind;                      // ind: came: the indicator colours to write (profile, brightness, lock on, lock off: RGB)
    uint8_t ind[12];
    cape_stage_bind bind;
    int recreate;
    cape_binding_model model;         // bind:model: the bindings (its Win Lock options are the default and do not count: wl: says)
    int checked;                      // set by the caller when check: answered (and refused, if it said so)
    int refused;
    char check_reason[CAPE_SLOT_WHY]; // the reason of a refusal of check:, for the save: that follows it
    // while the words come
    uint8_t name_utf8[1024];
    size_t name_len;
    unsigned name_next, name_total;
    size_t rgb_cap;
    int last_map, last_act;           // the last key of map: and of act: (ascending order)
    int act_key;                      // the act: whose ev: words are awaited, or -1
    uint8_t act_sub, act_start, act_run, act_rep;
    cape_event* act_ev;
    unsigned act_need, act_have;
    int act_four_next;                // the last ev: word stopped at 126 bytes: the next one starts with an event of 4 bytes
    size_t event_bytes;
} cape_stage_txn;

typedef struct {
    cape_stage_txn slot[CAPE_STAGE_SLOTS];
    unsigned ignored;                 // words of transactions that were not open for the node that sent them
} cape_stage;

typedef enum {
    CAPE_STAGE_WORD = 0,              // a word of a transaction (accepted, ignored, or the transaction failed)
    CAPE_STAGE_CHECK,
    CAPE_STAGE_SAVE,
    CAPE_STAGE_ABORT,
    CAPE_STAGE_MALFORMED              // not a word of hwslot1 at all (no transaction to blame): ignored
} cape_stage_request;

void cape_stage_init(cape_stage* st);
void cape_stage_free(cape_stage* st);
// Ends the transaction of a slot (a success, an attach) or every one of a node (the node was closed).
void cape_stage_end_slot(cape_stage* st, unsigned slot);
void cape_stage_end_owner(cape_stage* st, int owner);

// One word of "@<owner> hwslot ...". For check:, save: and abort:, *slot (0..2) and *txn say which transaction they name (abort:
// has already ended it if it was open for owner). Returns a cape_stage_request.
cape_stage_request cape_stage_word(cape_stage* st, int owner, const char* word, size_t len, unsigned* slot, uint32_t* txn);
// The transaction that a check: or save: names, if it is open for owner and slot and complete; else NULL, and why says what is wrong.
cape_stage_txn* cape_stage_get(cape_stage* st, int owner, unsigned slot, uint32_t txn, char* why, size_t cap);

#endif
