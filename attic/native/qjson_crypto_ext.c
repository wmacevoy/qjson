/*
 * qjson_crypto_ext.c — crypto SQL wrappers (ATTIC)
 *
 * Extracted from native/qjson_sqlite_ext.c on 2026-04-29 alongside the
 * underlying qjson_crypto.{c,h}.  Crypto belongs in an application
 * layer (per PLAN-wyatt-migration.md), not in the qjson SQLite
 * extension. The database layer should be identical SQLite ↔ PostgreSQL,
 * which crypto-as-SQL-functions breaks.
 *
 * Reference only — not built, not registered, not tested. Kept as a
 * record of the SQL function shapes if a host wants to register them
 * itself (e.g. via load_extension of a separate crypto extension).
 *
 * Underlying primitives in attic/native/qjson_crypto.{c,h}:
 *   qjson_sha256, qjson_hmac_sha256, qjson_aes_encrypt/decrypt,
 *   qjson_random_bytes, qjson_hkdf,
 *   qjson_shamir_split/recover,
 *   qjson_base64_encode/decode, qjson_base64url_encode/decode,
 *   qjson_jwt_sign/verify
 *
 * Build (when reactivated): cc -DQJSON_USE_CRYPTO ... -I$(LIBRESSL_DIR)/include
 *                          ... $(LIBRESSL_DIR)/lib/libcrypto.a
 *
 * Original Makefile target was:
 *   qjson_ext_crypto$(EXT_SUFFIX): native/qjson_sqlite_ext.c $(QJSON_SRC) \
 *       native/qjson_crypto.c $(LIBBF_SRC)
 *       $(CC) $(CFLAGS) -DQJSON_USE_CRYPTO -I$(LIBRESSL_DIR)/include \
 *           $(SHARED) -o $@ $^ $(LIBRESSL_DIR)/lib/libcrypto.a -lm
 */

/* ── Crypto SQL functions (requires LibreSSL) ──────────── */

#ifdef QJSON_USE_CRYPTO
#include "qjson_crypto.h"

static void sql_sha256(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *data; int len;
    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) {
        data = sqlite3_value_blob(argv[0]);
        len = sqlite3_value_bytes(argv[0]);
    } else {
        data = sqlite3_value_text(argv[0]);
        len = sqlite3_value_bytes(argv[0]);
    }
    if (!data) { sqlite3_result_null(ctx); return; }
    unsigned char hash[32];
    qjson_sha256(data, len, hash);
    sqlite3_result_blob(ctx, hash, 32, SQLITE_TRANSIENT);
}

static void sql_sha256_hex(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *data; int len;
    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) {
        data = sqlite3_value_blob(argv[0]);
        len = sqlite3_value_bytes(argv[0]);
    } else {
        data = sqlite3_value_text(argv[0]);
        len = sqlite3_value_bytes(argv[0]);
    }
    if (!data) { sqlite3_result_null(ctx); return; }
    unsigned char hash[32];
    qjson_sha256(data, len, hash);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", hash[i]);
    sqlite3_result_text(ctx, hex, 64, SQLITE_TRANSIENT);
}

static void sql_encrypt(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL)
        { sqlite3_result_null(ctx); return; }
    const void *pt = sqlite3_value_blob(argv[0]);
    int pt_len = sqlite3_value_bytes(argv[0]);
    const void *key = sqlite3_value_blob(argv[1]);
    int key_len = sqlite3_value_bytes(argv[1]);
    if (key_len != 32) { sqlite3_result_error(ctx, "key must be 32 bytes", -1); return; }
    /* IV(16) + ciphertext(padded up to +16) + HMAC(32) = up to +64 */
    void *out = sqlite3_malloc(pt_len + 64);
    int r = qjson_aes_encrypt(pt, pt_len, key, out);
    if (r < 0) { sqlite3_free(out); sqlite3_result_null(ctx); return; }
    sqlite3_result_blob(ctx, out, r, sqlite3_free);
}

static void sql_decrypt(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL)
        { sqlite3_result_null(ctx); return; }
    const void *ct = sqlite3_value_blob(argv[0]);
    int ct_len = sqlite3_value_bytes(argv[0]);
    const void *key = sqlite3_value_blob(argv[1]);
    int key_len = sqlite3_value_bytes(argv[1]);
    if (key_len != 32) { sqlite3_result_error(ctx, "key must be 32 bytes", -1); return; }
    if (ct_len < 28) { sqlite3_result_null(ctx); return; }
    void *out = sqlite3_malloc(ct_len);
    int r = qjson_aes_decrypt(ct, ct_len, key, out);
    if (r < 0) { sqlite3_free(out); sqlite3_result_null(ctx); return; }
    sqlite3_result_blob(ctx, out, r, sqlite3_free);
}

