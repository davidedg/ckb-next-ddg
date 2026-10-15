#include "cape_zipslot.h"

#include "cape_binding.h"
#include "cape_qz.h"
#include "cape_xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* cape_zip_state_name(cape_zip_state st){
    switch(st){
    case CAPE_ZIP_OK:
        return "ok";
    case CAPE_ZIP_NONE:
        return "none";
    case CAPE_ZIP_UNKNOWN:
        return "unknown";
    case CAPE_ZIP_CORRUPT:
        return "corrupt";
    case CAPE_ZIP_UNCOMPILABLE:
        return "uncompilable";
    case CAPE_ZIP_STALE:
        return "stale";
    }
    return "?";
}

static void say(char* why, const char* what, const char* detail){
    if(why)
        snprintf(why, CAPE_PROFILE_WHY, "%s: %s", what, detail);
}

// What a failure of the reading is to the caller: out of memory, an unsupported iCUE, or anything else that is not a profile of the level
static int reading_error(int rc){
    if(rc == CAPE_E_CAP || rc == CAPE_QZ_E_MEMORY || rc == CAPE_XML_E_MEMORY || rc == CAPE_PROFILE_E_MEMORY)
        return CAPE_E_CAP;
    if(rc == CAPE_PROFILE_E_UNKNOWN)
        return CAPE_PROFILE_E_UNKNOWN;
    return CAPE_PROFILE_E_CORRUPT;
}

int cape_zipslot_model_xml(const char* xml, size_t len, cape_profile_profile* p, char* why){
    if(!p)
        return CAPE_E_ARG;
    memset(p, 0, sizeof(*p));
    if(!xml)
        return CAPE_E_ARG;
    // The parser decodes the values in place: a copy of the document, which lives as long as the tree
    char* doc = malloc(len ? len : 1);
    if(!doc)
        return CAPE_E_CAP;
    memcpy(doc, xml, len);
    cape_xml_tree t;
    int rc = cape_xml_parse(&t, doc, len);
    if(rc == CAPE_OK)
        rc = cape_profile_read(&t, p, why);
    else
        say(why, "XML", cape_xml_strerror(rc));
    cape_xml_free(&t);
    free(doc);
    return rc == CAPE_OK ? CAPE_OK : reading_error(rc);
}

int cape_zipslot_model(const uint8_t* zip, size_t len, cape_profile_profile* p, char* why){
    if(!p)
        return CAPE_E_ARG;
    memset(p, 0, sizeof(*p));
    if(!zip)
        return CAPE_E_ARG;
    const int n = cape_qz_decode(zip, len, NULL, 0);
    if(n <= 0){
        say(why, "PROFILE.ZIP", n == 0 ? "an empty document" : cape_qz_strerror(n));
        return n == 0 ? CAPE_PROFILE_E_CORRUPT : reading_error(n);
    }
    uint8_t* xml = malloc((size_t)n);
    if(!xml)
        return CAPE_E_CAP;
    const int m = cape_qz_decode(zip, len, xml, (size_t)n);
    int rc;
    if(m != n){
        say(why, "PROFILE.ZIP", cape_qz_strerror(m < 0 ? m : CAPE_E_SIZE));
        rc = m < 0 ? reading_error(m) : CAPE_PROFILE_E_CORRUPT;
    } else
        rc = cape_zipslot_model_xml((const char*)xml, (size_t)n, p, why);
    free(xml);
    return rc;
}

