# Plan: Built-in crypto functions

## Motivation

Encryption is more foundational than algebra for most apps.
SQLCipher encrypts the whole database, but applications need
field-level encryption, content addressing, API auth, and
key management.  LibreSSL is already linked (for SQLCipher).
These functions make crypto a built-in, not an add-on.

## Functions

### Hashing
```sql
qjson_sha256(data)              -- blob or text → 32-byte blob
qjson_sha256_hex(data)          -- blob or text → 64-char hex string
```

### Authenticated encryption (AES-256-GCM)
```sql
qjson_encrypt(plaintext, key)   -- → nonce‖ciphertext‖tag (single blob)
qjson_decrypt(ciphertext, key)  -- → plaintext blob, or NULL on auth fail
```
Key must be 32 bytes.  12-byte nonce auto-generated.
Tag is 16 bytes appended.  Total overhead: 28 bytes.

### Random
```sql
qjson_random(n)                 -- → n cryptographic random bytes (blob)
qjson_random_hex(n)             -- → 2*n hex chars
```

### HMAC
```sql
qjson_hmac(data, key)           -- HMAC-SHA256 → 32-byte blob
```

### Key derivation
```sql
qjson_hkdf(ikm, salt, info, len)  -- HKDF-SHA256 → len-byte key blob
```

### Shamir secret sharing
```sql
qjson_shamir_split(secret_hex, M, N)  -- → QJSON array of N hex shares
qjson_shamir_recover(indices, keys)   -- → recovered secret hex
```
Uses libbf for modular arithmetic (already linked).
Ported from strata/shamir.c — libsodium replaced with
LibreSSL RAND_bytes.

### JWT (HS256)
```sql
qjson_jwt_sign(payload_json, secret)  -- → JWT string (HS256)
qjson_jwt_verify(jwt, secret)         -- → payload JSON, or NULL
```

## Implementation

All functions go in `native/qjson_sqlite_ext.c`, guarded by
`#ifdef QJSON_USE_CRYPTO` (set when building with LibreSSL).
The SQLCipher build and qjq binary always have them.

Shamir uses libbf (always available) + RAND_bytes from LibreSSL.

## Dependencies

- LibreSSL libcrypto (already linked for SQLCipher)
- libbf (already linked for exact arithmetic)
- No new dependencies

## Files

- `native/qjson_sqlite_ext.c` — all SQL function implementations
- `native/qjson_crypto.c` — crypto helpers (sha256, encrypt, etc.)
- `native/qjson_shamir.c` — ported from strata, LibreSSL random
- Tests in `test/test_qjson_sql.py`
