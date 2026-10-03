#include "cape.h"

#include <string.h>

// Replies are always CAPE_PKT_SIZE bytes; the bytes read from them are the documented ones (corsair-protocol
// devices/k95p.md and the pages of its commands: the header, the status in byte 4, the size in bytes 4-7, the chunk data from
// byte 4...).

const char* cape_errstr(int err){
    switch(err){
    case CAPE_E_IO:
        return "the keyboard did not accept a packet or did not answer";
    case CAPE_E_FW:
        return "not a tested keyboard or firmware level";
    case CAPE_E_PROTO:
        return "unexpected reply from the keyboard";
    case CAPE_E_STATUS:
        return "the keyboard reported an error status";
    case CAPE_E_NOTFOUND:
        return "file not found";
    case CAPE_E_TIMEOUT:
        return "the keyboard status did not settle";
    case CAPE_E_FORBIDDEN:
        return "packet is not a read-only command";
    case CAPE_E_BROKEN:
        return "session is not usable";
    case CAPE_E_OUT:
        return "could not write the dump";
    case CAPE_E_MASKED:
        return "the slot is not one this session may write";
    case CAPE_E_SPACE:
        return "not enough free space in the keyboard's flash";
    default:
        return cape_strerror(err);
    }
}

// ---------------------------------------------------------------------------------------------------------
// The list of packets that may be sent

static int tail_zero(const uint8_t* p, size_t from){
    for(size_t i = from; i < CAPE_PKT_SIZE; i++){
        if(p[i])
            return 0;
    }
    return 1;
}

static int name_char_ok(uint8_t c, int first){
    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;
    return !first && (c == '.' || c == '_' || c == '-');
}

int cape_name_valid(const char* name){
    if(!name)
        return 0;
    size_t n = 0;
    while(name[n]){
        if(n >= CAPE_NAME_MAX || !name_char_ok((uint8_t)name[n], n == 0))
            return 0;
        n++;
    }
    return n >= 1;
}

// The name of an "open" packet starts at byte 4 and is followed by zeros up to the end of the packet
static int packet_name_ok(const uint8_t* p){
    size_t n = 0;
    while(n <= CAPE_NAME_MAX && p[4 + n]){
        if(!name_char_ok(p[4 + n], n == 0))
            return 0;
        n++;
    }
    return n >= 1 && n <= CAPE_NAME_MAX && tail_zero(p, 4 + n);
}

