/* test_qjson.c — Tests for native C QJSON (using facts) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include "qjson.h"
#include "facts.h"

static char arena_buf[65536];

/* ── Parse basics ──────────────────────────────────────────── */

FACTS(ParseBasics) {
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    qjson_val *v = qjson_parse(&a, "42", 2);
    FACT(v != NULL, ==, 1);
    FACT((int)v->type, ==, (int)QJSON_NUMBER);
    FACT(v->num, ==, 42.0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "-3.14", 5);
    FACT(v != NULL, ==, 1);
    FACT((int)v->type, ==, (int)QJSON_NUMBER);
    FACT(v->num < -3.13, ==, 1);
    FACT(v->num > -3.15, ==, 1);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "\"hello\"", 7);
    FACT(v != NULL, ==, 1);
    FACT((int)v->type, ==, (int)QJSON_STRING);
    FACT(strcmp(v->str.s, "hello"), ==, 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "true", 4);
    FACT((int)v->type, ==, (int)QJSON_TRUE);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "false", 5);
    FACT((int)v->type, ==, (int)QJSON_FALSE);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "null", 4);
    FACT((int)v->type, ==, (int)QJSON_NULL);
}

/* ── Parse compound ────────────────────────────────────────── */

FACTS(ParseCompound) {
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    qjson_val *v = qjson_parse(&a, "[1,\"two\",3]", 11);
    FACT(v != NULL, ==, 1);
    FACT((int)v->type, ==, (int)QJSON_ARRAY);
    FACT(v->arr.count, ==, 3);
    FACT(v->arr.items[0]->num, ==, 1.0);
    FACT(v->arr.items[2]->num, ==, 3.0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{\"a\":1,\"b\":2}", 13);
    FACT(v != NULL, ==, 1);
    FACT((int)v->type, ==, (int)QJSON_OBJECT);
    qjson_val *va = qjson_obj_get(v, "a");
    FACT(va != NULL, ==, 1);
    FACT(va->num, ==, 1.0);
    qjson_val *vb = qjson_obj_get(v, "b");
    FACT(vb != NULL, ==, 1);
    FACT(vb->num, ==, 2.0);
}

/* ── Parse bignums ─────────────────────────────────────────── */

FACTS(ParseBignums) {
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    qjson_val *v = qjson_parse(&a, "42N", 3);
    FACT((int)v->type, ==, (int)QJSON_BIGINT);
    FACT(strcmp(v->str.s, "42"), ==, 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "42n", 3);
    FACT((int)v->type, ==, (int)QJSON_BIGINT);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14M", 5);
    FACT((int)v->type, ==, (int)QJSON_BIGDECIMAL);
    FACT(strcmp(v->str.s, "3.14"), ==, 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14m", 5);
    FACT((int)v->type, ==, (int)QJSON_BIGDECIMAL);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14L", 5);
    FACT((int)v->type, ==, (int)QJSON_BIGFLOAT);
    FACT(strcmp(v->str.s, "3.14"), ==, 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14l", 5);
    FACT((int)v->type, ==, (int)QJSON_BIGFLOAT);
}

/* ── Parse comments ────────────────────────────────────────── */

FACTS(ParseComments) {
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    qjson_val *v = qjson_parse(&a, "// comment\n42", 13);
    FACT((int)v->type, ==, (int)QJSON_NUMBER);
    FACT(v->num, ==, 42.0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "/* block */42", 13);
    FACT((int)v->type, ==, (int)QJSON_NUMBER);
    FACT(v->num, ==, 42.0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "/* /* nested */ */42", 20);
    FACT((int)v->type, ==, (int)QJSON_NUMBER);
    FACT(v->num, ==, 42.0);
}

/* ── Human-friendly syntax ─────────────────────────────────── */

FACTS(ParseHumanFriendly) {
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));

    qjson_val *v = qjson_parse(&a, "[1,2,3,]", 8);
    FACT(v->arr.count, ==, 3);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{\"a\":1,\"b\":2,}", 14);
    FACT(v->obj.count, ==, 2);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{a: 1, b: 2}", 12);
    FACT(v != NULL, ==, 1);
    FACT((int)v->type, ==, (int)QJSON_OBJECT);
    qjson_val *va = qjson_obj_get(v, "a");
    FACT(va != NULL, ==, 1);
    FACT(va->num, ==, 1.0);
}

/* ── Stringify ─────────────────────────────────────────────── */

FACTS(Stringify) {
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    char out[1024];

    const char *input = "{\"n\":42,\"s\":\"hello\",\"a\":[1,2],\"t\":true,\"f\":false,\"z\":null}";
    qjson_val *v = qjson_parse(&a, input, (int)strlen(input));
    FACT(v != NULL, ==, 1);
    FACT(qjson_is_json(v), ==, 1);
    int n = qjson_stringify(v, out, sizeof(out));
    FACT(n > 0, ==, 1);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "42N", 3);
    n = qjson_stringify(v, out, sizeof(out));
    FACT(strcmp(out, "42N"), ==, 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14M", 5);
    n = qjson_stringify(v, out, sizeof(out));
    FACT(strcmp(out, "3.14M"), ==, 0);

    qjson_arena_reset(&a);
    v = qjson_parse(&a, "3.14L", 5);
    n = qjson_stringify(v, out, sizeof(out));
    FACT(strcmp(out, "3.14L"), ==, 0);
    (void)n;
}

/* ── Views (WHERE/AND/OR/NOT/IN) ───────────────────────────── */

FACTS(Views) {
    qjson_arena a; qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    char out[2048];

    const char *s1 = "{x: ?A, y: ?B} where {a: ?A, b: ?B} in src";
    qjson_val *v = qjson_parse(&a, s1, (int)strlen(s1));
    FACT(v != NULL, ==, 1);
    FACT((int)v->type, ==, (int)QJSON_VIEW);
    FACT((int)v->view.pattern->type, ==, (int)QJSON_OBJECT);
    FACT((int)v->view.cond->type, ==, (int)QJSON_MATCH);
    int n = qjson_stringify(v, out, sizeof(out));
    FACT(strstr(out, "where") != NULL, ==, 1);
    FACT(strstr(out, " in ") != NULL, ==, 1);

    qjson_arena_reset(&a);
    const char *s2 = "?X where {a: ?X} in s1 and {b: ?X} in s2";
    v = qjson_parse(&a, s2, (int)strlen(s2));
    FACT(v != NULL, ==, 1);
    FACT((int)v->type, ==, (int)QJSON_VIEW);
    FACT((int)v->view.cond->type, ==, (int)QJSON_BINOP);
    FACT(strcmp(v->view.cond->binop.op, "and"), ==, 0);

    qjson_arena_reset(&a);
    const char *s3 = "?X where {a: ?X} in s1 or {b: ?X} in s2";
    v = qjson_parse(&a, s3, (int)strlen(s3));
    FACT(strcmp(v->view.cond->binop.op, "or"), ==, 0);

    qjson_arena_reset(&a);
    const char *s4 = "?X where {a: ?X} in s1 and not {b: ?X} in s2";
    v = qjson_parse(&a, s4, (int)strlen(s4));
    FACT((int)v->view.cond->binop.right->type, ==, (int)QJSON_NOTOP);

    qjson_arena_reset(&a);
    const char *s5 = "?X where ({a: ?X} in s1 or {b: ?X} in s2) and {c: ?X} in s3";
    v = qjson_parse(&a, s5, (int)strlen(s5));
    FACT(strcmp(v->view.cond->binop.op, "and"), ==, 0);
    FACT(strcmp(v->view.cond->binop.left->binop.op, "or"), ==, 0);

    /* Round-trip */
    qjson_arena_reset(&a);
    const char *s7 = "{r: ?X} where {a: ?X} in s1 and {b: ?X} in s2";
    v = qjson_parse(&a, s7, (int)strlen(s7));
    n = qjson_stringify(v, out, sizeof(out));
    qjson_arena_reset(&a);
    qjson_val *v2 = qjson_parse(&a, out, n);
    FACT(v2 != NULL, ==, 1);
    FACT((int)v2->type, ==, (int)QJSON_VIEW);
    char out2[2048];
    int n2 = qjson_stringify(v2, out2, sizeof(out2));
    FACT(n, ==, n2);
    FACT(memcmp(out, out2, n), ==, 0);

    /* Keywords as quoted strings still work */
    qjson_arena_reset(&a);
    v = qjson_parse(&a, "{\"where\": true, \"in\": false}", 28);
    FACT(v != NULL, ==, 1);
    FACT(v->obj.count, ==, 2);
    (void)n;
}

/* ── Registration ──────────────────────────────────────────── */

FACTS_REGISTER_ALL() {
    FACTS_REGISTER(ParseBasics);
    FACTS_REGISTER(ParseCompound);
    FACTS_REGISTER(ParseBignums);
    FACTS_REGISTER(ParseComments);
    FACTS_REGISTER(ParseHumanFriendly);
    FACTS_REGISTER(Stringify);
    FACTS_REGISTER(Views);
}

FACTS_MAIN
