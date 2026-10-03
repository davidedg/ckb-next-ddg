#include "profile_cape.h"
#include "cape_fw.h"
#include "cape_hwload.h"
#include "cape_hwsave.h"
#include "cape_hwbind.h"
#include "cape_stage.h"
#include "notify.h"
#include "nxp_proto.h"
#include "usb.h"

#include <assert.h>
#include <stddef.h>
#include <time.h>

// usb.c, set by --enable-experimental (main.c): the on-board profiles are reached only with it
extern int enable_experimental;

// The pause iCUE leaves before a packet of a file transfer that it sends after a reply (or a sleep of its own): 10.1 to 22 ms,
// usually 11 to 12. The packets of a burst of chunks are the exception, one USB transfer apart (~1 ms):
// see cape_usb_send_cont(). Used for the write itself and the status polls inside it (cape_usb_phase(), below).
#define CAPE_WRITE_PACING_NS  11000000L

// ---------------------------------------------------------------------------------------------------------
// The daemon's way to the keyboard, as a cape_io: 07 commands are sent, 0e and ff commands get a reply

static int cape_usb_send(void* ctx, const uint8_t* pkt){
    usbdevice* kb = ctx;
    uchar msg[MSG_SIZE];
    memcpy(msg, pkt, MSG_SIZE);
    return usbsend(kb, msg, MSG_SIZE, 1) == MSG_SIZE ? 0 : CAPE_E_IO;
}

static int cape_usb_xfer(void* ctx, const uint8_t* pkt, uint8_t* reply){
    usbdevice* kb = ctx;
    uchar msg[MSG_SIZE];
    memcpy(msg, pkt, MSG_SIZE);
    return usbrecv(kb, msg, MSG_SIZE, reply) ? 0 : CAPE_E_IO;
}

// One packet, once. usbsend() sends again a packet that times out, without end, which is right for lighting and wrong for a write to
// the flash of the keyboard: a packet the keyboard got and did not acknowledge would be sent twice. The steps are those of _usbsend()
// (usb.c), whose locking this has to keep to: the pause, the lock that keeps the macro thread's packets and the colours apart, the
// write. Anything but a whole packet written is a failure, a timeout included.
// (Only the sending half needs this: cape_usb_xfer() sends the commands that ask, "0e ...", and asking again changes nothing. The
// simulator of the tests counts a "07" or "7f" packet that goes that way.)
static int send_once(usbdevice* kb, const uint8_t* pkt, int pause){
    uchar msg[MSG_SIZE];
    memcpy(msg, pkt, MSG_SIZE);
    if(pause)
        kb->vtable.delay(kb, DELAY_SEND);
    queued_mutex_lock(mmutex(kb));
    int res = kb->vtable.write(kb, msg, MSG_SIZE, 0, __FILE_NOPATH__, __LINE__);
    queued_mutex_unlock(mmutex(kb));
    return res == MSG_SIZE ? 0 : CAPE_E_IO;
}

static int cape_usb_send_once(void* ctx, const uint8_t* pkt){
    return send_once(ctx, pkt, 1);
}

// The same, without the pause before it: for the packets of a burst of chunks after the first, and its commit (cape_io.send_cont).
// iCUE's are one USB transfer apart (~1 ms; a transfer takes about 0.4 to 1.3 ms), and a chunk
// gets no reply that there would be anything to wait for. The lock and the single try are the same.
static int cape_usb_send_cont(void* ctx, const uint8_t* pkt){
    return send_once(ctx, pkt, 0);
}