static void sql_random_bytes(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    int n = sqlite3_value_int(argv[0]);
    if (n <= 0 || n > 1024*1024) { sqlite3_result_null(ctx); return; }
    void *buf = sqlite3_malloc(n);
    qjson_random_bytes(buf, n);
    sqlite3_result_blob(ctx, buf, n, sqlite3_free);
}

static void sql_hmac(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL)
        { sqlite3_result_null(ctx); return; }
    const void *data = sqlite3_value_blob(argv[0]);
    int data_len = sqlite3_value_bytes(argv[0]);
    const void *key = sqlite3_value_blob(argv[1]);
    int key_len = sqlite3_value_bytes(argv[1]);
    unsigned char mac[32];
    qjson_hmac_sha256(data, data_len, key, key_len, mac);
    sqlite3_result_blob(ctx, mac, 32, SQLITE_TRANSIENT);
}

static void sql_hkdf(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *ikm = sqlite3_value_blob(argv[0]);
    int ikm_len = sqlite3_value_bytes(argv[0]);
    const void *salt = sqlite3_value_type(argv[1]) != SQLITE_NULL ? sqlite3_value_blob(argv[1]) : NULL;
    int salt_len = sqlite3_value_type(argv[1]) != SQLITE_NULL ? sqlite3_value_bytes(argv[1]) : 0;
    const void *info = sqlite3_value_type(argv[2]) != SQLITE_NULL ? sqlite3_value_blob(argv[2]) : NULL;
    int info_len = sqlite3_value_type(argv[2]) != SQLITE_NULL ? sqlite3_value_bytes(argv[2]) : 0;
    int out_len = sqlite3_value_int(argv[3]);
    if (out_len <= 0 || out_len > 8160) { sqlite3_result_null(ctx); return; }
    void *out = sqlite3_malloc(out_len);
    qjson_hkdf(ikm, ikm_len, salt, salt_len, info, info_len, out, out_len);
    sqlite3_result_blob(ctx, out, out_len, sqlite3_free);
}

static void sql_shamir_split(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *secret = (const char *)sqlite3_value_text(argv[0]);
    int m = sqlite3_value_int(argv[1]);
    int n = sqlite3_value_int(argv[2]);
    if (!secret || m < 1 || n < m || n > 255) {
        sqlite3_result_error(ctx, "shamir_split(secret_hex, M, N)", -1); return;
    }
    char **keys = sqlite3_malloc(sizeof(char*) * n);
    if (qjson_shamir_split(secret, m, n, NULL, keys) != 0) {
        sqlite3_free(keys);
        sqlite3_result_error(ctx, "shamir split failed", -1); return;
    }
    /* Build QJSON array: ["share1","share2",...] */
    dstr out; dstr_init(&out);
    dstr_catc(&out, '[');
    for (int i = 0; i < n; i++) {
        if (i > 0) dstr_catc(&out, ',');
        dstr_catc(&out, '"');
        dstr_cat(&out, keys[i]);
        dstr_catc(&out, '"');
        free(keys[i]);
    }
    dstr_catc(&out, ']');
    sqlite3_free(keys);
    sqlite3_result_text(ctx, out.buf, out.len, sqlite3_free);
}

static void sql_shamir_recover(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    /* qjson_shamir_recover(indices_json, keys_json)
       indices: [1, 3, 5]  keys: ["hex1", "hex2", "hex3"] */
    (void)argc;
    const char *idx_json = (const char *)sqlite3_value_text(argv[0]);
    const char *keys_json = (const char *)sqlite3_value_text(argv[1]);
    if (!idx_json || !keys_json) { sqlite3_result_null(ctx); return; }

    /* Simple JSON array parsing for indices and keys */
    int indices[256]; const char *keys[256]; char *key_bufs[256];
    int count = 0;

    /* Parse indices: [1, 3, 5] */
    const char *p = idx_json;
    while (*p && *p != '[') p++;
    if (*p) p++;
    while (*p && *p != ']' && count < 256) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        indices[count] = atoi(p);
        while (*p && *p != ',' && *p != ']') p++;
        count++;
    }

    /* Parse keys: ["hex1", "hex2", ...] */
    int kcount = 0;
    p = keys_json;
    while (*p && *p != '[') p++;
    if (*p) p++;
    while (*p && *p != ']' && kcount < count) {
        while (*p == ' ' || *p == ',' || *p == '"') p++;
        if (*p == ']') break;
        const char *start = p;
        while (*p && *p != '"') p++;
        int len = (int)(p - start);
        key_bufs[kcount] = sqlite3_malloc(len + 1);
        memcpy(key_bufs[kcount], start, len);
        key_bufs[kcount][len] = '\0';
        keys[kcount] = key_bufs[kcount];
        if (*p == '"') p++;
        kcount++;
    }

    if (kcount != count || count == 0) {
        for (int i = 0; i < kcount; i++) sqlite3_free(key_bufs[i]);
        sqlite3_result_error(ctx, "indices and keys must have same length", -1);
        return;
    }

    char out[256];
    int r = qjson_shamir_recover(indices, keys, count, 0, NULL, out, sizeof(out));
    for (int i = 0; i < kcount; i++) sqlite3_free(key_bufs[i]);

    if (r != 0) { sqlite3_result_null(ctx); return; }
    sqlite3_result_text(ctx, out, -1, SQLITE_TRANSIENT);
}

