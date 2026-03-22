/* ============================================================
 * qjson.c — QJSON native C API: QJSON + interval projection
 *
 * Arena-allocated recursive descent.  Zero malloc per parse.
 * ============================================================ */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include "qjson.h"

#ifdef QJSON_USE_LIBBF
#include "libbf.h"
/* No global bf_context — allocate per-call for thread safety. */
static void *_qjson_bf_realloc(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (size == 0) { free(ptr); return NULL; }
    return realloc(ptr, size);
}
#else
#include <fenv.h>
#pragma STDC FENV_ACCESS ON
#endif

/* ── Arena ───────────────────────────────────────────────── */

void qjson_arena_init(qjson_arena *a, void *buf, size_t cap) {
    a->buf = (char *)buf;
    a->used = 0;
    a->cap = cap;
}

void qjson_arena_reset(qjson_arena *a) { a->used = 0; }

void *qjson_arena_alloc(qjson_arena *a, size_t size) {
    size = (size + 7) & ~(size_t)7; /* align to 8 */
    if (a->used + size > a->cap) return NULL;
    void *p = a->buf + a->used;
    a->used += size;
    return p;
}

static char *arena_strdup(qjson_arena *a, const char *s, int len) {
    char *p = qjson_arena_alloc(a, len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ── JS64 encode/decode ──────────────────────────────────── */

static const char js64_alpha[] = "$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";

/* Reverse lookup: ASCII code → 6-bit value (255 = invalid) */
static unsigned char js64_rev[128] = {0};
static int js64_rev_init = 0;

static void js64_init_rev(void) {
    if (js64_rev_init) return;
    memset(js64_rev, 255, sizeof(js64_rev));
    for (int i = 0; i < 64; i++) {
        js64_rev[(unsigned char)js64_alpha[i]] = (unsigned char)i;
    }
    js64_rev_init = 1;
}

int qjson_js64_decode(const char *js64, int js64_len, char *out, int out_cap) {
    js64_init_rev();

    /* Strip whitespace: count valid JS64 chars */
    /* We process in-place, skipping whitespace */
    int blob_len;
    {
        /* js64_len chars (without leading '$') produce this many bytes */
        /* First, count non-whitespace chars */
        int clean_len = 0;
        for (int i = 0; i < js64_len; i++) {
            unsigned char c = (unsigned char)js64[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            clean_len++;
        }
        blob_len = (clean_len * 3) >> 2;
    }

    if (blob_len > out_cap) return -1;

    unsigned int code = 0;
    int bits = 0;
    int byte_idx = 0;

    for (int i = 0; i < js64_len; i++) {
        unsigned char c = (unsigned char)js64[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c >= 128 || js64_rev[c] == 255) return -1;
        unsigned int v = js64_rev[c];
        code = code | (v << bits);
        bits += 6;

        if (bits >= 8) {
            if (byte_idx < blob_len) {
                out[byte_idx] = (char)(code & 0xFF);
            }
            code = code >> 8;
            bits -= 8;
            byte_idx++;
        }
    }

    return blob_len;
}

int qjson_js64_encode(const char *data, int data_len, char *out, int out_cap) {
    /* Output length (without leading '$'): js64len = ((data_len * 4 + 2) / 3) */
    int js64len = ((data_len * 4 + 2) / 3);
    if (js64len > out_cap) return -1;

    /* The full JS64 encoding produces js64len+1 chars (including leading '$').
       We produce all js64len+1 chars internally but skip the first one ('$'). */
    unsigned int code = 0;
    int bits = 6; /* start with 6 zero bits (the implicit '$') */
    int byte_idx = 0;
    int out_idx = 0;

    for (int i = 0; i <= js64len; i++) {
        char ch = js64_alpha[code & 0x3F];
        if (i > 0) { /* skip the leading '$' */
            if (out_idx < out_cap) out[out_idx] = ch;
            out_idx++;
        }
        code = code >> 6;
        bits -= 6;
        if (bits < 6 || i == js64len) {
            if (byte_idx < data_len) {
                code = code | ((unsigned int)(unsigned char)data[byte_idx] << bits);
                bits += 8;
                byte_idx++;
            }
        }
    }

    return js64len;
}

/* ── Parser state ────────────────────────────────────────── */

typedef struct {
    const char *s;
    int         pos;
    int         len;
    qjson_arena   *arena;
} pstate;

static char peek(pstate *p) { return p->pos < p->len ? p->s[p->pos] : 0; }

static void skip_ws(pstate *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p->pos++; continue; }
        if (c == '/' && p->pos + 1 < p->len) {
            if (p->s[p->pos + 1] == '/') {
                p->pos += 2;
                while (p->pos < p->len && p->s[p->pos] != '\n') p->pos++;
                continue;
            }
            if (p->s[p->pos + 1] == '*') {
                p->pos += 2;
                int depth = 1;
                while (p->pos + 1 < p->len && depth > 0) {
                    if (p->s[p->pos] == '/' && p->s[p->pos+1] == '*') { depth++; p->pos += 2; }
                    else if (p->s[p->pos] == '*' && p->s[p->pos+1] == '/') { depth--; p->pos += 2; }
                    else p->pos++;
                }
                continue;
            }
        }
        break;
    }
}

static qjson_val *parse_value(pstate *p);
static qjson_val *parse_unbound(pstate *p);
static qjson_val *parse_string_val(pstate *p);

static qjson_val *make_val(pstate *p, qjson_type t) {
    qjson_val *v = qjson_arena_alloc(p->arena, sizeof(qjson_val));
    if (v) { memset(v, 0, sizeof(*v)); v->type = t; }
    return v;
}

/* ── String parsing ──────────────────────────────────────── */

static qjson_val *parse_string_val(pstate *p) {
    p->pos++; /* skip opening " */
    /* First pass: compute length */
    int start = p->pos, escaped_len = 0;
    while (p->pos < p->len && p->s[p->pos] != '"') {
        if (p->s[p->pos] == '\\') { p->pos++; }
        p->pos++;
        escaped_len++;
    }
    if (p->pos >= p->len) return NULL;
    int raw_end = p->pos;
    p->pos++; /* skip closing " */

    /* Second pass: unescape */
    char *buf = qjson_arena_alloc(p->arena, escaped_len + 1);
    if (!buf) return NULL;
    int out = 0, i = start;
    while (i < raw_end) {
        if (p->s[i] == '\\') {
            i++;
            switch (p->s[i]) {
                case '"':  buf[out++] = '"'; break;
                case '\\': buf[out++] = '\\'; break;
                case '/':  buf[out++] = '/'; break;
                case 'b':  buf[out++] = '\b'; break;
                case 'f':  buf[out++] = '\f'; break;
                case 'n':  buf[out++] = '\n'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'u':  /* simplified: ASCII only */
                    buf[out++] = '?'; i += 4; break;
                default:   buf[out++] = p->s[i]; break;
            }
        } else {
            buf[out++] = p->s[i];
        }
        i++;
    }
    buf[out] = '\0';

    qjson_val *v = make_val(p, QJSON_STRING);
    if (v) { v->str.s = buf; v->str.len = out; }
    return v;
}

/* Parse bare identifier (unquoted key) */
static char *parse_ident(pstate *p, int *out_len) {
    int start = p->pos;
    char c = peek(p);
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'))
        return NULL;
    p->pos++;
    while (p->pos < p->len) {
        c = p->s[p->pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '$') p->pos++;
        else break;
    }
    int len = p->pos - start;
    *out_len = len;
    return arena_strdup(p->arena, p->s + start, len);
}

/* ── Blob parsing (0j prefix → JS64) ─────────────────────── */

static int is_js64_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '$' || c == '_';
}

