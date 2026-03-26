# CLAUDE.md

## Project

QJSON — JSON superset with exact numerics (N/M/L suffixes, 0j blobs,
? unbound variables) and interval-projected SQL storage.  Five layers:
format, projection, SQL adapter, query translator, constraint solver.

Implementations in C, JavaScript, and Python.  All produce identical results.

## Commands

```bash
# Build SQLite extension (libbf exact arithmetic + constraint solver)
make

# Python format tests
python3 test/test_qjson.py

# JavaScript format tests
node test/test-qjson.js

# C tests (with libbf)
make test_qjson && ./test_qjson

# SQL adapter tests (SQLite)
python3 test/test_qjson_sql.py

# SQL adapter tests (SQLite + PostgreSQL)
docker compose up -d postgres
python3 test/test_qjson_sql.py --postgres
docker compose down

# Mortgage calculator example
python3 examples/compound_interest.py

# PostgreSQL functions install
psql -h localhost -p 5433 -U qjson -d qjson_test -f sql/qjson_pg.sql

# All tests
make test
```

## Architecture

```
Format:      parse/stringify QJSON text ↔ in-memory values
Projection:  value → [lo, str, hi] IEEE double interval
             roundDown/roundUp — identical across C, JS, Python
Comparison:  qjson_cmp_lt/le/eq/ne/gt/ge (6 functions, returns 0/1)
Arithmetic:  qjson_add/sub/mul/div/pow + transcendentals (libbf exact)
Solver:      qjson_solve_add/sub/mul/div/pow (3-term constraint propagation)
SQL adapter: normalized 8-table schema — store/load/remove any QJSON value
Query:       jq-like path expressions → SQL JOIN chains (pure translation)
Reconstruct: value_id → canonical QJSON text
```

### Source modules

| Module | Role |
|--------|------|
| `src/qjson.js` / `src/qjson.py` | QJSON parser + serializer + `is_json` + `is_bound` |
| `src/qjson-sql.js` / `src/qjson_sql.py` | Interval projection + SQL adapter (SQLite/SQLCipher/PG) |
| `src/qjson_query.py` | Query translator (path → SQL compiler, SELECT + UPDATE) |
| `native/qjson.h` / `native/qjson.c` | C: parse + stringify + project + cmp + is_json + is_bound |
| `native/qjson_sqlite_ext.c` | SQLite extension: cmp, arithmetic, solver, reconstruct, select, closure |
| `native/libbf/` | Vendored libbf (exact directed rounding + arithmetic) |
| `sql/qjson_pg.sql` | PostgreSQL: query translator + reconstruct + comparison |
| `wasm/qjson_wasm_init.c` | WASM: auto-registers extension via sqlite3_auto_extension |

### Packaging

| Package | Install | Contents |
|---------|---------|----------|
| pip `qjson` | `pip install qjson` | `qjson`, `qjson.sql`, `qjson.query` |
| npm `qjson` | `npm install qjson` | `qjson.js`, `qjson-sql.js` |
| Docker `qjson-postgres` | `docker pull ghcr.io/wmacevoy/qjson-postgres` | PG 16 + QJSON functions |
| WASM | release artifact `qjson-sqlcipher-wasm.tar.gz` | SQLCipher + QJSON in one binary |

### Key concepts

**QJSON types**: N = BigInt, M = BigDecimal, L = BigFloat, 0j = blob (JS64),
? = Unbound variable (`?X`, `?"quoted name"`).  Valid JSON is valid QJSON.

**Complex keys**: Object keys can be any QJSON value, not just strings.
`{42: "answer"}`, `{[1,2]: "pair"}`.  Non-string keys produce QMap (Python)
or `{$qjson: "map", entries: [...]}` (JS).

**Set shorthand**: `{a, b, c}` → `{a: true, b: true, c: true}`.
No new type — sets are objects where all values are `true`.

**Bare identifiers**: Valid anywhere a value is expected, parsed as strings.
`[alice, bob]` → `["alice", "bob"]`.  Keywords require word boundary.

**Predicates**: `is_json(v)` — no QJSON extensions (pure JSON).
`is_bound(v)` — no Unbound variables (recursive).

**Interval projection `[lo, str, hi]`**:
- `lo` — largest IEEE double <= exact value (`roundDown`)
- `hi` — smallest IEEE double >= exact value (`roundUp`)
- `str` — exact string representation, NULL when lo == hi (exact double)
- Exact doubles: lo == hi (point interval, 99.999% of values)
- Inexact: lo + 1 ULP == hi (1-ULP bracket)
- Unbound: lo = -Inf, str = "?name", hi = +Inf (matches everything)

**Comparison `qjson_cmp_[op]`** — six functions, each returns 0 or 1:
```c
qjson_cmp_lt(a_type, a_lo, a_str, a_len, a_hi,
             b_type, b_lo, b_str, b_len, b_hi)  // also: le, eq, ne, gt, ge
```
Unbound: same-name → equal; different-name or vs concrete → all return 1.

**Exact arithmetic** (17 SQL functions, libbf-backed):
`qjson_add`, `sub`, `mul`, `div`, `pow`, `neg`, `abs`, `sqrt`,
`exp`, `log`, `sin`, `cos`, `tan`, `atan`, `asin`, `acos`, `pi`.
All take/return TEXT decimal strings.

**Transitive closure** — `qjson_closure(root_id, set_path, prefix)`:
computes reachable pairs from a set of 2-tuples using WITH RECURSIVE.
Available as Python function (`qjson_query.qjson_closure`) and C SQL function.

**Constraint solver** — `qjson_solve_add(a, b, c)`:
3-term constraints where args are INTEGER (value_id lvalue) or TEXT (literal).
Returns: 0=inconsistent, 1=solved, 2=consistent, 3=underdetermined.
Propagation via leaf folding — each constraint fires at most once.

**SQL adapter**: normalized relational schema with configurable prefix
(default `qjson_`).  8 tables: `{prefix}value`, `{prefix}number`,
`{prefix}string`, `{prefix}blob`, `{prefix}array`, `{prefix}array_item`,
`{prefix}object`, `{prefix}object_item`.
- `object_item` uses `key_id INTEGER` (FK to value) — keys are full QJSON values
- SQLite/SQLCipher: `REAL` (8-byte), `BLOB`, `INTEGER PRIMARY KEY`
- PostgreSQL: `DOUBLE PRECISION` (8-byte), `BYTEA`, `SERIAL PRIMARY KEY`
- SQLCipher: pass `key='...'` to adapter; `PRAGMA key` issued before any operation

**Query translator**: jq-like path expressions compiled to SQL JOINs.
- `.key` → object child
- `[n]` → array index
- `[K]` → variable binding (shared across SELECT/WHERE)
- `.[]` → iterate all elements
- No `..` (recursive descent) — every path is a fixed JOIN chain
- WHERE with interval pushdown + qjson_cmp_[op] exact fallback
- AND / OR / NOT / parentheses

**Encryption at rest**:
- SQLite/SQLCipher: page-level, user-supplied key
- Browser: SQLCipher WASM + OPFS/IndexedDB
- PostgreSQL: encrypted volume (LUKS, cloud EBS/PD) — NOT pgcrypto on interval columns

## Constraints

**`src/` files must be portable ES5-style JavaScript:**
- `var` only (no `let`/`const`)
- `function` only (no arrows)
- No template literals, destructuring, spread, for-of
- Targets: Node 12+, Bun, Deno, QuickJS, Duktape

**C files**: C11, zero malloc (arena-allocated), no global mutable state.

**Language-agnostic results**: JS, Python, and C produce identical
intervals and comparison results for all inputs.

## License

MIT
