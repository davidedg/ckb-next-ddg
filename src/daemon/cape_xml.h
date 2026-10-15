#ifndef CAPE_XML_H
#define CAPE_XML_H

/*
 * The XML inside PROFILE.ZIP (cape_qz.h), the profile of a slot of the Corsair K95 RGB Platinum as iCUE writes it: a
 * cereal archive over Qt's QXmlStreamWriter. This module reads that XML into a tree of elements and writes a tree back
 * exactly as iCUE would. It knows nothing of what the elements mean (that is the model of the profile): only elements,
 * their values and the two attributes iCUE writes. Its only dependency is the C library.
 *
 * The subset, as iCUE writes it and as this module reads it:
 *  - the declaration <?xml version="1.0" encoding="UTF-8"?> at the very start (no byte order mark), then one root element;
 *  - elements that hold either other elements, or a value (character data, possibly empty), never both; whitespace
 *    between elements is formatting and is dropped;
 *  - the attributes size="dynamic" (a collection) and xml:space="preserve", nothing else; no namespaces;
 *  - in values: the entities &lt; &gt; &amp; &quot; &apos; and character references; UTF-8, only characters that XML
 *    allows; no comments, CDATA, processing instructions or document types.
 * Element names are ASCII: a letter or '_', then letters, digits, '_', '.' or '-'.
 *
 * Two departures from what an XML parser does, both to keep iCUE's own bytes: a value keeps its carriage returns as they
 * are (iCUE writes "\r\n" in a Text without escaping it, and XML would turn it into "\n"), and a value made only of
 * whitespace is kept (Qt's QDomDocument, iCUE's reader, drops it). A value is told from no content: <x></x> holds an empty
 * value (CAPE_XML_VALUE), <x/> holds nothing.
 *
 * Writing follows QXmlStreamWriter with automatic formatting and a tab per level, as iCUE sets it up: the declaration, a
 * line per element, a value on the line of its element; in values only < > & " are escaped (as &lt; &gt; &amp; &quot;),
 * carriage returns, line feeds and tabs are written as they are. xml:space="preserve" is written as iCUE's archive does:
 * on a value that is not empty and whose first or last character is a space for QChar::isSpace() (cape_xml_space()).
 * A tree read from iCUE's XML is written back to the same bytes.
 *
 * Memory: a parsed tree borrows the caller's buffer (values are decoded in place, so the buffer is changed) and
 * allocates one array of nodes, exactly as many as there are elements (36 bytes each: at CAPE_XML_NODES_MAX, 18 MiB).
 * The limits are checked before that allocation, so a document cannot make it larger. A tree being built allocates its
 * nodes and a copy of the names and values, growing as needed, within the same limits. cape_xml_free() releases both.
 *
 * Functions return CAPE_OK, an index or a number of bytes on success, and a negative code on failure: the CAPE_E_* codes
 * of cape_format.h or the CAPE_XML_E_* codes below.
 */

#include "cape_format.h"

#include <stddef.h>
#include <stdint.h>

// Limits, the same when reading and when building. The worst profile a slot can hold (a Macro of 256 rows on each of the
// 152 keys) has 14.6 MiB of XML, depth 17, 355 171 elements and at most 256 children for one element; the longest value
// of a profile is a Text of 1024 characters (at most 4 bytes each in UTF-8, and a carriage return for each line feed).
#define CAPE_XML_DOC_MAX       (16 * 1024 * 1024)   // the whole document: CAPE_QZ_XML_MAX of cape_qz.h
#define CAPE_XML_DEPTH_MAX     64                   // the root is at depth 1
#define CAPE_XML_NODES_MAX     (512u * 1024u)       // elements
#define CAPE_XML_CHILDREN_MAX  4096u                // children of one element
#define CAPE_XML_NAME_MAX      64                   // bytes of an element name
#define CAPE_XML_VALUE_MAX     (64 * 1024)          // bytes of one value, decoded

#define CAPE_XML_NONE  UINT32_MAX   // no node

// Flags of a node
#define CAPE_XML_VALUE    0x01u   // it holds a value, possibly empty (<x></x>); without it and without children it is <x/>
#define CAPE_XML_DYNAMIC  0x02u   // size="dynamic": a collection (never with a value)

