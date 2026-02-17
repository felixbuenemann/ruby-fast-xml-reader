/*
 * FastXmlReader — mmap-based XML pull reader for Ruby.
 *
 * Fast, lightweight XML pull reader using mmap + zero-copy scanning
 * with C-level name interning. API compatible with Nokogiri::XML::Reader.
 */

#include <ruby.h>
#include <ruby/encoding.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Node types matching Nokogiri::XML::Reader constants                */
/* ------------------------------------------------------------------ */
#define TYPE_ELEMENT      1
#define TYPE_TEXT          3
#define TYPE_END_ELEMENT  15

/* ------------------------------------------------------------------ */
/* Name interning cache (open-addressed hash table)                   */
/* ------------------------------------------------------------------ */
#define NAME_CACHE_SIZE  512  /* must be power of 2 */
#define NAME_CACHE_MASK  (NAME_CACHE_SIZE - 1)

typedef struct {
    const char *ptr;
    size_t len;
    VALUE rb_str;  /* frozen interned Ruby String */
} CacheEntry;

/* ------------------------------------------------------------------ */
/* Attribute storage                                                  */
/* ------------------------------------------------------------------ */
#define MAX_ATTRS 32

typedef struct {
    const char *name_ptr;
    size_t name_len;
    const char *val_ptr;
    size_t val_len;
    int val_has_entity;  /* 1 if value contains '&' */
} AttrEntry;

/* ------------------------------------------------------------------ */
/* Reader struct                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *data;
    size_t size;
    size_t pos;
    int is_mmap;       /* 1 = munmap on free, 0 = free() on free */

    int depth;         /* tree depth (incremented after open, decremented before close) */
    int report_depth;  /* depth to report for the current node */
    int node_type;
    int is_empty;

    /* Current element name (pointer into mmap buffer) */
    const char *name_ptr;
    size_t name_len;

    /* Current text value (pointer into mmap buffer or decoded) */
    const char *text_ptr;
    size_t text_len;
    int text_has_entity;  /* 1 if text contains '&' */
    VALUE decoded_text;   /* non-Qnil if entity-decoded text was allocated */

    /* Attributes for current element */
    AttrEntry attrs[MAX_ATTRS];
    int attr_count;

    /* Name interning cache */
    CacheEntry name_cache[NAME_CACHE_SIZE];

    rb_encoding *utf8;
} FastReader;

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */
static VALUE rb_cFastXmlReader;
static void reader_free(void *ptr);
static void reader_mark(void *ptr);
static size_t reader_memsize(const void *ptr);

static const rb_data_type_t reader_type = {
    .wrap_struct_name = "FastXmlReader",
    .function = { .dmark = reader_mark, .dfree = reader_free, .dsize = reader_memsize },
    .flags = RUBY_TYPED_FREE_IMMEDIATELY
};

/* ------------------------------------------------------------------ */
/* FNV-1a hash                                                        */
/* ------------------------------------------------------------------ */
static inline unsigned int fnv1a(const char *ptr, size_t len) {
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)ptr[i];
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Name interning                                                     */
/* ------------------------------------------------------------------ */
static VALUE intern_name(FastReader *r, const char *ptr, size_t len) {
    unsigned int idx = fnv1a(ptr, len) & NAME_CACHE_MASK;

    /* Linear probe */
    for (int i = 0; i < 8; i++) {
        CacheEntry *e = &r->name_cache[(idx + i) & NAME_CACHE_MASK];
        if (e->ptr == NULL) {
            /* Empty slot — create new interned string */
            VALUE s = rb_enc_str_new(ptr, (long)len, r->utf8);
            rb_str_freeze(s);
            e->ptr = RSTRING_PTR(s);  /* point to Ruby string's buffer */
            e->len = len;
            e->rb_str = s;
            return s;
        }
        if (e->len == len && memcmp(e->ptr, ptr, len) == 0) {
            return e->rb_str;
        }
    }

    /* Cache full for this bucket — just create a frozen string */
    VALUE s = rb_enc_str_new(ptr, (long)len, r->utf8);
    rb_str_freeze(s);
    return s;
}

/* ------------------------------------------------------------------ */
/* GC mark                                                            */
/* ------------------------------------------------------------------ */
static void reader_mark(void *ptr) {
    FastReader *r = (FastReader *)ptr;
    for (int i = 0; i < NAME_CACHE_SIZE; i++) {
        if (r->name_cache[i].rb_str != Qnil) {
            rb_gc_mark(r->name_cache[i].rb_str);
        }
    }
    if (r->decoded_text != Qnil) {
        rb_gc_mark(r->decoded_text);
    }
}

