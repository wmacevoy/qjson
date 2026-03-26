/* qjson_crypto.c — crypto primitives using LibreSSL/OpenSSL.
 *
 * SHA-256, AES-256-GCM, HMAC-SHA256, HKDF, random bytes.
 * Shamir secret sharing using libbf + RAND_bytes.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include "qjson_crypto.h"
#include "libbf.h"

/* ── SHA-256 ─────────────────────────────────────────────── */

int qjson_sha256(const void *data, size_t len, void *out) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, out, NULL);
    EVP_MD_CTX_free(ctx);
    return 0;
}

/* ── AES-256-GCM ─────────────────────────────────────────── */

#define NONCE_LEN 12
#define TAG_LEN   16

int qjson_aes_encrypt(const void *plaintext, size_t len,
                      const void *key32, void *out) {
    unsigned char *p = (unsigned char *)out;

    /* Generate random nonce */
    if (RAND_bytes(p, NONCE_LEN) != 1) return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key32, p) != 1)
        { EVP_CIPHER_CTX_free(ctx); return -1; }

    int outlen;
    if (EVP_EncryptUpdate(ctx, p + NONCE_LEN, &outlen, plaintext, (int)len) != 1)
        { EVP_CIPHER_CTX_free(ctx); return -1; }

    int finlen;
    if (EVP_EncryptFinal_ex(ctx, p + NONCE_LEN + outlen, &finlen) != 1)
        { EVP_CIPHER_CTX_free(ctx); return -1; }
    outlen += finlen;

    /* Append tag */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN,
                            p + NONCE_LEN + outlen) != 1)
        { EVP_CIPHER_CTX_free(ctx); return -1; }

    EVP_CIPHER_CTX_free(ctx);
    return NONCE_LEN + outlen + TAG_LEN;
}

int qjson_aes_decrypt(const void *ciphertext, size_t len,
                      const void *key32, void *out) {
    if (len < NONCE_LEN + TAG_LEN) return -1;

    const unsigned char *p = (const unsigned char *)ciphertext;
    const unsigned char *nonce = p;
    const unsigned char *ct = p + NONCE_LEN;
    int ct_len = (int)(len - NONCE_LEN - TAG_LEN);
    const unsigned char *tag = p + NONCE_LEN + ct_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key32, nonce) != 1)
        { EVP_CIPHER_CTX_free(ctx); return -1; }

    int outlen;
    if (EVP_DecryptUpdate(ctx, out, &outlen, ct, ct_len) != 1)
        { EVP_CIPHER_CTX_free(ctx); return -1; }

    /* Set expected tag */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN,
                            (void *)tag) != 1)
        { EVP_CIPHER_CTX_free(ctx); return -1; }

    int finlen;
    int ret = EVP_DecryptFinal_ex(ctx, (unsigned char *)out + outlen, &finlen);
    EVP_CIPHER_CTX_free(ctx);

    if (ret != 1) return -1;  /* auth failure */
    return outlen + finlen;
}

/* ── Random ──────────────────────────────────────────────── */

int qjson_random_bytes(void *buf, size_t n) {
    RAND_bytes(buf, (int)n);
    return 0;
}

/* ── HMAC-SHA256 ─────────────────────────────────────────── */

int qjson_hmac_sha256(const void *data, size_t data_len,
                      const void *key, size_t key_len,
                      void *out) {
    unsigned int md_len = 32;
    HMAC(EVP_sha256(), key, (int)key_len,
         data, data_len, out, &md_len);
    return 0;
}

/* ── HKDF-SHA256 ─────────────────────────────────────────── */

