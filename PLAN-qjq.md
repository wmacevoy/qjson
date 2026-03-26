# Plan: qjq — QJSON query tool (v2.0)

## Vision

One static binary. Your data encrypted. Query with paths,
solve constraints, compute closures. 2MB, no dependencies.

```bash
echo '{salary: 130000M, tax_rate: 0.24M}' | qjq
echo '{edge: {[a,b],[b,c],[c,d]}}' | qjq --closure .edge
qjq --db ~/data.qjson --key "..." '.items[K]' -w '.items[K].price > 100M'
qjq --solve '.take_home == .salary * (1 - .tax_rate)' < income.qjson
qjq --eval 'qjson_add("0.1", "0.2")'
```

## Architecture

```
qjq (~2MB static binary)
├── QuickJS          — JS engine, native BigInt/BigDecimal/BigFloat
├── qjson.js         — parse/stringify (runs unmodified in QuickJS)
├── qjson.c + libbf  — projection, comparison, 14 solvers
├── SQLCipher         — encrypted SQLite (page-level, user key)
├── LibreSSL crypto  — AES-256 for SQLCipher
└── qjq.c           — CLI glue: args → QuickJS → SQLCipher → stdout
```

## Build dependencies

| Component | Source | Size |
|-----------|--------|------|
| QuickJS | submodule `quickjs/` | ~300K |
| SQLCipher | submodule `sqlcipher-libressl/` (existing) | ~500K |
| LibreSSL | build dep (existing CI setup) | ~1M |
| qjson + libbf | `native/` (existing) | ~200K |
| **Total** | | **~2MB static** |

## Phases

### Phase 1: parse/stringify (no SQL)

- Add QuickJS as submodule
- `qjq.c`: init QuickJS, load `qjson.js`, expose CLI
- `make qjq` → static binary
- Modes: pipe format, pretty-print, bare ident parsing
- No database, no encryption — just a better `jq` for QJSON

### Phase 2: SQLite + queries

- Link SQLite amalgamation (plain, no encryption yet)
- Auto-register qjson extension via `sqlite3_auto_extension`
- `--db file` for persistent storage, `:memory:` default
- `--store .path` to store stdin at a path
- Path queries: `qjq '.items[K]' -w '.items[K].x > 5'`
- `--eval` for SQL expressions

### Phase 3: SQLCipher + encryption

- Replace SQLite with SQLCipher (link LibreSSL)
- `--key` for database encryption
- `--key-file` to read key from file
- `--shred` to securely delete
- Same API, just encrypted

### Phase 4: solver + closure

- `--solve 'formula'` for constraint solving
- `--closure .path` for transitive closure
- `--from` / `--to` filters

### Phase 5: release

- Cross-platform static builds (Linux glibc/musl, macOS x64/arm64)
- Release workflow: `qjq-linux-x64`, `qjq-macos-arm64`, etc.
- `brew install wmacevoy/tap/qjq` (maybe)
- Version: v2.0.0

## CLI design

```
qjq [OPTIONS] [PATH]

Input:
  stdin              QJSON text (default)
  --db FILE          persistent SQLCipher database
  --key KEY          encryption key
  --key-file FILE    read key from file

Modes:
  (no path)          parse stdin, emit canonical QJSON
  PATH               query: store stdin, select PATH
  -w EXPR            WHERE predicate for queries
  -s FORMULA         solve constraint
  -c SET_PATH        transitive closure
  --from VALUE       closure filter: starting node
  --to VALUE         closure filter: ending node
  -e EXPR            evaluate SQL expression
  --store PATH       store stdin at PATH in database

Output:
  -i N               indent (pretty-print)
  -r                 raw output (no quotes on strings)
  -c                 compact (default)

Database:
  --shred            securely delete database
  --export           dump database as QJSON
```

## What exists (reuse)

- `qjson.js` — parser/serializer, runs unmodified in QuickJS
- `qjson.c` + `libbf/` — C library, all projection/comparison
- `qjson_sqlite_ext.c` — all SQL functions, solver, closure
- `qjson_wasm_init.c` — auto-extension pattern (reuse for native)
- `sqlcipher-libressl/` — existing submodule, proven build
- `wasm/build.sh` — reference for linking everything together
- `.github/workflows/release.yml` — multi-platform build patterns

## What's new

- `quickjs/` submodule
- `qjq.c` — ~500 lines of CLI glue
- `Makefile` target: `make qjq`
- Release workflow additions for qjq binaries

## Not in scope

- Interactive REPL (future, maybe)
- Network/sync (lives in wyatt)
- GUI (stays browser-based)
