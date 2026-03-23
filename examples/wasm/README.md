# QJSON WASM

Encrypted QJSON storage and exact arithmetic in the browser,
powered by SQLCipher WASM with the QJSON extension (libbf)
compiled in.

## Quick start

```bash
# Download the WASM package from the release
curl -sL https://github.com/wmacevoy/qjson/releases/latest/download/qjson-sqlcipher-wasm.tar.gz | tar xz

# Serve (OPFS requires secure context)
python3 -m http.server 8080
```

## What's in the WASM package

The release artifact `qjson-sqlcipher-wasm.tar.gz` contains:

| File | Role |
|------|------|
| `sqlcipher.js` + `sqlcipher.wasm` | SQLCipher + QJSON extension (one binary) |
| `sqlcipher-worker.js` | Web Worker with OPFS/IndexedDB persistence |
| `sqlcipher-api.js` | Unified async API: `SQLCipher.open()` |
| `qjson-wasm.js` | QJSON adapter (store/load/reconstruct) |

All QJSON SQL functions are compiled in — no `load_extension()`:

```sql
-- Constraint solver (one formula, any direction)
SELECT qjson_solve(root_id,
    '.future == .present * POWER(1 + .rate, .periods)');

-- Exact arithmetic
SELECT qjson_mul('0.1', '0.2');  -- '0.02' (exact)

-- Comparison, query, reconstruct
SELECT qjson_reconstruct(root_id);
```

## Building from source

```bash
./wasm/build.sh ../sqlcipher-libressl
# Output: wasm/dist/
```

Or use the CI: the release workflow builds it automatically.