int qjson_hkdf(const void *ikm, size_t ikm_len,
               const void *salt, size_t salt_len,
               const void *info, size_t info_len,
               void *out, size_t out_len) {
    /* HKDF-Extract: PRK = HMAC-SHA256(salt, ikm) */
    unsigned char prk[32];
    unsigned int prk_len = 32;
    if (!salt || salt_len == 0) {
        unsigned char zero_salt[32] = {0};
        HMAC(EVP_sha256(), zero_salt, 32, ikm, ikm_len, prk, &prk_len);
    } else {
        HMAC(EVP_sha256(), salt, (int)salt_len, ikm, ikm_len, prk, &prk_len);
    }

    /* HKDF-Expand: OKM = T(1) || T(2) || ... */
    unsigned char *p = (unsigned char *)out;
    unsigned char t[32] = {0};
    size_t t_len = 0;
    size_t done = 0;

    for (int i = 1; done < out_len; i++) {
        unsigned char counter = (unsigned char)i;
        HMAC_CTX *hctx = HMAC_CTX_new();
        HMAC_Init_ex(hctx, prk, 32, EVP_sha256(), NULL);
        if (t_len > 0) HMAC_Update(hctx, t, t_len);
        if (info && info_len > 0) HMAC_Update(hctx, info, info_len);
        HMAC_Update(hctx, &counter, 1);
        unsigned int md_len = 32;
        HMAC_Final(hctx, t, &md_len);
        HMAC_CTX_free(hctx);
        t_len = 32;

        size_t copy = out_len - done;
        if (copy > 32) copy = 32;
        memcpy(p + done, t, copy);
        done += copy;
    }

    return 0;
}

/* ── Shamir secret sharing (libbf + LibreSSL RAND_bytes) ── */

static void *_shamir_realloc(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (size == 0) { free(ptr); return NULL; }
    return realloc(ptr, size);
}

static int _hex_to_bf(bf_context_t *ctx, bf_t *r, const char *hex) {
    if (!hex || !hex[0]) return -1;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    bf_init(ctx, r);
    bf_atof(r, hex, NULL, 16, BF_PREC_INF, BF_RNDZ);
    return 0;
}

static char *_bf_to_hex(const bf_t *a) {
    size_t len;
    return bf_ftoa(&len, a, 16, BF_PREC_INF, BF_RNDZ | BF_FTOA_FORMAT_FREE);
}

static void _bf_modp(bf_context_t *ctx, bf_t *r, const bf_t *a, const bf_t *p) {
    bf_t q, rem;
    bf_init(ctx, &q); bf_init(ctx, &rem);
    bf_divrem(&q, &rem, a, p, BF_PREC_INF, BF_RNDZ, BF_DIVREM_EUCLIDIAN);
    bf_set(r, &rem);
    bf_delete(&q); bf_delete(&rem);
}

static void _bf_mulmod(bf_context_t *ctx, bf_t *r, const bf_t *a, const bf_t *b, const bf_t *p) {
    bf_t tmp; bf_init(ctx, &tmp);
    bf_mul(&tmp, a, b, BF_PREC_INF, BF_RNDZ);
    _bf_modp(ctx, r, &tmp, p);
    bf_delete(&tmp);
}

static void _bf_addmod(bf_context_t *ctx, bf_t *r, const bf_t *a, const bf_t *b, const bf_t *p) {
    bf_t tmp; bf_init(ctx, &tmp);
    bf_add(&tmp, a, b, BF_PREC_INF, BF_RNDZ);
    _bf_modp(ctx, r, &tmp, p);
    bf_delete(&tmp);
}

static void _bf_submod(bf_context_t *ctx, bf_t *r, const bf_t *a, const bf_t *b, const bf_t *p) {
    bf_t tmp; bf_init(ctx, &tmp);
    bf_sub(&tmp, a, b, BF_PREC_INF, BF_RNDZ);
    _bf_modp(ctx, r, &tmp, p);
    bf_delete(&tmp);
}

static void _extended_gcd(bf_context_t *ctx, bf_t *x_out, const bf_t *a, const bf_t *b) {
    bf_t old_r, r, old_s, s, q, tmp, tmp2;
    bf_init(ctx, &old_r); bf_set(&old_r, a);
    bf_init(ctx, &r);     bf_set(&r, b);
    bf_init(ctx, &old_s);  bf_set_si(&old_s, 1);
    bf_init(ctx, &s);      bf_set_si(&s, 0);
    bf_init(ctx, &q); bf_init(ctx, &tmp); bf_init(ctx, &tmp2);

    while (!bf_is_zero(&r)) {
        bf_divrem(&q, &tmp, &old_r, &r, BF_PREC_INF, BF_RNDZ, BF_DIVREM_EUCLIDIAN);
        bf_set(&old_r, &r); bf_set(&r, &tmp);
        bf_mul(&tmp2, &q, &s, BF_PREC_INF, BF_RNDZ);
        bf_sub(&tmp, &old_s, &tmp2, BF_PREC_INF, BF_RNDZ);
        bf_set(&old_s, &s); bf_set(&s, &tmp);
    }
    bf_set(x_out, &old_s);
    bf_delete(&old_r); bf_delete(&r); bf_delete(&old_s);
    bf_delete(&s); bf_delete(&q); bf_delete(&tmp); bf_delete(&tmp2);
}