static void sql_base64_encode(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *data = sqlite3_value_blob(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    if (!data) { sqlite3_result_null(ctx); return; }
    char *out = qjson_base64_encode(data, len);
    sqlite3_result_text(ctx, out, -1, free);
}

static void sql_base64_decode(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *b64 = (const char *)sqlite3_value_text(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    if (!b64) { sqlite3_result_null(ctx); return; }
    size_t out_len;
    void *out = qjson_base64_decode(b64, len, &out_len);
    if (!out) { sqlite3_result_null(ctx); return; }
    sqlite3_result_blob(ctx, out, (int)out_len, free);
}

static void sql_base64url_encode(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *data = sqlite3_value_blob(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    if (!data) { sqlite3_result_null(ctx); return; }
    char *out = qjson_base64url_encode(data, len);
    sqlite3_result_text(ctx, out, -1, free);
}

static void sql_base64url_decode(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *b64 = (const char *)sqlite3_value_text(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    if (!b64) { sqlite3_result_null(ctx); return; }
    size_t out_len;
    void *out = qjson_base64url_decode(b64, len, &out_len);
    if (!out) { sqlite3_result_null(ctx); return; }
    sqlite3_result_blob(ctx, out, (int)out_len, free);
}

static void sql_jwt_sign(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *payload = (const char *)sqlite3_value_text(argv[0]);
    int pay_len = sqlite3_value_bytes(argv[0]);
    const void *secret = sqlite3_value_blob(argv[1]);
    int sec_len = sqlite3_value_bytes(argv[1]);
    if (!payload || !secret) { sqlite3_result_null(ctx); return; }
    char *jwt = qjson_jwt_sign(payload, pay_len, secret, sec_len);
    if (!jwt) { sqlite3_result_null(ctx); return; }
    sqlite3_result_text(ctx, jwt, -1, free);
}

static void sql_jwt_verify(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *jwt = (const char *)sqlite3_value_text(argv[0]);
    int jwt_len = sqlite3_value_bytes(argv[0]);
    const void *secret = sqlite3_value_blob(argv[1]);
    int sec_len = sqlite3_value_bytes(argv[1]);
    if (!jwt || !secret) { sqlite3_result_null(ctx); return; }
    char *payload = qjson_jwt_verify(jwt, jwt_len, secret, sec_len);
    if (!payload) { sqlite3_result_null(ctx); return; }
    sqlite3_result_text(ctx, payload, -1, free);
}

#endif /* QJSON_USE_CRYPTO */

/* Original SQL function registrations (in qjson_sqlite_ext.c init):
 *
 *   sqlite3_create_function(db, "qjson_sha256",     1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_sha256, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_sha256_hex", 1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_sha256_hex, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_encrypt",    2, SQLITE_UTF8, NULL, sql_encrypt, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_decrypt",    2, SQLITE_UTF8, NULL, sql_decrypt, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_random",     1, SQLITE_UTF8, NULL, sql_random_bytes, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_hmac",       2, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_hmac, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_hkdf",       4, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_hkdf, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_shamir_split",   3, SQLITE_UTF8, NULL, sql_shamir_split, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_shamir_recover", 2, SQLITE_UTF8, NULL, sql_shamir_recover, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_base64_encode",    1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_base64_encode, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_base64_decode",    1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_base64_decode, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_base64url_encode", 1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_base64url_encode, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_base64url_decode", 1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_base64url_decode, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_jwt_sign",   2, SQLITE_UTF8, NULL, sql_jwt_sign, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_jwt_verify", 2, SQLITE_UTF8, NULL, sql_jwt_verify, NULL, NULL);
 */
