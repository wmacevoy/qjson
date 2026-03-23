# QJSON WASM Examples

Encrypted QJSON storage and exact arithmetic in the browser,
powered by SQLCipher WASM with the QJSON extension (libbf)
compiled in.

## Quick start

```bash
# 1. Download the WASM package from the release
curl -sL https://github.com/wmacevoy/qjson/releases/download/v1.0.0/qjson-sqlcipher-wasm.tar.gz | tar xz

# 2. Copy an example into the same directory
cp examples/wasm/mortgage.html .

# 3. Serve (OPFS requires secure context)
python3 -m http.server 8080
# open http://localhost:8080/mortgage.html
```

## Examples

### mortgage.html

Exact mortgage calculator with constraint solver.
Leave one field blank — the solver fills it in via
`qjson_solve_*` SQL functions (libbf arithmetic).

- Solve for payment, principal, months, or rate
- All math runs inside SQLCipher (no JS arithmetic)
- Encrypted at rest via OPFS or IndexedDB

### index.html

QJSON storage demo — store, load, reconstruct, query,
export, shred.

## What's in the WASM package

The release artifact `qjson-sqlcipher-wasm.tar.gz` contains:

| File | Role |
|------|------|
| `sqlcipher.js` + `sqlcipher.wasm` | SQLCipher + QJSON extension (one binary) |
| `sqlcipher-worker.js` | Web Worker with OPFS/IndexedDB persistence |
| `sqlcipher-api.js` | Unified async API: `SQLCipher.open()` |
| `qjson-wasm.js` | QJSON adapter (store/load/reconstruct) |

All QJSON SQL functions are compiled in — no `load_extension()`:

- `qjson_solve_add/sub/mul/div/pow` — constraint solver
- `qjson_add/sub/mul/div/pow/sqrt/exp/log/sin/cos/...` — arithmetic
- `qjson_cmp_lt/le/eq/ne/gt/ge` — comparison
- `qjson_reconstruct` — value → QJSON text
- `qjson_select` — path query with WHERE

## Building from source

If you need to rebuild the WASM binary:

```bash
# Requires: emscripten, cmake, tcl
./wasm/build.sh ../sqlcipher-libressl
# Output: wasm/dist/
```

Or use the CI: the release workflow builds it automatically
on every tagged release.
