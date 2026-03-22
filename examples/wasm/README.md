# QJSON + SQLCipher WASM Example

Encrypted QJSON storage in the browser using SQLCipher WebAssembly.

## Setup

1. Build sqlcipher-libressl WASM (or download release artifacts):

```bash
cd ../sqlcipher-libressl
# follow WASM build instructions in that repo
```

2. Copy WASM files to this directory:

```bash
cp ../sqlcipher-libressl/wasm/dist/sqlcipher.js .
cp ../sqlcipher-libressl/wasm/dist/sqlcipher.wasm .
cp ../sqlcipher-libressl/wasm/sqlcipher-worker.js .
cp ../sqlcipher-libressl/wasm/sqlcipher-api.js .
```

3. Serve with any HTTP server (OPFS requires secure context):

```bash
python3 -m http.server 8080
# open http://localhost:8080
```

## What it demonstrates

- **Encrypted storage**: All QJSON data encrypted at rest with SQLCipher
- **Exact numerics**: BigInt (`9007199254740993N`), BigDecimal (`0.003M`), BigFloat (`3.14159...L`) survive the full round-trip
- **QJSON reconstruction**: Stored values reconstructed as canonical QJSON text
- **Interval projection**: `[lo, str, hi]` columns visible in direct SQL queries
- **Persistence**: OPFS (durable per COMMIT) or IndexedDB (durable on save)
- **Secure deletion**: Shred button overwrites all data with random bytes

## Files

| File | Role |
|------|------|
| `index.html` | Demo page with tests |
| `qjson-wasm.js` | QJSON adapter wrapping SQLCipher unified API |
| `sqlcipher-api.js` | SQLCipher v0.2.0 unified API (from sqlcipher-libressl) |
| `sqlcipher-worker.js` | Web Worker with OPFS/IndexedDB backends |
| `sqlcipher.js` + `.wasm` | Compiled SQLCipher (from sqlcipher-libressl) |