static qjson_val *parse_blob(pstate *p) {
    /* p->pos is right after the '0j' or '0J' prefix */
    js64_init_rev();

    /* Collect JS64 characters (allow embedded whitespace) */
    int start = p->pos;
    int char_count = 0;
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (is_js64_char(c)) { char_count++; p->pos++; }
        else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p->pos++; }
        else break;
    }

    /* Decode: char_count JS64 chars → (char_count * 3) >> 2 bytes */
    int blob_len = (char_count * 3) >> 2;
    char *data = qjson_arena_alloc(p->arena, blob_len > 0 ? blob_len : 1);
    if (!data) return NULL;

    int js64_span = p->pos - start;
    int decoded = qjson_js64_decode(p->s + start, js64_span, data, blob_len);
    if (decoded < 0) return NULL;

    qjson_val *v = make_val(p, QJSON_BLOB);
    if (v) { v->blob.data = data; v->blob.len = decoded; }
    return v;
}

/* ── Number parsing ──────────────────────────────────────── */

static qjson_val *parse_number(pstate *p) {
    int start = p->pos;
    if (peek(p) == '-') p->pos++;

    /* Check for 0j / 0J blob prefix */
    if (p->pos < p->len && p->s[p->pos] == '0' &&
        p->pos + 1 < p->len && (p->s[p->pos + 1] == 'j' || p->s[p->pos + 1] == 'J')) {
        p->pos += 2; /* skip '0j' */
        return parse_blob(p);
    }

    while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    int is_float = 0;
    if (p->pos < p->len && p->s[p->pos] == '.') {
        is_float = 1; p->pos++;
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        is_float = 1; p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    int raw_len = p->pos - start;
    char *raw = arena_strdup(p->arena, p->s + start, raw_len);

    /* Check suffix */
    char suffix = (p->pos < p->len) ? p->s[p->pos] : 0;
    if (suffix == 'N' || suffix == 'n') {
        p->pos++;
        qjson_val *v = make_val(p, QJSON_BIGINT);
        if (v) { v->str.s = raw; v->str.len = raw_len; }
        return v;
    }
    if (suffix == 'M' || suffix == 'm') {
        p->pos++;
        qjson_val *v = make_val(p, QJSON_BIGDEC);
        if (v) { v->str.s = raw; v->str.len = raw_len; }
        return v;
    }
    if (suffix == 'L' || suffix == 'l') {
        p->pos++;
        qjson_val *v = make_val(p, QJSON_BIGFLOAT);
        if (v) { v->str.s = raw; v->str.len = raw_len; }
        return v;
    }

    qjson_val *v = make_val(p, QJSON_NUM);
    if (v) v->num = atof(raw);
    (void)is_float;
    return v;
}

/* ── Array parsing ───────────────────────────────────────── */

static qjson_val *parse_array(pstate *p) {
    p->pos++; /* [ */
    skip_ws(p);

    /* Collect into temp array (max 256 items, then grow) */
    qjson_val *items[256];
    int count = 0, cap = 256;
    qjson_val **heap_items = NULL;
    qjson_val **cur = items;

    if (peek(p) == ']') { p->pos++; goto done; }
    for (;;) {
        qjson_val *item = parse_value(p);
        if (!item) return NULL;
        if (count >= cap) {
            /* Overflow stack — shouldn't happen for typical messages */
            return NULL;
        }
        cur[count++] = item;
        skip_ws(p);
        if (peek(p) == ']') { p->pos++; break; }
        if (peek(p) != ',') return NULL;
        p->pos++;
        skip_ws(p);
        if (peek(p) == ']') { p->pos++; break; } /* trailing comma */
    }

done:;
    qjson_val *v = make_val(p, QJSON_ARRAY);
    if (!v) return NULL;
    v->arr.count = count;
    v->arr.items = qjson_arena_alloc(p->arena, count * sizeof(qjson_val *));
    if (v->arr.items) memcpy(v->arr.items, cur, count * sizeof(qjson_val *));
    (void)heap_items;
    return v;
}

/* ── Object parsing ──────────────────────────────────────── */

static qjson_val *parse_object(pstate *p) {
    p->pos++; /* { */
    skip_ws(p);

    qjson_kv pairs[128];
    int count = 0;

    if (peek(p) == '}') { p->pos++; goto done; }
    for (;;) {
        skip_ws(p);
        /* Key: quoted string or bare identifier */
        const char *key; int key_len;
        if (peek(p) == '"') {
            qjson_val *ks = parse_string_val(p);
            if (!ks) return NULL;
            key = ks->str.s; key_len = ks->str.len;
        } else {
            key = parse_ident(p, &key_len);
            if (!key) return NULL;
        }
        skip_ws(p);
        if (peek(p) != ':') return NULL;
        p->pos++;
        qjson_val *val = parse_value(p);
        if (!val) return NULL;
        if (count < 128) {
            pairs[count].key = key;
            pairs[count].key_len = key_len;
            pairs[count].val = val;
            count++;
        }
        skip_ws(p);
        if (peek(p) == '}') { p->pos++; break; }
        if (peek(p) != ',') return NULL;
        p->pos++;
        skip_ws(p);
        if (peek(p) == '}') { p->pos++; break; } /* trailing comma */
    }

done:;
    qjson_val *v = make_val(p, QJSON_OBJECT);
    if (!v) return NULL;
    v->obj.count = count;
    v->obj.pairs = qjson_arena_alloc(p->arena, count * sizeof(qjson_kv));
    if (v->obj.pairs) memcpy(v->obj.pairs, pairs, count * sizeof(qjson_kv));
    return v;
}

/* ── Unbound parsing ─────────────────────────────────────── */

static qjson_val *parse_unbound(pstate *p) {
    p->pos++; /* skip '?' */
    const char *name; int name_len;
    if (p->pos < p->len && p->s[p->pos] == '"') {
        /* Quoted name: ?"Bob's Last Memo" */
        qjson_val *sv = parse_string_val(p);
        if (!sv) return NULL;
        name = sv->str.s;
        name_len = sv->str.len;
    } else {
        /* Bare identifier or anonymous */
        int start = p->pos;
        while (p->pos < p->len) {
            char c = p->s[p->pos];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') p->pos++;
            else break;
        }
        if (p->pos == start) {
            /* Just '?' alone → anonymous "_" */
            name = arena_strdup(p->arena, "_", 1);
            name_len = 1;
        } else {
            name_len = p->pos - start;
            name = arena_strdup(p->arena, p->s + start, name_len);
        }
    }
    if (!name) return NULL;
    qjson_val *v = make_val(p, QJSON_UNBOUND);
    if (v) { v->str.s = name; v->str.len = name_len; }
    return v;
}

/* ── Value dispatch ──────────────────────────────────────── */

static qjson_val *parse_value(pstate *p) {
    skip_ws(p);
    char c = peek(p);
    if (c == '"') return parse_string_val(p);
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == 't' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "true", 4) == 0)
        { p->pos += 4; return make_val(p, QJSON_TRUE); }
    if (c == 'f' && p->pos + 5 <= p->len && memcmp(p->s + p->pos, "false", 5) == 0)
        { p->pos += 5; return make_val(p, QJSON_FALSE); }
    if (c == 'n' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "null", 4) == 0)
        { p->pos += 4; return make_val(p, QJSON_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (c == '?') return parse_unbound(p);
    return NULL;
}

/* ── Public parse ────────────────────────────────────────── */

qjson_val *qjson_parse(qjson_arena *a, const char *text, int len) {
    pstate p = { text, 0, len, a };
    qjson_val *v = parse_value(&p);
    skip_ws(&p);
    return v;
}

/* ── Stringify ───────────────────────────────────────────── */

static int emit(char *buf, int pos, int cap, const char *s, int len) {
    if (pos + len <= cap) memcpy(buf + pos, s, len);
    return pos + len;
}

static int emit_char(char *buf, int pos, int cap, char c) {
    if (pos < cap) buf[pos] = c;
    return pos + 1;
}

static int emit_str_escaped(char *buf, int pos, int cap, const char *s, int len) {
    pos = emit_char(buf, pos, cap, '"');
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '"')       { pos = emit(buf, pos, cap, "\\\"", 2); }
        else if (c == '\\') { pos = emit(buf, pos, cap, "\\\\", 2); }
        else if (c == '\n') { pos = emit(buf, pos, cap, "\\n", 2); }
        else if (c == '\r') { pos = emit(buf, pos, cap, "\\r", 2); }
        else if (c == '\t') { pos = emit(buf, pos, cap, "\\t", 2); }
        else                { pos = emit_char(buf, pos, cap, c); }
    }
    pos = emit_char(buf, pos, cap, '"');
    return pos;
}

