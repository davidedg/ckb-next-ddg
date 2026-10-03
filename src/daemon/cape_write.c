#include "cape_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------
// The packets of a save that the session may send: only while cape_write_slot() runs, only for its slot, only in these shapes

static int tail_zero(const uint8_t* p, size_t from){
    for(size_t i = from; i < CAPE_PKT_SIZE; i++){
        if(p[i])
            return 0;
    }
    return 1;
}

// The name of an "open" packet: 1 to CAPE_NAME_MAX valid characters from byte 4, then zeros to the end
static int packet_name_ok(const uint8_t* p){
    size_t n = 0;
    while(n <= CAPE_NAME_MAX && p[4 + n])
        n++;
    if(n < 1 || n > CAPE_NAME_MAX)
        return 0;
    char name[CAPE_NAME_MAX + 1];
    memcpy(name, p + 4, n);
    name[n] = '\0';
    return cape_name_valid(name) && tail_zero(p, 4 + n);
}

static int write_allowed(const cape_session* s, const uint8_t* p){
    const cape_wsession* w = s->extra;
    if(!w || w->target < 0 || w->target >= CAPE_SLOTS || !((w->mask >> w->target) & 1))
        return 0;
    switch(p[0]){
    case 0x0e:
        // the size of the slot and "write mode" (the other 0e 17 packets are on the read list)
        return p[1] == 0x17 && (p[2] == 0x03 || p[2] == 0x0b) && p[3] == 0x00 && tail_zero(p, 4);
    case 0x07:
        if(p[1] == 0x15){
            // 07 15 SS 00, then 20 bytes that are all zero (clear) or a GUID that is not, a cookie and 00 01 (set)
            if(p[2] != w->target || p[3] != 0x00 || !tail_zero(p, 24))
                return 0;
            int guid = 0, any = 0;
            for(int i = 4; i < 20; i++)
                guid |= p[i] != 0;
            for(int i = 4; i < 24; i++)
                any |= p[i] != 0;
            return !any || (guid && p[22] == 0x00 && p[23] == 0x01);
        }
        if(p[1] != 0x17)
            return 0;
        switch(p[2]){
        case 0x05:   // open for writing, in the slot that is selected and is the one being written
            return p[3] == 0x00 && s->slot == w->target && packet_name_ok(p);
        case 0x09:   // commit a burst
        case 0x0e:   // the transaction marker
            return p[3] == 0x00 && tail_zero(p, 4);
        default:
            return 0;
        }
    case 0x7f:
        // 7f NN LL 00, LL bytes of a file and zeros: the header of a lighting frame too, which is why this only holds inside a save
        return p[1] >= 1 && p[1] <= CAPE_BURST_SIZE / CAPE_CHUNK_SIZE && p[2] >= 1 && p[2] <= CAPE_CHUNK_SIZE && p[3] == 0x00
                && tail_zero(p, 4 + (size_t)p[2]);
    default:
        return 0;
    }
}

int cape_wsession_open(cape_wsession* w, const cape_io* io, uint8_t mask){
    if(!w)
        return CAPE_E_ARG;
    memset(w, 0, sizeof(*w));
    w->target = -1;
    w->mask = mask & ((1u << CAPE_SLOTS) - 1);
    const int rc = cape_session_open(&w->s, io);
    if(rc == CAPE_OK){
        w->s.extra_allowed = write_allowed;
        w->s.extra = w;
    }
    return rc;
}

void cape_wsession_close(cape_wsession* w){
    if(!w)
        return;
    w->target = -1;
    cape_session_close(&w->s);
}

