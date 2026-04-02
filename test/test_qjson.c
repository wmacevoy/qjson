/* ============================================================
 * test_qjson.c — Tests + benchmarks for native C QJSON
 *
 * gcc -O2 -frounding-math -o test_qjson qjson.c test_qjson.c -lm && ./test_qjson
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include "../native/qjson.h"

static int pass = 0, fail = 0;

#define TEST(name, cond) do { \
    if (cond) { pass++; printf("  ok  %s\n", name); } \
    else { fail++; printf("  FAIL %s  [line %d]\n", name, __LINE__); } \
} while(0)

/* -- Correctness tests ---------------------------------------- */

static char arena_buf[65536];

static void test_parse_basic(void) {
    printf("=== Parse basics ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    qjson_val *v = qjson_parse(&a, "42", 2);
    TEST("parse integer", v && v->type == QJSON_NUMBER && v->num == 42);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "-3.14", 5);
    TEST("parse float", v && v->type == QJSON_NUMBER && v->num < -3.13 && v->num > -3.15);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "\"hello\"", 7);
    TEST("parse string", v && v->type == QJSON_STRING && strcmp(v->str.s, "hello") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "true", 4);
    TEST("parse true", v && v->type == QJSON_TRUE);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "false", 5);
    TEST("parse false", v && v->type == QJSON_FALSE);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "null", 4);
    TEST("parse null", v && v->type == QJSON_NULL);
}

