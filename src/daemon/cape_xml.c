#include "cape_xml.h"

#include <stdlib.h>
#include <string.h>

static const char PROLOGUE[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
#define PROLOGUE_LEN (sizeof(PROLOGUE) - 1)

const char* cape_xml_strerror(int err){
    switch(err){
    case CAPE_XML_E_SYNTAX:
        return "not the XML that iCUE writes";
    case CAPE_XML_E_LIMIT:
        return "XML over a limit";
    case CAPE_XML_E_MEMORY:
        return "out of memory";
    default:
        return cape_strerror(err);
    }
}

int cape_xml_space(uint32_t cp){
    return (cp >= 0x09 && cp <= 0x0d) || cp == 0x20 || cp == 0x85 || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

// A character that XML allows
static int xml_char(uint32_t cp){
    return cp == 0x09 || cp == 0x0a || cp == 0x0d || (cp >= 0x20 && cp <= 0xd7ff) || (cp >= 0xe000 && cp <= 0xfffd) ||
           (cp >= 0x10000 && cp <= 0x10ffff);
}

// One UTF-8 sequence at p, of at most n bytes. Returns its length, or 0 if it is not well formed (an overlong form, a
// surrogate, beyond U+10FFFF, a stray or missing continuation byte).
static size_t utf8_decode(const unsigned char* p, size_t n, uint32_t* cp){
    const unsigned char c = p[0];
    if(c < 0x80){
        *cp = c;
        return 1;
    }
    size_t len;
    uint32_t v, min;
    if(c >= 0xc2 && c <= 0xdf){
        len = 2;
        v = c & 0x1fu;
        min = 0x80;
    } else if(c >= 0xe0 && c <= 0xef){
        len = 3;
        v = c & 0x0fu;
        min = 0x800;
    } else if(c >= 0xf0 && c <= 0xf4){
        len = 4;
        v = c & 0x07u;
        min = 0x10000;
    } else
        return 0;
    if(n < len)
        return 0;
    for(size_t i = 1; i < len; i++){
        if((p[i] & 0xc0) != 0x80)
            return 0;
        v = v << 6 | (p[i] & 0x3fu);
    }
    if(v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
        return 0;
    *cp = v;
    return len;
}

static size_t utf8_encode(uint32_t cp, char* out){
    if(cp < 0x80){
        out[0] = (char)cp;
        return 1;
    }
    if(cp < 0x800){
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if(cp < 0x10000){
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18);
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static int name_start(char c){
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int name_char(char c){
    return name_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

static int xml_ws(char c){
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Whether a name is one the subset allows. CAPE_OK, CAPE_XML_E_SYNTAX or CAPE_XML_E_LIMIT.
static int check_name(const char* s, size_t n){
    if(n == 0 || !name_start(s[0]))
        return CAPE_XML_E_SYNTAX;
    for(size_t i = 1; i < n; i++)
        if(!name_char(s[i]))
            return CAPE_XML_E_SYNTAX;
    return n > CAPE_XML_NAME_MAX ? CAPE_XML_E_LIMIT : CAPE_OK;
}

// Reading

typedef struct {
    uint32_t node;        // the element's node (second pass)
    uint32_t name;        // offset and length of its name in the document
    uint32_t name_len;
    uint32_t children;
    uint32_t value_at;    // where its content begins, just after the '>' of the start tag
    uint32_t value_len;   // the decoded length of the text before its first child
    unsigned dynamic : 1;
    unsigned preserve : 1;
    unsigned mixed : 1;   // its content so far is more than formatting: a child that follows would be mixed content
} frame;

typedef struct {
    char* buf;
    size_t len;
    size_t pos;
    int fill;             // the second pass: nodes are filled in and values decoded in place
    cape_xml_tree* t;
    uint32_t count;       // elements so far
    unsigned depth;       // open elements
    frame stack[CAPE_XML_DEPTH_MAX];
} parser;

static int at(const parser* p, size_t off, char c){
    return p->pos + off < p->len && p->buf[p->pos + off] == c;
}

static size_t skip_ws(parser* p){
    const size_t start = p->pos;
    while(p->pos < p->len && xml_ws(p->buf[p->pos]))
        p->pos++;
    return p->pos - start;
}

// A reference at p->pos (on the '&'): its code point, and p->pos past the ';'.
static int read_reference(parser* p, uint32_t* cp){
    static const struct { const char* name; uint32_t cp; } named[] = {
        { "lt;", '<' }, { "gt;", '>' }, { "amp;", '&' }, { "quot;", '"' }, { "apos;", '\'' },
    };
    p->pos++;
    for(size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++){
        const size_t n = strlen(named[i].name);
        if(p->len - p->pos >= n && !memcmp(p->buf + p->pos, named[i].name, n)){
            p->pos += n;
            *cp = named[i].cp;
            return CAPE_OK;
        }
    }
    if(!at(p, 0, '#'))
        return CAPE_XML_E_SYNTAX;
    p->pos++;
    const int hex = at(p, 0, 'x');
    if(hex)
        p->pos++;
    uint32_t v = 0;
    size_t digits = 0;
    for(; p->pos < p->len; p->pos++, digits++){
        const char c = p->buf[p->pos];
        uint32_t d;
        if(c >= '0' && c <= '9')
            d = (uint32_t)(c - '0');
        else if(hex && c >= 'a' && c <= 'f')
            d = (uint32_t)(c - 'a' + 10);
        else if(hex && c >= 'A' && c <= 'F')
            d = (uint32_t)(c - 'A' + 10);
        else
            break;
        v = v * (hex ? 16u : 10u) + d;
        if(v > 0x10ffff)
            return CAPE_XML_E_SYNTAX;
    }
    if(!digits || !at(p, 0, ';') || !xml_char(v))
        return CAPE_XML_E_SYNTAX;
    p->pos++;
    *cp = v;
    return CAPE_OK;
}

// Character data from p->pos up to the next '<' or the end, decoded in place in the second pass (a decoded character is never
// longer than what it was written as). Sets *nonspace if there was anything but literal whitespace, *decoded to the decoded length.
static int read_text(parser* p, int* nonspace, size_t* decoded){
    size_t w = p->pos;
    unsigned brackets = 0;   // ']' just before, for "]]>", which character data cannot hold
    *nonspace = 0;
    while(p->pos < p->len && p->buf[p->pos] != '<'){
        const char c = p->buf[p->pos];
        uint32_t cp;
        if(c == '&'){
            int rc = read_reference(p, &cp);
            if(rc != CAPE_OK)
                return rc;
            *nonspace = 1;
            brackets = 0;
            char scratch[4];
            w += utf8_encode(cp, p->fill ? p->buf + w : scratch);
            continue;
        }
        const size_t n = utf8_decode((const unsigned char*)p->buf + p->pos, p->len - p->pos, &cp);
        if(!n || !xml_char(cp))
            return CAPE_XML_E_SYNTAX;
        if(c == '>' && brackets >= 2)
            return CAPE_XML_E_SYNTAX;
        brackets = c == ']' ? brackets + 1 : 0;
        if(!xml_ws(c))
            *nonspace = 1;
        if(p->fill)
            memmove(p->buf + w, p->buf + p->pos, n);
        w += n;
        p->pos += n;
    }
    *decoded = w;   // (made relative by the caller)
    return CAPE_OK;
}

// A start tag at p->pos (on the '<'). Sets *empty for <x/>.
static int read_start(parser* p, int* empty){
    cape_xml_tree* t = p->t;
    p->pos++;
    const size_t name = p->pos;
    while(p->pos < p->len && name_char(p->buf[p->pos]))
        p->pos++;
    const size_t name_len = p->pos - name;
    int rc = check_name(p->buf + name, name_len);
    if(rc != CAPE_OK){
        p->pos = name;
        return rc;
    }
    // The element itself, against the limits
    frame* parent = p->depth ? &p->stack[p->depth - 1] : NULL;
    if(p->depth >= CAPE_XML_DEPTH_MAX || p->count >= CAPE_XML_NODES_MAX || (parent && parent->children >= CAPE_XML_CHILDREN_MAX))
        return CAPE_XML_E_LIMIT;
    frame* f = &p->stack[p->depth];
    memset(f, 0, sizeof(*f));
    f->name = (uint32_t)name;
    f->name_len = (uint32_t)name_len;
    // Attributes
    for(;;){
        const size_t ws = skip_ws(p);
        if(at(p, 0, '>')){
            p->pos++;
            *empty = 0;
            break;
        }
        if(at(p, 0, '/') && at(p, 1, '>')){
            p->pos += 2;
            *empty = 1;
            break;
        }
        if(!ws)
            return CAPE_XML_E_SYNTAX;
        const size_t an = p->pos;
        while(p->pos < p->len && (name_char(p->buf[p->pos]) || p->buf[p->pos] == ':'))
            p->pos++;
        const size_t an_len = p->pos - an;
        skip_ws(p);
        if(!at(p, 0, '='))
            return CAPE_XML_E_SYNTAX;
        p->pos++;
        skip_ws(p);
        if(!at(p, 0, '"') && !at(p, 0, '\''))
            return CAPE_XML_E_SYNTAX;
        const char quote = p->buf[p->pos++];
        const size_t av = p->pos;
        while(p->pos < p->len && p->buf[p->pos] != quote && p->buf[p->pos] != '<')
            p->pos++;
        if(!at(p, 0, quote))
            return CAPE_XML_E_SYNTAX;
        const size_t av_len = p->pos - av;
        p->pos++;
        if(an_len == 4 && !memcmp(p->buf + an, "size", 4) && av_len == 7 && !memcmp(p->buf + av, "dynamic", 7) && !f->dynamic)
            f->dynamic = 1;
        else if(an_len == 9 && !memcmp(p->buf + an, "xml:space", 9) && av_len == 8 && !memcmp(p->buf + av, "preserve", 8) && !f->preserve)
            f->preserve = 1;
        else {
            p->pos = an;
            return CAPE_XML_E_SYNTAX;   // another attribute, another value, or one written twice
        }
    }
    f->value_at = (uint32_t)p->pos;
    // A child: the parent holds elements, not a value
    if(parent)
        parent->children++;
    const uint32_t index = p->count++;
    if(p->fill && index >= t->cap)
        return CAPE_XML_E_SYNTAX;   // not what the first pass counted: cannot happen
    if(p->fill){
        cape_xml_node* n = &t->nodes[index];
        memset(n, 0, sizeof(*n));
        n->name = (uint32_t)name;
        n->name_len = (uint8_t)name_len;
        n->depth = (uint8_t)(p->depth + 1);
        n->flags = f->dynamic ? CAPE_XML_DYNAMIC : 0;
        n->first = n->last = n->next = CAPE_XML_NONE;
        n->parent = parent ? parent->node : CAPE_XML_NONE;
        if(parent){
            cape_xml_node* pn = &t->nodes[parent->node];
            if(pn->last == CAPE_XML_NONE)
                pn->first = index;
            else
                t->nodes[pn->last].next = index;
            pn->last = index;
            pn->children++;
        }
        t->count = p->count;
    }
    f->node = index;
    p->depth++;
    return CAPE_OK;
}

// The element on top of the stack ends: as <x/> (empty), or at its end tag, at p->pos (on the "</")
static int read_end(parser* p, int empty){
    frame* f = &p->stack[p->depth - 1];
    if(!empty){
        p->pos += 2;
        const size_t name = p->pos;
        while(p->pos < p->len && name_char(p->buf[p->pos]))
            p->pos++;
        if(p->pos - name != f->name_len || memcmp(p->buf + name, p->buf + f->name, f->name_len) != 0){
            p->pos = name;
            return CAPE_XML_E_SYNTAX;
        }
        skip_ws(p);
        if(!at(p, 0, '>'))
            return CAPE_XML_E_SYNTAX;
        p->pos++;
    }
    if(f->children){
        if(f->preserve)
            return CAPE_XML_E_SYNTAX;
    } else if(!empty){
        // A value: <x></x> or <x>...</x>
        if(f->dynamic)
            return CAPE_XML_E_SYNTAX;
        if(f->value_len > CAPE_XML_VALUE_MAX)
            return CAPE_XML_E_LIMIT;
        if(p->fill){
            cape_xml_node* n = &p->t->nodes[f->node];
            n->flags |= CAPE_XML_VALUE;
            n->value = f->value_at;
            n->value_len = f->value_len;
        }
    }
    p->depth--;
    return CAPE_OK;
}

static int parse_pass(parser* p){
    p->pos = 0;
    p->count = 0;
    p->depth = 0;
    if(p->len < PROLOGUE_LEN || memcmp(p->buf, PROLOGUE, PROLOGUE_LEN) != 0)
        return CAPE_XML_E_SYNTAX;
    p->pos = PROLOGUE_LEN;
    int root_done = 0;
    for(;;){
        if(p->pos >= p->len)
            return root_done ? CAPE_OK : CAPE_XML_E_SYNTAX;
        if(p->buf[p->pos] != '<'){
            const size_t start = p->pos;
            int nonspace;
            size_t end;
            int rc = read_text(p, &nonspace, &end);
            if(rc != CAPE_OK)
                return rc;
            if(!p->depth || p->stack[p->depth - 1].children){
                // between elements, or outside the root: formatting only
                if(nonspace){
                    p->pos = start;
                    return CAPE_XML_E_SYNTAX;
                }
            } else {
                // the content of an element with no child so far: its value, unless a child follows
                frame* f = &p->stack[p->depth - 1];
                f->value_len = (uint32_t)(end - start);
                f->mixed = nonspace ? 1u : 0u;
            }
            continue;
        }
        if(at(p, 1, '/')){
            if(!p->depth)
                return CAPE_XML_E_SYNTAX;
            int rc = read_end(p, 0);
            if(rc != CAPE_OK)
                return rc;
        } else if(at(p, 1, '!') || at(p, 1, '?'))
            return CAPE_XML_E_SYNTAX;   // a comment, CDATA, a document type or a processing instruction
        else {
            if(root_done)
                return CAPE_XML_E_SYNTAX;   // a second root
            if(p->depth && p->stack[p->depth - 1].mixed)
                return CAPE_XML_E_SYNTAX;   // mixed content: a value and then an element
            int empty;
            int rc = read_start(p, &empty);
            if(rc != CAPE_OK)
                return rc;
            if(empty && (rc = read_end(p, 1)) != CAPE_OK)
                return rc;
        }
        if(!p->depth)
            root_done = 1;
    }
}

int cape_xml_parse(cape_xml_tree* t, char* xml, size_t len){
    if(!t || !xml)
        return CAPE_E_ARG;
    memset(t, 0, sizeof(*t));
    if(len > CAPE_XML_DOC_MAX)
        return CAPE_XML_E_LIMIT;
    parser* p = calloc(1, sizeof(*p));
    if(!p)
        return CAPE_XML_E_MEMORY;
    p->buf = xml;
    p->len = len;
    p->t = t;
    // First pass: everything is checked and the elements counted, nothing is changed or allocated
    int rc = parse_pass(p);
    if(rc == CAPE_OK){
        t->nodes = malloc((size_t)p->count * sizeof(*t->nodes));
        if(!t->nodes)
            rc = CAPE_XML_E_MEMORY;
    }
    // Second pass: the nodes, and the values decoded in place
    if(rc == CAPE_OK){
        t->cap = p->count;
        p->fill = 1;
        rc = parse_pass(p);
    }
    if(rc == CAPE_OK){
        t->text = xml;
        t->text_len = len;
    } else {
        const size_t where = p->pos;
        free(t->nodes);
        memset(t, 0, sizeof(*t));
        t->error_at = where;
    }
    free(p);
    return rc;
}

// Building

void cape_xml_init(cape_xml_tree* t){
    if(t)
        memset(t, 0, sizeof(*t));
}

void cape_xml_free(cape_xml_tree* t){
    if(!t)
        return;
    free(t->nodes);
    if(t->text_cap)
        free(t->text);   // a parsed tree's text is the caller's
    memset(t, 0, sizeof(*t));
}

// Whether a value is UTF-8 made of characters XML allows. CAPE_OK or CAPE_XML_E_SYNTAX.
static int check_value(const char* s, size_t n){
    for(size_t i = 0; i < n;){
        uint32_t cp;
        const size_t k = utf8_decode((const unsigned char*)s + i, n - i, &cp);
        if(!k || !xml_char(cp))
            return CAPE_XML_E_SYNTAX;
        i += k;
    }
    return CAPE_OK;
}

// Room for n more bytes of text (a built tree's text never goes over CAPE_XML_DOC_MAX)
static int text_room(cape_xml_tree* t, size_t n){
    if(t->text_len + n > CAPE_XML_DOC_MAX)
        return CAPE_XML_E_LIMIT;
    if(t->text_len + n <= t->text_cap)
        return CAPE_OK;
    size_t cap = t->text_cap ? t->text_cap : 4096;
    while(cap < t->text_len + n)
        cap *= 2;
    char* text = realloc(t->text, cap);
    if(!text)
        return CAPE_XML_E_MEMORY;
    t->text = text;
    t->text_cap = cap;
    return CAPE_OK;
}

int cape_xml_add(cape_xml_tree* t, uint32_t parent, const char* name, unsigned flags, const char* value, size_t value_len){
    if(!t || !name || (flags & ~(CAPE_XML_VALUE | CAPE_XML_DYNAMIC)) || flags == (CAPE_XML_VALUE | CAPE_XML_DYNAMIC))
        return CAPE_E_ARG;
    if(!(flags & CAPE_XML_VALUE))
        value_len = 0;
    else if(!value && value_len)
        return CAPE_E_ARG;
    if(t->count && !t->text_cap)
        return CAPE_E_ARG;   // a parsed tree is not added to
    cape_xml_node* pn = NULL;
    if(parent == CAPE_XML_NONE){
        if(t->count)
            return CAPE_E_ARG;   // a second root
    } else {
        if(parent >= t->count)
            return CAPE_E_ARG;
        pn = &t->nodes[parent];
        if(pn->flags & CAPE_XML_VALUE)
            return CAPE_E_ARG;   // an element holds a value or elements, not both
    }
    const size_t name_len = strlen(name);
    int rc = check_name(name, name_len);
    if(rc != CAPE_OK)
        return rc;
    if(value_len > CAPE_XML_VALUE_MAX)
        return CAPE_XML_E_LIMIT;
    if((rc = check_value(value, value_len)) != CAPE_OK)
        return rc;
    if(t->count >= CAPE_XML_NODES_MAX || (pn && (pn->depth >= CAPE_XML_DEPTH_MAX || pn->children >= CAPE_XML_CHILDREN_MAX)))
        return CAPE_XML_E_LIMIT;
    if(t->count == t->cap){
        const uint32_t cap = t->cap ? (t->cap * 2 < CAPE_XML_NODES_MAX ? t->cap * 2 : CAPE_XML_NODES_MAX) : 64;
        cape_xml_node* nodes = realloc(t->nodes, (size_t)cap * sizeof(*nodes));
        if(!nodes)
            return CAPE_XML_E_MEMORY;
        t->nodes = nodes;
        t->cap = cap;
        if(parent != CAPE_XML_NONE)
            pn = &t->nodes[parent];
    }
    if((rc = text_room(t, name_len + value_len)) != CAPE_OK)
        return rc;
    const uint32_t index = t->count++;
    cape_xml_node* n = &t->nodes[index];
    memset(n, 0, sizeof(*n));
    n->name = (uint32_t)t->text_len;
    n->name_len = (uint8_t)name_len;
    memcpy(t->text + t->text_len, name, name_len);
    t->text_len += name_len;
    n->value = (uint32_t)t->text_len;
    n->value_len = (uint32_t)value_len;
    if(value_len)
        memcpy(t->text + t->text_len, value, value_len);
    t->text_len += value_len;
    n->flags = (uint8_t)flags;
    n->parent = parent;
    n->first = n->last = n->next = CAPE_XML_NONE;
    n->depth = (uint8_t)(pn ? pn->depth + 1 : 1);
    if(pn){
        if(pn->last == CAPE_XML_NONE)
            pn->first = index;
        else
            t->nodes[pn->last].next = index;
        pn->last = index;
        pn->children++;
    }
    return (int)index;
}

// Writing

typedef struct {
    uint8_t* out;
    size_t cap;
    size_t len;   // what has been written, or would have been
} sink;

static void put(sink* s, const char* p, size_t n){
    if(s->out && s->len <= s->cap && n <= s->cap - s->len)
        memcpy(s->out + s->len, p, n);
    s->len += n;
}

static void put_tabs(sink* s, unsigned n){
    for(unsigned i = 0; i < n; i++)
        put(s, "\t", 1);
}

// A value, with the escapes of QXmlStreamWriter::writeCharacters
static void put_value(sink* s, const char* v, size_t n){
    size_t from = 0;
    for(size_t i = 0; i < n; i++){
        const char* e;
        switch(v[i]){
        case '<':
            e = "&lt;";
            break;
        case '>':
            e = "&gt;";
            break;
        case '&':
            e = "&amp;";
            break;
        case '"':
            e = "&quot;";
            break;
        default:
            continue;
        }
        put(s, v + from, i - from);
        put(s, e, strlen(e));
        from = i + 1;
    }
    put(s, v + from, n - from);
}

// iCUE's archive writes xml:space="preserve" on a value that is not empty and begins or ends with a space for QChar::isSpace()
static int preserve(const char* v, size_t n){
    if(!n)
        return 0;
    uint32_t first, last;
    if(!utf8_decode((const unsigned char*)v, n, &first))
        return 0;
    size_t i = n - 1;
    while(i > 0 && ((unsigned char)v[i] & 0xc0) == 0x80)
        i--;
    if(!utf8_decode((const unsigned char*)v + i, n - i, &last))
        return 0;
    return cape_xml_space(first) || cape_xml_space(last);
}

int cape_xml_emit(const cape_xml_tree* t, uint8_t* out, size_t cap){
    if(!t || !t->count || (!out && cap))
        return CAPE_E_ARG;
    sink s = { out, cap, 0 };
    put(&s, PROLOGUE, PROLOGUE_LEN);
    put(&s, "\n", 1);
    // Depth first, without recursion: open a node, go down to its first child, or close it and go on to the next sibling (closing
    // the parents that have no more children)
    uint32_t i = 0;
    while(i != CAPE_XML_NONE){
        const cape_xml_node* n = &t->nodes[i];
        const char* name = t->text + n->name;
        put_tabs(&s, n->depth - 1u);
        put(&s, "<", 1);
        put(&s, name, n->name_len);
        if(n->flags & CAPE_XML_DYNAMIC)
            put(&s, " size=\"dynamic\"", 15);
        if((n->flags & CAPE_XML_VALUE) && preserve(t->text + n->value, n->value_len))
            put(&s, " xml:space=\"preserve\"", 21);
        if(n->first != CAPE_XML_NONE){
            put(&s, ">\n", 2);
            i = n->first;
            continue;
        }
        if(n->flags & CAPE_XML_VALUE){
            put(&s, ">", 1);
            put_value(&s, t->text + n->value, n->value_len);
            put(&s, "</", 2);
            put(&s, name, n->name_len);
            put(&s, ">\n", 2);
        } else
            put(&s, "/>\n", 3);
        // on to the next sibling, closing every parent left with no more children
        while(i != CAPE_XML_NONE && t->nodes[i].next == CAPE_XML_NONE){
            i = t->nodes[i].parent;
            if(i != CAPE_XML_NONE){
                const cape_xml_node* pn = &t->nodes[i];
                put_tabs(&s, pn->depth - 1u);
                put(&s, "</", 2);
                put(&s, t->text + pn->name, pn->name_len);
                put(&s, ">\n", 2);
            }
        }
        if(i != CAPE_XML_NONE)
            i = t->nodes[i].next;
        if(s.len > CAPE_XML_DOC_MAX)
            return CAPE_XML_E_LIMIT;   // (checked after each element, so a tree far too large is not written to the end)
    }
    if(out && s.len > cap)
        return CAPE_E_CAP;
    return (int)s.len;
}

uint32_t cape_xml_child(const cape_xml_tree* t, uint32_t parent, const char* name){
    if(!t || !name || parent >= t->count)
        return CAPE_XML_NONE;
    for(uint32_t i = t->nodes[parent].first; i != CAPE_XML_NONE; i = t->nodes[i].next)
        if(cape_xml_is(t, i, name))
            return i;
    return CAPE_XML_NONE;
}

int cape_xml_is(const cape_xml_tree* t, uint32_t i, const char* name){
    if(!t || !name || i >= t->count)
        return 0;
    const size_t n = strlen(name);
    return n == t->nodes[i].name_len && !memcmp(t->text + t->nodes[i].name, name, n);
}
