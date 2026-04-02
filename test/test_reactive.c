/* test_reactive.c — Test push-based reactive notifications */

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

/* ���─ Callback tracking ──────────────────────────────────────── */

static int notify_count = 0;
static char last_notified[64] = "";

static void on_view_stale(const char *view_name, void *userdata) {
    (void)userdata;
    notify_count++;
    strncpy(last_notified, view_name, sizeof(last_notified) - 1);
    printf("    [notify] %s is stale (count=%d)\n", view_name, notify_count);
}

int count_a = 0, count_b = 0;
void cb_a(const char *v, void *u) { (void)v; (void)u; count_a++; }
void cb_b(const char *v, void *u) { (void)v; (void)u; count_b++; }

int main(void) {
    qjson_arena a;
    qjson_arena_init(&a, arena_buf, sizeof(arena_buf));
    char out[4096];
    int n;

    printf("=== Reactive store ===\n");

    qjson_db *db = qjson_db_open(&a);
    TEST("db open", db != NULL);

    /* Store initial facts */
    const char *ps = "{{parent: alice, child: bob}, {parent: bob, child: carol}}";
    qjson_val *parents = qjson_parse(&a, ps, (int)strlen(ps));
    TEST("parse parents", parents != NULL);
    qjson_db_set(db, "parents", parents);

    /* Define a view */
    const char *vs = "{grandparent: ?GP, grandchild: ?GC}"
        " where {parent: ?GP, child: ?P} in parents"
        " and {parent: ?P, child: ?GC} in parents";
    qjson_val *gp_view = qjson_parse(&a, vs, (int)strlen(vs));
    TEST("parse view", gp_view != NULL && gp_view->type == QJSON_VIEW);

    /* Check dependency extraction */
    const char *deps[16];
    int ndeps = qjson_view_deps(gp_view, deps, 16);
    TEST("deps count", ndeps == 1);
    TEST("deps[0] = parents", ndeps > 0 && strcmp(deps[0], "parents") == 0);

    qjson_db_define(db, "gp", gp_view);

    /* Resolve before watching */
    qjson_val *r1 = qjson_db_resolve(db, "gp");
    TEST("resolve initial", r1 != NULL && r1->arr.count == 1);
    n = qjson_stringify(r1, out, sizeof(out));
    printf("  initial: %.*s\n", n, out);

    /* Watch the view */
    printf("\n=== Watch + mutate ===\n");
    notify_count = 0;
    int wid = qjson_watch(db, "gp", on_view_stale, NULL);
    TEST("watch registered", wid > 0);

    /* Mutate: add carol→dave */
    const char *ps2 = "{{parent: alice, child: bob}, {parent: bob, child: carol}, {parent: carol, child: dave}}";
    qjson_val *parents2 = qjson_parse(&a, ps2, (int)strlen(ps2));
    qjson_db_set(db, "parents", parents2);  /* ← should fire watcher */
    TEST("watcher fired", notify_count == 1);
    TEST("notified gp", strcmp(last_notified, "gp") == 0);

    /* Re-resolve: should now have 2 grandparents */
    qjson_val *r2 = qjson_db_resolve(db, "gp");
    TEST("resolve after mutation", r2 != NULL && r2->arr.count == 2);
    n = qjson_stringify(r2, out, sizeof(out));
    printf("  after mutation: %.*s\n", n, out);

    /* Mutate again: different set (should NOT fire gp watcher) */
    printf("\n=== Unrelated mutation ===\n");
    notify_count = 0;
    const char *is = "{apple, banana}";
    qjson_val *items = qjson_parse(&a, is, (int)strlen(is));
    qjson_db_set(db, "items", items);  /* ← gp doesn't depend on items */
    TEST("no false fire", notify_count == 0);

    /* Unwatch */
    printf("\n=== Unwatch ===\n");
    qjson_unwatch(db, wid);
    notify_count = 0;
    qjson_db_set(db, "parents", parents);  /* mutate parents again */
    TEST("no fire after unwatch", notify_count == 0);

    /* Multiple watchers */
    printf("\n=== Multiple watchers ===\n");
    extern int count_a, count_b;
    extern void cb_a(const char *v, void *u);
    extern void cb_b(const char *v, void *u);
    int wa = qjson_watch(db, "gp", cb_a, NULL);
    int wb = qjson_watch(db, "gp", cb_b, NULL);
    qjson_db_set(db, "parents", parents2);
    TEST("both fire", count_a == 1 && count_b == 1);
    qjson_unwatch(db, wa);
    qjson_db_set(db, "parents", parents);
    TEST("only b fires", count_a == 1 && count_b == 2);
    qjson_unwatch(db, wb);

    /* Select with filter through db */
    printf("\n=== Select through db ===\n");
    qjson_db_set(db, "parents", parents2); /* restore 3 parents */
    const char *qs = "{grandparent: ?GP, grandchild: carol}";
    qjson_val *query = qjson_parse(&a, qs, (int)strlen(qs));
    qjson_val *r3 = qjson_db_select(db, "gp", query);
    TEST("select carol", r3 != NULL && r3->arr.count == 1);
    if (r3 && r3->arr.count == 1) {
        qjson_val *gp = qjson_obj_get(r3->arr.items[0], "grandparent");
        TEST("grandparent is alice", gp && strcmp(gp->str.s, "alice") == 0);
    }

    printf("\n%d/%d tests passed\n", pass, pass + fail);
    return fail ? 1 : 0;
}