int cape_pkt_allowed(const uint8_t* p){
    if(!p)
        return 0;
    switch(p[0]){
    case 0x0e:
        if(p[1] == 0x01)
            return tail_zero(p, 2);
        if(p[1] != 0x17)
            return 0;
        switch(p[2]){
        case 0x01: case 0x04: case 0x0d:
            return p[3] == 0x00 && tail_zero(p, 4);
        case 0x03:
            return p[3] == 0x01 && tail_zero(p, 4);
        default:
            return 0;
        }
    case 0x07:
        if(p[1] != 0x17)
            return 0;
        switch(p[2]){
        case 0x08: case 0x0a:
            return p[3] == 0x00 && tail_zero(p, 4);
        case 0x0c:
            return p[3] < CAPE_SLOTS && tail_zero(p, 4);
        case 0x07:
            return p[3] == 0x00 && packet_name_ok(p);
        default:
            return 0;
        }
    case 0xff:
        return p[1] >= 1 && p[1] <= CAPE_BURST_SIZE / CAPE_CHUNK_SIZE && p[2] >= 1 && p[2] <= CAPE_CHUNK_SIZE
                && p[3] == 0x00 && tail_zero(p, 4);
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Packet exit points

// On the read-only list, or on the list of a session that was opened to write (which is empty for one that was not)
static int packet_ok(const cape_session* s, const uint8_t* pkt){
    return cape_pkt_allowed(pkt) || (s->extra_allowed && s->extra_allowed(s, pkt));
}

// One body for both ways out of a packet that gets no reply: they differ only in the function of the io that carries it
static int tx_one(cape_session* s, const uint8_t* pkt, int cont){
    if(!s || !pkt)
        return CAPE_E_ARG;
    if(s->state == CAPE_S_REFUSED)
        return CAPE_E_FW;
    if(s->state != CAPE_S_OPENING && s->state != CAPE_S_READY)
        return CAPE_E_BROKEN;
    if(!packet_ok(s, pkt)){
        s->state = CAPE_S_BROKEN;
        return CAPE_E_FORBIDDEN;
    }
    s->tx_packets++;
    int (*out)(void*, const uint8_t*) = cont && s->io.send_cont ? s->io.send_cont : s->io.send;
    return out(s->io.ctx, pkt) ? CAPE_E_IO : CAPE_OK;
}

int cape_tx(cape_session* s, const uint8_t* pkt){
    return tx_one(s, pkt, 0);
}

int cape_tx_cont(cape_session* s, const uint8_t* pkt){
    return tx_one(s, pkt, 1);
}

void cape_session_phase(cape_session* s, cape_phase p){
    if(s && s->io.phase)
        s->io.phase(s->io.ctx, p);
}

// Common tail of the three progress functions below: report what s->progress now says, if anyone is listening.
static void report_progress(cape_session* s){
    if(s->io.progress)
        s->io.progress(s->io.ctx, cape_progress_value(&s->progress), CAPE_PROGRESS_TOTAL,
                        cape_progress_phase_name(s->progress.phase));
}

void cape_session_progress_begin(cape_session* s, cape_phase p, unsigned expected){
    if(!s)
        return;
    cape_progress_begin(&s->progress, p, expected);
    report_progress(s);
}

void cape_session_progress_begin_alone(cape_session* s, cape_phase p, unsigned expected){
    if(!s)
        return;
    cape_progress_begin_alone(&s->progress, p, expected);
    report_progress(s);
}

void cape_session_progress_step(cape_session* s){
    if(!s)
        return;
    cape_progress_step(&s->progress);
    report_progress(s);
}

void cape_session_progress_expect(cape_session* s, unsigned expected){
    if(s)
        cape_progress_expect(&s->progress, expected);   // (reported with the next step: no line of its own)
}

void cape_session_progress_beat(cape_session* s){
    if(s)
        report_progress(s);
}

int cape_txrx(cape_session* s, const uint8_t* pkt, uint8_t* reply){
    if(!s || !pkt || !reply)
        return CAPE_E_ARG;
    if(s->state == CAPE_S_REFUSED)
        return CAPE_E_FW;
    if(s->state != CAPE_S_OPENING && s->state != CAPE_S_READY)
        return CAPE_E_BROKEN;
    if(!packet_ok(s, pkt)){
        s->state = CAPE_S_BROKEN;
        return CAPE_E_FORBIDDEN;
    }
    s->tx_packets++;
    if(s->io.xfer(s->io.ctx, pkt, reply))
        return CAPE_E_IO;
    s->rx_packets++;
    return CAPE_OK;
}

// What a genuine reply to pkt must start with: an "0e ..." query gets its own first 4 bytes echoed back (the firmware answers
// "0e 01 00 00" with "0e 01 00 00 ...", "0e 17 0d 00" with "0e 17 0d 00 ...", and so on); the one command that does not start
// with "0e" is a chunk query ("ff N L 00"), whose reply starts with "0e" followed by its own first three bytes ("0e ff N L").
static void reply_header_of(const uint8_t* pkt, uint8_t header[4]){
    if(pkt[0] == 0xff){
        header[0] = 0x0e;
        header[1] = pkt[0];
        header[2] = pkt[1];
        header[3] = pkt[2];
    } else {
        memcpy(header, pkt, 4);
    }
}

// cape_txrx, but CAPE_E_PROTO if the reply does not start with what pkt should get back.
//
// Why this exists (the firmware answers from one reply buffer, so bytes beyond what a command writes are
// whatever an earlier reply left there): a query that goes unanswered for a while may be answered twice (once late, once
// promptly after the transport resends it, see profile_cape.c's cape_usb_xfer / usb.c's _usbrecv), and the extra reply is
// then sitting there for whichever exchange reads next -- not necessarily the one that caused it. This was seen on the keyboard,
// not just reasoned about: an "0e 05" and an "0e 00" reply that still carry the firmware version bytes of an "0e 01" answered
// earlier in the session.
//
// Why there is no retry here: the protocol carries no per-request identifier, so a stray reply of the very same command type
// as the one just asked (two "0e 01" in a row, for instance) looks exactly like a fresh, correct answer and cannot be told
// apart from one -- no amount of header checking closes that gap, and asking again would not either, since the resend is
// itself indistinguishable from the original to the keyboard. Catching what IS detectable (a reply of a different type) and
// ending the session cleanly is what this can honestly do; papering over a mismatch with a blind retry would risk quietly
// continuing on another exchange's stale answer instead of the one just asked for, which is worse than failing loudly.
static int txrx_matching(cape_session* s, const uint8_t* pkt, uint8_t* reply){
    int rc = cape_txrx(s, pkt, reply);
    if(rc)
        return rc;
    uint8_t want[4];
    reply_header_of(pkt, want);
    return memcmp(reply, want, sizeof want) ? CAPE_E_PROTO : CAPE_OK;
}

// ---------------------------------------------------------------------------------------------------------
// Commands

static void pkt_init(uint8_t* pkt, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3){
    memset(pkt, 0, CAPE_PKT_SIZE);
    pkt[0] = b0;
    pkt[1] = b1;
    pkt[2] = b2;
    pkt[3] = b3;
}

// "07 17 08": close the open file. Sending it when nothing is open is harmless.
static int send_close(cape_session* s){
    uint8_t pkt[CAPE_PKT_SIZE];
    pkt_init(pkt, 0x07, 0x17, 0x08, 0x00);
    s->file_open = 0;
    return cape_tx(s, pkt);
}

// A failure ends the session. A file that may be open gets one close command (its outcome does not matter).
static int fail(cape_session* s, int err){
    if(s->file_open)
        (void)send_close(s);
    s->file_open = 0;
    s->state = CAPE_S_BROKEN;
    return err;
}

int cape_session_fail(cape_session* s, int err){
    return s ? fail(s, err) : err;
}

// Reads the status of the last operation ("0e 17 0d", byte 4). 0 is CAPE_OK. 4 is CAPE_E_NOTFOUND if
// allow_notfound, otherwise an error like 0x0d ("invalid"). Any other value is not documented: it is read
// again a limited number of times in case it means "busy".
int cape_wait_status(cape_session* s, int allow_notfound, unsigned polls, unsigned interval_ms){
    uint8_t pkt[CAPE_PKT_SIZE], reply[CAPE_PKT_SIZE];
    pkt_init(pkt, 0x0e, 0x17, 0x0d, 0x00);
    for(unsigned poll = 0; ; poll++){
        int rc = txrx_matching(s, pkt, reply);
        if(rc)
            return rc;
        s->last_status = reply[4];
        if(reply[4] == 0x00)
            return CAPE_OK;
        if(reply[4] == 0x04 && allow_notfound)
            return CAPE_E_NOTFOUND;
        if(reply[4] == 0x04 || reply[4] == 0x0d)
            return CAPE_E_STATUS;
        if(poll >= 1)
            cape_session_progress_beat(s);   // the status has not settled by the second query or later: not silence
        if(poll + 1 >= polls)
            return CAPE_E_TIMEOUT;
        if(s->io.sleep_ms)
            s->io.sleep_ms(s->io.ctx, interval_ms);
    }
}

static int wait_status(cape_session* s, int allow_notfound){
    return cape_wait_status(s, allow_notfound, CAPE_STATUS_POLLS, CAPE_STATUS_POLL_MS);
}

// The slot table is well formed if every entry is either all zeros (empty slot) or has a GUID and the
// trailing "00 01" that every entry seen so far has.
static int slot_table_ok(const cape_slotid* slots){
    for(size_t i = 0; i < CAPE_SLOTS; i++){
        if(cape_slotid_is_empty(&slots[i])){
            if(slots[i].cookie || slots[i].flags[0] || slots[i].flags[1])
                return 0;
        } else if(slots[i].flags[0] != 0x00 || slots[i].flags[1] != 0x01){
            return 0;
        }
    }
    return 1;
}

int cape_session_open(cape_session* s, const cape_io* io){
    if(!s || !io || !io->send || !io->xfer)
        return CAPE_E_ARG;
    memset(s, 0, sizeof(*s));
    s->io = *io;
    s->slot = -1;
    s->state = CAPE_S_OPENING;

    uint8_t pkt[CAPE_PKT_SIZE], reply[CAPE_PKT_SIZE];
    int rc;

    // Who is it? A reply that is not an identification at all (txrx_matching) ends the session the same way any other
    // unexpected reply does: CAPE_E_PROTO, not CAPE_E_FW -- it says nothing about whether this keyboard is a tested one,
    // only that this particular answer was not its identification. CAPE_E_FW / CAPE_S_REFUSED is reserved for a reply that
    // genuinely is an identification (cape_fwinfo_from_ident succeeded) and says a keyboard or firmware level we have not
    // verified: that is the only case where "the keyboard says X" (cape_fw_describe(&s->fw, ...) at the call sites) is
    // something the keyboard actually said.
    pkt_init(pkt, 0x0e, 0x01, 0x00, 0x00);
    rc = txrx_matching(s, pkt, reply);
    if(rc)
        return fail(s, rc);
    // txrx_matching has already confirmed the header ("0e 01 00 00"), and reply is CAPE_PKT_SIZE (64) bytes, well above
    // CAPE_FW_IDENT_MIN_LEN: cape_fwinfo_from_ident cannot actually fail here. Kept because it is the function that reads
    // the identity, not because this line is reachable with a mismatched or short reply -- both are excluded above.
    if(cape_fwinfo_from_ident(reply, sizeof(reply), &s->fw) != CAPE_OK)
        return fail(s, CAPE_E_PROTO);
    if(!cape_fw_is_tested(&s->fw)){
        s->state = CAPE_S_REFUSED;
        return CAPE_E_FW;
    }

    // Does it behave like the keyboard we verified? The transfer buffer must be the 300 bytes we read in
    // bursts of, and the slot table must look like one.
    pkt_init(pkt, 0x0e, 0x17, 0x01, 0x00);
    rc = txrx_matching(s, pkt, reply);
    if(rc)
        return fail(s, rc);
    if((reply[14] | (reply[15] << 8)) != CAPE_BURST_SIZE)
        return fail(s, CAPE_E_PROTO);

    pkt_init(pkt, 0x0e, 0x17, 0x04, 0x00);
    rc = txrx_matching(s, pkt, reply);
    if(rc)
        return fail(s, rc);
    if(cape_slot_table_parse(reply + 4, CAPE_SLOTS * CAPE_SLOTID_SIZE, s->slots) != CAPE_OK
            || !slot_table_ok(s->slots))
        return fail(s, CAPE_E_PROTO);

    // A previous session that died in the middle of a file may have left it open, which would make the next
    // open fail. Closing when nothing is open is harmless, so do it, and do not look at the status.
    rc = send_close(s);
    if(rc)
        return fail(s, rc);

    s->state = CAPE_S_READY;
    return CAPE_OK;
}

static uint32_t rd32(const uint8_t* p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int cape_read_file(cape_session* s, unsigned slot, const char* name, uint8_t* buf, size_t cap, size_t* len){
    if(!s || !name || !len || (!buf && cap))
        return CAPE_E_ARG;
    *len = 0;
    if(s->state == CAPE_S_REFUSED)
        return CAPE_E_FW;
    if(s->state != CAPE_S_READY)
        return CAPE_E_BROKEN;
    if(slot >= CAPE_SLOTS || !cape_name_valid(name))
        return CAPE_E_RANGE;

    uint8_t pkt[CAPE_PKT_SIZE], reply[CAPE_PKT_SIZE];
    int rc;

    // The slot selection only concerns file operations, it does not change the active profile
    if(s->slot != (int)slot){
        pkt_init(pkt, 0x07, 0x17, 0x0c, (uint8_t)slot);
        rc = cape_tx(s, pkt);
        if(rc)
            return fail(s, rc);
        s->slot = (int)slot;
    }

    // Open. The file counts as open before the command is sent: if sending fails we cannot know.
    pkt_init(pkt, 0x07, 0x17, 0x07, 0x00);
    memcpy(pkt + 4, name, strlen(name));
    s->file_open = 1;
    rc = cape_tx(s, pkt);
    if(rc)
        return fail(s, rc);
    rc = wait_status(s, 1);
    if(rc == CAPE_E_NOTFOUND){
        rc = send_close(s);
        if(rc)
            return fail(s, rc);
        cape_session_progress_step(s);   // this file's read attempt is done, found or not
        return CAPE_E_NOTFOUND;
    }
    if(rc)
        return fail(s, rc);

    // Size of the open file (not "0e 17 03 00", which is the total of the slot)
    pkt_init(pkt, 0x0e, 0x17, 0x03, 0x01);
    rc = txrx_matching(s, pkt, reply);
    if(rc)
        return fail(s, rc);
    uint32_t size = rd32(reply + 4);
    if(size > CAPE_MAX_FILE_SIZE)
        return fail(s, CAPE_E_PROTO);
    if(size > cap){
        rc = send_close(s);
        if(rc)
            return fail(s, rc);
        *len = size;
        return CAPE_E_CAP;
    }

    // The data comes in bursts: "07 17 0a" fills the read buffer, the status must be 0, then up to five chunks
    size_t off = 0;
    while(off < size){
        pkt_init(pkt, 0x07, 0x17, 0x0a, 0x00);
        rc = cape_tx(s, pkt);
        if(rc)
            return fail(s, rc);
        rc = wait_status(s, 0);
        if(rc)
            return fail(s, rc);
        for(unsigned n = 1; n <= CAPE_BURST_SIZE / CAPE_CHUNK_SIZE && off < size; n++){
            size_t chunk = size - off;
            if(chunk > CAPE_CHUNK_SIZE)
                chunk = CAPE_CHUNK_SIZE;
            pkt_init(pkt, 0xff, (uint8_t)n, (uint8_t)chunk, 0x00);
            rc = txrx_matching(s, pkt, reply);
            if(rc)
                return fail(s, rc);
            memcpy(buf + off, reply + 4, chunk);
            off += chunk;
        }
    }

    rc = send_close(s);
    if(rc)
        return fail(s, rc);
    *len = size;
    cape_session_progress_step(s);
    return CAPE_OK;
}

void cape_session_close(cape_session* s){
    if(!s)
        return;
    if(s->state == CAPE_S_READY && s->file_open)
        (void)send_close(s);
    s->file_open = 0;
    s->state = CAPE_S_CLOSED;
}
