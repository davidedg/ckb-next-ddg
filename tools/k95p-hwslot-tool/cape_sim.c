#include "cape_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------
// Commands of the read sequence, recognised by their first four bytes. This is written on purpose in a
// different way from cape_pkt_allowed().

enum { C_UNKNOWN, C_IDENT, C_BUFSIZE, C_TABLE, C_STATUS, C_SIZE, C_SELECT, C_OPEN, C_FILL, C_CLOSE, C_CHUNK,
       // the save sequence
       C_TOTAL, C_WMODE, C_SETID, C_WOPEN, C_WCHUNK, C_WCOMMIT, C_MARKER };

static int decode(const uint8_t* p, int* has_reply){
    uint32_t head = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    *has_reply = 1;
    switch(head){
    case 0x0e010000u:
        return C_IDENT;
    case 0x0e170100u:
        return C_BUFSIZE;
    case 0x0e170400u:
        return C_TABLE;
    case 0x0e170d00u:
        return C_STATUS;
    case 0x0e170301u:
        return C_SIZE;
    case 0x0e170300u:
        return C_TOTAL;
    case 0x0e170b00u:
        return C_WMODE;
    default:
        break;
    }
    if((head >> 24) == 0xffu)
        return C_CHUNK;
    *has_reply = 0;
    if((head >> 24) == 0x7fu)
        return C_WCHUNK;
    switch(head){
    case 0x07170a00u:
        return C_FILL;
    case 0x07170800u:
        return C_CLOSE;
    case 0x07170700u:
        return C_OPEN;
    case 0x07170500u:
        return C_WOPEN;
    case 0x07170900u:
        return C_WCOMMIT;
    case 0x07170e00u:
        return C_MARKER;
    default:
        break;
    }
    if((head >> 8) == 0x07170cu)
        return C_SELECT;
    if((head >> 16) == 0x0715u && (head & 0xffu) == 0)
        return C_SETID;
    return C_UNKNOWN;
}

static int zeros_from(const uint8_t* p, size_t from){
    size_t nz = 0;
    for(size_t i = from; i < CAPE_PKT_SIZE; i++)
        nz += p[i] != 0;
    return nz == 0;
}

// ---------------------------------------------------------------------------------------------------------

static uint64_t rnd(cape_sim* k){
    uint64_t x = k->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    k->rng = x;
    return x * 0x2545f4914f6cdd1dull;
}

void sim_init(cape_sim* k, uint64_t seed){
    memset(k, 0, sizeof(*k));
    k->rng = seed ? seed : 0x9e3779b97f4a7c15ull;
    k->bufsize = CAPE_BURST_SIZE;
    k->selected = -1;
    k->open_file = -1;
    k->open_slot = -1;
    k->wopen = -1;
    k->wslot = -1;
    k->erase_slot = -1;
    k->flash_sectors = 2038;
    sim_set_ident(k, 0x1b1c, 0x1b2d, 0x0329, 0x0303);
}

unsigned sim_free_sectors(const cape_sim* k){
    if(k->free_forced)
        return k->free_sectors;
    unsigned used = 0;
    for(int s = 0; s < CAPE_SLOTS; s++)
        for(int i = 0; i < k->slot[s].nfiles; i++)
            used += k->slot[s].files[i].len ? (unsigned)((k->slot[s].files[i].len + 4095) / 4096) : 1u;
    return used < k->flash_sectors ? k->flash_sectors - used : 0;
}

void sim_free(cape_sim* k){
    for(int s = 0; s < CAPE_SLOTS; s++){
        for(int i = 0; i < k->slot[s].nfiles; i++)
            free(k->slot[s].files[i].data);
    }
    free(k->tx);
    memset(k, 0, sizeof(*k));
}

void sim_set_ident(cape_sim* k, uint16_t vid, uint16_t pid, uint16_t app_bcd, uint16_t bld_bcd){
    memset(k->ident, 0, sizeof(k->ident));
    k->ident[0] = 0x0e;
    k->ident[1] = 0x01;
    k->ident[23] = 0x01;   // the physical layout: ISO
    k->ident[8] = (uint8_t)(app_bcd & 0xff);
    k->ident[9] = (uint8_t)(app_bcd >> 8);
    k->ident[10] = (uint8_t)(bld_bcd & 0xff);
    k->ident[11] = (uint8_t)(bld_bcd >> 8);
    k->ident[12] = (uint8_t)(vid & 0xff);
    k->ident[13] = (uint8_t)(vid >> 8);
    k->ident[14] = (uint8_t)(pid & 0xff);
    k->ident[15] = (uint8_t)(pid >> 8);
}

