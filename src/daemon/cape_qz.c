#include "cape_qz.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZLIB_CONST   // next_in is const: zlib does not write to its input
#include <zlib.h>

const char* cape_qz_strerror(int err){
    switch(err){
    case CAPE_QZ_E_STREAM:
        return "not a good zlib stream";
    case CAPE_QZ_E_MEMORY:
        return "zlib could not get its memory";
    default:
        return cape_strerror(err);
    }
}

// zlib's working memory, through this module's malloc and free (zlib's own defaults would call them from inside libz)
static voidpf qz_alloc(voidpf opaque, uInt items, uInt size){
    (void)opaque;
    if(size && items > SIZE_MAX / size)
        return NULL;
    return malloc((size_t)items * size);
}

static void qz_free(voidpf opaque, voidpf address){
    (void)opaque;
    free(address);
}

static void qz_stream_init(z_stream* s){
    memset(s, 0, sizeof(*s));
    s->zalloc = qz_alloc;
    s->zfree = qz_free;
    s->opaque = NULL;
}

int cape_qz_decode(const uint8_t* blob, size_t len, uint8_t* out, size_t cap){
    if(!blob || (!out && cap))
        return CAPE_E_ARG;
    if(len < CAPE_QZ_HEADER + 1)
        return CAPE_E_SIZE;
    if(len > CAPE_QZ_BLOB_MAX)
        return CAPE_E_RANGE;
    const size_t declared = (size_t)blob[0] << 24 | (size_t)blob[1] << 16 | (size_t)blob[2] << 8 | (size_t)blob[3];
    if(declared > CAPE_QZ_XML_MAX)
        return CAPE_E_RANGE;
    if(!out)
        return (int)declared;
    if(cap < declared)
        return CAPE_E_CAP;

    z_stream s;
    qz_stream_init(&s);
    s.next_in = blob + CAPE_QZ_HEADER;
    s.avail_in = (uInt)(len - CAPE_QZ_HEADER);
    // inflateInit, not inflateInit2 with automatic detection: a gzip or raw deflate stream is not what qCompress writes
    int zr = inflateInit(&s);
    if(zr != Z_OK)
        return zr == Z_MEM_ERROR ? CAPE_QZ_E_MEMORY : CAPE_QZ_E_STREAM;
    s.next_out = out;
    s.avail_out = (uInt)declared;
    zr = inflate(&s, Z_FINISH);
    int rc = CAPE_OK;
    if(zr != Z_STREAM_END && (zr == Z_OK || zr == Z_BUF_ERROR) && s.avail_out == 0){
        // The declared length is all there, but the stream has not ended: one byte more tells data beyond the declared
        // length (the stream goes on) from a stream that only lacks its checksum or is cut short
        uint8_t extra;
        s.next_out = &extra;
        s.avail_out = 1;
        zr = inflate(&s, Z_FINISH);
        if(s.avail_out == 0)
            rc = CAPE_E_SIZE;
    }
    if(rc == CAPE_OK){
        if(zr == Z_STREAM_END){
            if(s.avail_in != 0 || s.total_out != declared)
                rc = CAPE_E_SIZE;    // bytes after the end of the stream, or fewer bytes than declared
        } else if(zr == Z_MEM_ERROR)
            rc = CAPE_QZ_E_MEMORY;
        else
            rc = CAPE_QZ_E_STREAM;   // Z_DATA_ERROR, Z_NEED_DICT, or the input ended before the stream did
    }
    inflateEnd(&s);
    return rc == CAPE_OK ? (int)declared : rc;
}

int cape_qz_encode(const uint8_t* xml, size_t len, uint8_t* out, size_t cap){
    if(!xml || !out)
        return CAPE_E_ARG;
    if(len == 0)
        return CAPE_E_SIZE;
    if(len > CAPE_QZ_XML_MAX)
        return CAPE_E_RANGE;
    const size_t room = cap < CAPE_QZ_BLOB_MAX ? cap : CAPE_QZ_BLOB_MAX;
    const int full = cap < CAPE_QZ_BLOB_MAX ? CAPE_E_CAP : CAPE_E_RANGE;
    if(room <= CAPE_QZ_HEADER)
        return full;

    z_stream s;
    qz_stream_init(&s);
    // deflateInit(level) is what compress2() does, with the default window, memory level and strategy
    int zr = deflateInit(&s, 9);
    if(zr != Z_OK)
        return zr == Z_MEM_ERROR ? CAPE_QZ_E_MEMORY : CAPE_QZ_E_STREAM;
    s.next_in = xml;
    s.avail_in = (uInt)len;
    s.next_out = out + CAPE_QZ_HEADER;
    s.avail_out = (uInt)(room - CAPE_QZ_HEADER);
    zr = deflate(&s, Z_FINISH);
    int rc;
    if(zr == Z_STREAM_END)
        rc = (int)(CAPE_QZ_HEADER + s.total_out);
    else if(zr == Z_OK || zr == Z_BUF_ERROR)
        rc = full;   // out of room before the end of the stream
    else
        rc = zr == Z_MEM_ERROR ? CAPE_QZ_E_MEMORY : CAPE_QZ_E_STREAM;
    deflateEnd(&s);
    if(rc < 0)
        return rc;
    out[0] = (uint8_t)(len >> 24);
    out[1] = (uint8_t)(len >> 16);
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)len;
    return rc;
}
