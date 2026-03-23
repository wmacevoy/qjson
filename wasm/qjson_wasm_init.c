/*
 * qjson_wasm_init.c — QJSON extension for SQLCipher WASM build.
 *
 * Include this file in the WASM compilation alongside sqlcipher_wasm.c.
 * It brings in the full qjson extension (comparison, arithmetic,
 * constraint solver, reconstruct, select) backed by libbf.
 *
 * The extension is auto-registered on every database open via
 * sqlite3_auto_extension(). No manual load_extension() needed.
 *
 * Build (add to the emcc command):
 *   emcc sqlcipher_wasm.c qjson_wasm_init.c \
 *     qjson.c libbf.c cutils.c \
 *     -DQJSON_USE_LIBBF -Iqjson_native -Ilibbf ...
 */

#include "qjson.h"
#include "qjson_sqlite_ext.c"

/* Auto-register on every connection open */
static void _qjson_auto_init(void) __attribute__((constructor));
static void _qjson_auto_init(void) {
    sqlite3_auto_extension((void(*)(void))sqlite3_qjsonext_init);
}
