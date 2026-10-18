// k95p-hwslot-tool - the hardware slots of the K95 RGB Platinum (1b1c:1b2d) with their PROFILE.ZIP, read, built, checked and
// written with the daemon's own code (cape_qz, cape_xml, cape_profile, cape_compile, the transport of cape.h and cape_write.h),
// standalone: no ckb-next daemon may run while it talks to the keyboard. Not built with ckb-next (build.sh).
//
// An image is a directory of files named slotN_<file>_<read|write>_NNN.bin, with an optional slot-table.txt (one line per slot:
// N, the 16 bytes of the GUID as hex, the cookie, the flag) when the table cannot be read from PROFILE.I (a write that stopped
// before PROFILE.I).
//
//   k95p-hwslot-tool [DEVICE] dump [--slot N] [--probe-lighting] DIR
// --probe-lighting reads all five supported lighting indices, including files beyond lghtcnt.cnt; logs absent files too.
//   k95p-hwslot-tool zip IMAGE [--slot N] [--xml F | --lighting-xml F] -o DIR
//   k95p-hwslot-tool zip IMAGE --slot N --xml F --new GUID NAME -o DIR
// --new: a new profile in slot N, empty in the image, as the daemon makes one (PROFILE.I from GUID, the profile id of the XML's
// local copy in braces or bare, and NAME, UTF-8; cookie 0, convert gives the cookie): the restore of an iCUE local copy made into
// a document by icue-local-to-xml.py.
//   k95p-hwslot-tool check IMAGE [--slot N] [--zip F]
//   k95p-hwslot-tool xml ZIP
//   k95p-hwslot-tool diff A B [--slot N] [--uuid-map]
//   k95p-hwslot-tool [DEVICE] convert --slot N IMAGE [--base dump|empty|retry|current|from DUMP] [--expect GUID:COOKIE]
//                             [--cookie-at-least N] [--dump DIR] [--go]
// DEVICE: --hidraw (the default: the keyboard, as root, with no ckb-next daemon running), or --sim IMAGE [--sim-out DIR]
// [--sim-fail-at N] (a simulated keyboard, cape_sim.c, loaded from an image and written back to one).
// Every command takes --compat LEVEL (only icue-5.51.40 exists) and --layout N (1 ANSI, 2 ISO, 3 ABNT, 4 JIS, 5 KS: the physical
// layout of the keyboard, which the lighting files depend on). An image says its layout in layout.txt ("N NAME", written by dump
// from the keyboard's identification); for an image without it the layout is --layout, else ISO, said on
// stderr. --layout and layout.txt must agree. convert --go compares the image's layout with the keyboard's before
// anything that is not a read.
//
// What is sent to the keyboard: dump only the packets of a read (cape.h) and a 07 04 01 at the end; convert without --go the same
// reads (the slot table); convert --go first those reads, which check the base before any other packet, then the write of one
// slot (cape_write.h) wrapped as the daemon wraps a save (software mode with its input table before, 07 04 01 after). On the
// simulator the packets it does not take (the change of mode) are written to the --transcript in their place, so that the
// transcript of a run there is the whole sequence of the same run on the keyboard: the prediction of a run.

#define _GNU_SOURCE

#include "cape.h"
#include "cape_compile.h"
#include "cape_hwslot.h"
#include "cape_qz.h"
#include "cape_sim.h"
#include "cape_write.h"
#include "cape_xml.h"
#include "cape_zipslot.h"
#include "hidraw_io.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>

#define WHY 256

static void die(const char* fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "k95p-hwslot-tool: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(2);
}

static void* must(void* p){
    if(!p)
        die("out of memory");
    return p;
}

// Images

typedef struct {
    cape_slot_files files[CAPE_SLOTS];
    int present[CAPE_SLOTS];
    cape_slotid table[CAPE_SLOTS];
    int table_known[CAPE_SLOTS];       // from slot-table.txt
    int layout;                        // from layout.txt, 0 if it has none
} image;

static int g_layout;                   // --layout, 0 if not given

static const char* layout_name(int layout){
    static const char* const names[] = { "?", "ANSI", "ISO", "ABNT", "JIS", "KS" };
    return layout >= 1 && layout <= 5 ? names[layout] : names[0];
}

static void image_free(image* im){
    for(int s = 0; s < CAPE_SLOTS; s++)
        cape_slot_files_free(&im->files[s]);
    memset(im, 0, sizeof(*im));
}

