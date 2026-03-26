/* qjson_crypto.h — crypto primitives for QJSON SQLite extension.
 *
 * Requires LibreSSL/OpenSSL libcrypto + libbf.
 * All functions are self-contained — no global state.
 */
#ifndef QJSON_CRYPTO_H
#define QJSON_CRYPTO_H

#include <stddef.h>

/* ── SHA-256 ─────────────────────────────────────────────── */

/* Hash data, write 32 bytes to out. Returns 0 on success. */
int qjson_sha256(const void *data, size_t len, void *out);

/* ── AES-256-CBC + HMAC-SHA256 (encrypt-then-MAC) ────────── */

/* Encrypt plaintext with 32-byte key.
   Output: 16-byte IV + ciphertext (PKCS7 padded) + 32-byte HMAC.
   out must have room for len + 64 bytes (IV + padding + HMAC).
   Returns output length, or -1 on error.

   Portable: same format as PostgreSQL pgcrypto equivalent. */
int qjson_aes_encrypt(const void *plaintext, size_t len,
                      const void *key32, void *out);

/* Decrypt ciphertext produced by qjson_aes_encrypt.
   Verifies HMAC first (constant-time), then decrypts.
   Returns plaintext length, or -1 on auth failure. */
int qjson_aes_decrypt(const void *ciphertext, size_t len,
                      const void *key32, void *out);

/* ── Random ──────────────────────────────────────────────── */

/* Fill buf with n cryptographic random bytes. Returns 0. */
int qjson_random_bytes(void *buf, size_t n);

/* ── HMAC-SHA256 ─────────────────────────────────────────── */

/* HMAC-SHA256(data, key) → 32 bytes to out. Returns 0. */
int qjson_hmac_sha256(const void *data, size_t data_len,
                      const void *key, size_t key_len,
                      void *out);

/* ── HKDF-SHA256 ─────────────────────────────────────────── */

/* HKDF extract + expand. Returns 0 on success. */
int qjson_hkdf(const void *ikm, size_t ikm_len,
               const void *salt, size_t salt_len,
               const void *info, size_t info_len,
               void *out, size_t out_len);

/* ── Shamir secret sharing ───────────────────────────────── */

#define QJSON_SHAMIR_DEFAULT_PRIME "ffffffffffffffffffffffffffffffea5"

/* Split secret_hex into N shares, any M recover it.
   out_keys[0..shares-1] filled with malloc'd hex strings.
   Returns 0 on success. Caller frees each out_keys[i]. */
int qjson_shamir_split(const char *secret_hex, int minimum, int shares,
                       const char *prime_hex, char **out_keys);

/* Recover value at target index (0 = secret) from count shares.
   Returns 0 on success. */
int qjson_shamir_recover(const int *indices, const char **keys_hex,
                         int count, int target, const char *prime_hex,
                         char *out_hex, size_t out_size);

/* ── Base64 / Base64url ──────────────────────────────────── */

/* Standard base64 encode. Returns malloc'd string. */
char *qjson_base64_encode(const void *data, size_t len);

/* Standard base64 decode. Returns malloc'd buffer, sets *out_len. */
void *qjson_base64_decode(const char *b64, size_t b64_len, size_t *out_len);

/* URL-safe base64 (no padding). Returns malloc'd string. */
char *qjson_base64url_encode(const void *data, size_t len);

/* URL-safe base64 decode. Returns malloc'd buffer, sets *out_len. */
void *qjson_base64url_decode(const char *b64, size_t b64_len, size_t *out_len);

/* ── JWT (HS256) ─────────────────────────────────────────── */

/* Sign payload JSON with secret. Returns malloc'd JWT string. */
char *qjson_jwt_sign(const char *payload, size_t payload_len,
                     const void *secret, size_t secret_len);

/* Verify JWT and return malloc'd payload JSON, or NULL on failure. */
char *qjson_jwt_verify(const char *jwt, size_t jwt_len,
                       const void *secret, size_t secret_len);

#endif
