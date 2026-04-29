# attic — reference material, not active code

Code parked here as part of refocusing qjson on its three layers:

- **bedrock**: format spec, grammar, SQL projection (`docs/qjson.md`)
- **foundation**: C reference impl (parse/stringify/project, libbf
  arithmetic, comparison, SQLite extension)
- **living space**: hosts (Python, JS, qjq) that bind to foundation

These files were prototypes or legacy reimplementations that lived in
the source tree. They are kept as a working reference while the proper
homes mature. They are **not built, not tested, and not part of the
qjson API**.

Do not link against any of this from foundation or living space.

## Inventory

### Datalog / query-layer prototypes (moved 2026-04-29)

The query language has its own repo:
[`../datalog`](https://github.com/wmacevoy/datalog). These were earlier
prototypes inside qjson; they should inform the datalog port and then
be deleted.

| File | Role | Successor |
|------|------|-----------|
| `native/qjson_resolve.{c,h}` | In-memory unification + pattern matching | `../datalog/native/datalog.c` |
| `native/qjson_db.{c,h}` | Reactive store: assert/retract/signal/freeze, watchers | `../datalog/native/datalog.c` (engine) |
| `native/qjson_solve_ext.c` | SQLite functions: `qjson_solve_{add,sub,mul,div,pow,sqrt,exp,log,sin,cos,tan,asin,acos,atan}` + `qjson_solve` (expression parser & propagator) | datalog resolver calls qjson arithmetic primitives (`qjson_add` etc.) |
| `test/test_resolve.c` | Resolver tests (grandparent, joins) | datalog test suite |
| `test/test_brain.c` | Ephemeral / freeze / signal tests | datalog test suite |
| `test/test_reactive.c` | watch/unwatch notification tests | datalog test suite |
| `test/test_solve.c` | Constraint-solver tests (compound interest, inverse ops) | datalog test suite |

### Crypto (moved 2026-04-29)

Per `PLAN-wyatt-migration.md`, crypto belongs in an application layer,
not in the database extension. The database layer should be identical
SQLite ↔ PostgreSQL; crypto-as-SQL-functions breaks that.

| File | Role |
|------|------|
| `native/qjson_crypto.{c,h}` | SHA-256, HMAC, AES-256-CBC+HMAC, HKDF, Shamir, base64{,url}, JWT (HS256). Requires LibreSSL. |
| `native/qjson_crypto_ext.c` | SQL wrappers for the above (`qjson_sha256`, `qjson_encrypt`, etc.) — extracted from the `#ifdef QJSON_USE_CRYPTO` block of `qjson_sqlite_ext.c`. |

### Legacy host reimplementations (moved 2026-04-29)

These reimplemented format, SQL adapter, and query translation in
JS/Python instead of binding to libqjson. Their replacement is a thin
binding (WASM for browser/Node, FFI for Python) that calls libqjson
for parse, project, compare, and arithmetic.

| File | Role | Replacement plan |
|------|------|------------------|
| `src/qjson.js` | ES5 standalone parser/stringifier | WASM build of libqjson + JS shim |
| `src/qjson-sql.js` | Browser SQLite/SQLCipher adapter | Same WASM, calling foundation's SQL extension |
| `src/qjson.py` | Pure-Python parser/stringifier (BigInt/BigDecimal/BigFloat/Blob/QMap/Unbound) | `pip install qjson` with native bindings to libqjson |
| `src/qjson_sql.py` | Python SQLite + PostgreSQL adapter | Native binding |
| `src/qjson_query.py` | Path → SQL JOIN compiler (also `qjson_closure` Python helper) | Move into foundation as the C `qjson_select` already does most of this; Python binding is thin wrapper |
| `src/qjq.py` | Python CLI host using the above | Replaced by a real foundation-bound CLI |
| `test/test_qjson.py` | Tests `src/qjson.py` (legacy parser) | C facts suite is the foundation truth |
| `test/test_qjson_sql.py` | Tests `qjson_sql.py` + `qjson_query.py` | C extension tests + future binding tests |
| `test/test_cross_comparison.py` | Tests cross-path numeric comparison via legacy adapter | Future binding tests |
| `test/test_projection_equivalence.py` | Compares Python projection to libbf — useful idea, but currently tests legacy code | Reimplement against foundation when bindings exist |
| `test/test-qjson.js` | Tests `src/qjson.js` | Future binding tests |

### qjq C host + JS embed (moved 2026-04-29)

The previous qjq CLI was a "legacy host": it embedded `src/qjson.js`
into a QuickJS interpreter rather than binding to libqjson. When
rebuilt as a foundation host (per `PLAN-qjq.md`), it should drop the
embed entirely.

| File | Role |
|------|------|
| `native/qjq.c` | QuickJS host: reads stdin, evaluates `qjson.js`, formats result. Should be replaced by a host that calls libqjson directly via FFI. |
| `native/embed_js.sh` | Build helper that wraps a JS file as a C string for embedding. Becomes unnecessary when the legacy JS goes away. |
| `native/libbf_shim.c` | QuickJS↔libbf glue (QuickJS renamed `dbuf_putc → __dbuf_putc`). Only needed because qjq builds libbf against QuickJS's `cutils.h` instead of libbf's own. |

The `wasm/` build script (`wasm/build-wasm.sh`) was wired to copy
`src/qjson.js` into the WASM bundle. It's now disabled with an
explanatory `exit 1` until the WASM build targets libqjson directly.
`wasm/qjson_wasm_init.c` is foundation code (registers the SQLite
extension via `sqlite3_auto_extension`) and stays in tree.

## When to delete

- The datalog/query-layer items can go once `../datalog` covers the
  same ground with passing tests.
- The crypto items can go once they are reborn as an application-layer
  library (wyatt or successor).
- The legacy host items can go once their replacement bindings exist
  and are tested against the foundation.