static void _bf_divmod(bf_context_t *ctx, bf_t *r, const bf_t *num, const bf_t *den, const bf_t *p) {
    bf_t inv, n, d;
    bf_init(ctx, &inv); bf_init(ctx, &n); bf_init(ctx, &d);
    _bf_modp(ctx, &n, num, p);
    _bf_modp(ctx, &d, den, p);
    _extended_gcd(ctx, &inv, &d, p);
    _bf_modp(ctx, &inv, &inv, p);
    _bf_mulmod(ctx, r, &n, &inv, p);
    bf_delete(&inv); bf_delete(&n); bf_delete(&d);
}

static void _random_below(bf_context_t *ctx, bf_t *r, const bf_t *p) {
    char *phex = _bf_to_hex(p);
    if (!phex) return;
    size_t hex_len = strlen(phex);
    size_t byte_count = (hex_len + 1) / 2;
    free(phex);

    unsigned char *buf = malloc(byte_count);
    if (!buf) return;
    char *hex = malloc(byte_count * 2 + 1);
    if (!hex) { free(buf); return; }

    bf_init(ctx, r);
    for (;;) {
        RAND_bytes(buf, (int)byte_count);
        for (size_t i = 0; i < byte_count; i++)
            sprintf(hex + i * 2, "%02x", buf[i]);
        hex[byte_count * 2] = '\0';
        bf_atof(r, hex, NULL, 16, BF_PREC_INF, BF_RNDZ);
        if (bf_cmp_lt(r, p)) break;
    }
    free(buf); free(hex);
}

int qjson_shamir_split(const char *secret_hex, int minimum, int shares,
                       const char *prime_hex, char **out_keys) {
    if (!secret_hex || minimum < 1 || shares < minimum || !out_keys)
        return -1;

    bf_context_t ctx;
    bf_context_init(&ctx, _shamir_realloc, NULL);
    bf_t prime, secret;

    if (!prime_hex) prime_hex = QJSON_SHAMIR_DEFAULT_PRIME;
    if (_hex_to_bf(&ctx, &prime, prime_hex) != 0) goto fail;
    if (_hex_to_bf(&ctx, &secret, secret_hex) != 0) goto fail;
    if (!bf_cmp_lt(&secret, &prime)) goto fail;

    /* Build polynomial */
    bf_t *cs = malloc(sizeof(bf_t) * minimum);
    if (!cs) goto fail;
    bf_init(&ctx, &cs[0]); bf_set(&cs[0], &secret);
    for (int i = 1; i < minimum; i++)
        _random_below(&ctx, &cs[i], &prime);

    /* Evaluate at x = 1..shares */
    bf_t x, val;
    bf_init(&ctx, &x); bf_init(&ctx, &val);
    for (int i = 0; i < shares; i++) {
        bf_set_si(&x, i + 1);
        /* Horner's method */
        bf_set_si(&val, 0);
        for (int j = minimum - 1; j >= 0; j--) {
            _bf_mulmod(&ctx, &val, &val, &x, &prime);
            _bf_addmod(&ctx, &val, &val, &cs[j], &prime);
        }
        out_keys[i] = _bf_to_hex(&val);
        if (!out_keys[i]) {
            for (int j = 0; j < i; j++) free(out_keys[j]);
            bf_delete(&x); bf_delete(&val);
            for (int j = 0; j < minimum; j++) bf_delete(&cs[j]);
            free(cs); goto fail;
        }
    }

    bf_delete(&x); bf_delete(&val);
    for (int i = 0; i < minimum; i++) bf_delete(&cs[i]);
    free(cs);
    bf_delete(&prime); bf_delete(&secret);
    bf_context_end(&ctx);
    return 0;

fail:
    bf_delete(&prime); bf_delete(&secret);
    bf_context_end(&ctx);
    return -1;
}

