/*
 * Reference implementation of nextup/nextdown/round_down/round_up
 * as a PostgreSQL extension using libbf directed rounding.
 *
 * Build: cc -shared -fPIC -I$(pg_config --includedir-server) \
 *          -I../native/libbf -o pg_nextafter_ref.so \
 *          pg_nextafter_ref.c ../native/libbf/libbf.c ../native/libbf/cutils.c -lm
 *
 * Install: CREATE FUNCTION ref_nextup(float8) RETURNS float8
 *            AS 'pg_nextafter_ref', 'pg_ref_nextup' LANGUAGE C IMMUTABLE STRICT;
 */
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>
#include <inttypes.h>
#include "libbf.h"

/* We don't link against PG headers — this is a standalone test harness. */
/* See test_pg_nextafter.py for the actual comparison test. */

static void *bf_realloc_fn(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (size == 0) { free(ptr); return NULL; }
    return realloc(ptr, size);
}

double ref_nextup(double x) {
    if (x != x || x == INFINITY) return x;
    if (x == 0.0) return 5e-324;
    return nextafter(x, INFINITY);
}

double ref_nextdown(double x) {
    if (x != x || x == -INFINITY) return x;
    if (x == 0.0) return -5e-324;
    return nextafter(x, -INFINITY);
}

void ref_round_down_up(const char *raw, double *lo, double *hi) {
    bf_context_t ctx;
    bf_context_init(&ctx, bf_realloc_fn, NULL);
    bf_t val;
    bf_init(&ctx, &val);
    bf_atof(&val, raw, NULL, 10, BF_PREC_INF, BF_RNDN);
    bf_get_float64(&val, lo, BF_RNDD);
    bf_get_float64(&val, hi, BF_RNDU);
    bf_delete(&val);
    bf_context_end(&ctx);
}

/* ── Test harness ─────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static uint64_t f2bits(double x) {
    uint64_t b; memcpy(&b, &x, 8); return b;
}

int main(int argc, char **argv) {
    /* Output CSV: value,ref_nextup,ref_nextdown,ref_lo,ref_hi */
    /* Test values chosen for edge cases */
    const char *test_values[] = {
        "0", "1", "-1", "0.1", "0.3", "-0.1",
        "0.5", "1.5", "2.0", "3.0",
        "99.99", "67432.50",
        "9007199254740992",   /* 2^53 (exact) */
        "9007199254740993",   /* 2^53 + 1 (inexact) */
        "1e-300", "1e-320",   /* near subnormal */
        "5e-324",             /* smallest positive */
        "1.7976931348623157e308",  /* DBL_MAX */
        "2e308",              /* overflow */
        "1e25",               /* large inexact */
        "0.1000000000000000055511151231257827021181583404541015625", /* exact 0.1 double */
        "3.141592653589793238462643383279",  /* pi-ish */
        NULL
    };

    printf("value,ref_nextup_bits,ref_nextdown_bits,ref_lo_bits,ref_hi_bits\n");
    for (int i = 0; test_values[i]; i++) {
        double v = atof(test_values[i]);
        double nu = ref_nextup(v);
        double nd = ref_nextdown(v);
        double lo, hi;
        ref_round_down_up(test_values[i], &lo, &hi);
        printf("%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
               test_values[i], f2bits(nu), f2bits(nd), f2bits(lo), f2bits(hi));
    }
    return 0;
}
