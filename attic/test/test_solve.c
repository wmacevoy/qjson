/* test_solve.c — Test equation solving in views */

#include <stdio.h>
#include <string.h>
#include <math.h>
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

    printf("=== Compound interest ===\n");

    /* Define a view with an equation:
       FV = P * (1 + R) ^ N */
    const char *data =
        "{"
        "  compound: {P: ?P, R: ?R, N: ?N, FV: ?FV}"
        "    where ?FV = ?P * (1 + ?R) ^ ?N"
        "}";
    qjson_val *facts = qjson_parse(&a, data, (int)strlen(data));
    TEST("parse compound", facts != NULL);

    qjson_val *view = qjson_obj_get(facts, "compound");
    TEST("view exists", view != NULL && view->type == QJSON_VIEW);
    TEST("view has equation", view->view.cond->type == QJSON_EQUATION);

    /* Stringify the view to see what we parsed */
    int n = qjson_stringify(view, out, sizeof(out));
    printf("  view: %.*s\n", n, out);

    /* Solve for FV: P=1000, R=0.05, N=10 → FV = 1000 * 1.05^10 ≈ 1628.89 */
    printf("\n=== Solve for FV ===\n");
    const char *q1 = "{P: 1000, R: 0.05, N: 10, FV: ?FV}";
    qjson_val *query1 = qjson_parse(&a, q1, (int)strlen(q1));
    qjson_val *r1 = qjson_select(&a, facts, "compound", query1);
    TEST("solve FV", r1 != NULL && r1->arr.count == 1);
    if (r1 && r1->arr.count == 1) {
        n = qjson_stringify(r1->arr.items[0], out, sizeof(out));
        printf("  result: %.*s\n", n, out);
        qjson_val *fv = qjson_obj_get(r1->arr.items[0], "FV");
        TEST("FV is number", fv != NULL && fv->type == QJSON_NUMBER);
        if (fv) {
            double expected = 1000.0 * pow(1.05, 10);
            TEST("FV ≈ 1628.89", fabs(fv->num - expected) < 0.01);
            printf("  FV = %.2f (expected %.2f)\n", fv->num, expected);
        }
    }

    /* Solve for P: FV=1628.89, R=0.05, N=10 → P = FV / (1+R)^N ≈ 1000 */
    /* This needs the solver to invert — currently just direct eval,
       so we test what works: FV on the left, expression on the right */

    /* Simple: 2 + 3 = ?X → ?X = 5 */
    printf("\n=== Simple equation solving ===\n");
    qjson_arena_reset(&a);
    const char *d2 = "{ calc: ?X where ?X = 2 + 3 }";
    facts = qjson_parse(&a, d2, (int)strlen(d2));
    TEST("parse simple", facts != NULL);
    qjson_val *r2 = qjson_resolve(&a, facts, qjson_obj_get(facts, "calc"));
    TEST("solve 2+3", r2 != NULL && r2->arr.count == 1);
    if (r2 && r2->arr.count == 1) {
        n = qjson_stringify(r2->arr.items[0], out, sizeof(out));
        printf("  2 + 3 = %.*s\n", n, out);
        double v;
        TEST("result is 5", r2->arr.items[0]->type == QJSON_NUMBER &&
             r2->arr.items[0]->num == 5.0);
    }

    /* Equation with match + equation combined:
       find prices where unit * qty = total */
    printf("\n=== Match + equation ===\n");
    qjson_arena_reset(&a);
    const char *d3 =
        "{"
        "  items: {"
        "    {name: apples, unit: 1.5, qty: 4},"
        "    {name: bread, unit: 3.0, qty: 2},"
        "    {name: milk, unit: 2.5, qty: 3}"
        "  },"
        "  totals: {name: ?N, total: ?T}"
        "    where {name: ?N, unit: ?U, qty: ?Q} in items"
        "      and ?T = ?U * ?Q"
        "}";
    facts = qjson_parse(&a, d3, (int)strlen(d3));
    TEST("parse items+totals", facts != NULL);

    qjson_val *r3 = qjson_resolve(&a, facts, qjson_obj_get(facts, "totals"));
    TEST("totals resolved", r3 != NULL && r3->arr.count == 3);
    if (r3) {
        n = qjson_stringify(r3, out, sizeof(out));
        printf("  totals: %.*s\n", n, out);
        /* Check first item: apples = 1.5 * 4 = 6.0 */
        if (r3->arr.count >= 1) {
            qjson_val *t0 = qjson_obj_get(r3->arr.items[0], "total");
            TEST("apples total = 6", t0 && t0->type == QJSON_NUMBER && t0->num == 6.0);
        }
    }

    /* Exact arithmetic with BigDecimal: 0.1M + 0.2M = 0.3M (not 0.30000000000000004) */
    printf("\n=== Exact BigDecimal arithmetic ===\n");
    qjson_arena_reset(&a);
    const char *d4 = "{ calc: ?X where ?X = 0.1M + 0.2M }";
    facts = qjson_parse(&a, d4, (int)strlen(d4));
    TEST("parse exact", facts != NULL);
    qjson_val *r4 = qjson_resolve(&a, facts, qjson_obj_get(facts, "calc"));
    TEST("solve 0.1M+0.2M", r4 != NULL && r4->arr.count == 1);
    if (r4 && r4->arr.count == 1) {
        n = qjson_stringify(r4->arr.items[0], out, sizeof(out));
        printf("  0.1M + 0.2M = %.*s\n", n, out);
        /* Must be exactly "0.3", not 0.30000000000000004 */
        TEST("exact 0.3M", r4->arr.items[0]->type == QJSON_BIGDECIMAL &&
             strcmp(r4->arr.items[0]->str.s, "0.3") == 0);
    }

    /* Compound interest with BigDecimal: 1000M * (1 + 0.05M) ^ 10 */
    printf("\n=== Exact compound interest ===\n");
    qjson_arena_reset(&a);
    const char *d5 =
        "{ compound: {P: ?P, R: ?R, N: ?N, FV: ?FV}"
        "    where ?FV = ?P * (1M + ?R) ^ ?N }";
    facts = qjson_parse(&a, d5, (int)strlen(d5));
    TEST("parse exact compound", facts != NULL);
    const char *q5 = "{P: 1000M, R: 0.05M, N: 10, FV: ?FV}";
    qjson_val *query5 = qjson_parse(&a, q5, (int)strlen(q5));
    qjson_val *r5 = qjson_select(&a, facts, "compound", query5);
    TEST("solve exact FV", r5 != NULL && r5->arr.count == 1);
    if (r5 && r5->arr.count == 1) {
        n = qjson_stringify(r5->arr.items[0], out, sizeof(out));
        printf("  exact result: %.*s\n", n, out);
        qjson_val *fv5 = qjson_obj_get(r5->arr.items[0], "FV");
        TEST("FV is BigDecimal", fv5 != NULL && fv5->type == QJSON_BIGDECIMAL);
        if (fv5) printf("  FV = %.*sM (exact)\n", fv5->str.len, fv5->str.s);
    }

    printf("\n%d/%d tests passed\n", pass, pass + fail);
    return fail ? 1 : 0;
}