int cape_zipslot_zip(const cape_profile_profile* p, uint8_t** zip, size_t* len, char* why){
    if(zip)
        *zip = NULL;
    if(len)
        *len = 0;
    if(!p || !zip || !len)
        return CAPE_E_ARG;
    cape_xml_tree t;
    int rc = cape_profile_build(p, &t, why);
    if(rc != CAPE_OK){
        cape_xml_free(&t);
        return rc == CAPE_XML_E_MEMORY || rc == CAPE_PROFILE_E_MEMORY ? CAPE_E_CAP : rc;
    }
    const int n = cape_xml_emit(&t, NULL, 0);
    uint8_t* xml = n > 0 ? malloc((size_t)n) : NULL;
    if(n <= 0 || !xml){
        cape_xml_free(&t);
        free(xml);
        if(n <= 0)
            say(why, "the XML of the profile", cape_xml_strerror(n));
        return n <= 0 ? n : CAPE_E_CAP;
    }
    rc = cape_xml_emit(&t, xml, (size_t)n);
    cape_xml_free(&t);
    if(rc != n){
        free(xml);
        say(why, "the XML of the profile", cape_xml_strerror(rc < 0 ? rc : CAPE_E_SIZE));
        return rc < 0 ? rc : CAPE_E_SIZE;
    }
    uint8_t* out = malloc(CAPE_QZ_BLOB_MAX);
    if(!out){
        free(xml);
        return CAPE_E_CAP;
    }
    const int zn = cape_qz_encode(xml, (size_t)n, out, CAPE_QZ_BLOB_MAX);
    free(xml);
    if(zn <= 0){
        free(out);
        say(why, "PROFILE.ZIP", cape_qz_strerror(zn));
        return zn == CAPE_QZ_E_MEMORY ? CAPE_E_CAP : zn < 0 ? zn : CAPE_E_SIZE;
    }
    uint8_t* fit = realloc(out, (size_t)zn);   // (a smaller block: realloc keeps the bytes)
    *zip = fit ? fit : out;
    *len = (size_t)zn;
    return CAPE_OK;
}

int cape_zipslot_is_lighting(const char* name){
    return name && (!strncmp(name, "lght_", 5) || !strcmp(name, CAPE_FILE_LAYERCOUNT));
}

int cape_zipslot_files(const cape_profile_profile* p, int layout, const uint8_t* info, size_t info_len, cape_slot_files* out,
                       cape_compile_lighting* lt, char* why){
    if(out)
        memset(out, 0, sizeof(*out));
    if(lt)
        memset(lt, 0, sizeof(*lt));
    if(!p || !out || !lt || (!info && info_len))
        return CAPE_E_ARG;
    cape_binding_model m;
    int rc = cape_compile_bindings(p, &m, why);
    if(rc == CAPE_OK){
        rc = cape_binding_build(&m, out);
        if(rc != CAPE_OK && rc != CAPE_E_CAP){
            // The model said what the bindings are, and they cannot be written: no profile of iCUE does that
            say(why, "the bindings", cape_strerror(rc));
            rc = CAPE_COMPILE_E_UNCOMPILABLE;
        }
    }
    cape_binding_free(&m);
    if(rc == CAPE_OK && info){
        uint8_t* i = malloc(info_len ? info_len : 1);
        if(!i)
            rc = CAPE_E_CAP;
        else {
            memcpy(i, info, info_len);
            if(info_len >= CAPE_COMPILE_IND_OFFSET + CAPE_COMPILE_IND_SIZE)
                rc = cape_compile_indicators(p, i + CAPE_COMPILE_IND_OFFSET);
            if(rc == CAPE_OK)
                rc = cape_slot_files_add(out, CAPE_FILE_INFO, i, info_len);
            free(i);
        }
    }
    if(rc == CAPE_OK)
        rc = cape_compile_lighting_build(p, layout, lt, why);
    for(size_t i = 0; rc == CAPE_OK && i < lt->nfiles; i++)
        rc = cape_slot_files_add(out, lt->file[i].name, lt->file[i].data, lt->file[i].len);
    if(rc != CAPE_OK){
        cape_slot_files_free(out);
        memset(lt, 0, sizeof(*lt));
    }
    return rc;
}

uint64_t cape_zipslot_bytes(const cape_slot_files* made, const cape_compile_lighting* lt, size_t zip_len){
    uint64_t n = zip_len;
    for(size_t i = 0; made && i < made->n; i++){
        if(!cape_zipslot_is_lighting(made->file[i].name))
            n += made->file[i].len;
    }
    return n + (lt ? lt->bytes : 0);
}
