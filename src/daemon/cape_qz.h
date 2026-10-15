#ifndef CAPE_QZ_H
#define CAPE_QZ_H

/*
 * The container of PROFILE.ZIP, iCUE's own copy of a profile slot of the Corsair K95 RGB Platinum (USB 1b1c:1b2d).
 *
 * iCUE writes it with Qt's qCompress(): a u32 big-endian length of the uncompressed data (the XML of the profile), then
 * one zlib stream (RFC 1950) of exactly that data, at compression level 9. Despite its name the file is not a ZIP
 * archive. The firmware never reads it; iCUE reads it back instead of the other files of the slot.
 *
 * This module only converts between the bytes of the file and the bytes of the XML: it knows nothing of XML, slots or
 * the keyboard. Its only dependencies are the C library and zlib. It allocates nothing but zlib's own working memory,
 * which goes through malloc() and free() of this module (so a test can make it fail): the caller provides every buffer.
 *
 * Decoding is strict, because the file comes from the keyboard and is not trusted:
 *  - the blob is at most CAPE_QZ_BLOB_MAX bytes, a 4-byte header and at least one byte of stream;
 *  - the declared length is at most CAPE_QZ_XML_MAX, and is checked before anything is inflated;
 *  - the stream must be a zlib stream (not gzip, not raw deflate, no preset dictionary) with a good Adler-32;
 *  - it must inflate to exactly the declared length, and end exactly where the blob ends.
 * Qt's own qUncompress() is more lenient (it trusts neither the length nor the end of the blob); what this module
 * accepts, Qt accepts too.
 *
 * Encoding uses deflate at level 9 with zlib's default window, memory level and strategy: what compress2(..., 9) does,
 * and so what qCompress() does. The classic zlib (1.2.11, 1.2.13, 1.3.1) gives the same bytes as iCUE on every
 * PROFILE.ZIP that iCUE is known to have written; zlib-ng gives different but equally valid ones. Compare the uncompressed data and the
 * header, never the compressed bytes. An encoded blob is never larger than CAPE_QZ_BLOB_MAX: what this module writes, it
 * can read back.
 *
 * Functions return a number of bytes on success and a negative code on failure: the CAPE_E_* codes of cape_format.h
 * or the CAPE_QZ_E_* codes below. Nothing is ever written beyond the capacity given by the caller.
 */

#include "cape_format.h"

#include <stddef.h>
#include <stdint.h>

#define CAPE_QZ_HEADER    4                    // u32 big-endian length of the uncompressed data
// The largest PROFILE.ZIP: the largest file the transport reads (CAPE_MAX_FILE_SIZE of cape.h). The worst profile a
// slot can hold (a Macro of 256 rows on each of the 152 keys) compresses to about 600 KiB, more than this: such a
// profile cannot be saved, since its ZIP could not be read back.
#define CAPE_QZ_BLOB_MAX  (256 * 1024)
// The largest XML accepted. The worst profile above is about 14.6 MiB of XML.
#define CAPE_QZ_XML_MAX   (16 * 1024 * 1024)

// Errors, in addition to the CAPE_E_* codes of cape_format.h
#define CAPE_QZ_E_STREAM  (-50)  // not a good zlib stream: header, data, checksum, preset dictionary, or cut short
#define CAPE_QZ_E_MEMORY  (-51)  // zlib could not get its working memory

// Short description of any CAPE_OK, CAPE_E_* or CAPE_QZ_E_* code.
const char* cape_qz_strerror(int err);

// Decodes a PROFILE.ZIP of len bytes into out. Returns the length of the XML (the declared one), or:
//  CAPE_E_ARG    blob is NULL, or out is NULL with a non-zero cap
//  CAPE_E_SIZE   len is less than the header and one byte of stream; or the stream inflates to more or fewer bytes
//                than declared, or does not end where the blob ends
//  CAPE_E_RANGE  len is more than CAPE_QZ_BLOB_MAX, or the declared length is more than CAPE_QZ_XML_MAX
//  CAPE_E_CAP    cap is less than the declared length
//  CAPE_QZ_E_STREAM, CAPE_QZ_E_MEMORY
// in this order. With out NULL (and cap 0) only the header is checked, nothing is inflated, and the declared length is
// returned: the size of the buffer to give to the second call. The XML is not checked in any way.
int cape_qz_decode(const uint8_t* blob, size_t len, uint8_t* out, size_t cap);

// Encodes len bytes of XML into out. Returns the length of the blob, or:
//  CAPE_E_ARG    xml or out is NULL
//  CAPE_E_SIZE   len is 0 (qCompress writes a header with no stream, which cape_qz_decode refuses)
//  CAPE_E_RANGE  len is more than CAPE_QZ_XML_MAX; or the blob would be more than CAPE_QZ_BLOB_MAX bytes, when cap is
//                at least CAPE_QZ_BLOB_MAX
//  CAPE_E_CAP    the blob does not fit in cap bytes, when cap is less than CAPE_QZ_BLOB_MAX
//  CAPE_QZ_E_MEMORY, CAPE_QZ_E_STREAM (zlib refused its own parameters: a broken zlib)
// in this order. A cap of CAPE_QZ_BLOB_MAX is always enough for a blob that can be read back.
int cape_qz_encode(const uint8_t* xml, size_t len, uint8_t* out, size_t cap);

#endif  // CAPE_QZ_H
