#include "cape_progress.h"

void cape_progress_init(cape_progress* p){
    if(!p)
        return;
    p->phase = CAPE_PHASE_READ;
    p->base = 0;
    p->width = 0;
    p->prep = 0;
    p->expected = 0;
    p->steps = 0;
    p->floor = 0;
}

static void band_of(cape_phase phase, unsigned* base, unsigned* width, unsigned* prep){
    switch(phase){
    case CAPE_PHASE_WRITE:
        *base = CAPE_PROGRESS_WRITE_BASE;
        *width = CAPE_PROGRESS_WRITE_WIDTH;
        *prep = CAPE_PROGRESS_WRITE_PREP;
        return;
    case CAPE_PHASE_VERIFY:
        *base = CAPE_PROGRESS_VERIFY_BASE;
        *width = CAPE_PROGRESS_VERIFY_WIDTH;
        *prep = 0;
        return;
    case CAPE_PHASE_READ:
    default:
        *base = CAPE_PROGRESS_READ_BASE;
        *width = CAPE_PROGRESS_READ_WIDTH;
        *prep = 0;
        return;
    }
}

// What the current phase's own state says the value is, ignoring the floor: base, plus the prep lump once it has
// been stepped, plus the files done so far, never more than "expected" of them and never past the band's own
// width (perfile_width = width - prep, divided evenly over "expected" files; 0 files expected is one lump instead).
static unsigned raw_value(const cape_progress* p){
    unsigned perfile_width = p->width - p->prep;   // prep < width always: band_of() never sets prep == width
    unsigned extra_steps = p->prep ? (p->steps > 0 ? p->steps - 1 : 0) : p->steps;   // steps beyond the prep, if any
    unsigned perfile_value;
    if(p->expected == 0)
        perfile_value = extra_steps > 0 ? perfile_width : 0;
    else {
        unsigned file_steps = extra_steps > p->expected ? p->expected : extra_steps;
        perfile_value = (file_steps * perfile_width) / p->expected;
    }
    unsigned prep_value = p->prep && p->steps >= 1 ? p->prep : 0;
    return p->base + prep_value + perfile_value;
}

void cape_progress_begin(cape_progress* p, cape_phase phase, unsigned expected){
    if(!p)
        return;
    band_of(phase, &p->base, &p->width, &p->prep);
    p->phase = phase;
    p->expected = expected;
    p->steps = 0;
    unsigned raw = raw_value(p);
    if(raw > p->floor)
        p->floor = raw;
}

void cape_progress_begin_alone(cape_progress* p, cape_phase phase, unsigned expected){
    if(!p)
        return;
    p->base = 0;
    p->width = CAPE_PROGRESS_TOTAL;
    p->prep = 0;
    p->phase = phase;
    p->expected = expected;
    p->steps = 0;
    unsigned raw = raw_value(p);
    if(raw > p->floor)
        p->floor = raw;
}

void cape_progress_step(cape_progress* p){
    if(!p)
        return;
    p->steps++;
    unsigned raw = raw_value(p);
    if(raw > p->floor)
        p->floor = raw;
}

void cape_progress_expect(cape_progress* p, unsigned expected){
    if(!p)
        return;
    p->expected = expected;
    unsigned raw = raw_value(p);
    if(raw > p->floor)
        p->floor = raw;
}

unsigned cape_progress_value(const cape_progress* p){
    return p ? p->floor : 0;
}

const char* cape_progress_phase_name(cape_phase phase){
    switch(phase){
    case CAPE_PHASE_WRITE:
        return "write";
    case CAPE_PHASE_VERIFY:
        return "verify";
    case CAPE_PHASE_READ:
    default:
        return "read";
    }
}