/* ------------------------------------------------------------------ */
/* Free                                                               */
/* ------------------------------------------------------------------ */
static void reader_free(void *ptr) {
    FastReader *r = (FastReader *)ptr;
    if (r->data) {
        if (r->is_mmap)
            munmap((void *)r->data, r->size);
        else
            xfree((void *)r->data);
    }
    xfree(r);
}

static size_t reader_memsize(const void *ptr) {
    return sizeof(FastReader);
}

/* ------------------------------------------------------------------ */
/* Allocate                                                           */
/* ------------------------------------------------------------------ */
static VALUE reader_alloc(VALUE klass) {
    FastReader *r = ALLOC(FastReader);
    memset(r, 0, sizeof(FastReader));
    r->decoded_text = Qnil;
    for (int i = 0; i < NAME_CACHE_SIZE; i++) {
        r->name_cache[i].rb_str = Qnil;
    }
    return TypedData_Wrap_Struct(klass, &reader_type, r);
}

/* ------------------------------------------------------------------ */
/* Helpers: scanning                                                  */
/* ------------------------------------------------------------------ */
static inline int at_end(FastReader *r) {
    return r->pos >= r->size;
}

static inline void skip_spaces(FastReader *r) {
    while (r->pos < r->size) {
        unsigned char c = (unsigned char)r->data[r->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            r->pos++;
        else
            break;
    }
}

/* Return 1 if [ptr, ptr+len) is all whitespace */
static inline int is_blank(const char *ptr, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)ptr[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Entity decoding                                                    */
/* ------------------------------------------------------------------ */
static VALUE decode_entities(FastReader *r, const char *src, size_t len) {
    /* Fast path: no entities */
    if (!memchr(src, '&', len)) {
        return rb_enc_str_new(src, (long)len, r->utf8);
    }

    /* Slow path: decode entities */
    VALUE buf = rb_enc_str_new(NULL, 0, r->utf8);
    /* Pre-allocate to source length (decoded is always <= source) */
    rb_str_modify_expand(buf, (long)len);

    const char *end = src + len;
    const char *p = src;

    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        if (!amp) {
            rb_str_cat(buf, p, (long)(end - p));
            break;
        }

        /* Copy text before '&' */
        if (amp > p) {
            rb_str_cat(buf, p, (long)(amp - p));
        }

        const char *semi = memchr(amp, ';', (size_t)(end - amp));
        if (!semi) {
            /* No closing ';' — copy '&' literally */
            rb_str_cat(buf, amp, 1);
            p = amp + 1;
            continue;
        }

        size_t elen = (size_t)(semi - amp - 1);  /* length of entity name */
        const char *ename = amp + 1;

        if (elen == 3 && memcmp(ename, "amp", 3) == 0) {
            rb_str_cat(buf, "&", 1);
        } else if (elen == 2 && memcmp(ename, "lt", 2) == 0) {
            rb_str_cat(buf, "<", 1);
        } else if (elen == 2 && memcmp(ename, "gt", 2) == 0) {
            rb_str_cat(buf, ">", 1);
        } else if (elen == 4 && memcmp(ename, "quot", 4) == 0) {
            rb_str_cat(buf, "\"", 1);
        } else if (elen == 4 && memcmp(ename, "apos", 4) == 0) {
            rb_str_cat(buf, "'", 1);
        } else if (elen > 1 && ename[0] == '#') {
            /* Numeric entity */
            unsigned long cp = 0;
            if (ename[1] == 'x' || ename[1] == 'X') {
                for (size_t i = 2; i < elen; i++) {
                    char c = ename[i];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp += (unsigned long)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp += (unsigned long)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp += (unsigned long)(c - 'A' + 10);
                }
            } else {
                for (size_t i = 1; i < elen; i++) {
                    cp = cp * 10 + (unsigned long)(ename[i] - '0');
                }
            }
            /* Encode code point as UTF-8 */
            char u8[4];
            int u8len;
            if (cp < 0x80) {
                u8[0] = (char)cp; u8len = 1;
            } else if (cp < 0x800) {
                u8[0] = (char)(0xC0 | (cp >> 6));
                u8[1] = (char)(0x80 | (cp & 0x3F));
                u8len = 2;
            } else if (cp < 0x10000) {
                u8[0] = (char)(0xE0 | (cp >> 12));
                u8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                u8[2] = (char)(0x80 | (cp & 0x3F));
                u8len = 3;
            } else {
                u8[0] = (char)(0xF0 | (cp >> 18));
                u8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                u8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                u8[3] = (char)(0x80 | (cp & 0x3F));
                u8len = 4;
            }
            rb_str_cat(buf, u8, u8len);
        } else {
            /* Unknown entity — copy literally */
            rb_str_cat(buf, amp, (long)(semi - amp + 1));
        }

        p = semi + 1;
    }

    return buf;
}

/* ------------------------------------------------------------------ */
/* Attribute value decode helper                                       */
/* ------------------------------------------------------------------ */
static VALUE make_attr_value(FastReader *r, AttrEntry *a) {
    if (a->val_has_entity) {
        return decode_entities(r, a->val_ptr, a->val_len);
    }
    return rb_enc_str_new(a->val_ptr, (long)a->val_len, r->utf8);
}

/* ------------------------------------------------------------------ */
/* Skip comment: <!-- ... -->                                         */
/* ------------------------------------------------------------------ */
static void skip_comment(FastReader *r) {
    /* pos is right after "<!--" */
    while (r->pos + 2 < r->size) {
        const char *p = memchr(r->data + r->pos, '-', r->size - r->pos - 1);
        if (!p) { r->pos = r->size; return; }
        r->pos = (size_t)(p - r->data);
        if (r->pos + 2 < r->size &&
            r->data[r->pos] == '-' &&
            r->data[r->pos + 1] == '-' &&
            r->data[r->pos + 2] == '>') {
            r->pos += 3;
            return;
        }
        r->pos++;
    }
    r->pos = r->size;
}

/* ------------------------------------------------------------------ */
/* Skip processing instruction: <? ... ?>                             */
/* ------------------------------------------------------------------ */
static void skip_pi(FastReader *r) {
    while (r->pos + 1 < r->size) {
        const char *p = memchr(r->data + r->pos, '?', r->size - r->pos - 1);
        if (!p) { r->pos = r->size; return; }
        r->pos = (size_t)(p - r->data);
        if (r->data[r->pos + 1] == '>') {
            r->pos += 2;
            return;
        }
        r->pos++;
    }
    r->pos = r->size;
}

/* ------------------------------------------------------------------ */
/* Skip CDATA: <![CDATA[ ... ]]>                                     */
/* ------------------------------------------------------------------ */
static void skip_cdata(FastReader *r) {
    while (r->pos + 2 < r->size) {
        const char *p = memchr(r->data + r->pos, ']', r->size - r->pos - 2);
        if (!p) { r->pos = r->size; return; }
        r->pos = (size_t)(p - r->data);
        if (r->data[r->pos + 1] == ']' && r->data[r->pos + 2] == '>') {
            r->pos += 3;
            return;
        }
        r->pos++;
    }
    r->pos = r->size;
}

/* ------------------------------------------------------------------ */
/* Skip DOCTYPE: <!DOCTYPE ... >  (simple, no internal subset)        */
/* ------------------------------------------------------------------ */
static void skip_doctype(FastReader *r) {
    int bracket_depth = 0;
    while (r->pos < r->size) {
        char c = r->data[r->pos++];
        if (c == '[') bracket_depth++;
        else if (c == ']') bracket_depth--;
        else if (c == '>' && bracket_depth == 0) return;
    }
}

/* ------------------------------------------------------------------ */
/* Parse attributes of current element                                */
/* Assumes pos is right after the element name (or after scanning     */
/* past a namespace prefix).                                          */
/* Returns position of '>' or '/>' terminator (after it).             */
/* ------------------------------------------------------------------ */
static void parse_attrs(FastReader *r) {
    r->attr_count = 0;
    r->is_empty = 0;

    while (r->pos < r->size) {
        skip_spaces(r);
        if (at_end(r)) return;

        char c = r->data[r->pos];

        if (c == '>') {
            r->pos++;
            return;
        }
        if (c == '/') {
            r->is_empty = 1;
            r->pos++;
            if (r->pos < r->size && r->data[r->pos] == '>')
                r->pos++;
            return;
        }

        /* Attribute name */
        size_t name_start = r->pos;
        while (r->pos < r->size) {
            c = r->data[r->pos];
            if (c == '=' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == '/')
                break;
            r->pos++;
        }
        size_t name_end = r->pos;

        /* Skip to '=' */
        skip_spaces(r);
        if (at_end(r) || r->data[r->pos] != '=') continue;
        r->pos++; /* skip '=' */
        skip_spaces(r);
        if (at_end(r)) return;

        /* Attribute value */
        char quote = r->data[r->pos];
        if (quote != '"' && quote != '\'') continue;
        r->pos++; /* skip opening quote */

        size_t val_start = r->pos;
        int has_entity = 0;

        /* Scan for closing quote using memchr */
        const char *q = memchr(r->data + r->pos, quote, r->size - r->pos);
        if (!q) { r->pos = r->size; return; }

        size_t val_end = (size_t)(q - r->data);

        /* Check for entities in the value */
        if (memchr(r->data + val_start, '&', val_end - val_start)) {
            has_entity = 1;
        }

        r->pos = val_end + 1; /* skip closing quote */

        /* Skip namespace attributes (xmlns and xmlns:*) */
        size_t nlen = name_end - name_start;
        if (nlen >= 5 && memcmp(r->data + name_start, "xmlns", 5) == 0 &&
            (nlen == 5 || r->data[name_start + 5] == ':')) {
            continue;
        }

        if (r->attr_count < MAX_ATTRS) {
            AttrEntry *a = &r->attrs[r->attr_count++];
            a->name_ptr = r->data + name_start;
            a->name_len = nlen;
            a->val_ptr = r->data + val_start;
            a->val_len = val_end - val_start;
            a->val_has_entity = has_entity;
        }
    }
}

/* ------------------------------------------------------------------ */
/* read — advance to next node                                        */
/* Returns 1 if a node was read, 0 if EOF.                            */
/* ------------------------------------------------------------------ */
static int reader_read_internal(FastReader *r) {
    r->decoded_text = Qnil;
    r->text_ptr = NULL;
    r->text_len = 0;
    r->text_has_entity = 0;
    r->attr_count = 0;

again:
    if (at_end(r)) return 0;

    if (r->data[r->pos] == '<') {
        r->pos++; /* skip '<' */
        if (at_end(r)) return 0;

        char c = r->data[r->pos];

        /* End element: </name> */
        if (c == '/') {
            r->pos++;
            size_t name_start = r->pos;

            /* Scan to '>' */
            const char *gt = memchr(r->data + r->pos, '>', r->size - r->pos);
            if (!gt) { r->pos = r->size; return 0; }
            r->pos = (size_t)(gt - r->data) + 1;

            /* Extract name (strip namespace prefix) */
            size_t raw_len = (size_t)(gt - r->data) - name_start;
            const char *nptr = r->data + name_start;
            const char *colon = memchr(nptr, ':', raw_len);
            if (colon) {
                size_t prefix_len = (size_t)(colon - nptr) + 1;
                nptr += prefix_len;
                raw_len -= prefix_len;
            }
            /* Trim trailing whitespace */
            while (raw_len > 0 && ((unsigned char)nptr[raw_len-1] <= ' '))
                raw_len--;

            r->name_ptr = nptr;
            r->name_len = raw_len;
            r->node_type = TYPE_END_ELEMENT;
            r->is_empty = 0;
            if (r->depth > 0) r->depth--;
            r->report_depth = r->depth;
            return 1;
        }

        /* Comment: <!-- */
        if (c == '!' && r->pos + 2 < r->size &&
            r->data[r->pos + 1] == '-' && r->data[r->pos + 2] == '-') {
            r->pos += 3;
            skip_comment(r);
            goto again;
        }

        /* CDATA: <![CDATA[ */
        if (c == '!' && r->pos + 7 < r->size &&
            memcmp(r->data + r->pos, "![CDATA[", 8) == 0) {
            r->pos += 8;
            skip_cdata(r);
            goto again;
        }

        /* DOCTYPE: <!DOCTYPE */
        if (c == '!' && r->pos + 7 < r->size &&
            memcmp(r->data + r->pos + 1, "DOCTYPE", 7) == 0) {
            r->pos += 8;
            skip_doctype(r);
            goto again;
        }

        /* Processing instruction: <? */
        if (c == '?') {
            r->pos++;
            skip_pi(r);
            goto again;
        }

        /* Start element: <name ... > or <name ... /> */
        size_t name_start = r->pos;

        /* Scan element name */
        while (r->pos < r->size) {
            c = r->data[r->pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                c == '>' || c == '/')
                break;
            r->pos++;
        }

        size_t name_end = r->pos;
        const char *nptr = r->data + name_start;
        size_t nlen = name_end - name_start;

        /* Strip namespace prefix */
        const char *colon = memchr(nptr, ':', nlen);
        if (colon) {
            size_t prefix_len = (size_t)(colon - nptr) + 1;
            nptr += prefix_len;
            nlen -= prefix_len;
        }

        r->name_ptr = nptr;
        r->name_len = nlen;
        r->node_type = TYPE_ELEMENT;

        /* Parse attributes and detect self-closing */
        parse_attrs(r);

        /* For self-closing elements, don't increment depth — the element
         * is emitted at the current depth and no end-element follows.
         * For open elements, increment depth (children will be deeper). */
        if (!r->is_empty) {
            /* Check if next content is immediately </name> — collapse to empty */
            size_t saved = r->pos;
            if (r->pos + 1 < r->size && r->data[r->pos] == '<' && r->data[r->pos + 1] == '/') {
                /* Peek ahead: is it "</samename>"? */
                size_t peek = r->pos + 2;
                /* Skip namespace prefix in closing tag */
                const char *peek_ptr = r->data + peek;
                size_t remaining = r->size - peek;
                const char *gt = memchr(peek_ptr, '>', remaining);
                if (gt) {
                    size_t close_name_len = (size_t)(gt - peek_ptr);
                    const char *close_nptr = peek_ptr;
                    const char *close_colon = memchr(close_nptr, ':', close_name_len);
                    if (close_colon) {
                        size_t plen = (size_t)(close_colon - close_nptr) + 1;
                        close_nptr += plen;
                        close_name_len -= plen;
                    }
                    /* Trim trailing whitespace */
                    while (close_name_len > 0 && ((unsigned char)close_nptr[close_name_len-1] <= ' '))
                        close_name_len--;

                    if (close_name_len == nlen && memcmp(close_nptr, nptr, nlen) == 0) {
                        /* Collapse: treat as empty element */
                        r->is_empty = 1;
                        r->pos = (size_t)(gt - r->data) + 1;
                    } else {
                        r->pos = saved;
                    }
                } else {
                    r->pos = saved;
                }
            }
        }

        r->report_depth = r->depth;
        if (!r->is_empty) {
            r->depth++;
        }

        return 1;

    } else {
        /* Text content: scan to next '<' */
        size_t text_start = r->pos;
        const char *lt = memchr(r->data + r->pos, '<', r->size - r->pos);
        size_t text_end;
        if (lt) {
            text_end = (size_t)(lt - r->data);
            r->pos = text_end;
        } else {
            text_end = r->size;
            r->pos = r->size;
        }

        size_t tlen = text_end - text_start;

        /* Skip blank text nodes (NOBLANKS equivalent) */
        if (is_blank(r->data + text_start, tlen)) {
            goto again;
        }

        r->node_type = TYPE_TEXT;
        r->report_depth = r->depth;
        r->text_ptr = r->data + text_start;
        r->text_len = tlen;
        r->text_has_entity = (memchr(r->text_ptr, '&', tlen) != NULL) ? 1 : 0;
        r->is_empty = 0;
        r->name_ptr = NULL;
        r->name_len = 0;

        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Ruby methods                                                       */
/* ------------------------------------------------------------------ */
/* Read all data from a Ruby IO into a malloc'd buffer (read in 1MB chunks). */
static void reader_init_from_io(FastReader *r, VALUE io) {
    ID id_read = rb_intern("read");
    VALUE chunk_size = INT2FIX(1024 * 1024);

    size_t capacity = 4 * 1024 * 1024;  /* 4MB initial */
    size_t total = 0;
    char *buf = xmalloc(capacity);

    for (;;) {
        VALUE chunk = rb_funcall(io, id_read, 1, chunk_size);
        if (NIL_P(chunk) || RSTRING_LEN(chunk) == 0) break;

        size_t clen = (size_t)RSTRING_LEN(chunk);
        while (total + clen > capacity) {
            capacity *= 2;
            buf = xrealloc(buf, capacity);
        }
        memcpy(buf + total, RSTRING_PTR(chunk), clen);
        total += clen;
    }

    if (total == 0) {
        xfree(buf);
        r->data = NULL;
        r->size = 0;
    } else {
        /* Shrink to exact size */
        buf = xrealloc(buf, total);
        r->data = buf;
        r->size = total;
    }
    r->is_mmap = 0;
}

static VALUE reader_initialize(VALUE self, VALUE arg) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);

    r->utf8 = rb_utf8_encoding();
    r->pos = 0;
    r->depth = 0;
    r->node_type = 0;
    r->is_empty = 0;

    if (RB_TYPE_P(arg, T_STRING)) {
        /* File path — mmap */
        const char *fpath = StringValueCStr(arg);

        int fd = open(fpath, O_RDONLY);
        if (fd < 0) rb_sys_fail(fpath);

        struct stat st;
        if (fstat(fd, &st) < 0) { close(fd); rb_sys_fail(fpath); }

        if (st.st_size == 0) {
            close(fd);
            r->data = NULL;
            r->size = 0;
            r->is_mmap = 1;
            return self;
        }

        void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map == MAP_FAILED) rb_sys_fail("mmap");
        madvise(map, (size_t)st.st_size, MADV_SEQUENTIAL);

        r->data = (const char *)map;
        r->size = (size_t)st.st_size;
        r->is_mmap = 1;
    } else {
        /* IO — read into malloc'd buffer */
        reader_init_from_io(r, arg);
    }

    return self;
}