int qjson_shamir_recover(const int *indices, const char **keys_hex, int count,
                         int target, const char *prime_hex,
                         char *out_hex, size_t out_size) {
    if (!indices || !keys_hex || count < 1 || !out_hex || out_size < 3)
        return -1;

    bf_context_t ctx;
    bf_context_init(&ctx, _shamir_realloc, NULL);
    bf_t prime;

    if (!prime_hex) prime_hex = QJSON_SHAMIR_DEFAULT_PRIME;
    if (_hex_to_bf(&ctx, &prime, prime_hex) != 0) {
        bf_context_end(&ctx); return -1;
    }

    bf_t *xs = malloc(sizeof(bf_t) * count);
    bf_t *ys = malloc(sizeof(bf_t) * count);
    if (!xs || !ys) { free(xs); free(ys); bf_delete(&prime); bf_context_end(&ctx); return -1; }

    for (int i = 0; i < count; i++) {
        bf_init(&ctx, &xs[i]); bf_set_si(&xs[i], indices[i]);
        if (_hex_to_bf(&ctx, &ys[i], keys_hex[i]) != 0) {
            for (int j = 0; j <= i; j++) { bf_delete(&xs[j]); bf_delete(&ys[j]); }
            free(xs); free(ys); bf_delete(&prime); bf_context_end(&ctx); return -1;
        }
    }

    bf_t xt, result;
    bf_init(&ctx, &xt); bf_set_si(&xt, target);
    bf_init(&ctx, &result);

    /* Lagrange interpolation */
    bf_t *nums = malloc(sizeof(bf_t) * count);
    bf_t *dens = malloc(sizeof(bf_t) * count);
    for (int i = 0; i < count; i++) {
        bf_init(&ctx, &nums[i]); bf_set_si(&nums[i], 1);
        bf_init(&ctx, &dens[i]); bf_set_si(&dens[i], 1);
        for (int j = 0; j < count; j++) {
            if (j == i) continue;
            bf_t diff; bf_init(&ctx, &diff);
            _bf_submod(&ctx, &diff, &xt, &xs[j], &prime);
            _bf_mulmod(&ctx, &nums[i], &nums[i], &diff, &prime);
            _bf_submod(&ctx, &diff, &xs[i], &xs[j], &prime);
            _bf_mulmod(&ctx, &dens[i], &dens[i], &diff, &prime);
            bf_delete(&diff);
        }
    }

    bf_t den; bf_init(&ctx, &den); bf_set_si(&den, 1);
    for (int i = 0; i < count; i++)
        _bf_mulmod(&ctx, &den, &den, &dens[i], &prime);

    bf_set_si(&result, 0);
    for (int i = 0; i < count; i++) {
        bf_t term, tmp; bf_init(&ctx, &term); bf_init(&ctx, &tmp);
        _bf_mulmod(&ctx, &term, &nums[i], &den, &prime);
        _bf_mulmod(&ctx, &term, &term, &ys[i], &prime);
        _bf_divmod(&ctx, &tmp, &term, &dens[i], &prime);
        _bf_addmod(&ctx, &result, &result, &tmp, &prime);
        bf_delete(&term); bf_delete(&tmp);
    }

    bf_t final; bf_init(&ctx, &final);
    _bf_divmod(&ctx, &final, &result, &den, &prime);

    char *hex = _bf_to_hex(&final);
    int rc = -1;
    if (hex && strlen(hex) + 1 <= out_size) {
        strcpy(out_hex, hex);
        rc = 0;
    }
    free(hex);

    bf_delete(&final); bf_delete(&den);
    for (int i = 0; i < count; i++) { bf_delete(&nums[i]); bf_delete(&dens[i]); }
    free(nums); free(dens);
    bf_delete(&xt); bf_delete(&result);
    for (int i = 0; i < count; i++) { bf_delete(&xs[i]); bf_delete(&ys[i]); }
    free(xs); free(ys);
    bf_delete(&prime);
    bf_context_end(&ctx);
    return rc;
}