static void test_parse_compound(void) {
    printf("\n=== Parse compound ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    qjson_val *v = qjson_parse(&a, "[1,2,3]", 7);
    TEST("parse array", v && v->type == QJSON_ARRAY && v->arr.count == 3);
    TEST("array[0]", qjson_arr_get(v, 0)->num == 1);
    TEST("array[2]", qjson_arr_get(v, 2)->num == 3);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{\"a\":1,\"b\":2}", 13);
    TEST("parse object", v && v->type == QJSON_OBJECT && v->obj.count == 2);
    TEST("obj.a", qjson_obj_get(v, "a") && qjson_obj_get(v, "a")->num == 1);
    TEST("obj.b", qjson_obj_get(v, "b") && qjson_obj_get(v, "b")->num == 2);
}

static void test_parse_bignum(void) {
    printf("\n=== Parse bignums ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    qjson_val *v = qjson_parse(&a, "42N", 3);
    TEST("BigInt N", v && v->type == QJSON_BIGINT && strcmp(v->str.s, "42") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "42n", 3);
    TEST("BigInt n (lc)", v && v->type == QJSON_BIGINT);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14M", 5);
    TEST("BigDecimal M", v && v->type == QJSON_BIGDECIMAL && strcmp(v->str.s, "3.14") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14m", 5);
    TEST("BigDecimal m (lc)", v && v->type == QJSON_BIGDECIMAL);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14L", 5);
    TEST("BigFloat L", v && v->type == QJSON_BIGFLOAT && strcmp(v->str.s, "3.14") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14l", 5);
    TEST("BigFloat l (lc)", v && v->type == QJSON_BIGFLOAT);
}

static void test_parse_comments(void) {
    printf("\n=== Parse comments ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    const char *s = "// line comment\n42";
    qjson_val *v = qjson_parse(&a, s, strlen(s));
    TEST("line comment", v && v->type == QJSON_NUMBER && v->num == 42);

    qjson_arena_reset(&a);
    s = "/* block */ 42";
    v = qjson_parse(&a, s, strlen(s));
    TEST("block comment", v && v->type == QJSON_NUMBER && v->num == 42);

    qjson_arena_reset(&a);
    s = "/* outer /* inner */ still */ 42";
    v = qjson_parse(&a, s, strlen(s));
    TEST("nested block comment", v && v->type == QJSON_NUMBER && v->num == 42);
}

static void test_parse_human(void) {
    printf("\n=== Parse human-friendly ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    const char *s = "[1, 2, 3,]";
    qjson_val *v = qjson_parse(&a, s, strlen(s));
    TEST("trailing comma array", v && v->type == QJSON_ARRAY && v->arr.count == 3);

    qjson_arena_reset(&a);
    s = "{\"a\": 1, \"b\": 2,}";
    v = qjson_parse(&a, s, strlen(s));
    TEST("trailing comma object", v && v->type == QJSON_OBJECT && v->obj.count == 2);

    qjson_arena_reset(&a);
    s = "{name: \"alice\", age: 30}";
    v = qjson_parse(&a, s, strlen(s));
    TEST("unquoted keys", v && v->type == QJSON_OBJECT && v->obj.count == 2);
    TEST("unquoted key value", qjson_obj_get(v, "name") && strcmp(qjson_str(qjson_obj_get(v, "name")), "alice") == 0);
}

static void test_stringify(void) {
    printf("\n=== Stringify ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    char out[1024];

    /* Round-trip: parse then stringify */
    const char *s = "{\"t\":\"c\",\"f\":\"temp\",\"a\":[{\"t\":\"a\",\"n\":\"kitchen\"},{\"t\":\"n\",\"v\":22}]}";
    qjson_val *v = qjson_parse(&a, s, strlen(s));
    TEST("parse term json", v != NULL);
    int n = qjson_stringify(v, out, sizeof(out));
    TEST("stringify term", n > 0 && strcmp(out, s) == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "42N", 3);
    n = qjson_stringify(v, out, sizeof(out));
    TEST("stringify BigInt -> N", strcmp(out, "42N") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14M", 5);
    n = qjson_stringify(v, out, sizeof(out));
    TEST("stringify BigDec -> M", strcmp(out, "3.14M") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14L", 5);
    n = qjson_stringify(v, out, sizeof(out));
    TEST("stringify BigFloat -> L", strcmp(out, "3.14L") == 0);
}

/* -- Projection tests ----------------------------------------- */

static void test_project(void) {
    printf("\n=== Projection (fesetround + strtod) ===\n");
    double lo, hi;

    /* Exact integer */
    qjson_project("42", 2, &lo, &hi);
    TEST("42 exact", lo == 42.0 && hi == 42.0);

    /* Exact decimal */
    qjson_project("67432.50", 8, &lo, &hi);
    TEST("67432.50 exact", lo == 67432.5 && hi == 67432.5);

    /* Non-exact: 0.1 (1-ULP bracket) */
    qjson_project("0.1", 3, &lo, &hi);
    TEST("0.1 bracketed", lo < hi && lo <= 0.1 && hi >= 0.1);
    TEST("0.1 tight (1-ULP)", nextafter(lo, INFINITY) == hi);

    /* Non-exact: 0.3 (rounds down, double < exact) */
    qjson_project("0.3", 3, &lo, &hi);
    TEST("0.3 bracketed", lo < hi);
    TEST("0.3 tight (1-ULP)", nextafter(lo, INFINITY) == hi);

    /* Large exact: 1e21 = 5^21 * 2^21, 5^21 < 2^53 */
    qjson_project("1000000000000000000000", 22, &lo, &hi);
    TEST("1e21 exact", lo == hi && lo == 1e21);

    /* Large non-exact: 2^53 + 1 */
    qjson_project("9007199254740993", 16, &lo, &hi);
    TEST("2^53+1 not exact", lo < hi);
    TEST("2^53+1 lo = 2^53", lo == 9007199254740992.0);
    TEST("2^53+1 hi = 2^53+2", hi == 9007199254740994.0);

    /* Very large non-exact: 1e25 (5^25 > 2^53) */
    qjson_project("10000000000000000000000000", 25, &lo, &hi);
    TEST("1e25 not exact", lo < hi);
    TEST("1e25 tight (1-ULP)", nextafter(lo, INFINITY) == hi);

    /* Overflow: 2e308 > DBL_MAX */
    qjson_project("2e308", 5, &lo, &hi);
    TEST("overflow lo = DBL_MAX", lo == DBL_MAX);
    TEST("overflow hi = +inf", hi == INFINITY);

    /* Negative overflow */
    qjson_project("-2e308", 6, &lo, &hi);
    TEST("neg overflow lo = -inf", lo == -INFINITY);
    TEST("neg overflow hi = -DBL_MAX", hi == -DBL_MAX);

    /* Underflow: 5e-325 < smallest subnormal */
    qjson_project("5e-325", 6, &lo, &hi);
    TEST("underflow lo = 0", lo == 0.0);
    TEST("underflow hi > 0", hi > 0.0 && hi <= 5e-324);

    /* Zero */
    qjson_project("0", 1, &lo, &hi);
    TEST("zero exact", lo == 0.0 && hi == 0.0);

    /* Negative non-exact */
    qjson_project("-0.1", 4, &lo, &hi);
    TEST("-0.1 bracketed", lo < hi && lo < 0 && hi < 0);
    TEST("-0.1 tight (1-ULP)", nextafter(lo, INFINITY) == hi);

    /* val_project: QJSON_NUMBER -> point interval */
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    qjson_val *v = qjson_parse(&a, "42", 2);
    qjson_val_project(v, &lo, &hi);
    TEST("val_project QJSON_NUMBER", lo == 42.0 && hi == 42.0);

    /* val_project: QJSON_BIGDECIMAL -> directed rounding */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "0.1M", 4);
    qjson_val_project(v, &lo, &hi);
    TEST("val_project 0.1M", lo < hi);
}

/* -- Decimal comparison tests --------------------------------- */

static void test_decimal_cmp(void) {
    printf("\n=== Decimal comparison ===\n");

    TEST("equal", qjson_decimal_cmp("42", 2, "42", 2) == 0);
    TEST("equal trailing zero", qjson_decimal_cmp("42.0", 4, "42", 2) == 0);
    TEST("equal leading zero", qjson_decimal_cmp("042", 3, "42", 2) == 0);
    TEST("equal decimal", qjson_decimal_cmp("67432.50", 8, "67432.5", 7) == 0);
    TEST("less", qjson_decimal_cmp("41", 2, "42", 2) == -1);
    TEST("greater", qjson_decimal_cmp("43", 2, "42", 2) == 1);
    TEST("frac less", qjson_decimal_cmp("0.1", 3, "0.2", 3) == -1);
    TEST("negative", qjson_decimal_cmp("-1", 2, "1", 1) == -1);
    TEST("both negative", qjson_decimal_cmp("-2", 2, "-1", 2) == -1);
    TEST("large equal", qjson_decimal_cmp(
        "1000000000000000000000", 22,
        "1000000000000000000000", 22) == 0);
    TEST("large less", qjson_decimal_cmp(
        "9007199254740992", 16,
        "9007199254740993", 16) == -1);
}

/* -- Interval comparison logic -------------------------------- */
/*
 * Implements the query pushdown formulas from docs/qjson.md:
 *   a <op> b = (interval_accept) OR ((interval_not_reject) AND cmp <op> 0)
 */

/* Helper: project and call a qjson_cmp_[op] function. */
typedef int (*cmp_fn)(QJSON_CMP_ARGS);

static int cmpop(const char *a, const char *b, cmp_fn fn) {
    double a_lo, a_hi, b_lo, b_hi;
    qjson_project(a, strlen(a), &a_lo, &a_hi);
    qjson_project(b, strlen(b), &b_lo, &b_hi);
    int a_type = (a_lo == a_hi) ? QJSON_NUMBER : QJSON_BIGDECIMAL;
    int b_type = (b_lo == b_hi) ? QJSON_NUMBER : QJSON_BIGDECIMAL;
    const char *a_str = (a_lo == a_hi) ? NULL : a;
    int a_sl = (a_lo == a_hi) ? 0 : (int)strlen(a);
    const char *b_str = (b_lo == b_hi) ? NULL : b;
    int b_sl = (b_lo == b_hi) ? 0 : (int)strlen(b);
    return fn(a_type, a_lo, a_str, a_sl, a_hi,
              b_type, b_lo, b_str, b_sl, b_hi);
}

#define LT(a, b)  cmpop(a, b, qjson_cmp_lt)
#define LE(a, b)  cmpop(a, b, qjson_cmp_le)
#define GT(a, b)  cmpop(a, b, qjson_cmp_gt)
#define GE(a, b)  cmpop(a, b, qjson_cmp_ge)
#define EQ(a, b)  cmpop(a, b, qjson_cmp_eq)
#define NE(a, b)  cmpop(a, b, qjson_cmp_ne)

static void test_interval_cmp(void) {
    printf("\n=== Interval comparison logic ===\n");

    /* -- Clearly separated (fast accept/reject) --------------- */
    TEST("1 < 2",        LT("1", "2"));
    TEST("2 > 1",        GT("2", "1"));
    TEST("!(2 < 1)",    !LT("2", "1"));
    TEST("!(1 > 2)",    !GT("1", "2"));
    TEST("1 <= 2",       LE("1", "2"));
    TEST("2 >= 1",       GE("2", "1"));
    TEST("1 != 2",       NE("1", "2"));
    TEST("!(1 == 2)",   !EQ("1", "2"));

    /* -- Exact doubles: point intervals ----------------------- */
    TEST("42 == 42",     EQ("42", "42"));
    TEST("42 != 43",     NE("42", "43"));
    TEST("42 < 43",      LT("42", "43"));
    TEST("!(42 < 42)",  !LT("42", "42"));
    TEST("42 <= 42",     LE("42", "42"));
    TEST("42 >= 42",     GE("42", "42"));
    TEST("!(42 > 42)",  !GT("42", "42"));

    /* -- Same value, different representation ----------------- */
    TEST("0.5 == 0.50",        EQ("0.5", "0.50"));
    TEST("42 == 42.0",         EQ("42", "42.0"));
    TEST("67432.5 == 67432.50", EQ("67432.5", "67432.50"));

    /* -- Non-exact: same double, different exact values -------- */
    /* 0.1 and 0.10000000000000000001 both round to the same  */
    /* double but are different exact values -- same interval.  */
    TEST("0.1 < 0.10000000000000000001",
        LT("0.1", "0.10000000000000000001"));
    TEST("0.1 != 0.10000000000000000001",
        NE("0.1", "0.10000000000000000001"));
    TEST("!(0.1 == 0.10000000000000000001)",
        !EQ("0.1", "0.10000000000000000001"));
    TEST("0.1 <= 0.10000000000000000001",
        LE("0.1", "0.10000000000000000001"));

    /* -- Overlapping intervals, different values -------------- */
    /* 0.1 rounds UP (interval [nextDown, 0.1])              */
    /* 0.3 rounds DOWN (interval [0.3, nextUp])              */
    TEST("0.1 < 0.3",   LT("0.1", "0.3"));
    TEST("0.3 > 0.1",   GT("0.3", "0.1"));
    TEST("0.1 != 0.3",  NE("0.1", "0.3"));

    /* -- Large integers beyond 2^53 -------------------------- */
    TEST("9007199254740992 < 9007199254740993",
        LT("9007199254740992", "9007199254740993"));
    TEST("9007199254740993 > 9007199254740992",
        GT("9007199254740993", "9007199254740992"));
    TEST("9007199254740993 == 9007199254740993",
        EQ("9007199254740993", "9007199254740993"));
    TEST("9007199254740993 != 9007199254740994",
        NE("9007199254740993", "9007199254740994"));

    /* -- Negative values ------------------------------------- */
    TEST("-1 < 1",       LT("-1", "1"));
    TEST("-2 < -1",      LT("-2", "-1"));
    TEST("-1 > -2",      GT("-1", "-2"));
    TEST("-0.1 == -0.1", EQ("-0.1", "-0.1"));
    TEST("-0.1 < 0.1",   LT("-0.1", "0.1"));

    /* -- Overflow / underflow -------------------------------- */
    TEST("1e308 < 2e308",   LT("1e308", "2e308"));
    TEST("5e-325 < 1e-323", LT("5e-325", "1e-323"));
    TEST("0 < 5e-325",      LT("0", "5e-325"));
    TEST("0 == 0",           EQ("0", "0"));

    /* -- Reflexivity: every value equals itself --------------- */
    TEST("0.1 == 0.1",      EQ("0.1", "0.1"));
    TEST("0.3 == 0.3",      EQ("0.3", "0.3"));
    TEST("!(0.1 < 0.1)",   !LT("0.1", "0.1"));
    TEST("!(0.1 > 0.1)",   !GT("0.1", "0.1"));
    TEST("0.1 <= 0.1",      LE("0.1", "0.1"));
    TEST("0.1 >= 0.1",      GE("0.1", "0.1"));

    /* -- Consistency: < and > are mirrors -------------------- */
    TEST("(a<b) == (b>a)",  LT("0.1", "0.3") == GT("0.3", "0.1"));
    TEST("(a<=b) == (b>=a)", LE("0.1", "0.3") == GE("0.3", "0.1"));
    TEST("(a==b) == (b==a)", EQ("0.1", "0.3") == EQ("0.3", "0.1"));
    TEST("(a!=b) == (b!=a)", NE("0.1", "0.3") == NE("0.3", "0.1"));

    /* -- Trichotomy: exactly one of <, ==, > is true --------- */
    {
        const char *pairs[][2] = {
            {"0.1", "0.3"}, {"42", "42"}, {"0.1", "0.1"},
            {"0.1", "0.10000000000000000001"},
            {"9007199254740992", "9007199254740993"},
            {"-0.1", "0.1"}, {"1e308", "2e308"},
        };
        for (int i = 0; i < (int)(sizeof(pairs)/sizeof(pairs[0])); i++) {
            const char *a = pairs[i][0], *b = pairs[i][1];
            int lt = LT(a, b), eq = EQ(a, b), gt = GT(a, b);
            char msg[128];
            snprintf(msg, sizeof(msg), "trichotomy: %s vs %s (%d+%d+%d=1)",
                     a, b, lt, eq, gt);
            TEST(msg, lt + eq + gt == 1);
        }
    }
}

/* -- Blob / JS64 tests ---------------------------------------- */

static void test_js64_encode_decode(void) {
    printf("\n=== JS64 encode/decode ===\n");
    char enc[64], dec[64];

    /* "Hello" = 0x48 0x65 0x6c 0x6c 0x6f */
    const char hello[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f};
    int enc_len = qjson_js64_encode(hello, 5, enc, sizeof(enc));
    TEST("js64 encode Hello len", enc_len == 7); /* ((5*4+2)/3) = 7 */

    /* Decode back */
    int dec_len = qjson_js64_decode(enc, enc_len, dec, sizeof(dec));
    TEST("js64 decode Hello len", dec_len == 5);
    TEST("js64 round-trip Hello", dec_len == 5 && memcmp(dec, hello, 5) == 0);

    /* Empty data */
    enc_len = qjson_js64_encode("", 0, enc, sizeof(enc));
    TEST("js64 encode empty", enc_len == 0);
    dec_len = qjson_js64_decode(enc, 0, dec, sizeof(dec));
    TEST("js64 decode empty", dec_len == 0);

    /* Single byte: 0xFF */
    const char ff[] = {(char)0xFF};
    enc_len = qjson_js64_encode(ff, 1, enc, sizeof(enc));
    TEST("js64 encode 0xFF len", enc_len == 2); /* ((1*4+2)/3) = 2 */
    dec_len = qjson_js64_decode(enc, enc_len, dec, sizeof(dec));
    TEST("js64 round-trip 0xFF", dec_len == 1 && (unsigned char)dec[0] == 0xFF);

    /* All zeros: 3 bytes */
    const char zeros[3] = {0, 0, 0};
    enc_len = qjson_js64_encode(zeros, 3, enc, sizeof(enc));
    TEST("js64 encode 3 zeros len", enc_len == 4); /* ((3*4+2)/3) = 4 */
    dec_len = qjson_js64_decode(enc, enc_len, dec, sizeof(dec));
    TEST("js64 round-trip 3 zeros", dec_len == 3 &&
         dec[0] == 0 && dec[1] == 0 && dec[2] == 0);
}

static void test_parse_blob(void) {
    printf("\n=== Parse blob ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    char out[256];

    /* First, encode "Hello" to know the expected JS64 body */
    const char hello[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f};
    char js64_body[16];
    int js64_len = qjson_js64_encode(hello, 5, js64_body, sizeof(js64_body));
    /* Build "0j<body>" string */
    char input[32];
    memcpy(input, "0j", 2);
    memcpy(input + 2, js64_body, js64_len);
    int input_len = 2 + js64_len;

    qjson_val *v = qjson_parse(&a, input, input_len);
    TEST("parse blob type", v && v->type == QJSON_BLOB);
    TEST("parse blob len", v && v->blob.len == 5);
    TEST("parse blob data", v && v->blob.len == 5 && memcmp(v->blob.data, hello, 5) == 0);

    /* Uppercase J */
    qjson_arena_reset(&a);
    memcpy(input, "0J", 2);
    v = qjson_parse(&a, input, input_len);
    TEST("parse blob 0J (uppercase)", v && v->type == QJSON_BLOB && v->blob.len == 5 &&
         memcmp(v->blob.data, hello, 5) == 0);

    /* Whitespace in JS64 body */
    qjson_arena_reset(&a);
    {
        char ws_input[64];
        int wi = 0;
        ws_input[wi++] = '0';
        ws_input[wi++] = 'j';
        /* Insert a space after every 3 chars */
        for (int i = 0; i < js64_len; i++) {
            ws_input[wi++] = js64_body[i];
            if (i == 2) ws_input[wi++] = ' ';
        }
        v = qjson_parse(&a, ws_input, wi);
        TEST("parse blob with whitespace", v && v->type == QJSON_BLOB && v->blob.len == 5 &&
             memcmp(v->blob.data, hello, 5) == 0);
    }

    /* Empty blob: 0j with no chars */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "0j", 2);
    TEST("parse empty blob", v && v->type == QJSON_BLOB && v->blob.len == 0);

    /* Stringify round-trip */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, input, input_len); /* the Hello blob */
    int n = qjson_stringify(v, out, sizeof(out));
    TEST("stringify blob prefix", n > 2 && out[0] == '0' && out[1] == 'j');
    /* Parse the stringified output back */
    qjson_arena_reset(&a);
    qjson_val *v2 = qjson_parse(&a, out, n);
    TEST("stringify blob round-trip type", v2 && v2->type == QJSON_BLOB);
    TEST("stringify blob round-trip data", v2 && v2->blob.len == 5 &&
         memcmp(v2->blob.data, hello, 5) == 0);

    /* Blob inside object */
    qjson_arena_reset(&a);
    {
        char obj_input[64];
        int oi = 0;
        memcpy(obj_input + oi, "{key: 0j", 8); oi += 8;
        memcpy(obj_input + oi, js64_body, js64_len); oi += js64_len;
        obj_input[oi++] = '}';
        v = qjson_parse(&a, obj_input, oi);
        TEST("blob in object", v && v->type == QJSON_OBJECT);
        qjson_val *bv = qjson_obj_get(v, "key");
        TEST("blob in object type", bv && bv->type == QJSON_BLOB);
        TEST("blob in object data", bv && bv->blob.len == 5 &&
             memcmp(bv->blob.data, hello, 5) == 0);
    }

    /* Blob projection: lo = hi = 0 */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, input, input_len);
    {
        double lo, hi;
        qjson_val_project(v, &lo, &hi);
        TEST("blob projection zero", lo == 0.0 && hi == 0.0);
    }
}

/* -- Unbound tests -------------------------------------------- */

static void test_parse_unbound(void) {
    printf("\n=== Parse unbound ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    qjson_val *v = qjson_parse(&a, "?X", 2);
    TEST("parse ?X type", v && v->type == QJSON_UNBOUND);
    TEST("parse ?X name", v && strcmp(v->str.s, "X") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "?", 1);
    TEST("parse ? anonymous", v && v->type == QJSON_UNBOUND && v->str.len == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "?_", 2);
    TEST("parse ?_ underscore", v && v->type == QJSON_UNBOUND && strcmp(v->str.s, "_") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "?myVar_1", 8);
    TEST("parse ?myVar_1", v && v->type == QJSON_UNBOUND && strcmp(v->str.s, "myVar_1") == 0);

    /* Quoted name */
    qjson_arena_reset(&a);
    const char *qs = "?\"hello world\"";
    v = qjson_parse(&a, qs, strlen(qs));
    TEST("parse ?\"hello world\"", v && v->type == QJSON_UNBOUND && strcmp(v->str.s, "hello world") == 0);

    /* In array */
    qjson_arena_reset(&a);
    const char *arr = "[?X, 42, ?Y]";
    v = qjson_parse(&a, arr, strlen(arr));
    TEST("unbound in array", v && v->type == QJSON_ARRAY && v->arr.count == 3);
    TEST("arr[0] unbound", v->arr.items[0]->type == QJSON_UNBOUND && strcmp(v->arr.items[0]->str.s, "X") == 0);
    TEST("arr[1] num", v->arr.items[1]->type == QJSON_NUMBER && v->arr.items[1]->num == 42);
    TEST("arr[2] unbound", v->arr.items[2]->type == QJSON_UNBOUND && strcmp(v->arr.items[2]->str.s, "Y") == 0);

    /* Stringify round-trip */
    qjson_arena_reset(&a);
    char out[256];
    v = qjson_parse(&a, "?X", 2);
    int n = qjson_stringify(v, out, sizeof(out));
    TEST("stringify ?X", n == 2 && strcmp(out, "?X") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "?_", 2);
    n = qjson_stringify(v, out, sizeof(out));
    TEST("stringify ?_", n == 2 && strcmp(out, "?_") == 0);

    /* Quoted round-trip */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, qs, strlen(qs));
    n = qjson_stringify(v, out, sizeof(out));
    TEST("stringify quoted", out[0] == '?' && out[1] == '"');
    /* Parse the stringified output back */
    qjson_arena_reset(&a);
    qjson_val *v2 = qjson_parse(&a, out, n);
    TEST("round-trip quoted type", v2 && v2->type == QJSON_UNBOUND);
    TEST("round-trip quoted name", v2 && strcmp(v2->str.s, "hello world") == 0);

    /* Projection: [-inf, +inf] */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "?X", 2);
    {
        double lo, hi;
        qjson_val_project(v, &lo, &hi);
        TEST("unbound projection lo=-inf", lo == -INFINITY);
        TEST("unbound projection hi=+inf", hi == INFINITY);
    }
}

/* -- Comparison fix tests ------------------------------------- */

static void test_cmp_fix(void) {
    printf("\n=== Comparison fix (type-aware) ===\n");

    /* 0.3M vs 0.3 (double): different values, should not be equal */
    double m_lo, m_hi, d_lo, d_hi;
    qjson_project("0.3", 3, &m_lo, &m_hi);
    d_lo = d_hi = 0.3; /* exact double */

    /* Old bug: this would call qjson_decimal_cmp with NULL str */
    /* 0.3M != 0.3 (double): exact 0.3 > ieee double 0.2999... */
    TEST("0.3M != 0.3", qjson_cmp_ne(QJSON_BIGDECIMAL, m_lo, "0.3", 3, m_hi,
                                      QJSON_NUMBER, d_lo, NULL, 0, d_hi));
    TEST("0.3M > 0.3",  qjson_cmp_gt(QJSON_BIGDECIMAL, m_lo, "0.3", 3, m_hi,
                                      QJSON_NUMBER, d_lo, NULL, 0, d_hi));

    /* 0.5M vs 0.5: same value, both exact */
    qjson_project("0.5", 3, &m_lo, &m_hi);
    d_lo = d_hi = 0.5;
    TEST("0.5M == 0.5", qjson_cmp_eq(QJSON_BIGDECIMAL, m_lo, NULL, 0, m_hi,
                                      QJSON_NUMBER, d_lo, NULL, 0, d_hi));

    /* ── Unbound comparison tests ─────────────────────────── */

    /* Same name: behaves as equal */
    TEST("?X eq ?X",  qjson_cmp_eq(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY));
    TEST("?X !ne ?X", !qjson_cmp_ne(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                     QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY));
    TEST("?X le ?X",  qjson_cmp_le(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY));
    TEST("?X ge ?X",  qjson_cmp_ge(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY));
    TEST("?X !lt ?X", !qjson_cmp_lt(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                     QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY));
    TEST("?X !gt ?X", !qjson_cmp_gt(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                     QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY));

    /* Different names: all operators true (unknown) */
    TEST("?X eq ?Y",  qjson_cmp_eq(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_UNBOUND, -INFINITY, "Y", 1, INFINITY));
    TEST("?X ne ?Y",  qjson_cmp_ne(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_UNBOUND, -INFINITY, "Y", 1, INFINITY));
    TEST("?X lt ?Y",  qjson_cmp_lt(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_UNBOUND, -INFINITY, "Y", 1, INFINITY));
    TEST("?X gt ?Y",  qjson_cmp_gt(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_UNBOUND, -INFINITY, "Y", 1, INFINITY));

    /* Unbound vs concrete: all operators true (unknown) */
    TEST("?X eq 42",  qjson_cmp_eq(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_NUMBER, 42.0, NULL, 0, 42.0));
    TEST("?X ne 42",  qjson_cmp_ne(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_NUMBER, 42.0, NULL, 0, 42.0));
    TEST("?X lt 42",  qjson_cmp_lt(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_NUMBER, 42.0, NULL, 0, 42.0));
    TEST("?X gt 42",  qjson_cmp_gt(QJSON_UNBOUND, -INFINITY, "X", 1, INFINITY,
                                    QJSON_NUMBER, 42.0, NULL, 0, 42.0));
}

/* -- Benchmark ------------------------------------------------ */

static void benchmark(void) {
    printf("\n=== Benchmark ===\n");

    /* Typical Prolog term serialization (persist hot path) */
    const char *msg =
        "{\"t\":\"c\",\"f\":\"reading\",\"a\":["
        "{\"t\":\"a\",\"n\":\"sensor_1\"},"
        "{\"t\":\"a\",\"n\":\"temperature\"},"
        "{\"t\":\"n\",\"v\":22},"
        "{\"t\":\"n\",\"v\":1710400000}]}";
    int msg_len = strlen(msg);

    char arena_bench[8192];
    qjson_arena a;
    char out[512];
    int iterations = 1000000;

    /* Parse benchmark */
    {
        qjson_arena_init(&a, arena_bench, sizeof(arena_bench));
        clock_t start = clock();
        for (int i = 0; i < iterations; i++) {
            qjson_arena_reset(&a);
            qjson_parse(&a, msg, msg_len);
        }
        clock_t end = clock();
        double ms = (double)(end - start) / CLOCKS_PER_SEC * 1000.0;
        printf("  Parse:     %d messages in %.1f ms (%.1f M msg/sec)\n",
               iterations, ms, iterations / ms / 1000.0);
    }

    /* Stringify benchmark */
    {
        qjson_arena_init(&a, arena_bench, sizeof(arena_bench));
        qjson_val *v = qjson_parse(&a, msg, msg_len);
        clock_t start = clock();
        for (int i = 0; i < iterations; i++) {
            qjson_stringify(v, out, sizeof(out));
        }
        clock_t end = clock();
        double ms = (double)(end - start) / CLOCKS_PER_SEC * 1000.0;
        printf("  Stringify: %d messages in %.1f ms (%.1f M msg/sec)\n",
               iterations, ms, iterations / ms / 1000.0);
    }

    /* Memory profile */
    {
        qjson_arena_init(&a, arena_bench, sizeof(arena_bench));
        qjson_parse(&a, msg, msg_len);
        printf("  Memory:    %zu bytes per parse (%zu byte arena, %d%% used)\n",
               a.used, sizeof(arena_bench), (int)(a.used * 100 / sizeof(arena_bench)));
        printf("  Message:   %d bytes input, %d bytes output\n",
               msg_len, (int)strlen(out));
        printf("  Malloc:    0 per message (arena-allocated)\n");
    }
}

/* -- Complex keys + set shorthand -------------------------------- */

static void test_complex_keys(void) {
    printf("\n=== Complex keys + set shorthand ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    char out[2048];

    /* Bare identifier as value */
    qjson_val *v = qjson_parse(&a, "alice", 5);
    TEST("bare ident value", v && v->type == QJSON_STRING &&
         v->str.len == 5 && memcmp(v->str.s, "alice", 5) == 0);

    /* Bare idents in array */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "[alice, bob]", 12);
    TEST("bare ident array", v && v->type == QJSON_ARRAY && v->arr.count == 2);
    TEST("bare ident arr[0]", v->arr.items[0]->type == QJSON_STRING &&
         strcmp(v->arr.items[0]->str.s, "alice") == 0);
    TEST("bare ident arr[1]", v->arr.items[1]->type == QJSON_STRING &&
         strcmp(v->arr.items[1]->str.s, "bob") == 0);

    /* Keyword boundary: truthy != true */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "truthy", 6);
    TEST("truthy is string", v && v->type == QJSON_STRING &&
         strcmp(v->str.s, "truthy") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "nullable", 8);
    TEST("nullable is string", v && v->type == QJSON_STRING &&
         strcmp(v->str.s, "nullable") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "falsehood", 9);
    TEST("falsehood is string", v && v->type == QJSON_STRING &&
         strcmp(v->str.s, "falsehood") == 0);

    /* Set shorthand: {alice, bob} */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{alice, bob, carol}", 19);
    TEST("set shorthand parse", v && v->type == QJSON_OBJECT && v->obj.count == 3);
    TEST("set key[0] string", v->obj.pairs[0].key->type == QJSON_STRING &&
         strcmp(v->obj.pairs[0].key->str.s, "alice") == 0);
    TEST("set val[0] true", v->obj.pairs[0].val->type == QJSON_TRUE);
    TEST("set key[2] string", v->obj.pairs[2].key->type == QJSON_STRING &&
         strcmp(v->obj.pairs[2].key->str.s, "carol") == 0);

    /* Set shorthand stringify */
    int n = qjson_stringify(v, out, sizeof(out));
    TEST("set shorthand stringify", n > 0 && strcmp(out, "{alice,bob,carol}") == 0);

    /* Complex key: number */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{42: \"answer\"}", 14);
    TEST("number key parse", v && v->type == QJSON_OBJECT && v->obj.count == 1);
    TEST("number key type", v->obj.pairs[0].key->type == QJSON_NUMBER &&
         v->obj.pairs[0].key->num == 42.0);
    TEST("number key value", v->obj.pairs[0].val->type == QJSON_STRING &&
         strcmp(v->obj.pairs[0].val->str.s, "answer") == 0);

    /* Complex key: array */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{[1,2]: \"pair\"}", 15);
    TEST("array key parse", v && v->type == QJSON_OBJECT && v->obj.count == 1);
    TEST("array key type", v->obj.pairs[0].key->type == QJSON_ARRAY &&
         v->obj.pairs[0].key->arr.count == 2);

    /* Complex key: boolean */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{true: 1}", 9);
    TEST("boolean key parse", v && v->type == QJSON_OBJECT && v->obj.count == 1);
    TEST("boolean key type", v->obj.pairs[0].key->type == QJSON_TRUE);

    /* Complex key: null */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{null: 1}", 9);
    TEST("null key parse", v && v->type == QJSON_OBJECT && v->obj.count == 1);
    TEST("null key type", v->obj.pairs[0].key->type == QJSON_NULL);

    /* Complex key: unbound */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{?X: 42}", 8);
    TEST("unbound key parse", v && v->type == QJSON_OBJECT && v->obj.count == 1);
    TEST("unbound key type", v->obj.pairs[0].key->type == QJSON_UNBOUND);

    /* Set of arrays (Datalog tuples) */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{[1,2], [3,4]}", 14);
    TEST("set of arrays", v && v->type == QJSON_OBJECT && v->obj.count == 2);
    TEST("set arr key[0]", v->obj.pairs[0].key->type == QJSON_ARRAY);
    TEST("set arr val[0] true", v->obj.pairs[0].val->type == QJSON_TRUE);
    n = qjson_stringify(v, out, sizeof(out));
    TEST("set of arrays stringify", strcmp(out, "{[1,2],[3,4]}") == 0);

    /* Trailing comma in set */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{a, b,}", 7);
    TEST("trailing comma set", v && v->type == QJSON_OBJECT && v->obj.count == 2);

    /* Mixed map with string and non-string keys */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{name: 1, 42: 2}", 16);
    TEST("mixed keys parse", v && v->type == QJSON_OBJECT && v->obj.count == 2);
    TEST("mixed key[0] string", v->obj.pairs[0].key->type == QJSON_STRING);
    TEST("mixed key[1] number", v->obj.pairs[1].key->type == QJSON_NUMBER);

    /* Map syntax stringify (string keys quoted) */
    n = qjson_stringify(v, out, sizeof(out));
    TEST("mixed keys stringify", strcmp(out, "{\"name\":1,42:2}") == 0);

    /* Backward compat: string-key object round-trip */
    qjson_arena_reset(&a);
    const char *s = "{\"x\":1,\"y\":2}";
    v = qjson_parse(&a, s, (int)strlen(s));
    n = qjson_stringify(v, out, sizeof(out));
    TEST("string key round-trip", strcmp(out, s) == 0);

    /* is_json with complex keys */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{42: 1}", 7);
    TEST("is_json complex key", !qjson_is_json(v));
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{name: 1}", 9);
    TEST("is_json string key", qjson_is_json(v));

    /* is_bound with unbound key */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{?X: 42}", 8);
    TEST("is_bound unbound key", !qjson_is_bound(v));

    /* obj_get still works for string keys */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{name: alice, age: 30}", 22);
    qjson_val *got = qjson_obj_get(v, "name");
    TEST("obj_get string key", got && got->type == QJSON_STRING &&
         strcmp(got->str.s, "alice") == 0);
    got = qjson_obj_get(v, "age");
    TEST("obj_get number val", got && got->type == QJSON_NUMBER && got->num == 30.0);

    /* Empty object is still {} */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{}", 2);
    TEST("empty object", v && v->type == QJSON_OBJECT && v->obj.count == 0);
    n = qjson_stringify(v, out, sizeof(out));
    TEST("empty object stringify", strcmp(out, "{}") == 0);
}

/* -- View / Datalog ------------------------------------------- */

static void test_views(void) {
    printf("=== Views (WHERE/AND/OR/NOT/IN) ===\n");
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    char out[2048];
    int n;

    /* Simple view: pattern WHERE pattern IN source */
    qjson_arena_reset(&a);
    const char *s1 = "{x: ?A, y: ?B} where {a: ?A, b: ?B} in src";
    qjson_val *v = qjson_parse(&a, s1, (int)strlen(s1));
    TEST("view parse", v != NULL && v->type == QJSON_VIEW);
    TEST("view pattern", v->view.pattern != NULL && v->view.pattern->type == QJSON_OBJECT);
    TEST("view cond match", v->view.cond != NULL && v->view.cond->type == QJSON_MATCH);
    n = qjson_stringify(v, out, sizeof(out));
    TEST("view stringify", strstr(out, "where") != NULL && strstr(out, " in ") != NULL);

    /* AND condition */
    qjson_arena_reset(&a);
    const char *s2 = "?X where {a: ?X} in s1 and {b: ?X} in s2";
    v = qjson_parse(&a, s2, (int)strlen(s2));
    TEST("view AND parse", v != NULL && v->type == QJSON_VIEW);
    TEST("view AND cond", v->view.cond->type == QJSON_BINOP);
    TEST("view AND op", strcmp(v->view.cond->binop.op, "and") == 0);
    n = qjson_stringify(v, out, sizeof(out));
    TEST("view AND stringify", strstr(out, " and ") != NULL);

    /* OR condition */
    qjson_arena_reset(&a);
    const char *s3 = "?X where {a: ?X} in s1 or {b: ?X} in s2";
    v = qjson_parse(&a, s3, (int)strlen(s3));
    TEST("view OR parse", v != NULL && v->type == QJSON_VIEW);
    TEST("view OR cond", v->view.cond->type == QJSON_BINOP);
    TEST("view OR op", strcmp(v->view.cond->binop.op, "or") == 0);

    /* NOT condition */
    qjson_arena_reset(&a);
    const char *s4 = "?X where {a: ?X} in s1 and not {b: ?X} in s2";
    v = qjson_parse(&a, s4, (int)strlen(s4));
    TEST("view NOT parse", v != NULL && v->type == QJSON_VIEW);
    TEST("view NOT cond", v->view.cond->type == QJSON_BINOP);
    TEST("view NOT right", v->view.cond->binop.right->type == QJSON_NOTOP);

    /* Parenthesized condition */
    qjson_arena_reset(&a);
    const char *s5 = "?X where ({a: ?X} in s1 or {b: ?X} in s2) and {c: ?X} in s3";
    v = qjson_parse(&a, s5, (int)strlen(s5));
    TEST("view paren parse", v != NULL && v->type == QJSON_VIEW);
    TEST("view paren top AND", v->view.cond->type == QJSON_BINOP &&
         strcmp(v->view.cond->binop.op, "and") == 0);
    TEST("view paren left OR", v->view.cond->binop.left->type == QJSON_BINOP &&
         strcmp(v->view.cond->binop.left->binop.op, "or") == 0);

    /* View as object entry value */
    qjson_arena_reset(&a);
    const char *s6 = "{gp: {grandparent: ?GP, grandchild: ?GC}"
        " where {parent: ?GP, child: ?P} in parents"
        " and {parent: ?P, child: ?GC} in parents}";
    v = qjson_parse(&a, s6, (int)strlen(s6));
    TEST("view in object parse", v != NULL && v->type == QJSON_OBJECT);
    TEST("view in object count", v->obj.count == 1);
    TEST("view in object key", v->obj.pairs[0].key->type == QJSON_STRING &&
         strcmp(v->obj.pairs[0].key->str.s, "gp") == 0);
    TEST("view in object val", v->obj.pairs[0].val->type == QJSON_VIEW);
    n = qjson_stringify(v, out, sizeof(out));
    TEST("view in object stringify", strstr(out, "where") != NULL);

    /* Round-trip: parse → stringify → parse */
    qjson_arena_reset(&a);
    const char *s7 = "{r: ?X} where {a: ?X} in s1 and {b: ?X} in s2";
    v = qjson_parse(&a, s7, (int)strlen(s7));
    n = qjson_stringify(v, out, sizeof(out));
    qjson_arena_reset(&a);
    qjson_val *v2 = qjson_parse(&a, out, n);
    TEST("view round-trip type", v2 != NULL && v2->type == QJSON_VIEW);
    TEST("view round-trip cond", v2->view.cond->type == QJSON_BINOP);
    char out2[2048];
    int n2 = qjson_stringify(v2, out2, sizeof(out2));
    TEST("view round-trip stable", n == n2 && memcmp(out, out2, n) == 0);

    /* keywords as quoted strings still work */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{\"where\": true, \"in\": false}", 28);
    TEST("quoted keywords", v && v->type == QJSON_OBJECT && v->obj.count == 2);

    (void)n;
}

/* -- Main ----------------------------------------------------- */

int main(void) {
    test_parse_basic();
    test_parse_compound();
    test_parse_bignum();
    test_parse_comments();
    test_parse_human();
    test_stringify();
    test_project();
    test_decimal_cmp();
    test_interval_cmp();
    test_js64_encode_decode();
    test_parse_blob();
    test_parse_unbound();
    test_cmp_fix();
    test_complex_keys();
    test_views();
    benchmark();

    printf("\n%d/%d tests passed\n", pass, pass + fail);
    return fail ? 1 : 0;
}