static VALUE reader_read(VALUE self) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);

    if (reader_read_internal(r)) {
        return Qtrue;
    }
    return Qfalse;
}

static VALUE reader_each(VALUE self) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);

    RETURN_ENUMERATOR(self, 0, NULL);

    while (reader_read_internal(r)) {
        rb_yield(self);
    }

    return self;
}

static VALUE reader_name(VALUE self) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);

    if (r->name_ptr == NULL || r->name_len == 0)
        return Qnil;

    return intern_name(r, r->name_ptr, r->name_len);
}

static VALUE reader_node_type(VALUE self) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);
    return INT2FIX(r->node_type);
}

static VALUE reader_depth(VALUE self) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);
    return INT2FIX(r->report_depth);
}

static VALUE reader_value(VALUE self) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);

    if (r->text_ptr == NULL || r->text_len == 0)
        return Qnil;

    if (r->text_has_entity) {
        if (r->decoded_text == Qnil) {
            r->decoded_text = decode_entities(r, r->text_ptr, r->text_len);
        }
        return r->decoded_text;
    }

    return rb_enc_str_new(r->text_ptr, (long)r->text_len, r->utf8);
}

static VALUE reader_attribute(VALUE self, VALUE attr_name) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);

    const char *needle = StringValuePtr(attr_name);
    long needle_len = RSTRING_LEN(attr_name);

    for (int i = 0; i < r->attr_count; i++) {
        AttrEntry *a = &r->attrs[i];
        if ((long)a->name_len == needle_len && memcmp(a->name_ptr, needle, (size_t)needle_len) == 0) {
            return make_attr_value(r, a);
        }
    }

    return Qnil;
}