static int stringify_val(const qjson_val *v, char *buf, int pos, int cap) {
    if (!v) return emit(buf, pos, cap, "null", 4);
    switch (v->type) {
    case QJSON_NULL:  return emit(buf, pos, cap, "null", 4);
    case QJSON_TRUE:  return emit(buf, pos, cap, "true", 4);
    case QJSON_FALSE: return emit(buf, pos, cap, "false", 5);
    case QJSON_NUM: {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%.17g", v->num);
        return emit(buf, pos, cap, tmp, n);
    }
    case QJSON_BIGINT:
        pos = emit(buf, pos, cap, v->str.s, v->str.len);
        return emit_char(buf, pos, cap, 'N');
    case QJSON_BIGDEC:
        pos = emit(buf, pos, cap, v->str.s, v->str.len);
        return emit_char(buf, pos, cap, 'M');
    case QJSON_BIGFLOAT:
        pos = emit(buf, pos, cap, v->str.s, v->str.len);
        return emit_char(buf, pos, cap, 'L');
    case QJSON_UNBOUND: {
        pos = emit_char(buf, pos, cap, '?');
        /* Check if name needs quoting (not a simple identifier) */
        int needs_quote = 0;
        if (v->str.len == 0) needs_quote = 0; /* empty → just ? */
        else {
            char c0 = v->str.s[0];
            if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_'))
                needs_quote = 1;
            if (!needs_quote) {
                for (int i = 1; i < v->str.len; i++) {
                    char ci = v->str.s[i];
                    if (!((ci >= 'a' && ci <= 'z') || (ci >= 'A' && ci <= 'Z') ||
                          (ci >= '0' && ci <= '9') || ci == '_'))
                        { needs_quote = 1; break; }
                }
            }
        }
        if (needs_quote)
            return emit_str_escaped(buf, pos, cap, v->str.s, v->str.len);
        return emit(buf, pos, cap, v->str.s, v->str.len);
    }
    case QJSON_BLOB: {
        pos = emit(buf, pos, cap, "0j", 2);
        /* Compute JS64 output length: ((data_len * 4 + 2) / 3) */
        int enc_len = ((v->blob.len * 4 + 2) / 3);
        if (pos + enc_len <= cap) {
            qjson_js64_encode(v->blob.data, v->blob.len, buf + pos, enc_len);
        }
        return pos + enc_len;
    }
    case QJSON_STRING:
        return emit_str_escaped(buf, pos, cap, v->str.s, v->str.len);
    case QJSON_ARRAY:
        pos = emit_char(buf, pos, cap, '[');
        for (int i = 0; i < v->arr.count; i++) {
            if (i > 0) pos = emit_char(buf, pos, cap, ',');
            pos = stringify_val(v->arr.items[i], buf, pos, cap);
        }
        return emit_char(buf, pos, cap, ']');
    case QJSON_OBJECT:
        pos = emit_char(buf, pos, cap, '{');
        for (int i = 0; i < v->obj.count; i++) {
            if (i > 0) pos = emit_char(buf, pos, cap, ',');
            pos = emit_str_escaped(buf, pos, cap, v->obj.pairs[i].key, v->obj.pairs[i].key_len);
            pos = emit_char(buf, pos, cap, ':');
            pos = stringify_val(v->obj.pairs[i].val, buf, pos, cap);
        }
        return emit_char(buf, pos, cap, '}');
    }
    return pos;
}