static void cape_usb_sleep(void* ctx, unsigned ms){
    (void)ctx;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// cape_io.phase, for cmd_hwsave_cape(): the write and status polls use iCUE's pace;
// reading the slot and reading it back use the fixed pace chosen on tested hardware.
static void cape_usb_phase(void* ctx, cape_phase p){
    usbdevice* kb = ctx;
    kb->usbdelay_ns = p == CAPE_PHASE_WRITE ? CAPE_WRITE_PACING_NS : CAPE_READ_PACING_NS;
}

static void cape_usb_log(void* ctx, cape_loglevel level, const char* msg){
    int idx = INDEX_OF((usbdevice*)ctx, keyboard);
    switch(level){
    case CAPE_LOG_INFO:
        ckb_info("ckb%d: on-board profiles: %s", idx, msg);
        break;
    case CAPE_LOG_WARN:
        ckb_warn("ckb%d: on-board profiles: %s", idx, msg);
        break;
    default:
        ckb_err("ckb%d: on-board profiles: %s", idx, msg);
        break;
    }
}

// A read of the on-board files made in hardware mode leaves the keys dark: on a slot with static layers its lighting stops at the
// read and plays again only after "07 04 01" (firmware 3.29; whether an effect stops too is not known). iCUE never reads in hardware
// mode: it switches to software mode first, and sends 07 04 01 when it closes. The daemon reads in hardware mode (at the attach, and
// a slot read or checked while the keyboard runs its slots), so after such a read it sends that mode switch itself, alone, as at its
// exit (NEEDS_MODE_ONLY_EXIT: no key input table): the keyboard stays in hardware mode and its active slot is lit again. Nothing in
// software mode, where the daemon's frames paint the keys, and nothing after a failure of the transport. Called after the session
// has ended (its packets are not this one's business) and before the line that answers the command.
static void hw_lighting_resume(usbdevice* kb, int rc){
    if(kb->active || rc == CAPE_E_IO)
        return;
    uchar msg[MSG_SIZE] = { CMD_SET, FIELD_SPECIAL, MODE_HARDWARE };
    if(!usbsend(kb, msg, MSG_SIZE, 1))
        ckb_warn("ckb%d: on-board profiles: the lighting of the active slot was not restarted", INDEX_OF(kb, keyboard));
    DELAY_30MS();
}

// ---------------------------------------------------------------------------------------------------------

// Why kb->cape_ok is false, for the lines that refuse (a K95 RGB Platinum: the others never get there)
static const char* not_ok_why(void){
    return enable_experimental ? "the firmware is not a tested one" : "on-board profile access needs --enable-experimental";
}

void cape_device_start(usbdevice* kb){
    kb->cape_ok = false;
    if(!IS_PLATINUM(kb))
        return;
    // Reading and writing the on-board profiles is experimental: without the option not one packet of it is sent
    if(!enable_experimental){
        ckb_info("ckb%d: K95 RGB Platinum: on-board profiles not accessed (they need --enable-experimental)", INDEX_OF(kb, keyboard));
        return;
    }
    // What getfwversion() read from "0e 01". The daemon keeps 32 bits, a level that does not fit the 16-bit
    // BCD of the table cannot be a tested one (0 is not in it)
    cape_fwinfo fw;
    fw.vid = kb->vendor;
    fw.pid = kb->product;
    fw.app_bcd = kb->fwversion <= 0xffff ? (uint16_t)kb->fwversion : 0;
    fw.bld_bcd = kb->bldversion <= 0xffff ? (uint16_t)kb->bldversion : 0;
    char have[64], tested[128];
    cape_fw_describe(&fw, have, sizeof(have));
    if(cape_fw_is_tested(&fw)){
        kb->cape_ok = true;
        ckb_info("ckb%d: K95 RGB Platinum %s: tested firmware, on-board profile access enabled (experimental)", INDEX_OF(kb, keyboard), have);
    } else {
        cape_fw_tested_list(tested, sizeof(tested));
        ckb_warn("ckb%d: K95 RGB Platinum %s: not a tested firmware level (tested: %s), the on-board profiles will not be touched",
                 INDEX_OF(kb, keyboard), have, tested);
    }
}

// ---------------------------------------------------------------------------------------------------------
// hwload and hwsave

static_assert(CAPE_HW_NAME_UNITS == MD_NAME_LEN, "the names of cape_hwload.h are not the size of the names of hwprofile");
static_assert(sizeof(ushort) == sizeof(uint16_t), "ushort is not a UTF-16 unit");
static_assert(sizeof(usbid) == CAPE_SLOTID_SIZE, "usbid is not the entry of the slot table");

// What the daemon keeps of the on-board profiles between commands: hwload's reading of the slots, brought up to date by the saves
// (so that kb->hw after a save is what hwload would make now, without reading again), and the copies of the slots whose save failed
struct cape_state {
    cape_hwsave_state hs;
    cape_hwload found;
    cape_slotid slots[CAPE_SLOTS];   // the slot table found was read with (an empty slot is all zeros): hw->id[0] is made of it
    int have;                        // found and slots hold a load
    // The slots as read whole (bindings included), what hwslot's record of a slot is made of; an image changes, and its
    // generation with it, whenever a load reads the keyboard or a save of that slot is written and read back. Owned: freed with the state
    cape_slot_image img[CAPE_SLOTS];
    unsigned gen[CAPE_SLOTS];
    // Where cape_usb_progress() (below) prints "hwsave progress" lines to: set by save_edit() right before the session that can call
    // it opens. Meaningless the rest of the time (nothing else ever gives a session a .progress). The slot is a number, not a mode:
    // a save made from hardware mode erases the profile of the modes first (hwslot)
    unsigned progress_slot;
    int progress_nnumber;
    const char* progress_type;       // the word of the line: "hwsave", "hwsavecheck" (a heartbeat of a check) or "hwread"
    // hwslot1: the preparations of the saves, and the records of the slots for get :hwbind, made again when the image changes
    cape_stage stage;
    cape_hwbind_record rec[CAPE_SLOTS];
    unsigned rec_gen[CAPE_SLOTS];
    int rec_ok[CAPE_SLOTS];
};

static struct cape_state* state_of(usbdevice* kb){
    if(!kb->cape){
        kb->cape = calloc(1, sizeof(*kb->cape));   // (all zeros is the state cape_hwsave_state_init() makes)
        if(kb->cape)
            cape_stage_init(&kb->cape->stage);
    }
    return kb->cape;
}

// cape_io.progress, for a save (save_edit()), the read of one slot (hwslot_read()) and a check of hwslot (check_edit() from
// hwslot_check()): struct cape_state's progress_slot/progress_nnumber/progress_type above say where to print the line and with which
// word; each of them sets them right before its session opens. hwload at attach and the check of hwsave have no .progress.
static void cape_usb_progress(void* ctx, unsigned done, unsigned total, const char* phase){
    usbdevice* kb = ctx;
    struct cape_state* cs = kb->cape;
    if(!cs)
        return;
    nprintf(kb, cs->progress_nnumber, NULL, "mode %u %s progress %u %u %s\n", cs->progress_slot + 1,
            cs->progress_type ? cs->progress_type : "hwsave", done, total, phase);
}

const cape_slot_image* cape_device_image(usbdevice* kb, unsigned slot, unsigned* gen){
    const struct cape_state* cs = kb ? kb->cape : NULL;
    if(!cs || !cs->have || slot >= CAPE_SLOTS)
        return NULL;
    if(gen)
        *gen = cs->gen[slot];
    return &cs->img[slot];
}

void cape_device_free(usbdevice* kb){
    if(!kb->cape)
        return;
    cape_hwsave_state_free(&kb->cape->hs);
    cape_hwload_images_free(kb->cape->img);
    cape_stage_free(&kb->cape->stage);
    for(unsigned i = 0; i < CAPE_SLOTS; i++)
        cape_hwbind_free(&kb->cape->rec[i]);
    free(kb->cape);
    kb->cape = NULL;
}

// What cape_hwload_read() found, in the daemon's hardware profile: mode i + 1 is slot i, as for the legacy keyboards
// (id[0] and name[0] are the profile). Nothing of this goes to the keyboard.
static void hw_from_cape(hwprofile* hw, const cape_hwload* r, const cape_slotid slots[CAPE_SLOTS], const char* serial){
    cape_hw_synth_id(serial, CAPE_HW_PROFILE_ID, slots, (uint8_t*)&hw->id[0]);
    const char* profile_name = CAPE_HW_PROFILE_NAME;
    for(size_t i = 0; profile_name[i] && i < MD_NAME_LEN; i++)
        hw->name[0][i] = (ushort)(unsigned char)profile_name[i];
    for(int i = 0; i < CAPE_SLOTS; i++){
        if(r->slot[i].present)
            memcpy(&hw->id[i + 1], r->slot[i].id, sizeof(usbid));
        else
            cape_hw_synth_id(serial, i, NULL, (uint8_t*)&hw->id[i + 1]);
        memcpy(hw->name[i + 1], r->slot[i].name, sizeof(hw->name[i + 1]));
        // The lighting, LED by LED (the index that printrgb() reads with keymap[].led), when it is an image of static layers
        const cape_light_slot* L = &r->slot[i].light;
        if(!r->slot[i].present || L->state != CAPE_LIGHT_STATIC)
            continue;
        lighting* out = hw->light + i;
        cape_light_leds(&L->image, L->ind_ok ? L->ind : NULL, N_KEYS_EXTENDED, out->r, out->g, out->b);
        hw->lightknown[i] = 1;
    }
}

int cmd_hwload_cape(usbdevice* kb, usbmode* dummy1, int dummy2, int apply, const char* dummy3){
    (void)dummy1;
    (void)dummy2;
    (void)apply;
    (void)dummy3;

    int idx = INDEX_OF(kb, keyboard);
    if(!kb->cape_ok){
        ckb_warn("ckb%d: on-board profiles: not read, %s", idx, not_ok_why());
        return 0;
    }
    // One exit for everything after this point, so that the delay is always put back
    long saved_delay = kb->usbdelay_ns;
    kb->usbdelay_ns = CAPE_READ_PACING_NS;
    cape_io io = { .ctx = kb, .send = cape_usb_send, .xfer = cape_usb_xfer, .sleep_ms = cape_usb_sleep };
    cape_session s;
    cape_hwload found;
    cape_slot_image images[CAPE_SLOTS];
    int rc = cape_session_open(&s, &io);
    if(rc == CAPE_OK)
        rc = cape_hwload_read_all(&s, &found, images, 0, cape_usb_log, kb);   // PROFILE.I only: the rest when asked (hwslot read:)
    else
        memset(images, 0, sizeof(images));
    cape_session_close(&s);
    kb->usbdelay_ns = saved_delay;
    hw_lighting_resume(kb, rc);

    if(rc == CAPE_OK){
        hwprofile* hw = calloc(1, sizeof(hwprofile));
        if(!hw){
            ckb_err("ckb%d: on-board profiles: out of memory", idx);
            cape_hwload_images_free(images);
            return 0;
        }
        hw_from_cape(hw, &found, s.slots, kb->serial);
        free(kb->hw);
        kb->hw = hw;
        struct cape_state* cs = state_of(kb);
        if(cs){
            cs->found = found;
            memcpy(cs->slots, s.slots, sizeof(cs->slots));
            cs->have = 1;
            cape_hwload_images_free(cs->img);
            for(unsigned i = 0; i < CAPE_SLOTS; i++){
                cs->img[i] = images[i];   // (moved)
                cs->gen[i]++;
            }
            memset(images, 0, sizeof(images));
            // The keyboard is read as it is: what a failed save left of a slot is not to be rebuilt from a copy of what it was
            for(unsigned i = 0; i < CAPE_SLOTS; i++)
                cape_hwsave_state_drop(&cs->hs, i);
        }
        cape_hwload_images_free(images);   // (nothing left if they were moved; all three if there was no state)
        return 0;
    }
    // A keyboard that does not answer is a fault of the connection, for which the caller may reset it. Any other
    // failure would come back after a reset, so it is reported and left at that.
    ckb_warn("ckb%d: on-board profiles: not loaded (%s)", idx, cape_errstr(rc));
    if(rc == CAPE_E_IO)
        return -1;
    if(!kb->hw)
        kb->features &= ~FEAT_HWLOAD;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// hwsave and its check

// The picture of a mode of the GUI as a save needs it, a copy, so that the seconds a save or a check takes are not spent holding
// imutex. slot is -1 for a mode that is not a slot of the keyboard.
typedef struct {
    int slot;
    uint8_t r[N_KEYS_EXTENDED], g[N_KEYS_EXTENDED], b[N_KEYS_EXTENDED];
    uint16_t name[MD_NAME_LEN];
    size_t name_units;
    uint8_t guid[16];    // the one a new profile in the slot would have (the same that hwload made for the empty slot)
} cape_pic;

#define CAPE_CHECK_LINE 384

// Copies what a save or its check needs from a mode (imutex held)
static void cape_pic_take(usbdevice* kb, usbmode* mode, cape_pic* pic){
    memset(pic, 0, sizeof(*pic));
    const size_t index = (size_t)(mode - kb->profile->mode);   // (a mode before the first is a very large index)
    pic->slot = index < CAPE_SLOTS ? (int)index : -1;
    if(pic->slot < 0)
        return;
    static_assert(sizeof(pic->r) == sizeof(mode->light.r), "the picture is the size of the lighting of a mode");
    memcpy(pic->r, mode->light.r, sizeof(pic->r));
    memcpy(pic->g, mode->light.g, sizeof(pic->g));
    memcpy(pic->b, mode->light.b, sizeof(pic->b));
    size_t n = 0;
    while(n < MD_NAME_LEN && mode->name[n])
        n++;
    memcpy(pic->name, mode->name, n * sizeof(pic->name[0]));
    pic->name_units = n;
    uint8_t id[CAPE_SLOTID_SIZE];
    cape_hw_synth_id(kb->serial, pic->slot, NULL, id);
    memcpy(pic->guid, id, sizeof(pic->guid));
}

static cape_slot_edit edit_of(const cape_pic* pic, int force){
    cape_slot_edit e;
    memset(&e, 0, sizeof(e));
    e.r = pic->r;
    e.g = pic->g;
    e.b = pic->b;
    e.leds = N_KEYS_EXTENDED;
    e.name = pic->name;   // (no units: no name, and the plan keeps the one the slot has)
    e.name_units = pic->name_units;
    e.guid = pic->guid;
    e.force = force;
    return e;
}

static const char* slot_state_name(cape_wslot_state st){
    switch(st){
    case CAPE_WSLOT_UNCHANGED:
        return "unchanged";
    case CAPE_WSLOT_INCOMPLETE:
        return "incomplete";
    case CAPE_WSLOT_WRITTEN:
        return "written";
    }
    return "unknown";
}

// The text after "hwsavecheck": the verdict and what it is made of. light_keep: the edit keeps the lighting as it is (hwslot), which the
// line says (lights=kept) instead of colours of a picture
static void check_line(const cape_hwsave_result* r, int light_keep, char* line, size_t cap){
    if(r->err != CAPE_OK){
        snprintf(line, cap, "error err=%s", cape_errstr(r->err));
        return;
    }
    const cape_slot_plan* p = &r->plan;
    size_t n = (size_t)snprintf(line, cap, "%s colours=%u layers=%u", cape_slot_verdict_name(p->verdict), p->colours, p->layers);
#define ADD(...) do { if(n < cap) n += (size_t)snprintf(line + n, cap - n, __VA_ARGS__); } while(0)
    if(p->replaces)
        ADD(" replaces=%s%s%s", p->replaces & CAPE_REPLACES_EFFECTS ? "effects" : "",
            (p->replaces & CAPE_REPLACES_EFFECTS) && (p->replaces & CAPE_REPLACES_UNKNOWN) ? "," : "",
            p->replaces & CAPE_REPLACES_UNKNOWN ? "unknown" : "");
    if(p->ignored)
        ADD(" ignored=%u", p->ignored);
    if(p->name_changed)
        ADD(" name=changed");
    if(p->buttons_changed)
        ADD(" buttons=changed");
    if(p->bindings_changed)
        ADD(" bindings=changed");
    if(p->winlock_changed)
        ADD(" winlock=changed");
    if(p->indicators_changed)
        ADD(" indicators=changed");
    if(light_keep)
        ADD(" lights=kept");
    if(r->from_snapshot)
        ADD(" kept=yes");
    if(p->verdict == CAPE_SLOT_REFUSED && p->reason[0])
        ADD(" reason=%s", p->reason);
#undef ADD
}

// The check of an edit of slot 0..2: reads the slot (call it without imutex) and writes nothing to it. beat_node >= 0: "hwsavecheck
// progress" lines go there while it reads (hwslot: a heartbeat for the GUI's guard, the slot can take long to read)
static void check_edit(usbdevice* kb, unsigned slot, const cape_slot_edit* edit, cape_hwsave_result* r, int beat_node){
    struct cape_state* cs = state_of(kb);
    if(!cs){
        memset(r, 0, sizeof(*r));
        r->err = CAPE_E_CAP;
        return;
    }
    // The reads of a check may be sent again (usbsend), and go at the pace of hwload
    long saved_delay = kb->usbdelay_ns;
    kb->usbdelay_ns = CAPE_READ_PACING_NS;
    cape_io io = { .ctx = kb, .send = cape_usb_send, .xfer = cape_usb_xfer, .sleep_ms = cape_usb_sleep };
    if(beat_node >= 0){
        cs->progress_slot = slot;
        cs->progress_nnumber = beat_node;
        cs->progress_type = "hwsavecheck";
        io.progress = cape_usb_progress;
    }
    (void)cape_hwsave_check(&cs->hs, &io, slot, edit, r);
    kb->usbdelay_ns = saved_delay;
    hw_lighting_resume(kb, r->err);
    if(r->err != CAPE_OK)
        ckb_warn("ckb%d: on-board profiles: the check of a save to slot %u failed (%s)", INDEX_OF(kb, keyboard), slot + 1, cape_errstr(r->err));
}

// The check of hwsave: the picture and the name of a mode; line is what follows "hwsavecheck "
static void cape_hwsavecheck(usbdevice* kb, const cape_pic* pic, char* line, size_t cap){
    if(!kb->cape_ok){
        snprintf(line, cap, "error err=%s", not_ok_why());
        return;
    }
    struct cape_state* cs = state_of(kb);
    if(pic->slot < 0 || !cs){
        snprintf(line, cap, "refused colours=0 layers=0 reason=%s", cs ? "not a hardware mode" : "out of memory");
        return;
    }
    const cape_slot_edit edit = edit_of(pic, 0);
    cape_hwsave_result r;
    check_edit(kb, (unsigned)pic->slot, &edit, &r, -1);
    check_line(&r, 0, line, cap);
}

void cmd_get_hwsavecheck_cape(usbdevice* kb, usbmode* mode, int nnumber){
    cape_pic pic;
    cape_pic_take(kb, mode, &pic);
    char line[CAPE_CHECK_LINE];
    queued_mutex_unlock(imutex(kb));
    cape_hwsavecheck(kb, &pic, line, sizeof(line));
    queued_mutex_lock(imutex(kb));
    nprintf(kb, nnumber, mode, "hwsavecheck %s\n", line);
}

// What the daemon knows of a slot after a save that was written and read back: kb->hw is made as hwload makes it, from what hwload
// read with the slot replaced (and hw->id[0], which is made of the whole slot table, follows the new cookie), and the copy of the slot
// read whole is the one made from the files written
static void hw_after_save(usbdevice* kb, struct cape_state* cs, unsigned slot, const cape_hwsave_result* r, cape_slot_image* image){
    if(!cs->have){
        cape_slot_image_free(image);
        return;   // nothing was read yet: the next hwload makes it all
    }
    cape_slot_image_free(&cs->img[slot]);
    cs->img[slot] = *image;   // (moved)
    memset(image, 0, sizeof(*image));
    cs->gen[slot]++;
    cs->found.slot[slot] = r->slot;
    cs->slots[slot] = r->plan.id;
    hwprofile* hw = calloc(1, sizeof(hwprofile));
    if(!hw){
        ckb_err("ckb%d: on-board profiles: out of memory, the daemon's copy of the slot is left as it was", INDEX_OF(kb, keyboard));
        return;
    }
    hw_from_cape(hw, &cs->found, cs->slots, kb->serial);
    free(kb->hw);
    kb->hw = hw;
}

// The one line that says how a save ended (see cmd_hwsave_cape in profile_cape.h), for slot 0..2 on node nnumber. hwmode_failed: the
// keyboard did not go back to hardware mode after a save made from there (hwslot), said before the prose of err= and reason=
static void report_save(usbdevice* kb, int nnumber, unsigned slot, const cape_hwsave_result* r, int kept, int hwmode_failed){
    const int idx = INDEX_OF(kb, keyboard);
    const char* hw = hwmode_failed ? " hwmode=failed" : "";
    if(r->err == CAPE_OK && !r->wrote){
        ckb_info("ckb%d: on-board profiles: slot %u was not written (%s)", idx, slot + 1, r->plan.reason);
        nprintf(kb, nnumber, NULL, "mode %u hwsave skipped refused%s reason=%s\n", slot + 1, hw, r->plan.reason);
    } else if(r->err == CAPE_OK){
        ckb_info("ckb%d: on-board profiles: slot %u written and read back (%u packets)", idx, slot + 1, r->packets);
        nprintf(kb, nnumber, NULL, "mode %u hwsave ok packets=%u%s\n", slot + 1, r->packets, hw);
    } else {
        // Where it stopped: the read of the slot, a step of the write or the reading back
        const char* stage = "read";
        const char* file = "";
        if(r->wrote){
            stage = cape_wstage_name(r->write.stage);
            file = r->write.file;
            if(r->write.err == CAPE_OK){
                stage = cape_wstage_name(CAPE_WSTAGE_VERIFY);
                file = r->verify.file;
            }
        }
        const char* state = slot_state_name(r->write.slot);   // (unchanged too if nothing was written: the report is empty)
        // Not enough free sectors: the writer stopped before writing anything, and says how many were needed and free
        char space[40] = "";
        if(r->err == CAPE_E_SPACE)
            snprintf(space, sizeof(space), " space=%u/%u", r->write.need_sectors, r->write.free_sectors);
        ckb_err("ckb%d: on-board profiles: the save to slot %u failed at %s%s%s (%s%s), the slot is %s", idx, slot + 1, stage,
                file[0] ? " " : "", file, cape_errstr(r->err), space, state);
        // (err is last: it is prose, with spaces in it)
        nprintf(kb, nnumber, NULL, "mode %u hwsave fail stage=%s file=%s slot=%s kept=%s%s%s err=%s\n", slot + 1, stage, file[0] ? file : "-",
                state, kept ? "yes" : "no", hw, space, cape_errstr(r->err));
    }
}

// The save proper, shared by hwsave and hwslot: slot 0..2, progress lines to node nnumber. Call it without imutex.
// It brings the daemon's copies up to date after a save written and read back; the caller reports it (report_save)
static void save_edit(usbdevice* kb, struct cape_state* cs, int nnumber, unsigned slot, const cape_slot_edit* edit, cape_hwsave_result* r){
    // For cape_usb_progress(), right before the session that calls it opens
    cs->progress_slot = slot;
    cs->progress_nnumber = nnumber;
    cs->progress_type = "hwsave";

    // From here to the end there is no way out that leaves the delay changed. The initial pause is for the read that opens the
    // session and the one that follows (reading the slot as it is): cape_hwsave_run() switches it with .phase from there.
    cape_slot_image image;
    long saved_delay = kb->usbdelay_ns;
    kb->usbdelay_ns = CAPE_READ_PACING_NS;
    cape_io io = { .ctx = kb, .send = cape_usb_send_once, .xfer = cape_usb_xfer, .sleep_ms = cape_usb_sleep, .send_cont = cape_usb_send_cont,
                   .phase = cape_usb_phase, .progress = cape_usb_progress };
    (void)cape_hwsave_run_ex(&cs->hs, &io, slot, edit, r, &image);
    kb->usbdelay_ns = saved_delay;

    // (cape_hwsave_run has ended its session: the last thing it did with the keyboard was reading a status, so the lighting frame
    // that command.c sends next cannot come in the middle of a file)
    if(r->slot_ok)
        hw_after_save(kb, cs, slot, r, &image);
    else
        cape_slot_image_free(&image);
}

int cmd_hwsave_cape(usbdevice* kb, usbmode* mode, int nnumber, int dummy3, const char* dummy4){
    (void)dummy3;
    (void)dummy4;

    const int idx = INDEX_OF(kb, keyboard);
    if(!kb->cape_ok){
        ckb_warn("ckb%d: on-board profiles: not saved, %s", idx, not_ok_why());
        nprintf(kb, nnumber, mode, "hwsave fail stage=prepare file=- slot=unchanged kept=no err=%s\n", not_ok_why());
        return 0;
    }
    cape_pic pic;
    queued_mutex_lock(imutex(kb));
    cape_pic_take(kb, mode, &pic);
    queued_mutex_unlock(imutex(kb));
    struct cape_state* cs = state_of(kb);
    if(pic.slot < 0 || !cs){
        ckb_warn("ckb%d: on-board profiles: not saved, %s", idx, cs ? "the mode is not one of the slots of the keyboard" : "out of memory");
        nprintf(kb, nnumber, mode, "hwsave skipped refused reason=%s\n", cs ? "not a hardware mode" : "out of memory");
        return 0;
    }
    const cape_slot_edit edit = edit_of(&pic, 1);
    cape_hwsave_result r;
    save_edit(kb, cs, nnumber, (unsigned)pic.slot, &edit, &r);
    report_save(kb, nnumber, (unsigned)pic.slot, &r, cs->hs.has_snap[pic.slot], 0);
    // A failure of the transport is reported, not returned: 0 for every outcome, so that nothing writes again (see profile_cape.h)
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// hwslot1 (cape_hwslot.h): the record of a slot, the preparation of a save, its check and its save

void cape_node_closed(usbdevice* kb, int node){
    if(kb && kb->cape)
        cape_stage_end_owner(&kb->cape->stage, node);
}

// What an edit of hwslot is made of, and the room for its picture
typedef struct {
    cape_slot_edit e;
    uint8_t r[N_KEYS_EXTENDED], g[N_KEYS_EXTENDED], b[N_KEYS_EXTENDED];
    uint8_t set[N_KEYS_EXTENDED];
    uint8_t guid[16];
    cape_slotid base;
} c_edit;

// The edit a transaction describes. The LEDs of rgb: are the names of the daemon's keymap; a name that is not one, or that has no light,
// or two names of the same LED with other colours, is an error of the preparation. Returns 0, or -1 with why.
static int txn_edit(usbdevice* kb, const cape_stage_txn* t, unsigned slot, int force, c_edit* ce, char* why, size_t cap){
    memset(ce, 0, sizeof(*ce));
    ce->e.r = ce->r;
    ce->e.g = ce->g;
    ce->e.b = ce->b;
    ce->e.leds = N_KEYS_EXTENDED;
    for(size_t i = 0; i < t->nrgb; i++){
        const cape_stage_rgb* x = &t->rgb[i];
        int led = -2;
        for(int k = 0; k < N_KEYS_EXTENDED && led == -2; k++)
            if(kb->keymap[k].name && !strcmp(kb->keymap[k].name, x->led))
                led = kb->keymap[k].led;
        if(led < 0 || led >= N_KEYS_EXTENDED){
            snprintf(why, cap, led == -2 ? "there is no LED called %s" : "%s has no light", x->led);
            return -1;
        }
        if(ce->set[led] && (ce->r[led] != x->rgb[0] || ce->g[led] != x->rgb[1] || ce->b[led] != x->rgb[2])){
            snprintf(why, cap, "the LED of %s has two colours", x->led);
            return -1;
        }
        ce->set[led] = 1;
        ce->r[led] = x->rgb[0];
        ce->g[led] = x->rgb[1];
        ce->b[led] = x->rgb[2];
    }
    if(t->has_name){
        ce->e.name = t->name;
        ce->e.name_units = t->name_units;
    }
    uint8_t id[CAPE_SLOTID_SIZE];
    cape_hw_synth_id(kb->serial, (int)slot, NULL, id);
    memcpy(ce->guid, id, sizeof(ce->guid));
    ce->e.guid = ce->guid;
    ce->e.force = force;
    ce->e.bindings = t->bind == CAPE_STAGE_BIND_MODEL ? &t->model : NULL;
    ce->e.recreate = t->recreate;
    ce->e.light_keep = t->light == CAPE_STAGE_LIGHT_KEEP;
    ce->e.has_winlock = t->has_wl;
    ce->e.winlock = t->wl;
    ce->e.has_ind = t->has_ind;
    memcpy(ce->e.ind, t->ind, sizeof(ce->e.ind));
    if(!t->base_empty && cape_slotid_parse(t->base, sizeof(t->base), &ce->base) != CAPE_OK){
        snprintf(why, cap, "the base is not an entry of the slot table");
        return -1;
    }
    ce->e.base = &ce->base;
    return 0;
}

static void hwslot_check(usbdevice* kb, struct cape_state* cs, int owner, unsigned slot, uint32_t txn){
    char why[CAPE_SLOT_WHY];
    cape_stage_txn* t = cape_stage_get(&cs->stage, owner, slot, txn, why, sizeof(why));
    c_edit ce;
    if(t && txn_edit(kb, t, slot, 0, &ce, why, sizeof(why)) != 0){
        cape_stage_end_slot(&cs->stage, slot);   // (a preparation that cannot be one: nothing of it is to be saved)
        t = NULL;
    }
    if(!t){
        nprintf(kb, owner, NULL, "mode %u hwsavecheck error err=staging %s\n", slot + 1, why);
        return;
    }
    cape_hwsave_result r;
    check_edit(kb, slot, &ce.e, &r, owner);
    char line[CAPE_CHECK_LINE];
    check_line(&r, ce.e.light_keep, line, sizeof(line));
    t->checked = r.err == CAPE_OK;
    t->refused = r.err == CAPE_OK && r.plan.verdict == CAPE_SLOT_REFUSED;
    snprintf(t->check_reason, sizeof(t->check_reason), "%s", r.plan.reason);
    nprintf(kb, owner, NULL, "mode %u hwsavecheck %s\n", slot + 1, line);
}

// A save made while the keyboard is in hardware mode is made in software mode (the daemon never writes in hardware mode): the profile
// of the modes is erased first (the software mode of the save has no bindings of a software profile), the keyboard switched, saved,
// and switched back whatever happened. Returns whether the profile was erased (the caller's pointers into it are gone).
static int hwslot_save(usbdevice* kb, struct cape_state* cs, int owner, unsigned slot, uint32_t txn){
    const int idx = INDEX_OF(kb, keyboard);
    char why[CAPE_SLOT_WHY];
    cape_stage_txn* t = cape_stage_get(&cs->stage, owner, slot, txn, why, sizeof(why));
    if(t && !t->checked){
        snprintf(why, sizeof(why), "the transaction was not checked");
        t = NULL;
    }
    c_edit ce;
    if(t && txn_edit(kb, t, slot, 1, &ce, why, sizeof(why)) != 0)
        t = NULL;
    if(!t){
        nprintf(kb, owner, NULL, "mode %u hwsave fail stage=prepare file=- slot=unchanged kept=no err=staging %s\n", slot + 1, why);
        return 0;
    }
    // What needs no reading is decided before the keyboard is switched
    if(t->refused){
        nprintf(kb, owner, NULL, "mode %u hwsave skipped refused reason=%s\n", slot + 1, t->check_reason);
        return 0;
    }
    const devcmd* vt = &kb->vtable;
    const int wrapped = !kb->active;
    if(wrapped){
        vt->eraseprofile(kb, kb->profile->currentmode, owner, 0, 0);
        if(vt->active(kb, kb->profile->currentmode, owner, 0, 0)){
            const int back = vt->idle(kb, kb->profile->currentmode, owner, 0, 0);
            ckb_err("ckb%d: on-board profiles: not saved, the keyboard did not switch to software mode", idx);
            nprintf(kb, owner, NULL, "mode %u hwsave fail stage=prepare file=- slot=unchanged kept=no%s err=the keyboard did not switch to "
                    "software mode\n", slot + 1, back ? " hwmode=failed" : "");
            return 1;
        }
    }
    cape_hwsave_result r;
    save_edit(kb, cs, owner, slot, &ce.e, &r);
    int hwmode_failed = 0;
    if(wrapped)
        hwmode_failed = vt->idle(kb, kb->profile->currentmode, owner, 0, 0) != 0;
    else
        (void)vt->updatergb(kb, 1);
    report_save(kb, owner, slot, &r, cs->hs.has_snap[slot], hwmode_failed);
    if(r.err == CAPE_OK && r.wrote)
        cape_stage_end_slot(&cs->stage, slot);   // done; a failure keeps it for the Retry
    return wrapped;
}

// hwslot read:<m> (cape_hwslot.h, 3): the whole of slot 0..2 into the daemon's copy, if it has only its PROFILE.I. Nothing is written,
// the mode of the keyboard is not changed (a read works in hardware mode too, and then the lighting is restarted after it:
// hw_lighting_resume()), and a failure leaves the copy as it was
static void hwslot_read(usbdevice* kb, struct cape_state* cs, int owner, unsigned slot){
    const int idx = INDEX_OF(kb, keyboard);
    if(!cs->have){
        nprintf(kb, owner, NULL, "mode %u hwread fail err=the slots were not read when the keyboard was attached\n", slot + 1);
        return;
    }
    if(cs->img[slot].whole){
        nprintf(kb, owner, NULL, "mode %u hwread ok gen=%u packets=0\n", slot + 1, cs->gen[slot]);
        return;
    }
    cs->progress_slot = slot;
    cs->progress_nnumber = owner;
    cs->progress_type = "hwread";
    long saved_delay = kb->usbdelay_ns;
    kb->usbdelay_ns = CAPE_READ_PACING_NS;
    cape_io io = { .ctx = kb, .send = cape_usb_send, .xfer = cape_usb_xfer, .sleep_ms = cape_usb_sleep, .progress = cape_usb_progress };
    cape_session s;
    cape_hwload found = cs->found;
    cape_slot_image img;
    memset(&img, 0, sizeof(img));
    int rc = cape_session_open(&s, &io);
    if(rc == CAPE_OK){
        cape_session_progress_begin_alone(&s, CAPE_PHASE_READ, CAPE_PROGRESS_READ_FILES);
        rc = cape_hwload_read_one(&s, slot, 1, &found, &img, cape_usb_log, kb);
    }
    const unsigned packets = s.tx_packets;
    cape_session_close(&s);
    kb->usbdelay_ns = saved_delay;
    hw_lighting_resume(kb, rc);
    hwprofile* hw = rc == CAPE_OK ? calloc(1, sizeof(hwprofile)) : NULL;
    if(rc == CAPE_OK && !hw)
        rc = CAPE_E_CAP;
    if(rc != CAPE_OK){
        cape_slot_image_free(&img);
        ckb_warn("ckb%d: on-board profiles: slot %u not read (%s)", idx, slot + 1, cape_errstr(rc));
        nprintf(kb, owner, NULL, "mode %u hwread fail err=%s\n", slot + 1, cape_errstr(rc));
        return;
    }
    // The slot as it is now: its entry of the table too (the other slots keep the one of the attach)
    cape_slot_image_free(&cs->img[slot]);
    cs->img[slot] = img;   // (moved)
    cs->gen[slot]++;
    cs->found.slot[slot] = found.slot[slot];
    cs->slots[slot] = s.slots[slot];
    hw_from_cape(hw, &cs->found, cs->slots, kb->serial);
    free(kb->hw);
    kb->hw = hw;
    ckb_info("ckb%d: on-board profiles: slot %u read, %u packets", idx, slot + 1, packets);
    nprintf(kb, owner, NULL, "mode %u hwread ok gen=%u packets=%u\n", slot + 1, cs->gen[slot], packets);
}

// "read:<m>", m = 1..3 in decimal: the slot index, or -1
static int read_word(const char* word){
    if(strncmp(word, "read:", 5) || word[5] < '1' || word[5] > '0' + CAPE_SLOTS || word[6])
        return -1;
    return word[5] - '1';
}

int cmd_hwslot_cape(usbdevice* kb, int nnumber, const char* word){
    struct cape_state* cs = state_of(kb);
    if(!cs || !word)
        return 0;
    if(!strncmp(word, "read:", 5)){
        const int slot = read_word(word);
        if(slot < 0)
            ckb_warn("ckb%d: on-board profiles: a word of hwslot that is not one, ignored", INDEX_OF(kb, keyboard));
        else if(!kb->cape_ok)
            nprintf(kb, nnumber, NULL, "mode %d hwread fail err=%s\n", slot + 1, not_ok_why());
        else
            hwslot_read(kb, cs, nnumber, (unsigned)slot);
        return 0;
    }
    unsigned slot = 0;
    uint32_t txn = 0;
    const cape_stage_request req = cape_stage_word(&cs->stage, nnumber, word, strlen(word), &slot, &txn);
    if(req == CAPE_STAGE_MALFORMED){
        ckb_warn("ckb%d: on-board profiles: a word of hwslot that is not one, or from node %d, ignored", INDEX_OF(kb, keyboard), nnumber);
        return 0;
    }
    if(!kb->cape_ok){
        if(req == CAPE_STAGE_CHECK)
            nprintf(kb, nnumber, NULL, "mode %u hwsavecheck error err=%s\n", slot + 1, not_ok_why());
        else if(req == CAPE_STAGE_SAVE)
            nprintf(kb, nnumber, NULL, "mode %u hwsave fail stage=prepare file=- slot=unchanged kept=no err=%s\n", slot + 1, not_ok_why());
        // Nothing is kept of a preparation that can never be checked or saved
        cape_stage_end_owner(&cs->stage, nnumber);
        return 0;
    }
    if(req == CAPE_STAGE_CHECK)
        hwslot_check(kb, cs, nnumber, slot, txn);
    else if(req == CAPE_STAGE_SAVE)
        return hwslot_save(kb, cs, nnumber, slot, txn);
    return 0;
}

// The record of slot 0..2 for get :hwbind, made again when the daemon's copy of the slot has changed
static const cape_hwbind_record* record_of(usbdevice* kb, struct cape_state* cs, unsigned slot, unsigned* gen){
    const cape_slot_image* img = cape_device_image(kb, slot, gen);
    if(!img || !img->whole)
        return NULL;   // (not read whole yet: "nocache", and the GUI asks for hwslot read:)
    if(cs->rec_ok[slot] && cs->rec_gen[slot] == *gen)
        return &cs->rec[slot];
    cape_hwbind_free(&cs->rec[slot]);
    cs->rec_ok[slot] = 0;
    uint16_t name[CAPE_INFO_NAME_UNITS];
    int units = -1;
    if(img->present && img->info_ok)
        units = cape_info_get_name(&img->info, name, CAPE_INFO_NAME_UNITS);
    uint8_t entry[CAPE_SLOTID_SIZE], ind[CAPE_IND_COUNT * 3];
    cape_slotid_build(&img->id, entry, sizeof(entry));
    memcpy(ind, img->light.ind, sizeof(ind));
    cape_hwbind_light light = CAPE_HWBIND_LIGHT_UNKNOWN;
    if(img->light.state == CAPE_LIGHT_STATIC)
        light = CAPE_HWBIND_LIGHT_STATIC;
    else if(img->light.state == CAPE_LIGHT_EMPTY)
        light = CAPE_HWBIND_LIGHT_EMPTY;
    else if(img->light.state == CAPE_LIGHT_EFFECTS)
        light = CAPE_HWBIND_LIGHT_EFFECTS;
    const cape_hwbind_slot s = { img->present ? entry : NULL, &img->bind, units >= 0 ? name : NULL, units >= 0 ? (size_t)units : 0,
                                 img->present && img->light.ind_ok ? ind : NULL, light, img->light.layers,
                                 img->present && img->refused ? img->reason : NULL };
    if(cape_hwbind_make(&s, slot + 1, *gen, &cs->rec[slot]) != CAPE_OK){
        cape_hwbind_free(&cs->rec[slot]);
        return NULL;
    }
    cs->rec_gen[slot] = *gen;
    cs->rec_ok[slot] = 1;
    return &cs->rec[slot];
}

// A decimal number without leading zeros, from *p to the next ':' or the end
static int parse_dec(const char** p, unsigned* out){
    const char* s = *p;
    if(*s < '0' || *s > '9' || (s[0] == '0' && s[1] >= '0' && s[1] <= '9'))
        return 0;
    unsigned long v = 0;
    while(*s >= '0' && *s <= '9'){
        v = v * 10 + (unsigned)(*s - '0');
        if(v > 0xffffffUL)
            return 0;
        s++;
    }
    *out = (unsigned)v;
    *p = s;
    return 1;
}

void cmd_get_hwbind_cape(usbdevice* kb, int nnumber, const char* setting){
    // :hwbind:<m>:<p>
    const char* p = setting + strlen(":hwbind:");
    unsigned m = 0, page = 0;
    if(!parse_dec(&p, &m) || *p++ != ':' || !parse_dec(&p, &page) || *p || m < 1 || m > CAPE_SLOTS)
        return;
    struct cape_state* cs = kb->cape;
    unsigned gen = 0;
    const cape_hwbind_record* r = cs ? record_of(kb, cs, m - 1, &gen) : NULL;
    char line[CAPE_HWSLOT_LINE_MAX + 1];
    if(!r){
        if(cape_hwbind_error_line(m, gen, "nocache", line, sizeof(line)) > 0)
            nprintf(kb, nnumber, NULL, "%s", line);
        return;
    }
    if(cape_hwbind_line(r, page, line, sizeof(line)) < 0){
        if(cape_hwbind_error_line(m, gen, "range", line, sizeof(line)) > 0)
            nprintf(kb, nnumber, NULL, "%s", line);
        return;
    }
    nprintf(kb, nnumber, NULL, "%s", line);
}
