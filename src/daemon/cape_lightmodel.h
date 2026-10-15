#ifndef CAPE_LIGHTMODEL_H
#define CAPE_LIGHTMODEL_H

#include "cape_profile.h"

// The complete lighting part of a slot, in XML order. Unknown is distinct from an empty known list.
// The model owns its layers, strings and transitions. Copying preserves the original doubles.
typedef struct {
    int known;
    cape_profile_layer* layers;
    uint32_t count;
} cape_lightmodel;

// out must be empty on entry to copy/take; on failure it is unchanged. Free is safe on an empty or partial owned model.
void cape_lightmodel_free(cape_lightmodel* model);
int cape_lightmodel_copy(const cape_lightmodel* source, cape_lightmodel* out);

// Words of the complete lighting body: lights, then consecutive layer blocks ending in lend.
// emit receives one NUL-terminated word (at most CAPE_HWSLOT_WORD_MAX bytes) and returns CAPE_OK or an error.
// txn is NULL for a record, or eight lowercase hex digits for staging; it is inserted after every tag.
// No callback is made for a structurally invalid model. A callback error stops emission.
// Body: lights:unknown or lights:known:total:active, then each layer in consecutive XML order:
// layer:i:uuid:enabled, lname:i:part/parts:text, select:i:all|concrete:nkeys, key:i:j:part/parts:text,
// run:i:startpress:startprofile:stoppress:stoprelease:after:play, effect:i:kind:uuid, ename:i:part/parts:text,
// animation:i:duration_ms:brightness, curve:i:npoints and point:i:j:time:rgb|rgba:r:g:b[:a] for custom types,
// param:i:field:value[:value] in the effect's schema order, lend:i. Names are percent-encoded UTF-8;
// doubles have 17 significant digits; colour parameters are two full ARGB words. The field/type table is in cape_lightmodel.c.
// Missing, duplicate and inapplicable fields fail. Solid needs paired endpoints of one RGBA colour, including when muted.
// Structural XML bounds apply here; hardware semantics apply at preflight.
typedef int (*cape_lightmodel_word)(void* context, const char* word);
int cape_lightmodel_emit(const cape_lightmodel* model, const char* txn, cape_lightmodel_word emit, void* context);

// Incremental strict decoder of the record body (without transaction fields). Allocation follows validated model bounds,
// never the advertised number of text fragments. Unknown/empty finish at lights; other models finish at the last lend.
// On error the reader stays failed. A completed model is moved out only by take; free also releases partial data.
typedef struct cape_lightmodel_reader cape_lightmodel_reader;
cape_lightmodel_reader* cape_lightmodel_reader_new(void);
void cape_lightmodel_reader_free(cape_lightmodel_reader* reader);
int cape_lightmodel_read_word(cape_lightmodel_reader* reader, const char* word);
int cape_lightmodel_ready(const cape_lightmodel_reader* reader);
int cape_lightmodel_take(cape_lightmodel_reader* reader, cape_lightmodel* out);

#endif