int qjson_stringify(const qjson_val *v, char *buf, int cap) {
    int n = stringify_val(v, buf, 0, cap > 0 ? cap - 1 : 0);
    if (cap > 0) buf[n < cap ? n : cap - 1] = '\0';
    return n;
}

/* ── Object key lookup ───────────────────────────────────── */

qjson_val *qjson_obj_get(const qjson_val *v, const char *key) {
    if (!v || v->type != QJSON_OBJECT) return NULL;
    int klen = strlen(key);
    for (int i = 0; i < v->obj.count; i++) {
        if (v->obj.pairs[i].key_len == klen &&
            memcmp(v->obj.pairs[i].key, key, klen) == 0)
            return v->obj.pairs[i].val;
    }
    return NULL;
}

/* ── Interval projection ────────────────────────────────── */

#ifdef QJSON_USE_LIBBF

void qjson_project(const char *raw, int len, double *lo, double *hi) {
    char buf[320];
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, raw, len);
    buf[len] = '\0';

    bf_context_t ctx;
    bf_context_init(&ctx, _qjson_bf_realloc, NULL);
    bf_t val;
    bf_init(&ctx, &val);
    bf_atof(&val, buf, NULL, 10, BF_PREC_INF, BF_RNDN);
    bf_get_float64(&val, lo, BF_RNDD);
    bf_get_float64(&val, hi, BF_RNDU);
    bf_delete(&val);
    bf_context_end(&ctx);
}