// Errors, in addition to the CAPE_E_* codes of cape_format.h
#define CAPE_XML_E_SYNTAX  (-60)   // not well formed, or outside the subset above
#define CAPE_XML_E_LIMIT   (-61)   // over one of the limits above
#define CAPE_XML_E_MEMORY  (-62)   // an allocation failed

typedef struct {
    uint32_t name;        // offset of the name in the tree's text
    uint32_t value;       // offset of the value in the tree's text (CAPE_XML_VALUE), decoded
    uint32_t value_len;
    uint32_t parent;      // CAPE_XML_NONE for the root
    uint32_t first;       // first child, CAPE_XML_NONE if none
    uint32_t last;        // last child
    uint32_t next;        // next sibling
    uint16_t children;    // number of children
    uint8_t name_len;
    uint8_t depth;        // 1 for the root
    uint8_t flags;
} cape_xml_node;

typedef struct {
    cape_xml_node* nodes;   // node 0 is the root
    uint32_t count;
    uint32_t cap;
    char* text;             // names and values (not terminated): the caller's buffer for a parsed tree, a copy for a built one
    size_t text_len;
    size_t text_cap;        // 0 for a parsed tree (the text is not the tree's)
    size_t error_at;        // after a failed cape_xml_parse: offset of the byte it stopped at
} cape_xml_tree;

// Short description of any CAPE_OK, CAPE_E_* or CAPE_XML_E_* code.
const char* cape_xml_strerror(int err);

// Whether QChar::isSpace() is true for a code point (U+0009..U+000D, U+0020, U+0085, U+00A0, and the Unicode separators).
int cape_xml_space(uint32_t cp);

// Reads len bytes of XML into t (which need not be initialised; on failure it is left empty, with error_at set). The
// buffer is changed (values are decoded in place) and must stay alive and unchanged as long as t. Returns CAPE_OK or:
//  CAPE_E_ARG          t or xml is NULL
//  CAPE_XML_E_LIMIT    over a limit (the document, depth, elements, children, a name or a value)
//  CAPE_XML_E_SYNTAX   anything else that is not the subset
//  CAPE_XML_E_MEMORY
int cape_xml_parse(cape_xml_tree* t, char* xml, size_t len);

// An empty tree, to build.
void cape_xml_init(cape_xml_tree* t);

// Adds an element as the last child of parent, or as the root with CAPE_XML_NONE (only once, as the first node). flags is
// CAPE_XML_VALUE (then value_len bytes of value, copied, may be 0) or CAPE_XML_DYNAMIC or 0; value is ignored without
// CAPE_XML_VALUE. Returns the index of the new node, or:
//  CAPE_E_ARG          t or name NULL, value NULL with a length, parent not a node of t, a second root, unknown flags,
//                      both flags, or a parent that holds a value
//  CAPE_XML_E_SYNTAX   a name or a value that the subset does not allow (UTF-8, a character XML does not allow)
//  CAPE_XML_E_LIMIT, CAPE_XML_E_MEMORY
int cape_xml_add(cape_xml_tree* t, uint32_t parent, const char* name, unsigned flags, const char* value, size_t value_len);

// Writes the tree as iCUE would into out. Returns the number of bytes, or (with out NULL and cap 0) the number it would
// write, or: CAPE_E_ARG (t NULL, no root, out NULL with a cap), CAPE_E_CAP (cap too small; nothing useful in out),
// CAPE_XML_E_LIMIT (more than CAPE_XML_DOC_MAX bytes).
int cape_xml_emit(const cape_xml_tree* t, uint8_t* out, size_t cap);

// Releases what the tree allocated and leaves it empty. Safe on an empty or failed tree.
void cape_xml_free(cape_xml_tree* t);

// The first child of parent named name, or CAPE_XML_NONE.
uint32_t cape_xml_child(const cape_xml_tree* t, uint32_t parent, const char* name);

// Whether node i is named name.
int cape_xml_is(const cape_xml_tree* t, uint32_t i, const char* name);

#endif  // CAPE_XML_H
