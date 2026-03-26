/* ============================================================
 * qjson.h — QJSON native C API: QJSON + interval projection
 *
 * Arena-allocated: zero malloc per parse.  Reset between messages.
 *
 *   qjson_arena a; qjson_arena_init(&a, buf, sizeof(buf));
 *   qjson_val *v = qjson_parse(&a, text, len);
 *   char out[1024]; int n = qjson_stringify(v, out, sizeof(out));
 *   qjson_arena_reset(&a);  // ready for next message
 * ============================================================ */

#ifndef QJSON_H
#define QJSON_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    QJSON_NULL, QJSON_TRUE, QJSON_FALSE,
    QJSON_NUM,          /* double */
    QJSON_BIGINT,       /* raw string, suffix N */
    QJSON_BIGDEC,       /* raw string, suffix M */
    QJSON_BIGFLOAT,     /* raw string, suffix L */
    QJSON_BLOB,         /* binary data, 0j prefix (JS64 encoded) */
    QJSON_STRING,
    QJSON_ARRAY,
    QJSON_OBJECT,
    QJSON_UNBOUND       /* ?name — unbound variable, str holds name */
} qjson_type;

typedef struct qjson_val qjson_val;

typedef struct {
    qjson_val  *key;
    qjson_val  *val;
} qjson_kv;

struct qjson_val {
    qjson_type type;
    union {
        double  num;
        struct { const char *s; int len; } str;   /* string/bignum raw text */
        struct { const char *data; int len; } blob; /* binary blob data */
        struct { qjson_val **items; int count; } arr;
        struct { qjson_kv  *pairs; int count; } obj;
    };
};

/* ── Arena ───────────────────────────────────────────────── */

typedef struct {
    char   *buf;
    size_t  used;
    size_t  cap;
} qjson_arena;

void  qjson_arena_init(qjson_arena *a, void *buf, size_t cap);
void  qjson_arena_reset(qjson_arena *a);
void *qjson_arena_alloc(qjson_arena *a, size_t size);

/* ── Parse ───────────────────────────────────────────────── */

/* Parse QJSON text.  Returns root value or NULL on error.
   All memory from arena — no malloc. */
qjson_val *qjson_parse(qjson_arena *a, const char *text, int len);

/* ── Stringify ───────────────────────────────────────────── */

/* Write QJSON to buffer.  Returns bytes written (excluding NUL).
   BigInt→N, BigDecimal→M, BigFloat→L. */
int qjson_stringify(const qjson_val *v, char *buf, int cap);

/* ── Accessors ───────────────────────────────────────────── */

static inline qjson_type  qjson_type_of(const qjson_val *v) { return v ? v->type : QJSON_NULL; }
static inline double   qjson_num(const qjson_val *v)     { return v && v->type == QJSON_NUM ? v->num : 0; }
static inline const char *qjson_str(const qjson_val *v)  { return v && v->type == QJSON_STRING ? v->str.s : NULL; }
static inline int      qjson_str_len(const qjson_val *v) { return v && v->type == QJSON_STRING ? v->str.len : 0; }
static inline int      qjson_arr_len(const qjson_val *v) { return v && v->type == QJSON_ARRAY ? v->arr.count : 0; }
static inline qjson_val  *qjson_arr_get(const qjson_val *v, int i) {
    return (v && v->type == QJSON_ARRAY && i >= 0 && i < v->arr.count) ? v->arr.items[i] : NULL;
}
static inline int      qjson_obj_len(const qjson_val *v) { return v && v->type == QJSON_OBJECT ? v->obj.count : 0; }
qjson_val *qjson_obj_get(const qjson_val *v, const char *key);
static inline const char *qjson_unbound_name(const qjson_val *v) { return v && v->type == QJSON_UNBOUND ? v->str.s : NULL; }
static inline int      qjson_unbound_name_len(const qjson_val *v) { return v && v->type == QJSON_UNBOUND ? v->str.len : 0; }

