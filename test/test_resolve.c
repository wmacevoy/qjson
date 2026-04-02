/* test_resolve.c — Test the view resolver */

#include <stdio.h>
#include <string.h>
#include "qjson.h"
#include "qjson_resolve.h"

static char arena_buf[65536];
static int pass = 0, fail = 0;

#define TEST(name, expr) do { \
    if (expr) { pass++; printf("  ok  %s\n", name); } \
    else { fail++; printf("  FAIL %s  [line %d]\n", name, __LINE__); } \
} while(0)

int main(void) {
    qjson_arena a;
    qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    char out[4096];

    printf("=== Grandparent resolution ===\n");

    /* Parse facts + view definition */
    const char *data =
        "{"
        "  parents: {"
        "    {parent: alice, child: bob},"
        "    {parent: bob, child: carol},"
        "    {parent: carol, child: dave}"
        "  },"
        "  gp: {grandparent: ?GP, grandchild: ?GC}"
        "    where {parent: ?GP, child: ?P} in parents"
        "      and {parent: ?P, child: ?GC} in parents"
        "}";
    qjson_val *facts = qjson_parse(&a, data, (int)strlen(data));
    TEST("parse facts", facts != NULL && facts->type == QJSON_OBJECT);

    /* Check the view is stored */
    qjson_val *gp = qjson_obj_get(facts, "gp");
    TEST("gp is view", gp != NULL && gp->type == QJSON_VIEW);

    /* Resolve: all grandparents */
    qjson_val *all = qjson_resolve(&a, facts, gp);
    TEST("resolve all", all != NULL && all->type == QJSON_ARRAY);
    TEST("resolve count", all->arr.count == 2);

    int n = qjson_stringify(all, out, sizeof(out));
    printf("  all grandparents: %.*s\n", n, out);

    /* Each result should be {grandparent: X, grandchild: Y} */
    for (int i = 0; i < all->arr.count; i++) {
        TEST("result is object", all->arr.items[i]->type == QJSON_OBJECT);
    }

    /* Select with filter: grandchild = carol */
    printf("\n=== Select with filter ===\n");
    const char *q1 = "{grandparent: ?GP, grandchild: carol}";
    qjson_val *query1 = qjson_parse(&a, q1, (int)strlen(q1));
    qjson_val *r1 = qjson_select(&a, facts, "gp", query1);
    TEST("select carol", r1 != NULL && r1->type == QJSON_ARRAY);
    TEST("select carol count", r1->arr.count == 1);
    n = qjson_stringify(r1, out, sizeof(out));
    printf("  grandchild=carol: %.*s\n", n, out);

    /* The grandparent of carol should be alice */
    if (r1->arr.count == 1) {
        qjson_val *gp_val = qjson_obj_get(r1->arr.items[0], "grandparent");
        TEST("grandparent is alice", gp_val && gp_val->type == QJSON_STRING &&
             strcmp(gp_val->str.s, "alice") == 0);
    }

    /* Select with filter: grandparent = bob */
    const char *q2 = "{grandparent: bob, grandchild: ?GC}";
    qjson_val *query2 = qjson_parse(&a, q2, (int)strlen(q2));
    qjson_val *r2 = qjson_select(&a, facts, "gp", query2);
    TEST("select bob", r2 != NULL && r2->type == QJSON_ARRAY);
    TEST("select bob count", r2->arr.count == 1);
    n = qjson_stringify(r2, out, sizeof(out));
    printf("  grandparent=bob: %.*s\n", n, out);

    if (r2->arr.count == 1) {
        qjson_val *gc_val = qjson_obj_get(r2->arr.items[0], "grandchild");
        TEST("grandchild is dave", gc_val && gc_val->type == QJSON_STRING &&
             strcmp(gc_val->str.s, "dave") == 0);
    }

    /* No match: grandchild = nobody */
    const char *q3 = "{grandparent: ?GP, grandchild: nobody}";
    qjson_val *query3 = qjson_parse(&a, q3, (int)strlen(q3));
    qjson_val *r3 = qjson_select(&a, facts, "gp", query3);
    TEST("select nobody", r3 != NULL && r3->arr.count == 0);

    printf("\n%d/%d tests passed\n", pass, pass + fail);
    return fail ? 1 : 0;
}
