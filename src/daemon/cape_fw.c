#include "cape_fw.h"
#include "cape_format.h"

#include <stdio.h>

// Devices and firmware levels the on-board profile file system has been verified on. To add a level, verify
// it on a real keyboard first (see cape_fw.h), then add a row here and rebuild. Do not add a row to make an
// untested keyboard work.
static const cape_fwinfo tested[] = {
    // K95 RGB Platinum, application firmware 3.29, bootloader 3.03
    { 0x1b1c, 0x1b2d, 0x0329, 0x0303 },
};

#define TESTED_COUNT  (sizeof(tested) / sizeof(tested[0]))

int cape_fwinfo_from_ident(const uint8_t* reply, size_t len, cape_fwinfo* out){
    if(!reply || !out)
        return CAPE_E_ARG;
    if(len < CAPE_FW_IDENT_MIN_LEN)
        return CAPE_E_SIZE;
    if(reply[0] != 0x0e || reply[1] != 0x01)
        return CAPE_E_MAGIC;
    out->app_bcd = (uint16_t)(reply[8] | (reply[9] << 8));
    out->bld_bcd = (uint16_t)(reply[10] | (reply[11] << 8));
    out->vid = (uint16_t)(reply[12] | (reply[13] << 8));
    out->pid = (uint16_t)(reply[14] | (reply[15] << 8));
    return CAPE_OK;
}

int cape_fw_is_tested(const cape_fwinfo* info){
    if(!info)
        return 0;
    for(size_t i = 0; i < TESTED_COUNT; i++){
        if(info->vid == tested[i].vid && info->pid == tested[i].pid
                && info->app_bcd == tested[i].app_bcd && info->bld_bcd == tested[i].bld_bcd)
            return 1;
    }
    return 0;
}

// The versions are BCD, so printing the bytes in hexadecimal gives the usual decimal notation
static int describe(const cape_fwinfo* info, char* buf, size_t cap){
    return snprintf(buf, cap, "%04x:%04x fw %x.%02x bld %x.%02x", (unsigned)info->vid, (unsigned)info->pid,
                    (unsigned)(info->app_bcd >> 8), (unsigned)(info->app_bcd & 0xff),
                    (unsigned)(info->bld_bcd >> 8), (unsigned)(info->bld_bcd & 0xff));
}

int cape_fw_describe(const cape_fwinfo* info, char* buf, size_t cap){
    if(!info || (!buf && cap))
        return CAPE_E_ARG;
    return describe(info, buf, cap);
}

int cape_fw_tested_list(char* buf, size_t cap){
    if(!buf && cap)
        return CAPE_E_ARG;
    size_t total = 0;
    for(size_t i = 0; i < TESTED_COUNT; i++){
        char item[48];
        int n = describe(&tested[i], item, sizeof(item));
        if(n < 0)
            return n;
        // Append ", " and the item, keeping the text terminated and within cap
        const char* sep = i ? ", " : "";
        int m = snprintf(buf && total < cap ? buf + total : NULL, total < cap ? cap - total : 0, "%s%s", sep, item);
        if(m < 0)
            return m;
        total += (size_t)m;
    }
    return (int)total;
}
