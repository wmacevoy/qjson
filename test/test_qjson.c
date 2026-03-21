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
    TEST("parse integer", v && v->type == QJSON_NUM && v->num == 42);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "-3.14", 5);
    TEST("parse float", v && v->type == QJSON_NUM && v->num < -3.13 && v->num > -3.15);

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
    TEST("BigDecimal M", v && v->type == QJSON_BIGDEC && strcmp(v->str.s, "3.14") == 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14m", 5);
    TEST("BigDecimal m (lc)", v && v->type == QJSON_BIGDEC);

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
    TEST("line comment", v && v->type == QJSON_NUM && v->num == 42);

    qjson_arena_reset(&a);
    s = "/* block */ 42";
    v = qjson_parse(&a, s, strlen(s));
    TEST("block comment", v && v->type == QJSON_NUM && v->num == 42);

    qjson_arena_reset(&a);
    s = "/* outer /* inner */ still */ 42";
    v = qjson_parse(&a, s, strlen(s));
    TEST("nested block comment", v && v->type == QJSON_NUM && v->num == 42);
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

    /* val_project: QJSON_NUM -> point interval */
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    qjson_val *v = qjson_parse(&a, "42", 2);
    qjson_val_project(v, &lo, &hi);
    TEST("val_project QJSON_NUM", lo == 42.0 && hi == 42.0);

    /* val_project: QJSON_BIGDEC -> directed rounding */
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
 * Implements the query pushdown formulas from docs/qsql-intervals.md:
 *   a <op> b = (interval_accept) OR ((interval_not_reject) AND cmp <op> 0)
 */

/* Helper: project + qjson_cmp in one call */
static int cmp(const char *a, const char *b) {
    double a_lo, a_hi, b_lo, b_hi;
    qjson_project(a, strlen(a), &a_lo, &a_hi);
    qjson_project(b, strlen(b), &b_lo, &b_hi);
    return qjson_cmp(a_lo, a_hi, a, strlen(a), b_lo, b_hi, b, strlen(b));
}

#define LT(a, b)  (cmp(a, b) <  0)
#define LE(a, b)  (cmp(a, b) <= 0)
#define GT(a, b)  (cmp(a, b) >  0)
#define GE(a, b)  (cmp(a, b) >= 0)
#define EQ(a, b)  (cmp(a, b) == 0)
#define NE(a, b)  (cmp(a, b) != 0)

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
    benchmark();

    printf("\n%d/%d tests passed\n", pass, pass + fail);
    return fail ? 1 : 0;
}