#else /* fesetround + strtod fallback */

void qjson_project(const char *raw, int len, double *lo, double *hi) {
    char buf[320];
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, raw, len);
    buf[len] = '\0';

    int saved = fegetround();

    fesetround(FE_DOWNWARD);
    volatile double vlo = strtod(buf, NULL);

    fesetround(FE_UPWARD);
    volatile double vhi = strtod(buf, NULL);

    fesetround(saved);
    *lo = vlo;
    *hi = vhi;
}

#endif

void qjson_val_project(const qjson_val *v, double *lo, double *hi) {
    if (!v) { *lo = *hi = 0; return; }
    switch (v->type) {
    case QJSON_NUM:
        *lo = *hi = v->num;
        return;
    case QJSON_BIGINT:
    case QJSON_BIGDEC:
    case QJSON_BIGFLOAT:
        qjson_project(v->str.s, v->str.len, lo, hi);
        return;
    case QJSON_UNBOUND:
        *lo = -INFINITY;
        *hi = INFINITY;
        return;
    case QJSON_BLOB:
        *lo = *hi = 0;
        return;
    default:
        *lo = *hi = 0;
        return;
    }
}

/* ── Decimal string comparison ──────────────────────────── */

#ifdef QJSON_USE_LIBBF

int qjson_decimal_cmp(const char *a, int a_len, const char *b, int b_len) {
    char ab[320], bb[320];
    if (a_len >= (int)sizeof(ab)) a_len = (int)sizeof(ab) - 1;
    if (b_len >= (int)sizeof(bb)) b_len = (int)sizeof(bb) - 1;
    memcpy(ab, a, a_len); ab[a_len] = '\0';
    memcpy(bb, b, b_len); bb[b_len] = '\0';

    bf_context_t ctx;
    bf_context_init(&ctx, _qjson_bf_realloc, NULL);
    bf_t av, bv;
    bf_init(&ctx, &av);
    bf_init(&ctx, &bv);
    bf_atof(&av, ab, NULL, 10, BF_PREC_INF, BF_RNDN);
    bf_atof(&bv, bb, NULL, 10, BF_PREC_INF, BF_RNDN);
    int r = bf_cmp(&av, &bv);
    bf_delete(&av);
    bf_delete(&bv);
    bf_context_end(&ctx);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

#else /* string-based fallback */

static int abs_decimal_cmp(const char *a, int al, const char *b, int bl) {
    /* Find decimal points */
    int a_dot = -1, b_dot = -1;
    for (int i = 0; i < al; i++) if (a[i] == '.') { a_dot = i; break; }
    for (int i = 0; i < bl; i++) if (b[i] == '.') { b_dot = i; break; }

    int a_int_len = a_dot >= 0 ? a_dot : al;
    int b_int_len = b_dot >= 0 ? b_dot : bl;

    /* Skip leading zeros */
    int ai = 0, bi = 0;
    while (ai < a_int_len && a[ai] == '0') ai++;
    while (bi < b_int_len && b[bi] == '0') bi++;

    int a_sig = a_int_len - ai;
    int b_sig = b_int_len - bi;

    /* Compare integer part length (more digits = larger) */
    if (a_sig != b_sig) return a_sig > b_sig ? 1 : -1;

    /* Compare integer part digits */
    for (int i = 0; i < a_sig; i++) {
        if (a[ai + i] != b[bi + i])
            return a[ai + i] > b[bi + i] ? 1 : -1;
    }

    /* Integer parts equal — compare fractional digits */
    const char *af = a_dot >= 0 ? a + a_dot + 1 : "";
    int af_len = a_dot >= 0 ? al - a_dot - 1 : 0;
    const char *bf = b_dot >= 0 ? b + b_dot + 1 : "";
    int bf_len = b_dot >= 0 ? bl - b_dot - 1 : 0;

    int max_frac = af_len > bf_len ? af_len : bf_len;
    for (int i = 0; i < max_frac; i++) {
        char ac = i < af_len ? af[i] : '0';
        char bc = i < bf_len ? bf[i] : '0';
        if (ac != bc) return ac > bc ? 1 : -1;
    }

    return 0;
}

int qjson_decimal_cmp(const char *a, int a_len, const char *b, int b_len) {
    int a_neg = (a_len > 0 && a[0] == '-');
    int b_neg = (b_len > 0 && b[0] == '-');
    if (a_neg && !b_neg) return -1;
    if (!a_neg && b_neg) return 1;

    int cmp = abs_decimal_cmp(
        a + a_neg, a_len - a_neg,
        b + b_neg, b_len - b_neg
    );
    return a_neg ? -cmp : cmp;
}

#endif

/* ── Interval comparison ────────────────────────────────── */

#ifdef QJSON_USE_LIBBF
/* Compare exact double value against decimal string using libbf. */
static int _cmp_double_vs_str(double dval, const char *str, int str_len) {
    char buf[320];
    if (str_len >= (int)sizeof(buf)) str_len = (int)sizeof(buf) - 1;
    memcpy(buf, str, str_len); buf[str_len] = '\0';

    bf_context_t ctx;
    bf_context_init(&ctx, _qjson_bf_realloc, NULL);
    bf_t a, b;
    bf_init(&ctx, &a);
    bf_init(&ctx, &b);
    bf_set_float64(&a, dval);
    bf_atof(&b, buf, NULL, 10, BF_PREC_INF, BF_RNDN);
    int r = bf_cmp(&a, &b);
    bf_delete(&a);
    bf_delete(&b);
    bf_context_end(&ctx);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
#else
/* Fallback: convert double to decimal string, compare strings. */
static int _cmp_double_vs_str(double dval, const char *str, int str_len) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%.21g", dval);
    return -abs_decimal_cmp(str, str_len, buf, n);
}
#endif

int qjson_cmp(int a_type, double a_lo, const char *a_str, int a_str_len, double a_hi,
              int b_type, double b_lo, const char *b_str, int b_str_len, double b_hi)
{
    /* Unbound: compare names if both unbound, otherwise unbound matches all */
    if (a_type == QJSON_UNBOUND || b_type == QJSON_UNBOUND) {
        if (a_type == QJSON_UNBOUND && b_type == QJSON_UNBOUND) {
            if (!a_str && !b_str) return 0;
            if (!a_str) return -1;
            if (!b_str) return 1;
            int min = a_str_len < b_str_len ? a_str_len : b_str_len;
            int r = memcmp(a_str, b_str, min);
            if (r != 0) return r < 0 ? -1 : 1;
            return a_str_len < b_str_len ? -1 : a_str_len > b_str_len ? 1 : 0;
        }
        return 0; /* unbound matches any concrete value */
    }

    /* Fast interval checks */
    if (a_hi < b_lo) return -1;                     /* a definitely < b */
    if (a_lo > b_hi) return  1;                     /* a definitely > b */
    if (a_lo == a_hi && b_lo == b_hi) return 0;     /* both exact doubles */

    /* Overlap zone: need exact comparison */
    if (a_lo == a_hi) {
        /* a is exact double, b has decimal str */
        return _cmp_double_vs_str(a_lo, b_str, b_str_len);
    }
    if (b_lo == b_hi) {
        /* b is exact double, a has decimal str */
        return -_cmp_double_vs_str(b_lo, a_str, a_str_len);
    }
    /* Both have decimal strings */
    return qjson_decimal_cmp(a_str, a_str_len, b_str, b_str_len);
}