static uint8_t* read_whole(const char* path, size_t* len){
    FILE* f = fopen(path, "rb");
    if(!f)
        return NULL;
    size_t cap = 4096, n = 0;
    uint8_t* b = must(malloc(cap));
    for(;;){
        if(n == cap)
            b = must(realloc(b, cap *= 2));
        const size_t r = fread(b + n, 1, cap - n, f);
        if(!r)
            break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b;
}

static int hexval(char c){
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int guid_parse(const char* s, uint8_t g[16]){
    for(int i = 0; i < 16; i++){
        const int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
        if(b < 0)
            return -1;
        g[i] = (uint8_t)(a << 4 | b);
    }
    return 0;
}

static void guid_text(const uint8_t g[16], char out[33]){
    for(int i = 0; i < 16; i++)
        sprintf(out + 2 * i, "%02x", g[i]);
}

static void image_load(const char* dir, image* im){
    memset(im, 0, sizeof(*im));
    DIR* d = opendir(dir);
    if(!d)
        die("%s: %s", dir, strerror(errno));
    struct dirent* e;
    while((e = readdir(d))){
        unsigned slot, number;
        char name[64], op[8];
        const size_t nl = strlen(e->d_name);
        if(nl < 5 || strcmp(e->d_name + nl - 4, ".bin") || sscanf(e->d_name, "slot%u_%63[^\n]", &slot, name) != 2 || slot >= CAPE_SLOTS)
            continue;
        // <file>_<op>_<NNN>.bin: the file name ends before the last "_read_" or "_write_"
        char* tail = NULL;
        for(char* p = name; (p = strstr(p, "_")); p++)
            if(sscanf(p, "_%7[a-z]_%u.bin", op, &number) == 2 && (!strcmp(op, "read") || !strcmp(op, "write")))
                tail = p;
        if(!tail)
            continue;
        *tail = 0;
        if(cape_slot_files_find(&im->files[slot], name))
            die("%s: %s of slot %u twice", dir, name, slot);
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        size_t len;
        uint8_t* data = read_whole(path, &len);
        if(!data)
            die("%s: %s", path, strerror(errno));
        if(cape_slot_files_add(&im->files[slot], name, data, len) != CAPE_OK)
            die("%s: cannot hold %s", dir, name);
        free(data);
        im->present[slot] = 1;
    }
    closedir(d);
    char path[4096];
    snprintf(path, sizeof(path), "%s/slot-table.txt", dir);
    FILE* f = fopen(path, "r");
    char line[256];
    while(f && fgets(line, sizeof(line), f)){
        unsigned slot, cookie, flag;
        char guid[40];
        if(line[0] == '#' || sscanf(line, "%u %39s %u %u", &slot, guid, &cookie, &flag) != 4 || slot >= CAPE_SLOTS)
            continue;
        if(guid_parse(guid, im->table[slot].guid))
            die("%s: a GUID that is not 32 hex digits", path);
        im->table[slot].cookie = cookie;
        im->table[slot].flag = (uint8_t)flag;
        im->table_known[slot] = 1;
    }
    if(f)
        fclose(f);
    snprintf(path, sizeof(path), "%s/layout.txt", dir);
    f = fopen(path, "r");
    if(f && (!fgets(line, sizeof(line), f) || sscanf(line, "%d", &im->layout) != 1 || im->layout < 1 || im->layout > 5))
        die("%s: not a layout 1 to 5", path);
    if(f)
        fclose(f);
}

// The layout of an image: its layout.txt, or --layout, or ISO (said once)
static int image_layout(const image* im){
    if(im->layout && g_layout && im->layout != g_layout)
        die("the image is of layout %d (%s), --layout says %d", im->layout, layout_name(im->layout), g_layout);
    if(im->layout)
        return im->layout;
    if(g_layout)
        return g_layout;
    static int said;
    if(!said++)
        fprintf(stderr, "k95p-hwslot-tool: layout ISO assumed (the image has no layout.txt; --layout N to give another)\n");
    return CAPE_PROFILE_LAYOUT_ISO;
}

static void layout_write(const char* dir, int layout){
    char path[4096];
    snprintf(path, sizeof(path), "%s/layout.txt", dir);
    FILE* f = fopen(path, "w");
    if(!f || fprintf(f, "%d %s\n", layout, layout_name(layout)) < 0)
        die("%s: %s", path, strerror(errno));
    fclose(f);
}

// The entry of the slot table of a slot of an image: slot-table.txt, else from its PROFILE.I
static int image_id(const image* im, int slot, cape_slotid* id){
    memset(id, 0, sizeof(*id));
    if(im->table_known[slot]){
        *id = im->table[slot];
        return 0;
    }
    const cape_slot_file* info = cape_slot_files_find(&im->files[slot], CAPE_FILE_INFO);
    cape_info i;
    if(!info || cape_info_parse(info->data, info->len, &i) != CAPE_OK)
        return -1;
    memcpy(id->guid, i.guid, 16);
    id->cookie = i.cookie & 0xffffff;
    id->flag = CAPE_SLOTID_FLAG;
    return 0;
}

static void write_file(const char* dir, const char* name, const uint8_t* data, size_t len){
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    if(!f || fwrite(data, 1, len, f) != len)
        die("%s: %s", path, strerror(errno));
    fclose(f);
}

static void image_write_slot(const char* dir, int slot, const cape_slot_files* files, const char* op){
    if(mkdir(dir, 0755) && errno != EEXIST)
        die("%s: %s", dir, strerror(errno));
    for(size_t i = 0; i < files->n; i++){
        char name[64];
        snprintf(name, sizeof(name), "slot%d_%s_%s_000.bin", slot, files->file[i].name, op);
        write_file(dir, name, files->file[i].data, files->file[i].len);
    }
}

static void table_write(const char* dir, const cape_slotid table[CAPE_SLOTS]){
    char path[4096];
    snprintf(path, sizeof(path), "%s/slot-table.txt", dir);
    FILE* f = fopen(path, "w");
    if(!f)
        die("%s: %s", path, strerror(errno));
    fprintf(f, "# slot, GUID (16 bytes as stored), cookie, flag\n");
    for(int s = 0; s < CAPE_SLOTS; s++){
        char g[33];
        guid_text(table[s].guid, g);
        fprintf(f, "%d %s %u %u\n", s, g, (unsigned)table[s].cookie, table[s].flag);
    }
    fclose(f);
}

// Models

// The XML of a PROFILE.ZIP, or of a file that is an XML already (it starts with '<')
static char* xml_of(const uint8_t* data, size_t len, size_t* xlen, char* why){
    if(len && data[0] == '<'){
        char* x = must(malloc(len));
        memcpy(x, data, len);
        *xlen = len;
        return x;
    }
    const int n = cape_qz_decode(data, len, NULL, 0);
    if(n <= 0){
        snprintf(why, WHY, "not a PROFILE.ZIP: %s", cape_qz_strerror(n));
        return NULL;
    }
    char* x = must(malloc((size_t)n));
    if(cape_qz_decode(data, len, (uint8_t*)x, (size_t)n) != n){
        free(x);
        snprintf(why, WHY, "not a PROFILE.ZIP");
        return NULL;
    }
    *xlen = (size_t)n;
    return x;
}

// The model of a PROFILE.ZIP, or of an XML document (it starts with '<'), read as the daemon reads it (cape_zipslot.h)
static int data_model(const uint8_t* data, size_t len, cape_profile_profile* p, char* why){
    if(len && data[0] == '<')
        return cape_zipslot_model_xml((const char*)data, len, p, why);
    return cape_zipslot_model(data, len, p, why);
}

static void file_model(const char* path, cape_profile_profile* p){
    size_t len;
    uint8_t* data = read_whole(path, &len);
    if(!data)
        die("%s: %s", path, strerror(errno));
    char why[WHY];
    if(data_model(data, len, p, why) != CAPE_OK)
        die("%s: %s", path, why);
    free(data);
}

// A model written and compressed: PROFILE.ZIP (cape_zipslot.h, as the daemon writes it)
static uint8_t* model_zip(const cape_profile_profile* p, size_t* len){
    uint8_t* zip = NULL;
    char why[WHY] = "";
    const int rc = cape_zipslot_zip(p, &zip, len, why);
    if(rc != CAPE_OK)
        die("the model cannot be written as a PROFILE.ZIP (%s): %s", cape_compile_strerror(rc), why);
    return zip;
}

// The files a model compiles to: the bindings, PROFILE.I (info with the indicator colours of the model) and the lighting (with
// effects, the files of the other layers only: lt->effects says so); cape_zipslot.h, as the daemon makes them
static int model_files(const cape_profile_profile* p, int layout, const cape_slot_file* info, cape_slot_files* out,
                       cape_compile_lighting* lt, char* why){
    const int rc = cape_zipslot_files(p, layout, info ? info->data : NULL, info ? info->len : 0, out, lt, why);
    if(rc == CAPE_E_CAP)
        die("out of memory");
    return rc;
}

static void random_bytes(void* ctx, uint8_t out[16]){
    (void)ctx;
    if(getrandom(out, 16, 0) != 16)
        die("no random bytes: %s", strerror(errno));
}

// check

typedef struct {
    int ok, diff;
} check_result;

// The model of the slot's ZIP (or zip_path) against its files: compiled, and the files imported with the model as the base
static check_result check_slot(const image* im, int slot, const char* zip_path, int quiet){
    check_result r = { 0, 0 };
    const cape_slot_files* files = &im->files[slot];
    cape_profile_profile p;
    char why[WHY] = "";
    if(zip_path)
        file_model(zip_path, &p);
    else {
        const cape_slot_file* zip = cape_slot_files_find(files, CAPE_FILE_ZIP);
        if(!zip || data_model(zip->data, zip->len, &p, why) != CAPE_OK){
            printf("slot %d: %s\n", slot, zip ? why : "no PROFILE.ZIP");
            r.diff++;
            return r;
        }
    }
    cape_slot_files made;
    static cape_compile_lighting lt;
    const int layout = image_layout(im);
    const int rc = model_files(&p, layout, cape_slot_files_find(files, CAPE_FILE_INFO), &made, &lt, why);
    if(rc != CAPE_OK){
        printf("slot %d: the ZIP does not compile (%s): %s\n", slot, cape_compile_strerror(rc), why);
        r.diff++;
    }
    for(size_t i = 0; rc == CAPE_OK && i < made.n; i++){
        const cape_slot_file* x = cape_slot_files_find(files, made.file[i].name);
        if(!x){
            printf("MISSING   slot %d %s: made from the ZIP, not in the image\n", slot, made.file[i].name);
            r.diff++;
            continue;
        }
        size_t k = 0;
        while(k < x->len && k < made.file[i].len && x->data[k] == made.file[i].data[k])
            k++;
        if(k == x->len && k == made.file[i].len){
            if(!quiet)
                printf("OK        slot %d %s\n", slot, x->name);
            r.ok++;
        } else {
            printf("DIFF      slot %d %s: %zu bytes made, %zu in the image, first difference at %zu\n", slot, x->name, made.file[i].len,
                   x->len, k);
            r.diff++;
        }
    }
    // the files the image has and the ZIP does not make
    for(size_t i = 0; rc == CAPE_OK && i < files->n; i++){
        const char* name = files->file[i].name;
        if(!strcmp(name, CAPE_FILE_ZIP) || cape_slot_files_find(&made, name))
            continue;
        printf("EXTRA     slot %d %s: in the image, not made from the ZIP\n", slot, name);
        r.diff++;
    }
    cape_slot_files_free(&made);
    // the files imported back with the model as the base: the model again
    cape_compile_import_opts o;
    memset(&o, 0, sizeof(o));
    o.layout = layout;
    o.random = random_bytes;
    cape_profile_profile q;
    const int irc = cape_compile_import(files, &p, &o, &q, why);
    if(irc != CAPE_OK){
        printf("IMPORT    slot %d: the files are not imported (%s): %s\n", slot, cape_compile_strerror(irc), why);
        r.diff++;
    } else if(cape_profile_equal(&p, &q) != 1){
        printf("IMPORT    slot %d: the files imported with the ZIP as the base are another model\n", slot);
        r.diff++;
    } else if(!quiet)
        printf("IMPORT    slot %d: the files imported with the ZIP as the base are the same model (%u actions kept)\n", slot, o.kept);
    cape_profile_free(&q);
    cape_profile_free(&p);
    return r;
}

static int cmd_check(int argc, char** argv){
    const char* path = NULL;
    const char* zip = NULL;
    int only = -1;
    for(int i = 0; i < argc; i++){
        if(!strcmp(argv[i], "--zip") && i + 1 < argc)
            zip = argv[++i];
        else if(!strcmp(argv[i], "--slot") && i + 1 < argc)
            only = atoi(argv[++i]);
        else if(!path)
            path = argv[i];
        else
            die("check: what is %s?", argv[i]);
    }
    if(!path)
        die("check IMAGE [--slot N] [--zip F]");
    image im;
    image_load(path, &im);
    check_result all = { 0, 0 };
    int slots = 0;
    for(int s = 0; s < CAPE_SLOTS; s++){
        if((only >= 0 && s != only) || !im.present[s])
            continue;
        const check_result r = check_slot(&im, s, zip, 0);
        all.ok += r.ok;
        all.diff += r.diff;
        slots++;
    }
    if(!slots)
        die("%s: no slot to check", path);
    printf("check: %d slot(s), %d files the same, %d differences\n", slots, all.ok, all.diff);
    image_free(&im);
    return all.diff ? 1 : 0;
}

// zip

// The PROFILE.I of a new profile, as the daemon makes one (cape_slot.c): the constant tail, the GUID, cookie 0 and the name
static void new_info(const char* guid, const char* name, uint8_t out[CAPE_INFO_SIZE], cape_slotid* id){
    char hex[33];
    size_t n = 0;
    for(const char* c = guid; *c; c++){
        if(*c == '{' || *c == '}' || *c == '-')
            continue;
        if(n == 32 || hexval(*c) < 0)
            die("--new: %s is not a GUID", guid);
        hex[n++] = *c;
    }
    hex[n] = 0;
    cape_info info;
    cape_info_init(&info);
    if(n != 32 || guid_parse(hex, info.guid))
        die("--new: %s is not a GUID", guid);
    static const uint8_t zero[16] = { 0 };
    if(!memcmp(info.guid, zero, 16))
        die("--new: the GUID of a profile is not zero");
    uint16_t units[CAPE_INFO_NAME_UNITS];
    const int nu = cape_hwslot_utf16((const uint8_t*)name, strlen(name), units, CAPE_INFO_NAME_UNITS);
    if(nu <= 0 || cape_info_set_name(&info, units, (size_t)nu) != CAPE_OK)
        die("--new: the name %s cannot be written to PROFILE.I (UTF-8, 1 to %d UTF-16 units)", name, CAPE_INFO_NAME_MAX);
    info.cookie = 0;
    if(cape_info_build(&info, out, CAPE_INFO_SIZE) != CAPE_INFO_SIZE)
        die("--new: PROFILE.I not made");
    memset(id, 0, sizeof(*id));
    memcpy(id->guid, info.guid, 16);
    id->flag = CAPE_SLOTID_FLAG;
}

static int cmd_zip(int argc, char** argv){
    const char* path = NULL;
    const char* out = NULL;
    const char* xml = NULL;
    const char* lighting = NULL;
    const char* new_guid = NULL;
    const char* new_name = NULL;
    int only = -1;
    for(int i = 0; i < argc; i++){
        if(!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if(!strcmp(argv[i], "--new") && i + 2 < argc){
            new_guid = argv[++i];
            new_name = argv[++i];
        }
        else if(!strcmp(argv[i], "--xml") && i + 1 < argc)
            xml = argv[++i];
        else if(!strcmp(argv[i], "--lighting-xml") && i + 1 < argc)
            lighting = argv[++i];
        else if(!strcmp(argv[i], "--slot") && i + 1 < argc)
            only = atoi(argv[++i]);
        else if(!path)
            path = argv[i];
        else
            die("zip: what is %s?", argv[i]);
    }
    if(!path || !out || (xml && lighting) || (new_guid && (!xml || only < 0 || only >= CAPE_SLOTS)))
        die("zip IMAGE [--slot N] [--xml F | --lighting-xml F] -o DIR, or zip IMAGE --slot N --xml F --new GUID NAME -o DIR");
    image im;
    image_load(path, &im);
    const int layout = image_layout(&im);
    int done = 0;
    cape_slotid table[CAPE_SLOTS];
    memset(table, 0, sizeof(table));
    uint8_t made_info[CAPE_INFO_SIZE];
    cape_slot_file fresh = { .name = CAPE_FILE_INFO, .data = made_info, .len = sizeof(made_info) };
    if(new_guid){
        // the slot must be empty in the image: its table entry too, when the image has the table
        if(im.present[only] || (im.table_known[only] && !cape_slotid_is_empty(&im.table[only])))
            die("--new: slot %d of %s is not empty", only, path);
        new_info(new_guid, new_name, made_info, &table[only]);
    }
    for(int s = 0; s < CAPE_SLOTS; s++){
        if((only >= 0 && s != only) || (!im.present[s] && !new_guid))
            continue;
        const cape_slot_files* files = &im.files[s];
        const cape_slot_file* info = new_guid ? &fresh : cape_slot_files_find(files, CAPE_FILE_INFO);
        if(!info || (!new_guid && image_id(&im, s, &table[s])))
            die("slot %d: no PROFILE.I", s);
        cape_profile_profile p;
        char why[WHY] = "";
        cape_compile_import_opts o;
        memset(&o, 0, sizeof(o));
        if(xml){
            // the model of a document; the files compiled from it
            file_model(xml, &p);
        } else {
            // the files imported (converting): with the model of --lighting-xml, or of the slot's own ZIP, as the base
            cape_profile_profile base;
            int have_base = 0;
            const cape_slot_file* zip = cape_slot_files_find(files, CAPE_FILE_ZIP);
            if(lighting){
                file_model(lighting, &base);
                have_base = 1;
            } else if(zip && data_model(zip->data, zip->len, &base, why) == CAPE_OK)
                have_base = 1;
            o.layout = layout;
            o.convert = 1;
            o.random = random_bytes;
            const int rc = cape_compile_import(files, have_base ? &base : NULL, &o, &p, why);
            if(have_base)
                cape_profile_free(&base);
            if(rc != CAPE_OK)
                die("slot %d: the files are not imported (%s): %s", s, cape_compile_strerror(rc), why);
        }
        cape_slot_files made;
        static cape_compile_lighting lt;
        int rc = model_files(&p, layout, info, &made, &lt, why);
        if(rc != CAPE_OK)
            die("slot %d: the model does not compile (%s): %s", s, cape_compile_strerror(rc), why);
        size_t zl;
        uint8_t* zipdata = model_zip(&p, &zl);
        rc = cape_slot_files_add(&made, CAPE_FILE_ZIP, zipdata, zl);
        free(zipdata);
        if(rc != CAPE_OK)
            die("out of memory");
        image_write_slot(out, s, &made, "write");
        printf("slot %d: %zu files, PROFILE.ZIP %zu bytes; %u actions, %u layers (%u enabled)%s%s%s%s\n", s, made.n, zl, p.nbindings,
               p.nlayers, lt.layers,
               xml ? "; from the XML" : "", new_guid ? "; a new profile" : "", o.converted ? "; macro files that became Macros (the subtype changes)" : "",
               o.relaid ? "; the files of the bindings laid out again as iCUE writes them" : "");
        if(o.converted)
            printf("slot %d: %u macro files converted\n", s, o.converted);
        cape_slot_files_free(&made);
        cape_profile_free(&p);
        done++;
    }
    if(!done)
        die("%s: no slot", path);
    table_write(out, table);   // the entries of the slots written, those of the image
    layout_write(out, layout);
    image_free(&im);
    return 0;
}

// xml

static int cmd_xml(int argc, char** argv){
    if(argc != 1)
        die("xml ZIP");
    size_t len, xl;
    uint8_t* data = read_whole(argv[0], &len);
    if(!data)
        die("%s: %s", argv[0], strerror(errno));
    char why[WHY];
    char* x = xml_of(data, len, &xl, why);
    if(!x)
        die("%s: %s", argv[0], why);
    fwrite(x, 1, xl, stdout);
    free(x);
    free(data);
    return 0;
}

// diff

// The identities of b made those of a where they stand for the same thing: an action on the same key, a layer at the same place
static void map_uuids(const cape_profile_profile* a, cape_profile_profile* b){
    for(uint32_t i = 0; i < b->nbindings; i++)
        for(uint32_t j = 0; j < a->nbindings; j++)
            if(a->bindings[j].key.len == b->bindings[i].key.len
               && !memcmp(a->bindings[j].key.p ? a->bindings[j].key.p : "", b->bindings[i].key.p ? b->bindings[i].key.p : "",
                          a->bindings[j].key.len))
                b->bindings[i].action.id = a->bindings[j].action.id;
    for(uint32_t i = 0; i < b->nlayers && i < a->nlayers; i++){
        b->layers[i].id = a->layers[i].id;
        b->layers[i].effect.id = a->layers[i].effect.id;
    }
}

static int cmd_diff(int argc, char** argv){
    const char* pa = NULL;
    const char* pb = NULL;
    int only = -1, uuid_map = 0;
    for(int i = 0; i < argc; i++){
        if(!strcmp(argv[i], "--uuid-map"))
            uuid_map = 1;
        else if(!strcmp(argv[i], "--slot") && i + 1 < argc)
            only = atoi(argv[++i]);
        else if(!pa)
            pa = argv[i];
        else if(!pb)
            pb = argv[i];
        else
            die("diff: what is %s?", argv[i]);
    }
    if(!pa || !pb)
        die("diff A B [--slot N] [--uuid-map]");
    image a, b;
    image_load(pa, &a);
    image_load(pb, &b);
    int differ = 0, same = 0;
    for(int s = 0; s < CAPE_SLOTS; s++){
        if(only >= 0 && s != only)
            continue;
        if(a.present[s] != b.present[s]){
            printf("DIFF  slot %d: in one image only\n", s);
            differ++;
            continue;
        }
        if(!a.present[s])
            continue;
        const cape_slot_files* fa = &a.files[s];
        const cape_slot_files* fb = &b.files[s];
        for(int side = 0; side < 2; side++){
            const cape_slot_files* x = side ? fb : fa;
            const cape_slot_files* y = side ? fa : fb;
            for(size_t i = 0; i < x->n; i++)
                if(!cape_slot_files_find(y, x->file[i].name)){
                    printf("DIFF  slot %d %s: only in %s\n", s, x->file[i].name, side ? pb : pa);
                    differ++;
                }
        }
        for(size_t i = 0; i < fa->n; i++){
            const cape_slot_file* x = &fa->file[i];
            const cape_slot_file* y = cape_slot_files_find(fb, x->name);
            if(!y)
                continue;
            if(!strcmp(x->name, CAPE_FILE_ZIP)){
                cape_profile_profile ma, mb;
                char why[WHY];
                if(data_model(x->data, x->len, &ma, why) != CAPE_OK || data_model(y->data, y->len, &mb, why) != CAPE_OK){
                    printf("DIFF  slot %d PROFILE.ZIP: not read: %s\n", s, why);
                    differ++;
                } else {
                    if(uuid_map)
                        map_uuids(&ma, &mb);
                    if(cape_profile_equal(&ma, &mb) == 1){
                        printf("SAME  slot %d PROFILE.ZIP (the model%s)\n", s, uuid_map ? ", identities mapped" : "");
                        same++;
                    } else {
                        printf("DIFF  slot %d PROFILE.ZIP: another model\n", s);
                        differ++;
                    }
                }
                cape_profile_free(&ma);
                cape_profile_free(&mb);
                continue;
            }
            // PROFILE.I without its cookie (and GUID, with --uuid-map)
            size_t from = 0;
            int skip = !strcmp(x->name, CAPE_FILE_INFO);
            int eq = x->len == y->len;
            for(size_t k = 0; eq && k < x->len; k++){
                // PROFILE.I: 2-17 the GUID, 18-21 the cookie (cape_format.h)
                if(skip && ((k >= 18 && k < 22) || (uuid_map && k >= 2 && k < 18)))
                    continue;
                if(x->data[k] != y->data[k]){
                    eq = 0;
                    from = k;
                }
            }
            if(eq){
                printf("SAME  slot %d %s%s\n", s, x->name, skip ? (uuid_map ? " (but GUID and cookie)" : " (but the cookie)") : "");
                same++;
            } else {
                printf("DIFF  slot %d %s: %zu and %zu bytes, first difference at %zu\n", s, x->name, x->len, y->len, from);
                differ++;
            }
        }
    }
    printf("diff: %d the same, %d different\n", same, differ);
    image_free(&a);
    image_free(&b);
    return differ ? 1 : 0;
}

// The keyboard: hidraw or the simulator

typedef struct {
    int sim;
    hidraw_io hid;
    cape_sim k;
    const char* sim_out;
    cape_io io;
    cape_io inner;     // the simulator's own io, which io logs and passes to
    FILE* transcript;
} device;

// The simulator's packets go through here to the transcript, in order with the packets it does not take (the change of mode,
// which are only written down): the transcript of a run on the simulator is the whole sequence a run on the keyboard would send
static int sim_send(void* ctx, const uint8_t* p){
    device* d = ctx;
    hidraw_log(d->transcript, "TX", p);
    return d->inner.send(d->inner.ctx, p);
}
static int sim_cont(void* ctx, const uint8_t* p){
    device* d = ctx;
    hidraw_log(d->transcript, "TX", p);
    return d->inner.send_cont ? d->inner.send_cont(d->inner.ctx, p) : d->inner.send(d->inner.ctx, p);
}
static int sim_xfer(void* ctx, const uint8_t* p, uint8_t* reply){
    device* d = ctx;
    hidraw_log(d->transcript, "TX", p);
    const int rc = d->inner.xfer(d->inner.ctx, p, reply);
    if(!rc)
        hidraw_log(d->transcript, "RX", reply);
    return rc;
}
static void sim_sleep(void* ctx, unsigned ms){
    device* d = ctx;
    if(d->inner.sleep_ms)
        d->inner.sleep_ms(d->inner.ctx, ms);
}
static void sim_phase(void* ctx, cape_phase p){
    device* d = ctx;
    if(d->inner.phase)
        d->inner.phase(d->inner.ctx, p);
}

// A packet outside a session: sent to the keyboard, only written down on the simulator (which does not take them)
static int device_send(device* d, const uint8_t p[CAPE_PKT_SIZE]){
    if(d->sim){
        hidraw_log(d->transcript, "TX", p);
        return 0;
    }
    return hidraw_send(&d->hid, p);
}

static void device_open(device* d, const char* sim_image, const char* sim_out, unsigned fail_at, const char* transcript){
    memset(d, 0, sizeof(*d));
    d->transcript = transcript ? fopen(transcript, "w") : NULL;
    if(transcript && !d->transcript)
        die("%s: %s", transcript, strerror(errno));
    if(sim_image){
        d->sim = 1;
        d->sim_out = sim_out;
        sim_init(&d->k, 1);
        image im;
        image_load(sim_image, &im);
        for(int s = 0; s < CAPE_SLOTS; s++){
            cape_slotid id;
            if(!im.present[s] && !im.table_known[s])
                continue;
            if(image_id(&im, s, &id) == 0 && !cape_slotid_is_empty(&id))
                sim_set_slot(&d->k, s, id.guid, id.cookie);
            for(size_t i = 0; i < im.files[s].n; i++)
                if(sim_add_file(&d->k, s, im.files[s].file[i].name, im.files[s].file[i].data, im.files[s].file[i].len))
                    die("the simulator holds %d files per slot", SIM_MAX_FILES);
        }
        image_free(&im);
        d->k.allow_write = 1;
        d->k.fail_at = fail_at;
        d->inner = sim_io(&d->k);
        d->io = (cape_io){ .ctx = d, .send = sim_send, .xfer = sim_xfer, .sleep_ms = sim_sleep, .send_cont = sim_cont, .phase = sim_phase };
        return;
    }
    char why[WHY];
    if(hidraw_open(&d->hid, why, sizeof(why)))
        die("%s", why);
    d->hid.transcript = d->transcript;
    d->io = hidraw_cape_io(&d->hid);
}

// Back to hardware mode after a session, as the daemon does (a read in hardware mode turns the lighting off)
static void device_hardware_mode(device* d){
    if(!d->sim && !d->hid.tested)
        return;
    const uint8_t hw[CAPE_PKT_SIZE] = { 0x07, 0x04, 0x01 };
    if(device_send(d, hw))
        fprintf(stderr, "k95p-hwslot-tool: the return to hardware mode (07 04 01) was not sent\n");
}

static void device_close(device* d){
    if(d->sim){
        if(d->sim_out){
            cape_slotid table[CAPE_SLOTS];
            for(int s = 0; s < CAPE_SLOTS; s++){
                cape_slotid_parse(d->k.slot[s].id, CAPE_SLOTID_SIZE, &table[s]);
                cape_slot_files f;
                memset(&f, 0, sizeof(f));
                for(int i = 0; i < d->k.slot[s].nfiles; i++)
                    cape_slot_files_add(&f, d->k.slot[s].files[i].name, d->k.slot[s].files[i].data, d->k.slot[s].files[i].len);
                if(f.n)
                    image_write_slot(d->sim_out, s, &f, "read");
                cape_slot_files_free(&f);
            }
            if(mkdir(d->sim_out, 0755) && errno != EEXIST)
                die("%s: %s", d->sim_out, strerror(errno));
            table_write(d->sim_out, table);
        }
        if(d->k.violations || d->k.protocol_errors || d->k.write_errors)
            printf("simulator: %u violations, %u protocol errors, %u write errors\n", d->k.violations, d->k.protocol_errors,
                   d->k.write_errors);
        sim_free(&d->k);
    } else
        hidraw_close(&d->hid);
    if(d->transcript)
        fclose(d->transcript);
}

// dump

static int read_into(cape_session* s, int slot, const char* name, cape_slot_files* out){
    static uint8_t buf[CAPE_MAX_FILE_SIZE];
    size_t len;
    const int rc = cape_read_file(s, (unsigned)slot, name, buf, sizeof(buf), &len);
    if(rc == CAPE_OK && cape_slot_files_add(out, name, buf, len) != CAPE_OK)
        die("out of memory");
    return rc;
}

static int cmd_dump(device* d, int argc, char** argv){
    const char* dir = NULL;
    int only = -1;
    int probe_lighting = 0;
    for(int i = 0; i < argc; i++){
        if(!strcmp(argv[i], "--slot") && i + 1 < argc)
            only = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--probe-lighting"))
            probe_lighting = 1;
        else if(!dir)
            dir = argv[i];
        else
            die("dump: what is %s?", argv[i]);
    }
    if(!dir)
        die("dump [--slot N] [--probe-lighting] DIR");
    if(mkdir(dir, 0755))
        die("%s: %s (a dump goes to a new directory)", dir, strerror(errno));
    cape_session s;
    int rc = cape_session_open(&s, &d->io);
    if(rc != CAPE_OK){
        device_close(d);
        die("the session did not open: %s", cape_errstr(rc));
    }
    for(int slot = 0; slot < CAPE_SLOTS && rc == CAPE_OK; slot++){
        if((only >= 0 && slot != only) || cape_slotid_is_empty(&s.slots[slot]))
            continue;
        cape_slot_files f;
        memset(&f, 0, sizeof(f));
        static const char* const fixed[] = { CAPE_FILE_INFO, CAPE_FILE_ZIP, CAPE_FILE_DAT, CAPE_FILE_MAP, CAPE_FILE_LAYERCOUNT };
        for(size_t i = 0; i < 5 && (rc == CAPE_OK || rc == CAPE_E_NOTFOUND); i++)
            rc = read_into(&s, slot, fixed[i], &f);
        // the macro files the DAT names, the layers lghtcnt.cnt counts
        const cape_slot_file* dat = cape_slot_files_find(&f, CAPE_FILE_DAT);
        cape_dat_entry e[CAPE_SLOT_MACROS_MAX];
        const int n = dat ? cape_dat_parse(dat->data, dat->len, e, CAPE_SLOT_MACROS_MAX) : 0;
        for(int i = 0; i < n && (rc == CAPE_OK || rc == CAPE_E_NOTFOUND); i++){
            char name[5];
            memcpy(name, e[i].name, 4);
            name[4] = 0;
            if(cape_macro_filename_parse(name) >= 0 && !cape_slot_files_find(&f, name))
                rc = read_into(&s, slot, name, &f);
        }
        const cape_slot_file* cnt = cape_slot_files_find(&f, CAPE_FILE_LAYERCOUNT);
        uint32_t layers = 0;
        if(cnt && cape_layercount_parse(cnt->data, cnt->len, &layers) == CAPE_OK && layers > CAPE_LAYERS_MAX)
            layers = CAPE_LAYERS_MAX;
        const uint32_t probe_limit = probe_lighting ? CAPE_LAYERS_MAX : layers;
        for(uint32_t l = 0; (probe_lighting || cnt) && l < probe_limit && (rc == CAPE_OK || rc == CAPE_E_NOTFOUND); l++)
            for(const char* ext = "dkr"; *ext && (rc == CAPE_OK || rc == CAPE_E_NOTFOUND); ext++){
                char name[10];
                cape_light_filename(l, *ext, name);
                rc = read_into(&s, slot, name, &f);
                if(probe_lighting)
                    printf("lighting probe slot %d %s: %s\n", slot, name,
                           rc == CAPE_OK ? "present" : rc == CAPE_E_NOTFOUND ? "absent" : cape_errstr(rc));
            }
        if(rc == CAPE_E_NOTFOUND)
            rc = CAPE_OK;
        image_write_slot(dir, slot, &f, "read");
        const cape_slot_file* info = cape_slot_files_find(&f, CAPE_FILE_INFO);
        cape_info pi;
        if(!info || cape_info_parse(info->data, info->len, &pi) != CAPE_OK || memcmp(pi.guid, s.slots[slot].guid, 16)
           || (pi.cookie & 0xffffff) != s.slots[slot].cookie)
            printf("WARNING slot %d: the slot table and PROFILE.I do not agree on the GUID and cookie\n", slot);
        char g[33];
        guid_text(s.slots[slot].guid, g);
        size_t bytes = 0;
        for(size_t i = 0; i < f.n; i++)
            bytes += f.file[i].len;
        if(probe_lighting && rc == CAPE_OK){
            uint32_t reported = 0;
            rc = cape_slot_bytes(&s, (unsigned)slot, &reported);
            if(rc == CAPE_OK)
                printf("slot inventory slot %d: read %zu bytes, reported %u bytes\n", slot, bytes, (unsigned)reported);
        }
        printf("slot %d: GUID %s, cookie %u, %zu files, %zu bytes%s\n", slot, g, (unsigned)s.slots[slot].cookie, f.n, bytes,
               cape_slot_files_find(&f, CAPE_FILE_ZIP) ? "" : ", no PROFILE.ZIP");
        cape_slot_files_free(&f);
    }
    table_write(dir, s.slots);
    // the layout: the keyboard's (its identification); the simulator has none of its own
    const int layout = d->sim ? (g_layout ? g_layout : CAPE_PROFILE_LAYOUT_ISO) : d->hid.layout;
    if(layout < 1 || layout > 5)
        printf("WARNING: the keyboard says a layout %d that is not one of the five: no layout.txt\n", layout);
    else {
        layout_write(dir, layout);
        printf("layout %d (%s)%s\n", layout, layout_name(layout), d->sim ? ", given to the simulator" : ", from the keyboard");
    }
    const unsigned packets = s.tx_packets;
    cape_session_close(&s);
    if(rc == CAPE_OK)
        device_hardware_mode(d);
    printf("dump: %s, %u packets%s\n", rc == CAPE_OK ? "done" : cape_errstr(rc), packets, d->sim ? " (simulated keyboard)" : "");
    return rc == CAPE_OK ? 0 : 1;
}

// convert

// The write of iCUE is wrapped as the daemon wraps it when it saves a slot from hardware mode: software mode, its input table,
// then the save, then 07 04 01. These are the packets the daemon sends, byte for byte (the last packet of the table carries, past
// its 2 entries, what was left in the daemon's buffer).
static const char* const wrap_before[] = {
    "070402",
    "07401e0000c001c002c003c004c005c006c007c008c009c00ac00bc00cc00dc00ec00fc010c011c012c013c014c015c016c017c018c019c01ac01bc01cc01dc0",
    "07401e001ec01fc020c021c022c023c024c025c026c027c028c029c02ac02bc02cc02dc02ec02fc030c031c032c033c034c035c036c037c038c039c03ac03bc0",
    "07401e003cc03dc03ec03fc040c041c042c043c044c045c046c047c048c049c04ac04bc04cc04dc04ec04fc050c051c052c053c054c055c056c057c058c059c0",
    "07401e005ac05bc05cc05dc05ec05fc060c061c062c063c064c065c066c067c068c069c06ac06bc06cc06dc06ec06fc070c071c072c073c074c075c076c077c0",
    "07401e0078c079c07ac07bc07cc07dc07ec07fc080c081c082c083c084c085c086c087c088c089c08ac08bc08cc08dc08ec08fc090c091c092c093c094c095c0",
    "0740020096c097c07ac07bc07cc07dc07ec07fc080c081c082c083c084c085c086c087c088c089c08ac08bc08cc08dc08ec08fc090c091c092c093c094c095c0",
    "0705020003",
};

static void send_hex(device* d, const char* hex){
    uint8_t p[CAPE_PKT_SIZE] = { 0 };
    for(size_t i = 0; hex[2 * i] && i < CAPE_PKT_SIZE; i++)
        p[i] = (uint8_t)(hexval(hex[2 * i]) << 4 | hexval(hex[2 * i + 1]));
    if(device_send(d, p))
        die("a packet of the change of mode was not sent: nothing was written");
}

// The files of a slot in the order iCUE writes them: the macro files, PROFILE.DAT, PROFILE.I, PROFILE.MAP, PROFILE.ZIP, the
// lighting from the layer at the bottom, lghtcnt.cnt
static void write_order(const cape_slot_files* in, cape_slot_files* out){
    memset(out, 0, sizeof(*out));
    for(int pass = 0; pass < 7; pass++){
        static const char* const fixed[] = { NULL, CAPE_FILE_DAT, CAPE_FILE_INFO, CAPE_FILE_MAP, CAPE_FILE_ZIP, NULL, CAPE_FILE_LAYERCOUNT };
        if(fixed[pass]){
            const cape_slot_file* f = cape_slot_files_find(in, fixed[pass]);
            if(f && cape_slot_files_add(out, f->name, f->data, f->len) != CAPE_OK)
                die("out of memory");
            continue;
        }
        if(pass == 0)
            for(unsigned m = 0; m < 0x1000; m++){
                char name[5];
                cape_macro_filename(m, name);
                const cape_slot_file* f = cape_slot_files_find(in, name);
                if(f && cape_slot_files_add(out, f->name, f->data, f->len) != CAPE_OK)
                    die("out of memory");
            }
        else
            for(unsigned l = 0; l < CAPE_LAYERS_MAX; l++)
                for(const char* ext = "dkr"; *ext; ext++){
                    char name[10];
                    cape_light_filename(l, *ext, name);
                    const cape_slot_file* f = cape_slot_files_find(in, name);
                    if(f && cape_slot_files_add(out, f->name, f->data, f->len) != CAPE_OK)
                        die("out of memory");
                }
    }
    if(out->n != in->n)
        die("the image holds a file iCUE does not write");
}

static int id_equal(const cape_slotid* a, const cape_slotid* b){
    return !memcmp(a->guid, b->guid, 16) && (a->cookie & 0xffffff) == (b->cookie & 0xffffff) && a->flag == b->flag;
}

static void id_text(const cape_slotid* id, char out[48]){
    char g[33];
    guid_text(id->guid, g);
    if(cape_slotid_is_empty(id))
        snprintf(out, 48, "empty");
    else
        snprintf(out, 48, "%s:%u", g, (unsigned)id->cookie);
}

static int cmd_convert(device* d, int argc, char** argv){
    const char* path = NULL;
    const char* base = "dump";
    const char* from = NULL;
    const char* expect = NULL;
    const char* dump = NULL;
    long at_least = -1;
    int slot = -1, go = 0;
    for(int i = 0; i < argc; i++){
        if(!strcmp(argv[i], "--slot") && i + 1 < argc)
            slot = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--base") && i + 1 < argc){
            base = argv[++i];
            if(!strcmp(base, "from")){
                if(i + 1 >= argc)
                    die("--base from DUMP");
                from = argv[++i];
            }
        } else if(!strcmp(argv[i], "--expect") && i + 1 < argc)
            expect = argv[++i];
        else if(!strcmp(argv[i], "--dump") && i + 1 < argc)
            dump = argv[++i];
        else if(!strcmp(argv[i], "--cookie-at-least") && i + 1 < argc)
            at_least = atol(argv[++i]);
        else if(!strcmp(argv[i], "--go"))
            go = 1;
        else if(!path)
            path = argv[i];
        else
            die("convert: what is %s?", argv[i]);
    }
    if(!path || slot < 0 || slot >= CAPE_SLOTS)
        die("convert --slot N IMAGE [--base dump|empty|retry|current|from DUMP] [--expect GUID:COOKIE] [--cookie-at-least N] "
            "[--dump DIR] [--go]");
    if(strcmp(base, "dump") && strcmp(base, "empty") && strcmp(base, "retry") && strcmp(base, "current") && strcmp(base, "from"))
        die("--base: dump, empty, retry, current or from DUMP");
    if(!strcmp(base, "current") && !expect)
        die("--base current needs --expect GUID:COOKIE, the value you read and agree to overwrite");
    image im;
    image_load(path, &im);
    if(!im.present[slot] || !cape_slot_files_find(&im.files[slot], CAPE_FILE_ZIP))
        die("%s: slot %d has no PROFILE.ZIP (make one with zip)", path, slot);
    // Fail closed: the image must check clean
    const check_result r = check_slot(&im, slot, NULL, 1);
    if(r.diff)
        die("%s: slot %d does not check clean (k95p-hwslot-tool check): nothing is written", path, slot);
    cape_slotid image_slot;
    if(image_id(&im, slot, &image_slot))
        die("%s: slot %d has no readable PROFILE.I", path, slot);
    cape_slot_files files;
    write_order(&im.files[slot], &files);
    unsigned sectors = 0;
    size_t bytes = 0;
    for(size_t i = 0; i < files.n; i++){
        sectors += cape_file_sectors(files.file[i].len);
        bytes += files.file[i].len;
    }
    // The cookie: never one already used. The largest of the image's, the base's, the attempts made from this image, and the one given
    char attempts[4096];
    snprintf(attempts, sizeof(attempts), "%s/attempts.txt", path);
    uint32_t cookie = image_slot.cookie;
    cape_slotid last_attempt;
    int have_attempt = 0;
    FILE* f = fopen(attempts, "r");
    char line[256];
    while(f && fgets(line, sizeof(line), f)){
        int s;
        char g[40];
        unsigned c;
        if(sscanf(line, "slot %d %39s %u", &s, g, &c) == 3 && s == slot && !guid_parse(g, last_attempt.guid)){
            last_attempt.cookie = c;
            last_attempt.flag = CAPE_SLOTID_FLAG;
            have_attempt = 1;
            if(c > cookie)
                cookie = c;
        }
    }
    if(f)
        fclose(f);
    if(at_least >= 0 && (uint32_t)at_least > cookie)
        cookie = (uint32_t)at_least;
    // The current entry of the slot table: a read session in hardware mode (the packets of a dump's opening), before anything
    // that is not a read
    cape_session rs;
    int rc = cape_session_open(&rs, &d->io);
    if(rc != CAPE_OK){
        device_hardware_mode(d);
        device_close(d);
        die("the session did not open: %s", cape_errstr(rc));
    }
    const cape_slotid current = rs.slots[slot];
    cape_session_close(&rs);
    const int layout = image_layout(&im);
    if(!d->sim && d->hid.layout != layout){
        device_hardware_mode(d);
        device_close(d);
        die("the keyboard says layout %d (%s), the image is of %d (%s): nothing was written", d->hid.layout,
            layout_name(d->hid.layout), layout, layout_name(layout));
    }
    char now[48], img[48];
    id_text(&current, now);
    id_text(&image_slot, img);
    int base_ok = 0;
    if(!strcmp(base, "dump"))
        base_ok = id_equal(&current, &image_slot);
    else if(!strcmp(base, "empty"))
        base_ok = cape_slotid_is_empty(&current);
    else if(!strcmp(base, "retry"))
        base_ok = cape_slotid_is_empty(&current) || id_equal(&current, &image_slot) || (have_attempt && id_equal(&current, &last_attempt));
    else if(!strcmp(base, "current"))
        base_ok = !strcmp(now, expect);
    else {
        image other;
        image_load(from, &other);
        cape_slotid was;
        base_ok = image_id(&other, slot, &was) == 0 && id_equal(&current, &was);
        image_free(&other);
    }
    if((current.cookie & 0xffffff) > cookie)
        cookie = current.cookie & 0xffffff;
    cookie = cape_cookie_next(cookie);
    printf("convert slot %d: %zu files, %zu bytes, %u sectors; on the keyboard now %s, the image %s, base %s: %s\n", slot, files.n, bytes,
           sectors, now, img, base, base_ok ? "as expected" : "NOT as expected");
    printf("convert slot %d: GUID of the image, cookie %u\n", slot, (unsigned)cookie);
    if(dump){
        // What "0e 17 0b" will answer: the sectors of the flash less those the files of the three slots of the dump take
        image all;
        image_load(dump, &all);
        unsigned used = 0;
        for(int s = 0; s < CAPE_SLOTS; s++)
            for(size_t i = 0; i < all.files[s].n; i++)
                used += cape_file_sectors(all.files[s].file[i].len);
        image_free(&all);
        printf("convert slot %d: free sectors by the model of the dump: %u of 2038 (the save needs %u)%s\n", slot, 2038 - used, sectors,
               sectors > 2038 - used ? ": IT WOULD NOT FIT" : "");
    }
    if(!go || !base_ok){
        device_hardware_mode(d);
        device_close(d);
        cape_slot_files_free(&files);
        image_free(&im);
        if(!base_ok)
            die("the slot on the keyboard is not the base: nothing was written");
        printf("convert: nothing written (no --go)\n");
        return 0;
    }
    // Software mode, as the daemon writes, then the write session; its table must still be the one checked. With the daemon's
    // timing as well (setactive_kb): 30 ms after the change of mode, the table at about the pace of its sends (3 ms apart when the
    // daemon sends it: here 2 ms more than the pause of a send), 30 ms after the commit
    const size_t nwrap = sizeof(wrap_before) / sizeof(wrap_before[0]);
    for(size_t k = 0; k < nwrap; k++){
        send_hex(d, wrap_before[k]);
        if(d->io.sleep_ms)
            d->io.sleep_ms(d->io.ctx, k == 0 || k == nwrap - 1 ? 30 : 2);
    }
    cape_wsession w;
    rc = cape_wsession_open(&w, &d->io, (uint8_t)(1u << slot));
    if(rc != CAPE_OK || !id_equal(&w.s.slots[slot], &current)){
        if(rc == CAPE_OK)
            cape_wsession_close(&w);
        device_hardware_mode(d);
        device_close(d);
        die(rc != CAPE_OK ? "the write session did not open: nothing was written" : "the slot changed between the two reads: nothing was written");
    }
    // PROFILE.I with the new cookie; the entry of the table
    cape_slot_file* info = (cape_slot_file*)cape_slot_files_find(&files, CAPE_FILE_INFO);
    cape_info i;
    if(!info || cape_info_parse(info->data, info->len, &i) != CAPE_OK)
        die("PROFILE.I not read");
    i.cookie = cookie;
    if(cape_info_build(&i, info->data, info->len) < 0)
        die("PROFILE.I not made");
    cape_slotid id = image_slot;
    id.cookie = cookie;
    id.flag = CAPE_SLOTID_FLAG;
    // recorded before a packet is written: a retry knows what this attempt may have left
    f = fopen(attempts, "a");
    char g[33];
    guid_text(id.guid, g);
    if(!f || fprintf(f, "slot %d %s %u\n", slot, g, (unsigned)cookie) < 0)
        die("%s: %s (nothing was written)", attempts, strerror(errno));
    fclose(f);
    if(d->io.phase)
        d->io.phase(d->io.ctx, CAPE_PHASE_WRITE);
    cape_write_report rep;
    rc = cape_write_slot(&w, (unsigned)slot, &id, &files, &rep);
    printf("write: %s, stage %s%s%s, %u packets, status %u, sectors %u of %u free, the slot %s\n", rc == CAPE_OK ? "done" : cape_errstr(rc),
           cape_wstage_name(rep.stage), rep.file[0] ? " " : "", rep.file, rep.packets, rep.status, rep.need_sectors, rep.free_sectors,
           rep.slot == CAPE_WSLOT_WRITTEN ? "written" : rep.slot == CAPE_WSLOT_INCOMPLETE ? "NOT WHOLE" : "unchanged");
    if(rc == CAPE_OK){
        if(d->io.phase)
            d->io.phase(d->io.ctx, CAPE_PHASE_VERIFY);
        rc = cape_write_verify(&w, (unsigned)slot, &id, &files, &rep);
        printf("verify: %s%s%s, %u packets\n", rc == CAPE_OK ? "every file as written" : cape_errstr(rc), rep.file[0] ? " " : "", rep.file,
               rep.packets);
    }
    cape_wsession_close(&w);
    device_hardware_mode(d);
    device_close(d);
    cape_slot_files_free(&files);
    image_free(&im);
    return rc == CAPE_OK ? 0 : 1;
}

int main(int argc, char** argv){
    const char* sim = NULL;
    const char* sim_out = NULL;
    const char* transcript = NULL;
    unsigned fail_at = 0;
    int i = 1;
    for(; i < argc && argv[i][0] == '-'; i++){
        if(!strcmp(argv[i], "--hidraw"))
            sim = NULL;
        else if(!strcmp(argv[i], "--sim") && i + 1 < argc)
            sim = argv[++i];
        else if(!strcmp(argv[i], "--sim-out") && i + 1 < argc)
            sim_out = argv[++i];
        else if(!strcmp(argv[i], "--sim-fail-at") && i + 1 < argc)
            fail_at = (unsigned)atoi(argv[++i]);
        else if(!strcmp(argv[i], "--transcript") && i + 1 < argc)
            transcript = argv[++i];
        else if(!strcmp(argv[i], "--layout") && i + 1 < argc){
            const char* l = argv[++i];
            g_layout = atoi(l);
            for(int n = 1; n <= 5; n++)
                if(!strcasecmp(l, layout_name(n)))
                    g_layout = n;
            if(g_layout < 1 || g_layout > 5)
                die("--layout: 1 ANSI, 2 ISO, 3 ABNT, 4 JIS or 5 KS");
        } else if(!strcmp(argv[i], "--compat") && i + 1 < argc){
            if(strcmp(argv[++i], CAPE_PROFILE_LEVEL))
                die("--compat: the only level is %s", CAPE_PROFILE_LEVEL);
        } else
            die("unknown option %s", argv[i]);
    }
    if(i >= argc)
        die("usage: k95p-hwslot-tool [--hidraw | --sim IMAGE [--sim-out DIR] [--sim-fail-at N]] [--transcript F] [--layout N] [--compat %s] "
            "dump|zip|check|xml|diff|convert ...", CAPE_PROFILE_LEVEL);
    const char* cmd = argv[i];
    argc -= i + 1;
    argv += i + 1;
    if(!strcmp(cmd, "zip"))
        return cmd_zip(argc, argv);
    if(!strcmp(cmd, "check"))
        return cmd_check(argc, argv);
    if(!strcmp(cmd, "xml"))
        return cmd_xml(argc, argv);
    if(!strcmp(cmd, "diff"))
        return cmd_diff(argc, argv);
    if(strcmp(cmd, "dump") && strcmp(cmd, "convert"))
        die("unknown command %s", cmd);
    device d;
    device_open(&d, sim, sim_out, fail_at, transcript);
    if(!strcmp(cmd, "dump")){
        const int rc = cmd_dump(&d, argc, argv);
        device_close(&d);
        return rc;
    }
    return cmd_convert(&d, argc, argv);
}
