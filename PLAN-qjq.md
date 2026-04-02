# Plan: qjq — QJSON query tool

## Vision

One static binary. Parse, format, query. qjq is one possible host
for libqjson — a demo CLI that happens to be useful.

libqjson is the product: a C library and SQLite extension.
It does not embed a scripting language — the host brings its own.
qjq uses QuickJS. Other hosts (Fossil/Tcl, Redis/Lua, Python, Node,
browser/WASM) embed libqjson through their own language bindings.

```bash
echo '{salary: 130000M, tax_rate: 0.24M}' | qjq
echo '{edge: {[a,b],[b,c],[c,d]}}' | qjq --closure .edge
qjq --db ~/data.qjson --key "..." '.items[K]' -w '.items[K].price > 100M'
qjq --eval 'qjson_add("0.1", "0.2")'
```

## Architecture

```
libqjson (C library — the core product)
├── qjson.c + qjson_lex.c + qjson_parse.c  — Lemon parser, stringify, project, cmp
├── qjson_sqlite_ext.c                      — SQLite extension (all SQL functions)
├── libbf/                                  — exact arithmetic
├── qjson_crypto.c                          — optional (LibreSSL)
└── qjson_types.h                           — shared bitmask enum (with libbfxx)

qjq (~1MB static binary — one host for libqjson)
├── QuickJS          — JS engine (vendored, BigInt support)
├── qjson.js         — parse/stringify (runs in QuickJS)
├── libqjson         — all C functions
└── qjq.c           — CLI glue: args → QuickJS → stdout
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
  --key-file FILE    read encryption key from file

Keys (never CLI args — visible in ps):
  QJSON_KEY          env var: encryption key
  QJSON_KEY_FILE     env var: path to key file
  --key-file FILE    arg: path to key file (same as env)

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

## What exists (done)

- `native/qjq.c` — CLI glue (~170 lines)
- `vendor/quickjs/` — vendored QuickJS 2025-09-13
- `native/qjson_js.h` — embedded qjson.js (generated)
- `native/libbf_shim.c` — bridges libbf↔QuickJS cutils
- `make qjq` — builds 1.0MB binary, works today
- `qjson.js` — parser/serializer, runs in QuickJS
- `qjson.c` + `qjson_lex.c` + `qjson_parse.c` — Lemon parser, libqjson core
- `qjson_sqlite_ext.c` — all SQL functions, solver, closure
- `qjson_wasm_init.c` — auto-extension pattern

## What's next

- Phase 2: link SQLite, auto-register extension, `--db`, path queries
- Phase 3: SQLCipher + `--key` (link LibreSSL)
- Phase 4: solver + closure CLI flags
- Fix qjson.js BigDecimal/BigFloat handling (QuickJS 2025 dropped native support)
- Release workflow for static binaries

## Design principle

libqjson does not choose a trigger language.  qjq is one host (QuickJS).
Other hosts embed libqjson through their own language:

| Host | Language | Use case |
|------|----------|----------|
| qjq | QuickJS (JS) | CLI tool, pipe mode |
| Browser | WASM | Client-side encrypted storage |
| Fossil-like | Tcl (Jim/TH1) | SQLite-native scripting |
| Redis-like | Lua | In-process triggers |
| Python app | Python | `pip install qjson` |
| Node app | JS | `npm install qjson` |

## Not in scope for qjq

- Embedding a trigger language in libqjson (host's job)
- Interactive REPL (future, maybe)
- Network/sync
- GUI (stays browser-based)