void sim_set_slot(cape_sim* k, int slot, const uint8_t guid[16], uint32_t cookie){
    memcpy(k->slot[slot].id, guid, 16);
    k->slot[slot].id[16] = (uint8_t)(cookie & 0xff);
    k->slot[slot].id[17] = (uint8_t)((cookie >> 8) & 0xff);
    k->slot[slot].id[18] = (uint8_t)((cookie >> 16) & 0xff);
    k->slot[slot].id[19] = 0x01;
}

int sim_add_file(cape_sim* k, int slot, const char* name, const uint8_t* data, size_t len){
    sim_slot* sl = &k->slot[slot];
    sim_file* f = NULL;
    for(int i = 0; i < sl->nfiles; i++){
        if(!strcmp(sl->files[i].name, name))
            f = &sl->files[i];
    }
    if(f){
        free(f->data);
    } else {
        if(sl->nfiles >= SIM_MAX_FILES)
            return -1;
        f = &sl->files[sl->nfiles++];
        snprintf(f->name, sizeof(f->name), "%s", name);
    }
    f->data = malloc(len ? len : 1);
    if(!f->data){
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    if(len)
        memcpy(f->data, data, len);
    f->len = len;
    return 0;
}

void sim_leave_open(cape_sim* k, int slot, const char* name){
    for(int i = 0; i < k->slot[slot].nfiles; i++){
        if(!strcmp(k->slot[slot].files[i].name, name)){
            k->selected = slot;
            k->open_slot = slot;
            k->open_file = i;
            k->pos = 0;
            return;
        }
    }
    fprintf(stderr, "sim_leave_open: no file %s\n", name);
    exit(2);
}

unsigned sim_count(const cape_sim* k, int b0, int b1, int b2){
    unsigned n = 0;
    for(unsigned i = 0; i < k->ntx; i++){
        if(k->tx[i][0] != b0)
            continue;
        if(b1 >= 0 && (k->tx[i][1] != b1 || k->tx[i][2] != b2))
            continue;
        n++;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------

static unsigned record(cape_sim* k, const uint8_t* pkt){
    if(k->ntx == k->tx_cap){
        unsigned cap = k->tx_cap ? k->tx_cap * 2 : 256;
        uint8_t (*p)[CAPE_PKT_SIZE] = realloc(k->tx, (size_t)cap * CAPE_PKT_SIZE);
        if(!p){
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
        k->tx = p;
        k->tx_cap = cap;
    }
    memcpy(k->tx[k->ntx++], pkt, CAPE_PKT_SIZE);
    return k->ntx;
}

// A name in a packet: 1 to 11 characters of [A-Za-z0-9._-], then zeros
static int name_ok(const uint8_t* p){
    const char allowed[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";
    size_t n = 0;
    while(p[4 + n] && n < 12)
        n++;
    if(n < 1 || n > 11)
        return 0;
    for(size_t i = 0; i < n; i++){
        if(!memchr(allowed, p[4 + i], sizeof(allowed) - 1))
            return 0;
    }
    return zeros_from(p, 4 + n);
}

// Executes one command. reply is NULL for commands without a reply.
static void execute(cape_sim* k, int cmd, const uint8_t* p, uint8_t* reply){
    switch(cmd){
    case C_IDENT:
        memcpy(reply, k->ident, CAPE_PKT_SIZE);
        break;
    case C_BUFSIZE:
        memcpy(reply, "\x0e\x17\x01\x00", 4);
        reply[14] = (uint8_t)(k->bufsize & 0xff);
        reply[15] = (uint8_t)(k->bufsize >> 8);
        break;
    case C_TABLE:
        memcpy(reply, "\x0e\x17\x04\x00", 4);
        if(k->table_override){
            memcpy(reply + 4, k->table, sizeof(k->table));
        } else {
            for(int i = 0; i < CAPE_SLOTS; i++)
                memcpy(reply + 4 + i * CAPE_SLOTID_SIZE, k->slot[i].id, CAPE_SLOTID_SIZE);
        }
        break;
    case C_STATUS: {
        k->status_queries++;
        uint8_t st = k->status;
        if(k->busy_left){
            k->busy_left--;
            st = 0x02;   // busy (the value is a guess: the real keyboard answers late instead, and the code polls whatever it says)
        }
        if(k->force_count && k->status_queries >= k->force_from && k->status_queries - k->force_from < k->force_count)
            st = k->force_status;
        memcpy(reply, "\x0e\x17\x0d\x00", 4);
        reply[4] = st;
        break;
    }
    case C_SIZE: {
        size_t len = 0;
        if(k->open_file < 0)
            k->protocol_errors++;
        else
            len = k->slot[k->open_slot].files[k->open_file].len;
        memcpy(reply, "\x0e\x17\x03\x01", 4);
        reply[4] = (uint8_t)(len & 0xff);
        reply[5] = (uint8_t)((len >> 8) & 0xff);
        reply[6] = (uint8_t)((len >> 16) & 0xff);
        reply[7] = (uint8_t)((len >> 24) & 0xff);
        break;
    }
    case C_SELECT:
        if(p[3] >= CAPE_SLOTS){
            k->violations++;
            break;
        }
        if(k->open_file >= 0)
            k->protocol_errors++;  // the slot is changed under an open file
        if(k->wopen >= 0)
            k->write_errors++;
        k->selected = p[3];
        k->status = 0;
        break;
    case C_OPEN: {
        // A file that is already open is an error ("CAPE: ... already open" in iCUE); the status value is a guess
        if(k->selected < 0 || k->open_file >= 0 || k->wopen >= 0){
            k->status = 0x0d;
            break;
        }
        sim_slot* sl = &k->slot[k->selected];
        k->status = 4;  // not found
        for(int i = 0; i < sl->nfiles; i++){
            if(!strcmp(sl->files[i].name, (const char*)p + 4)){  // p + 4 is terminated: name_ok() checked
                k->open_slot = k->selected;
                k->open_file = i;
                k->pos = 0;
                k->rbuf_fresh = 0;
                k->status = 0;
                break;
            }
        }
        break;
    }
    case C_FILL: {
        if(k->open_file < 0){
            k->protocol_errors++;
            k->status = 0x0d;
            break;
        }
        const sim_file* f = &k->slot[k->open_slot].files[k->open_file];
        if(k->pos >= f->len){
            k->protocol_errors++;  // asking for more of a file that is finished
            k->status = 0x0d;
            break;
        }
        size_t n = f->len - k->pos;
        if(n > sizeof(k->rbuf))
            n = sizeof(k->rbuf);
        memcpy(k->rbuf, f->data + k->pos, n);
        k->rbuf_len = n;
        k->pos += n;
        k->rbuf_fresh = 1;
        k->next_chunk = 1;
        k->status = 0;
        break;
    }
    case C_CLOSE:
        if(k->wopen >= 0 && k->wchunks)
            k->write_errors++;   // chunks that were never committed
        k->open_file = -1;
        k->open_slot = -1;
        k->rbuf_fresh = 0;
        k->wopen = -1;
        k->wbuf_len = 0;
        k->wchunks = 0;
        break;
    case C_CHUNK: {
        unsigned n = p[1], ll = p[2];
        reply[0] = 0x0e;
        reply[1] = 0xff;
        reply[2] = (uint8_t)n;
        reply[3] = (uint8_t)ll;
        size_t want = 0;
        if(n >= 1 && n <= 5 && k->rbuf_len > (size_t)(n - 1) * 60){
            want = k->rbuf_len - (size_t)(n - 1) * 60;
            if(want > 60)
                want = 60;
        }
        // Without a fresh buffer, or out of order, or with a wrong length, the reply keeps its stale bytes
        if(!k->rbuf_fresh || n != k->next_chunk || ll != want){
            k->protocol_errors++;
            break;
        }
        memcpy(reply + 4, k->rbuf + (size_t)(n - 1) * 60, ll);
        k->next_chunk++;
        if((size_t)(k->next_chunk - 1) * 60 >= k->rbuf_len)
            k->rbuf_fresh = 0;
        break;
    }
    // ---- the size of the selected slot (a read, which a save asks too), then the save sequence (allow_write only)
    case C_TOTAL: {
        size_t total = 0;
        if(k->selected < 0){
            k->write_errors++;
        } else {
            for(int i = 0; i < k->slot[k->selected].nfiles; i++)
                total += k->slot[k->selected].files[i].len;
        }
        memcpy(reply, "\x0e\x17\x03\x00", 4);
        reply[4] = (uint8_t)(total & 0xff);
        reply[5] = (uint8_t)((total >> 8) & 0xff);
        reply[6] = (uint8_t)((total >> 16) & 0xff);
        reply[7] = (uint8_t)((total >> 24) & 0xff);
        break;
    }
    case C_WMODE: {
        const uint32_t bytes = sim_free_sectors(k) * 4096u;
        memcpy(reply, "\x0e\x17\x0b\x00", 4);
        reply[4] = (uint8_t)(bytes & 0xff);
        reply[5] = (uint8_t)((bytes >> 8) & 0xff);
        reply[6] = (uint8_t)((bytes >> 16) & 0xff);
        reply[7] = (uint8_t)((bytes >> 24) & 0xff);
        break;
    }
    case C_SETID: {
        const int slot = p[2];
        if(slot >= CAPE_SLOTS){
            k->violations++;
            break;
        }
        int clear = 1;
        for(int i = 4; i < 24; i++)
            clear &= p[i] == 0;
        if(clear){
            // The GUID is cleared now; the files go at the marker that follows (the slot is empty from then)
            memset(k->slot[slot].id, 0, CAPE_SLOTID_SIZE);
            k->erase_pending = 1;
            k->erase_slot = slot;
            k->markers = 0;
            k->clears++;
            k->busy_left = k->busy_after_clear;
        } else {
            if(k->erase_pending)
                k->write_errors++;     // a GUID set before the marker that empties the slot: iCUE never does
            if(p[23] != 0x01)
                k->write_errors++;     // every entry of the table ends with 01, after a cookie of 24 bits
            memcpy(k->slot[slot].id, p + 4, CAPE_SLOTID_SIZE);
            k->sets++;
        }
        k->status = 0;
        break;
    }
    case C_MARKER:
        k->markers++;
        if(k->erase_pending){
            sim_slot* sl = &k->slot[k->erase_slot];
            for(int i = 0; i < sl->nfiles; i++){
                free(sl->files[i].data);
                sl->files[i].data = NULL;
            }
            sl->nfiles = 0;
            k->erase_pending = 0;
            k->erases++;
        } else {
            if(k->wopen >= 0)
                k->write_errors++;     // a file is still open when the transaction ends
            if(!k->sets)
                k->write_errors++;
        }
        k->status = 0;
        break;
    case C_WOPEN: {
        // A file is written into a slot that has an identity, one at a time
        int has_id = 0;
        for(int i = 0; i < 16 && k->selected >= 0; i++)
            has_id |= k->slot[k->selected].id[i] != 0;
        if(k->selected < 0 || !has_id || k->wopen >= 0 || k->open_file >= 0 || k->erase_pending){
            k->write_errors++;
            k->status = 0x0d;
            break;
        }
        sim_slot* sl = &k->slot[k->selected];
        int at = -1;
        for(int i = 0; i < sl->nfiles; i++){
            if(!strcmp(sl->files[i].name, (const char*)p + 4))
                at = i;
        }
        if(at >= 0){
            free(sl->files[at].data);
            sl->files[at].data = malloc(1);
            sl->files[at].len = 0;
        } else {
            if(sl->nfiles >= SIM_MAX_FILES){
                k->status = 0x0d;   // the file system is full
                break;
            }
            at = sl->nfiles++;
            snprintf(sl->files[at].name, sizeof(sl->files[at].name), "%s", (const char*)p + 4);
            sl->files[at].data = malloc(1);
            sl->files[at].len = 0;
        }
        k->wopen = at;
        k->wslot = k->selected;
        k->wbuf_len = 0;
        k->wchunks = 0;
        k->wnext = 1;
        k->status = 0;
        break;
    }
    case C_WCHUNK: {
        const unsigned n = p[1], ll = p[2];
        if(k->wopen < 0 || n != k->wnext || n < 1 || n > 5 || ll < 1 || ll > 60 || k->wbuf_len + ll > sizeof(k->wbuf)){
            k->write_errors++;
            break;
        }
        memcpy(k->wbuf + k->wbuf_len, p + 4, ll);
        k->wbuf_len += ll;
        k->wchunks++;
        k->wnext++;
        break;
    }
    case C_WCOMMIT: {
        if(k->wopen < 0){
            k->write_errors++;
            k->status = 0x0d;
            break;
        }
        sim_file* f = &k->slot[k->wslot].files[k->wopen];
        if(k->corrupt_commit_at && k->commits + 1 == k->corrupt_commit_at && k->wbuf_len)
            k->wbuf[0] ^= 0x01;
        if(!k->wchunks && f->len)
            k->write_errors++;     // a commit of nothing is only what iCUE does for an empty file
        uint8_t* grown = realloc(f->data, f->len + k->wbuf_len + 1);
        if(!grown){
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
        memcpy(grown + f->len, k->wbuf, k->wbuf_len);
        f->data = grown;
        f->len += k->wbuf_len;
        k->wbuf_len = 0;
        k->wchunks = 0;
        k->wnext = 1;
        k->commits++;
        k->status = 0;
        break;
    }
    default:
        break;
    }
}

static int handle(cape_sim* k, const uint8_t* pkt, uint8_t* reply, int via_xfer, int cont){
    unsigned idx = record(k, pkt);
    int has_reply;
    int cmd = decode(pkt, &has_reply);
    // May this packet go without a pause? (Decided here, from the packet and the burst the keyboard has, not from the writer.)
    const int may_cont = (cmd == C_WCHUNK && pkt[1] >= 2) || (cmd == C_WCOMMIT && k->wchunks > 0);
    if(cont){
        k->cont_packets++;
        k->cont_errors += !may_cont;
    } else if(k->expect_cont && may_cont){
        k->cont_missed++;
    }
    uint8_t scratch[CAPE_PKT_SIZE];
    uint8_t* out = reply ? reply : scratch;
    // Reply buffer: random bytes, then the documented ones on top
    for(size_t i = 0; i < CAPE_PKT_SIZE; i += 8){
        uint64_t r = rnd(k);
        memcpy(out + i, &r, 8);
    }
    if(k->fail_at == idx)
        return -1;
    if(cmd == C_UNKNOWN){
        k->violations++;
        return 0;
    }
    if(cmd > C_TOTAL && !k->allow_write){
        k->violations++;   // a write command to a keyboard that was not told to take any (the size of a slot is a read)
        return 0;
    }
    int padded;
    switch(cmd){
    case C_OPEN:
    case C_WOPEN:
        padded = name_ok(pkt);
        break;
    case C_CHUNK:
        padded = zeros_from(pkt, 4) && pkt[3] == 0;
        break;
    case C_WCHUNK:
        // 7f NN LL 00 and LL bytes of data, then zeros
        padded = pkt[3] == 0 && pkt[2] >= 1 && pkt[2] <= 60 && zeros_from(pkt, 4 + (size_t)pkt[2]);
        break;
    case C_SETID:
        padded = pkt[2] < CAPE_SLOTS && zeros_from(pkt, 24);
        break;
    default:
        padded = zeros_from(pkt, 4);
        break;
    }
    if(!padded)
        k->violations++;
    if(has_reply != via_xfer)
        k->io_mismatch++;
    if((cmd == C_OPEN || cmd == C_WOPEN) && !padded)
        return 0;  // not a name: nothing to look up
    if((cmd == C_WCHUNK || cmd == C_SETID) && !padded)
        return 0;
    execute(k, cmd, pkt, has_reply ? out : NULL);
    if(via_xfer && k->corrupt_at == idx)
        out[k->corrupt_byte] ^= 0x5a;
    return 0;
}

static int sim_send(void* ctx, const uint8_t* pkt){
    return handle(ctx, pkt, NULL, 0, 0);
}

static int sim_send_cont(void* ctx, const uint8_t* pkt){
    return handle(ctx, pkt, NULL, 0, 1);
}

static int sim_xfer(void* ctx, const uint8_t* pkt, uint8_t* reply){
    return handle(ctx, pkt, reply, 1, 0);
}

static void sim_sleep(void* ctx, unsigned ms){
    ((cape_sim*)ctx)->sleeps++;
    ((cape_sim*)ctx)->sleep_ms_total += ms;
}

cape_io sim_io(cape_sim* k){
    cape_io io = { .ctx = k, .send = sim_send, .xfer = sim_xfer, .sleep_ms = sim_sleep, .send_cont = sim_send_cont };
    return io;
}
