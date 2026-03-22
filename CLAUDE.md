# CLAUDE.md

## Project

QJSON — JSON superset with exact numerics (N/M/L suffixes, 0j blobs)
and interval-projected SQL storage.  Four layers: format (parse/stringify),
projection (`[lo, str, hi]` intervals), SQL adapter (SQLite/SQLCipher/PostgreSQL),
and query translator (jq-like paths → SQL).

Implementations in C, JavaScript, and Python.  All produce identical results.

## Commands

```bash
# Build SQLite extension (libbf exact comparison)
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
Comparison:  qjson_cmp (4 lines: brackets → exact → decimal fallback)
SQL adapter: normalized 8-table schema — store/load/remove any QJSON value
Query:       jq-like path expressions → SQL JOIN chains (pure translation)
Reconstruct: value_id → canonical QJSON text (PG: qjson_reconstruct)
```

### Source modules

| Module | Role |
|--------|------|
| `src/qjson.js` / `src/qjson.py` | QJSON parser + serializer |
| `src/qjson-sql.js` / `src/qjson_sql.py` | Interval projection + SQL adapter (SQLite/SQLCipher/PG) |
| `src/qjson_query.py` | Query translator (path → SQL compiler, SELECT + UPDATE) |
| `native/qjson.h` / `native/qjson.c` | C: parse + stringify + project + cmp |
| `native/qjson_sqlite_ext.c` | SQLite extension: qjson_cmp + qjson_decimal_cmp via libbf |
| `native/libbf/` | Vendored libbf (exact directed rounding) |
| `sql/qjson_pg.sql` | PostgreSQL: query translator + reconstruct + exact comparison |

### Packaging

| Package | Install | Contents |
|---------|---------|----------|
| pip `qjson` | `pip install qjson` | `qjson`, `qjson.sql`, `qjson.query` |
| npm `qjson` | `npm install qjson` | `qjson.js`, `qjson-sql.js` |
| Docker `qjson-postgres` | `docker pull ghcr.io/wmacevoy/qjson-postgres` | PG 16 + QJSON functions |

Package structure: `qjson/` (pip), `package.json` (npm), `Dockerfile` (Docker).

### Key concepts

**QJSON types**: N = BigInt, M = BigDecimal, L = BigFloat, 0j = blob (JS64).
Valid JSON is valid QJSON.

**Interval projection `[lo, str, hi]`**:
- `lo` — largest IEEE double <= exact value (`roundDown`)
- `hi` — smallest IEEE double >= exact value (`roundUp`)
- `str` — exact string representation, NULL when lo == hi (exact double)
- Exact doubles: lo == hi (point interval, 99.999% of values)
- Inexact: lo + 1 ULP == hi (1-ULP bracket)

**Comparison `qjson_cmp`**:
```c
if (a_hi < b_lo) return -1;                  // intervals separated
if (a_lo > b_hi) return  1;                  // intervals separated
if (a_lo == a_hi && b_lo == b_hi) return 0;  // both exact doubles
return qjson_decimal_cmp(a_str, a_len, b_str, b_len);
```

**SQL adapter**: normalized relational schema with configurable prefix
(default `qjson_`).  8 tables: `{prefix}value`, `{prefix}number`,
`{prefix}string`, `{prefix}blob`, `{prefix}array`, `{prefix}array_item`,
`{prefix}object`, `{prefix}object_item`.
- SQLite/SQLCipher: `REAL` (8-byte), `BLOB`, `INTEGER PRIMARY KEY`
- PostgreSQL: `DOUBLE PRECISION` (8-byte), `BYTEA`, `SERIAL PRIMARY KEY`
- SQLCipher: pass `key='...'` to adapter; `PRAGMA key` issued before any operation

**Query translator**: jq-like path expressions compiled to SQL JOINs.
- `.key` → object child
- `[n]` → array index
- `[K]` → variable binding (shared across SELECT/WHERE)
- `.[]` → iterate all elements
- No `..` (recursive descent) — every path is a fixed JOIN chain, pure translation
- WHERE with interval pushdown + libbf/NUMERIC exact fallback
- AND / OR / NOT / parentheses

**Database extensions**:
- SQLite: `qjson_ext.dylib/.so` — `qjson_cmp()`, `qjson_decimal_cmp()` via libbf
- PostgreSQL: `sql/qjson_pg.sql` — `qjson_select()`, `qjson_reconstruct()`,
  `qjson_cmp()`, `qjson_decimal_cmp()` via PG NUMERIC

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