const char* cape_wstage_name(cape_wstage stage){
    switch(stage){
    case CAPE_WSTAGE_NONE:
        return "none";
    case CAPE_WSTAGE_PREPARE:
        return "prepare";
    case CAPE_WSTAGE_CLEAR:
        return "clear";
    case CAPE_WSTAGE_IDENTITY:
        return "identity";
    case CAPE_WSTAGE_FILE:
        return "file";
    case CAPE_WSTAGE_MARKER:
        return "marker";
    case CAPE_WSTAGE_DONE:
        return "done";
    case CAPE_WSTAGE_VERIFY:
        return "verify";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------------
// The save

static void pkt_init(uint8_t* pkt, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3){
    memset(pkt, 0, CAPE_PKT_SIZE);
    pkt[0] = b0;
    pkt[1] = b1;
    pkt[2] = b2;
    pkt[3] = b3;
}

// The end of a save that did not finish (rep says where: its stage and file were set before the step that failed): the file that may
// be open is closed (once), the session is over, and the report says what happened
static int stop(cape_wsession* w, cape_write_report* rep, int err, unsigned tx0){
    rep->err = err;
    rep->status = w->s.last_status;
    w->target = -1;
    (void)cape_session_fail(&w->s, err);
    rep->packets = w->s.tx_packets - tx0;
    return err;
}

// The GUID, cookie and "00 01" as the payload of 07 15 SS 00
static void set_packet(uint8_t* pkt, unsigned slot, const cape_slotid* id){
    pkt_init(pkt, 0x07, 0x15, (uint8_t)slot, 0x00);
    if(id)
        cape_slotid_build(id, pkt + 4, CAPE_SLOTID_SIZE);
}

unsigned cape_file_sectors(size_t len){
    return len == 0 ? 1u : (unsigned)((len + CAPE_SECTOR_SIZE - 1) / CAPE_SECTOR_SIZE);
}

// Checks what the caller gives before anything is sent
static int check_arguments(const cape_slotid* id, const cape_slot_files* files){
    static const uint8_t zero[16] = { 0 };
    if(!id || !files || files->n == 0)
        return CAPE_E_ARG;
    if(!memcmp(id->guid, zero, sizeof(zero)) || id->flags[0] != 0x00 || id->flags[1] != 0x01)
        return CAPE_E_RANGE;
    const cape_slot_file* info = NULL;
    for(size_t i = 0; i < files->n; i++){
        const cape_slot_file* f = &files->file[i];
        if(!cape_name_valid(f->name) || (!f->data && f->len) || f->len > CAPE_MAX_FILE_SIZE)
            return CAPE_E_RANGE;
        for(size_t j = 0; j < i; j++){
            if(!strcmp(files->file[j].name, f->name))
                return CAPE_E_RANGE;
        }
        if(!strcmp(f->name, CAPE_FILE_INFO))
            info = f;
    }
    // PROFILE.I says the same GUID and cookie as the table entry that is set: the firmware and iCUE compare them
    cape_info parsed;
    if(!info || cape_info_parse(info->data, info->len, &parsed) != CAPE_OK || memcmp(parsed.guid, id->guid, sizeof(parsed.guid)) != 0
            || parsed.cookie != id->cookie)
        return CAPE_E_RANGE;
    return CAPE_OK;
}

int cape_write_slot(cape_wsession* w, unsigned slot, const cape_slotid* id, const cape_slot_files* files, cape_write_report* rep){
    cape_write_report local;
    if(!rep)
        rep = &local;
    memset(rep, 0, sizeof(*rep));
    rep->stage = CAPE_WSTAGE_PREPARE;
    rep->slot = CAPE_WSLOT_UNCHANGED;
    if(!w)
        return rep->err = CAPE_E_ARG;
    if(slot >= CAPE_SLOTS)
        return rep->err = CAPE_E_RANGE;
    if(!((w->mask >> slot) & 1))
        return rep->err = CAPE_E_MASKED;
    if(w->s.state == CAPE_S_REFUSED)
        return rep->err = CAPE_E_FW;
    if(w->s.state != CAPE_S_READY || !w->s.extra_allowed)
        return rep->err = CAPE_E_BROKEN;
    int rc = check_arguments(id, files);
    if(rc != CAPE_OK)
        return rep->err = rc;

    const unsigned tx0 = w->s.tx_packets;
    uint8_t pkt[CAPE_PKT_SIZE], reply[CAPE_PKT_SIZE];
    w->target = (int)slot;

    // Select the slot, look at its size (not used, as by iCUE) and at the free space of the flash: they must be answers to these
    pkt_init(pkt, 0x07, 0x17, 0x0c, (uint8_t)slot);
    rep->stage = CAPE_WSTAGE_CLEAR;
    if((rc = cape_tx(&w->s, pkt)) != CAPE_OK)
        return stop(w, rep, rc, tx0);
    w->s.slot = (int)slot;
    static const uint8_t asks[2][4] = { { 0x0e, 0x17, 0x03, 0x00 }, { 0x0e, 0x17, 0x0b, 0x00 } };
    for(int i = 0; i < 2; i++){
        pkt_init(pkt, asks[i][0], asks[i][1], asks[i][2], asks[i][3]);
        if((rc = cape_txrx(&w->s, pkt, reply)) != CAPE_OK)
            return stop(w, rep, rc, tx0);
        if(memcmp(reply, asks[i], 4) != 0)
            return stop(w, rep, CAPE_E_PROTO, tx0);
    }
    // The files must fit in the free sectors before the slot is cleared (the prudent way: its old files count as used)
    const uint32_t free_bytes = (uint32_t)reply[4] | (uint32_t)reply[5] << 8 | (uint32_t)reply[6] << 16 | (uint32_t)reply[7] << 24;
    rep->free_sectors = free_bytes / CAPE_SECTOR_SIZE;
    for(size_t i = 0; i < files->n; i++)
        rep->need_sectors += cape_file_sectors(files->file[i].len);
    if(rep->need_sectors > rep->free_sectors)
        return stop(w, rep, CAPE_E_SPACE, tx0);

    // Clear the GUID: from the moment this may have been sent the slot is not what it was
    set_packet(pkt, slot, NULL);
    rep->slot = CAPE_WSLOT_INCOMPLETE;
    if((rc = cape_tx(&w->s, pkt)) != CAPE_OK)
        return stop(w, rep, rc, tx0);
    if((rc = cape_wait_status(&w->s, 0, CAPE_WRITE_STATUS_POLLS, CAPE_WRITE_STATUS_POLL_MS)) != CAPE_OK)
        return stop(w, rep, rc, tx0);
    pkt_init(pkt, 0x07, 0x17, 0x0e, 0x00);
    if((rc = cape_tx(&w->s, pkt)) != CAPE_OK)
        return stop(w, rep, rc, tx0);

    // The GUID and cookie, and the slot selected again
    rep->stage = CAPE_WSTAGE_IDENTITY;
    set_packet(pkt, slot, id);
    if((rc = cape_tx(&w->s, pkt)) != CAPE_OK)
        return stop(w, rep, rc, tx0);
    if((rc = cape_wait_status(&w->s, 0, CAPE_WRITE_STATUS_POLLS, CAPE_WRITE_STATUS_POLL_MS)) != CAPE_OK)
        return stop(w, rep, rc, tx0);
    pkt_init(pkt, 0x07, 0x17, 0x0c, (uint8_t)slot);
    if((rc = cape_tx(&w->s, pkt)) != CAPE_OK)
        return stop(w, rep, rc, tx0);
    cape_session_progress_step(&w->s);   // the erase-and-identity preamble is done: cape_progress.h's WRITE lump

    // The files
    for(size_t i = 0; i < files->n; i++){
        const cape_slot_file* f = &files->file[i];
        rep->stage = CAPE_WSTAGE_FILE;
        snprintf(rep->file, sizeof(rep->file), "%s", f->name);
        pkt_init(pkt, 0x07, 0x17, 0x05, 0x00);
        memcpy(pkt + 4, f->name, strlen(f->name));
        w->s.file_open = 1;   // it counts as open before the command is sent: if sending fails we cannot know
        if((rc = cape_tx(&w->s, pkt)) != CAPE_OK)
            return stop(w, rep, rc, tx0);
        if((rc = cape_wait_status(&w->s, 0, CAPE_WRITE_STATUS_POLLS, CAPE_WRITE_STATUS_POLL_MS)) != CAPE_OK)
            return stop(w, rep, rc, tx0);
        size_t off = 0;
        do {
            // A burst: up to five chunks of up to 60 bytes, then its commit (a file of no bytes is the commit alone). As in iCUE's
            // saves, the first chunk goes after the pause any packet gets and the rest of the burst, the commit included, without one:
            // no chunk gets a reply, so the one thing they wait for is the transfer of the packet before (~1 ms). Per burst that is
            // (chunks - 1) chunks and the commit: a save has as many of them as it has chunks.
            unsigned chunks = 0;
            for(unsigned n = 1; n <= CAPE_BURST_SIZE / CAPE_CHUNK_SIZE && off < f->len; n++){
                size_t chunk = f->len - off;
                if(chunk > CAPE_CHUNK_SIZE)
                    chunk = CAPE_CHUNK_SIZE;
                pkt_init(pkt, 0x7f, (uint8_t)n, (uint8_t)chunk, 0x00);
                memcpy(pkt + 4, f->data + off, chunk);
                if((rc = (n == 1 ? cape_tx(&w->s, pkt) : cape_tx_cont(&w->s, pkt))) != CAPE_OK)
                    return stop(w, rep, rc, tx0);
                off += chunk;
                chunks++;
            }
            pkt_init(pkt, 0x07, 0x17, 0x09, 0x00);
            if((rc = (chunks ? cape_tx_cont(&w->s, pkt) : cape_tx(&w->s, pkt))) != CAPE_OK)
                return stop(w, rep, rc, tx0);
            if((rc = cape_wait_status(&w->s, 0, CAPE_WRITE_STATUS_POLLS, CAPE_WRITE_STATUS_POLL_MS)) != CAPE_OK)
                return stop(w, rep, rc, tx0);
        } while(off < f->len);
        pkt_init(pkt, 0x07, 0x17, 0x08, 0x00);
        w->s.file_open = 0;
        if((rc = cape_tx(&w->s, pkt)) != CAPE_OK)
            return stop(w, rep, rc, tx0);
        cape_session_progress_step(&w->s);
    }
    rep->file[0] = '\0';

    // The end of the transaction
    rep->stage = CAPE_WSTAGE_MARKER;
    pkt_init(pkt, 0x07, 0x17, 0x0e, 0x00);
    if((rc = cape_tx(&w->s, pkt)) != CAPE_OK)
        return stop(w, rep, rc, tx0);
    if((rc = cape_wait_status(&w->s, 0, CAPE_WRITE_STATUS_POLLS, CAPE_WRITE_STATUS_POLL_MS)) != CAPE_OK)
        return stop(w, rep, rc, tx0);

    w->target = -1;   // the write packets are refused again
    rep->stage = CAPE_WSTAGE_DONE;
    rep->slot = CAPE_WSLOT_WRITTEN;
    rep->status = w->s.last_status;
    rep->packets = w->s.tx_packets - tx0;
    return CAPE_OK;
}

int cape_write_verify(cape_wsession* w, unsigned slot, const cape_slotid* id, const cape_slot_files* files, cape_write_report* rep){
    cape_write_report local;
    if(!rep)
        rep = &local;
    memset(rep, 0, sizeof(*rep));
    rep->stage = CAPE_WSTAGE_VERIFY;
    rep->slot = CAPE_WSLOT_WRITTEN;
    if(!w || !id || !files)
        return rep->err = CAPE_E_ARG;
    if(slot >= CAPE_SLOTS)
        return rep->err = CAPE_E_RANGE;
    if(w->s.state != CAPE_S_READY)
        return rep->err = w->s.state == CAPE_S_REFUSED ? CAPE_E_FW : CAPE_E_BROKEN;
    const unsigned tx0 = w->s.tx_packets;

    // The slot table says what was set
    uint8_t pkt[CAPE_PKT_SIZE], reply[CAPE_PKT_SIZE];
    pkt_init(pkt, 0x0e, 0x17, 0x04, 0x00);
    int rc = cape_txrx(&w->s, pkt, reply);
    if(rc != CAPE_OK){
        (void)cape_session_fail(&w->s, rc);
        rep->packets = w->s.tx_packets - tx0;
        return rep->err = rc;
    }
    cape_slotid table[CAPE_SLOTS];
    if(reply[0] != 0x0e || reply[1] != 0x17 || reply[2] != 0x04 || cape_slot_table_parse(reply + 4, CAPE_SLOTS * CAPE_SLOTID_SIZE, table) != CAPE_OK){
        (void)cape_session_fail(&w->s, CAPE_E_PROTO);
        rep->packets = w->s.tx_packets - tx0;
        return rep->err = CAPE_E_PROTO;
    }
    w->s.slots[slot] = table[slot];
    uint8_t want[CAPE_SLOTID_SIZE], have[CAPE_SLOTID_SIZE];
    cape_slotid_build(id, want, sizeof(want));
    cape_slotid_build(&table[slot], have, sizeof(have));
    rep->packets = w->s.tx_packets - tx0;
    if(memcmp(want, have, sizeof(want)) != 0)
        return rep->err = CAPE_E_PROTO;   // the identity is not the one that was set (file is empty)

    // Every file, byte for byte
    uint8_t* buf = malloc(CAPE_MAX_FILE_SIZE);
    if(!buf)
        return rep->err = CAPE_E_CAP;
    for(size_t i = 0; i < files->n; i++){
        const cape_slot_file* f = &files->file[i];
        size_t len = 0;
        snprintf(rep->file, sizeof(rep->file), "%s", f->name);
        rc = cape_read_file(&w->s, slot, f->name, buf, CAPE_MAX_FILE_SIZE, &len);
        rep->packets = w->s.tx_packets - tx0;
        if(rc != CAPE_OK && rc != CAPE_E_NOTFOUND){
            rep->status = w->s.last_status;
            free(buf);
            return rep->err = rc;   // the session failed: it is over, and cape_read_file() ended it
        }
        if(rc == CAPE_E_NOTFOUND || len != f->len || (f->len && memcmp(buf, f->data, f->len) != 0)){
            rep->status = w->s.last_status;
            free(buf);
            return rep->err = CAPE_E_PROTO;
        }
    }
    free(buf);
    rep->file[0] = '\0';
    rep->status = w->s.last_status;
    return CAPE_OK;
}