static VALUE reader_empty_element_p(VALUE self) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);
    return r->is_empty ? Qtrue : Qfalse;
}

static VALUE reader_close(VALUE self) {
    FastReader *r;
    TypedData_Get_Struct(self, FastReader, &reader_type, r);

    if (r->data) {
        if (r->is_mmap)
            munmap((void *)r->data, r->size);
        else
            xfree((void *)r->data);
        r->data = NULL;
        r->size = 0;
    }
    return Qnil;
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */
void Init_fast_xml_reader(void) {
    rb_cFastXmlReader = rb_define_class("FastXmlReader", rb_cObject);
    rb_define_alloc_func(rb_cFastXmlReader, reader_alloc);

    rb_define_method(rb_cFastXmlReader, "initialize", reader_initialize, 1);
    rb_define_method(rb_cFastXmlReader, "read", reader_read, 0);
    rb_define_method(rb_cFastXmlReader, "each", reader_each, 0);
    rb_define_method(rb_cFastXmlReader, "name", reader_name, 0);
    rb_define_method(rb_cFastXmlReader, "node_type", reader_node_type, 0);
    rb_define_method(rb_cFastXmlReader, "depth", reader_depth, 0);
    rb_define_method(rb_cFastXmlReader, "value", reader_value, 0);
    rb_define_method(rb_cFastXmlReader, "attribute", reader_attribute, 1);
    rb_define_method(rb_cFastXmlReader, "empty_element?", reader_empty_element_p, 0);
    rb_define_method(rb_cFastXmlReader, "self_closing?", reader_empty_element_p, 0);
    rb_define_method(rb_cFastXmlReader, "close", reader_close, 0);

    rb_define_const(rb_cFastXmlReader, "TYPE_ELEMENT", INT2FIX(TYPE_ELEMENT));
    rb_define_const(rb_cFastXmlReader, "TYPE_TEXT", INT2FIX(TYPE_TEXT));
    rb_define_const(rb_cFastXmlReader, "TYPE_END_ELEMENT", INT2FIX(TYPE_END_ELEMENT));
}
