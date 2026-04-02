/* test_brain.c — Test ephemeral, mineralize, fossilize */

#include <stdio.h>
#include <string.h>
#include "qjson.h"
#include "qjson_db.h"

static char arena_buf[131072];
static int pass = 0, fail = 0;

#define TEST(name, expr) do { \
    if (expr) { pass++; printf("  ok  %s\n", name); } \
    else { fail++; printf("  FAIL %s  [line %d]\n", name, __LINE__); } \
} while(0)

/* ── Ephemeral callback: check grandparent count during "what if" ── */

static int ephemeral_gp_count = 0;

static int check_what_if(qjson_db *db, void *ud) {
    (void)ud;
    qjson_val *during = qjson_db_resolve(db, "gp");
    ephemeral_gp_count = during ? during->arr.count : -1;
    return 42; /* return value passed through */
}

int main(void) {
    qjson_arena a;
    qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    char out[4096];
    int n;

    /* Set up db with parents + gp view */
    qjson_db *db = qjson_db_open(&a);

    const char *ps = "{{parent: alice, child: bob}, {parent: bob, child: carol}, {parent: carol, child: dave}}";
    qjson_val *parents = qjson_parse(&a, ps, (int)strlen(ps));
    qjson_db_set(db, "parents", parents);

    const char *vs = "{grandparent: ?GP, grandchild: ?GC}"
        " where {parent: ?GP, child: ?P} in parents"
        " and {parent: ?P, child: ?GC} in parents";
    qjson_val *gp_view = qjson_parse(&a, vs, (int)strlen(vs));
    qjson_db_define(db, "gp", gp_view);

    /* ── Ephemeral ──────────────────────────────────────────── */
    printf("=== Ephemeral (what-if) ===\n");

    /* Before: 2 grandparents (alice→carol, bob→dave) */
    qjson_val *before = qjson_db_resolve(db, "gp");
    TEST("before ephemeral", before && before->arr.count == 2);
    n = qjson_stringify(before, out, sizeof(out));
    printf("  before: %.*s\n", n, out);

    /* What if dave had a child eve?  (4 parents → 3 grandparents) */
    const char *ps_what_if = "{{parent: alice, child: bob}, {parent: bob, child: carol},"
        " {parent: carol, child: dave}, {parent: dave, child: eve}}";
    qjson_val *parents_what_if = qjson_parse(&a, ps_what_if, (int)strlen(ps_what_if));

    int eph_result = qjson_ephemeral(db, "parents", parents_what_if, check_what_if, NULL);
    TEST("during ephemeral 3 gps", ephemeral_gp_count == 3);
    TEST("ephemeral returns 42", eph_result == 42);

    /* After: back to 2 grandparents */
    qjson_val *after_eph = qjson_db_resolve(db, "gp");
    TEST("after ephemeral reverts", after_eph && after_eph->arr.count == 2);

    /* ── Mineralize ─────────────────────────────────────────── */
    printf("\n=== Mineralize ===\n");

    TEST("gp is view", qjson_db_get(db, "gp")->type == QJSON_VIEW);

    qjson_mineralize(db, "gp");

    qjson_val *gp_facts = qjson_db_get(db, "gp");
    TEST("gp is array after mineralize", gp_facts && gp_facts->type == QJSON_ARRAY);
    TEST("gp has 2 facts", gp_facts && gp_facts->arr.count == 2);
    n = qjson_stringify(gp_facts, out, sizeof(out));
    printf("  mineralized: %.*s\n", n, out);

    /* ── Fossilize ──────────────────────────────────────────── */
    printf("\n=== Fossilize ===\n");

    /* Re-define gp as view, add another view */
    qjson_db_define(db, "gp", gp_view);

    const char *cv = "?X where {parent: ?X, child: ?} in parents";
    qjson_val *parent_names_view = qjson_parse(&a, cv, (int)strlen(cv));
    qjson_db_define(db, "parent_names", parent_names_view);

    TEST("gp is view", qjson_db_get(db, "gp")->type == QJSON_VIEW);
    TEST("parent_names is view", qjson_db_get(db, "parent_names")->type == QJSON_VIEW);

    qjson_fossilize(db);

    TEST("gp fossilized", qjson_db_get(db, "gp")->type == QJSON_ARRAY);
    TEST("parent_names fossilized", qjson_db_get(db, "parent_names")->type == QJSON_ARRAY);

    n = qjson_stringify(qjson_db_get(db, "gp"), out, sizeof(out));
    printf("  gp: %.*s\n", n, out);
    n = qjson_stringify(qjson_db_get(db, "parent_names"), out, sizeof(out));
    printf("  parent_names: %.*s\n", n, out);

    printf("\n%d/%d tests passed\n", pass, pass + fail);
    return fail ? 1 : 0;
}