/* Returns 1 if value is valid JSON (no BigInt/BigDecimal/BigFloat/Blob/Unbound).
   Recursive: arrays and objects must contain only JSON types. */
int qjson_is_json(const qjson_val *v);

/* Returns 1 if value contains no Unbound variables.
   Recursive: arrays and objects must be fully bound. */
int qjson_is_bound(const qjson_val *v);

/* ── Interval projection ────────────────────────────────── */

/* Project decimal string → [lo, hi] IEEE double interval.
   lo = largest double ≤ exact value  (ieee_double_round_down)
   hi = smallest double ≥ exact value (ieee_double_round_up)

   Exact doubles: lo == hi (point interval).
   Non-exact:     nextafter(lo, +inf) == hi (1-ULP bracket).
   Overflow:      lo = DBL_MAX, hi = +inf  (or symmetric for negative).

   With -DQJSON_USE_LIBBF: uses libbf (from QuickJS) for exact
   directed rounding.  Without: uses fesetround() + strtod().
   Both produce identical results. */
void qjson_project(const char *raw, int len, double *lo, double *hi);

/* Project a parsed qjson_val to its interval.
   QJSON_NUM:      lo == hi == val->num (plain doubles are exact).
   QJSON_BIGINT/QJSON_BIGDEC/QJSON_BIGFLOAT: directed rounding on raw string.
   QJSON_UNBOUND:  lo = -INFINITY, hi = +INFINITY (matches everything).
   Other types: lo = hi = 0. */
void qjson_val_project(const qjson_val *v, double *lo, double *hi);

/* Compare two decimal strings numerically.
   Returns -1 (a < b), 0 (a == b), 1 (a > b).
   With -DQJSON_USE_LIBBF: exact arbitrary-precision comparison.
   Without: string-based decimal comparison (no scientific notation). */
int qjson_decimal_cmp(const char *a, int a_len, const char *b, int b_len);

/* Compare two projected values.  Six operator-specific functions,
   each returning 0 (false) or 1 (true).

   Uses intervals for fast accept/reject, falls through to
   exact comparison only in the overlap zone (~0.001%).

   type is the qjson_type enum value — needed to resolve exact values
   when one side is an exact double (str=NULL) and the other is not,
   and to handle unbound variables.

   Unbound semantics:
     Same-name unbounds (?X vs ?X): behave as equal.
       eq→1, ne→0, le→1, ge→1, lt→0, gt→0
     Different-name unbounds (?X vs ?Y) or unbound vs concrete:
       all six return 1 (unknown — could satisfy any relation).

   See docs/qjson.md for the full comparison spec. */

#define QJSON_CMP_ARGS \
    int a_type, double a_lo, const char *a_str, int a_str_len, double a_hi, \
    int b_type, double b_lo, const char *b_str, int b_str_len, double b_hi

int qjson_cmp_lt(QJSON_CMP_ARGS);   /* a <  b */
int qjson_cmp_le(QJSON_CMP_ARGS);   /* a <= b */
int qjson_cmp_eq(QJSON_CMP_ARGS);   /* a == b */
int qjson_cmp_ne(QJSON_CMP_ARGS);   /* a != b */
int qjson_cmp_gt(QJSON_CMP_ARGS);   /* a >  b */
int qjson_cmp_ge(QJSON_CMP_ARGS);   /* a >= b */

/* ── JS64 encode/decode ─────────────────────────────────── */

/* Decode JS64 characters to raw bytes.
   Input does NOT include the leading '$' (caller strips it / supplies body only).
   Internally prepends '$' (6 zero bits) for decoding.
   Returns number of decoded bytes, or -1 on error.
   Whitespace in input is skipped. */
int qjson_js64_decode(const char *js64, int js64_len, char *out, int out_cap);

/* Encode raw bytes to JS64 characters.
   Output does NOT include the leading '$' (caller prepends '0j' for QJSON).
   Returns number of JS64 characters written, or -1 on error. */
int qjson_js64_encode(const char *data, int data_len, char *out, int out_cap);

#endif
