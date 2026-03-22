/*
 * qjson_sqlite_ext.c — SQLite loadable extension for exact QJSON comparison.
 *
 * Registers two SQL functions:
 *   qjson_decimal_cmp(a TEXT, b TEXT) → INTEGER
 *     Exact decimal string comparison using libbf.
 *     Returns -1 (a < b), 0 (a == b), 1 (a > b).
 *
 *   qjson_cmp(a_lo REAL, a_hi REAL, a_str TEXT,
 *             b_lo REAL, b_hi REAL, b_str TEXT) → INTEGER
 *     Full 3-tier interval comparison:
 *       1. [brackets] — indexed lo/hi rejects most rows
 *       2. {braces}   — both exact doubles, no string decode
 *       3. val()      — libbf exact fallback for overlap zone
 *
 * Build:
 *   cc -shared -fPIC -DQJSON_USE_LIBBF -o qjson_ext.dylib \
 *     native/qjson_sqlite_ext.c native/qjson.c \
 *     native/libbf/libbf.c native/libbf/cutils.c -lm
 */

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "qjson.h"

/* ── qjson_decimal_cmp(a TEXT, b TEXT) → INTEGER ──────────── */

static void sql_qjson_decimal_cmp(
    sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL ||
        sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    const char *a = (const char *)sqlite3_value_text(argv[0]);
    int a_len = sqlite3_value_bytes(argv[0]);
    const char *b = (const char *)sqlite3_value_text(argv[1]);
    int b_len = sqlite3_value_bytes(argv[1]);
    sqlite3_result_int(ctx, qjson_decimal_cmp(a, a_len, b, b_len));
}

/* ── qjson_cmp(a_lo, a_hi, a_str, b_lo, b_hi, b_str) → INT  */

static void sql_qjson_cmp(
    sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    double a_lo = sqlite3_value_double(argv[0]);
    double a_hi = sqlite3_value_double(argv[1]);
    const char *a_str = (const char *)sqlite3_value_text(argv[2]);
    int a_len = a_str ? sqlite3_value_bytes(argv[2]) : 0;

    double b_lo = sqlite3_value_double(argv[3]);
    double b_hi = sqlite3_value_double(argv[4]);
    const char *b_str = (const char *)sqlite3_value_text(argv[5]);
    int b_len = b_str ? sqlite3_value_bytes(argv[5]) : 0;

    sqlite3_result_int(ctx,
        qjson_cmp(a_lo, a_hi, a_str, a_len,
                  b_lo, b_hi, b_str, b_len));
}

/* ── Extension entry point ───────────────────────────────── */

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_qjsonext_init(
    sqlite3 *db, char **pzErrMsg,
    const sqlite3_api_routines *pApi)
{
    SQLITE_EXTENSION_INIT2(pApi);

    sqlite3_create_function(db, "qjson_decimal_cmp", 2,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC,
        NULL, sql_qjson_decimal_cmp, NULL, NULL);

    sqlite3_create_function(db, "qjson_cmp", 6,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC,
        NULL, sql_qjson_cmp, NULL, NULL);

    return SQLITE_OK;
}
