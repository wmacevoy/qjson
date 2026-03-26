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

/* ── AES-256-GCM ─────────────────────────────────────────── */

/* Encrypt plaintext with 32-byte key.
   Output: 12-byte nonce + ciphertext + 16-byte tag.
   out must have room for len + 28 bytes.
   Returns output length, or -1 on error. */
int qjson_aes_encrypt(const void *plaintext, size_t len,
                      const void *key32, void *out);

/* Decrypt ciphertext produced by qjson_aes_encrypt.
   Input: 12-byte nonce + ciphertext + 16-byte tag.
   out must have room for len - 28 bytes.
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

#endif
