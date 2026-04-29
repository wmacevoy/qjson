# CLAUDE.md

## Project

QJSON — a clean superset of JSON with arbitrary-precision numerics
(`N`/`M`/`L` suffixes), binary blobs (`0j`), unbound variables (`?`),
and a specified projection into SQL with libbf-backed exact comparison.

## Layers

The project is organized in three layers. **Do not blur the boundaries.**

### Bedrock — definitions

- **What QJSON is**: format, types, grammar, canonical form
- **How it projects to SQL**: `[lo, str, hi]` interval schema, comparison semantics
- **Type widening rule**: `max(a, b)` over the bitmask type enum

The spec lives in [`docs/qjson.md`](docs/qjson.md). Bedrock is documentation,
not code; it is the source of truth that foundation and living-space must agree on.

The query language is its own bedrock, in its own repo:
**[`../datalog`](https://github.com/wmacevoy/datalog)** (sibling checkout).
Pattern matching, unification, equation solving, assert/retract/signal,
reactive watchers — all of that belongs there. qjson exposes arithmetic
and comparison primitives that datalog calls into.

### Foundation — C reference implementation

`native/` contains the canonical C library and SQLite extension:

| File | Role |
|------|------|
| `qjson_types.h` | Bitmask type enum (shared with libbfxx C++ peer) |
| `qjson.{h,c}` | Parse driver, stringify, project, compare, predicates |
| `qjson_lex.{h,c}` | Hand-written byte-stream lexer |
| `qjson_parse.y` → `qjson_parse.{c,h}` | Lemon grammar (~30 productions) |
| `qjson_parse_ctx.h` | Parse context (value stack, arena semantic actions) |
| `qjson_sqlite_ext.c` | SQLite extension: SQL adapter, comparison, arithmetic, projection, closure, query translator |
| `libbf/` | Vendored libbf (exact directed rounding + arithmetic) |
| `lemon/` | Vendored Lemon parser generator |

`vendor/quickjs/` is vendored only for the attic'd `qjq` host;
foundation does not depend on it.

Foundation rules: C11, arena-allocated (zero malloc for values),
no global mutable state, libbf is the baseline truth for arithmetic.

### Living space — hosts

Hosts bring their own language and bind to the foundation. **There are
no working hosts in this tree right now.** The previous hosts all
embedded legacy reimplementations rather than calling the foundation;
they have been parked in `attic/` until proper bindings are built.

| Host | Plan | Current state |
|------|------|---------------|
| `qjq` (CLI) | QuickJS + libqjson FFI | Attic: `attic/native/qjq.c` embeds `attic/src/qjson.js`. See `PLAN-qjq.md`. |
| Python | `pip install qjson`, native bindings to libqjson | Attic: `attic/src/qjson.py`, `qjson_sql.py`, `qjson_query.py`, `qjq.py` — legacy reimplementations. |
| Node / Browser | `npm install qjson`, WASM build of libqjson | Attic: `attic/src/qjson.js`, `qjson-sql.js`. WASM build script is broken pending real bindings. |
| WASM | `wasm/qjson_wasm_init.c` auto-registers via `sqlite3_auto_extension` | Source intact but `wasm/build-wasm.sh` disabled — references attic JS. |

**Rule**: a new host must call libqjson (foundation) for parse,
projection, comparison, and arithmetic. It must not reimplement them.

## Commands

```bash
make                       # qjson_ext.dylib (SQLite extension)
make test                  # C foundation tests (parse/stringify/project + facts)
make native/qjson_parse.c  # regenerate Lemon parser after editing .y

# qjson_ext_sqlcipher$(EXT_SUFFIX) is also available for SQLCipher headers
```

The Python and JS test suites that previously ran with `make test` are
in `attic/test/` — they tested the legacy reimplementations, not the
foundation, so they are not gated. The C suite (`./test_qjson` driven
by the facts library) is the foundation truth.

## Key concepts (bedrock summary)

**Types**: `null`, `boolean`, `number` (IEEE double), `string`, `array`,
`object`. QJSON adds `N` BigInt, `M` BigDecimal, `L` BigFloat,
`0j` blob (JS64), `?` unbound variable.

**Bitmask type enum**: type IDs encode groups; numeric ordering matches
type widening (`NUMBER < BIGINT < BIGFLOAT < BIGDECIMAL`). Shared with
the libbfxx C++ peer via `qjson_types.h`.

**Complex keys**: object keys can be any QJSON value, not just strings.
`{42: "answer"}`, `{[1,2]: "pair"}`. Non-string keys produce QMap
(Python) or `{$qjson: "map", entries: [...]}` (JS).

**Set shorthand**: `{a, b, c}` ≡ `{a: true, b: true, c: true}`.
No new type — sets are objects whose values are all `true`.

**Bare identifiers**: valid anywhere a value is expected, parsed as
strings. `[alice, bob]` ≡ `["alice", "bob"]`. Reserved keywords need
word boundary.

**Predicates**: `is_json(v)` — pure JSON, no QJSON extensions.
`is_bound(v)` — no unbound variables (recursive).

**Interval projection `[lo, str, hi]`**:
- `lo` = largest IEEE double ≤ exact value
- `hi` = smallest IEEE double ≥ exact value
- `str` = exact string representation, NULL when `lo == hi`
- Exact doubles: point interval (`lo == hi`), ~99.999% of values
- Inexact: 1-ULP bracket (`lo + 1ulp == hi`)
- Unbound: `lo = -Inf`, `str = "?name"`, `hi = +Inf`

**Comparison `qjson_cmp_[lt|le|eq|ne|gt|ge]`** — six SQL functions, each
returns 0 or 1. Unbound: same-name → equal; different-name or vs
concrete → all six return 1.

**Arbitrary-precision arithmetic** (libbf-backed SQL functions, all
take/return TEXT decimal strings):
`qjson_add`, `sub`, `mul`, `div`, `pow`, `neg`, `abs`, `sqrt`,
`exp`, `log`, `sin`, `cos`, `tan`, `atan`, `asin`, `acos`, `pi`.

**SQL adapter**: normalized 8-table schema with configurable prefix
(default `qjson_`): `{prefix}value`, `number`, `string`, `blob`,
`array`, `array_item`, `object`, `object_item`.
- `object_item.key_id INTEGER` (FK to value) — keys are full QJSON values
- SQLite/SQLCipher: `REAL` (8-byte), `BLOB`, `INTEGER PRIMARY KEY`
- SQLCipher: `key='...'` triggers `PRAGMA key` before any operation

**Query translator** (path → SQL JOINs, in `qjson_sqlite_ext.c`,
also as the attic Python reference at `attic/src/qjson_query.py`):
`.key` (object child), `[n]` (array index), `[K]` (variable binding,
shared across SELECT/WHERE), `.[]` (iterate). **No `..`** (recursive
descent) — every path is a fixed JOIN chain. WHERE supports interval
pushdown plus `qjson_cmp_*` exact fallback, with AND / OR / NOT / parens.

**Transitive closure** — `qjson_closure(root_id, set_path, prefix)`:
reachable pairs from a set of 2-tuples via `WITH RECURSIVE`.

**Encryption at rest**: SQLite/SQLCipher page-level with user-supplied
key; browser via SQLCipher WASM + OPFS/IndexedDB.

## Out of scope (moved out)

These belonged to the query layer or to legacy hosts and have been
moved or removed:

- **Query language, pattern matching, unification, equation solving,
  assert/retract/signal, reactive watchers** → live in
  [`../datalog`](https://github.com/wmacevoy/datalog).
- **Constraint solver SQL functions** (`qjson_solve_*`) → parked in
  [`attic/native/qjson_solve_ext.c`](attic/native/qjson_solve_ext.c).
- **In-memory resolver, reactive store, brain ops** → parked in
  [`attic/native/qjson_resolve.{c,h}`, `qjson_db.{c,h}`].
- **Crypto** (SHA-256, AES, HMAC, HKDF, Shamir, JWT) → parked in
  [`attic/native/qjson_crypto.{c,h}`](attic/) and the SQL wrappers in
  `attic/native/qjson_crypto_ext.c`. Belongs in an application layer
  per `PLAN-wyatt-migration.md`, not in the database extension.
- **Legacy host reimplementations** (`src/qjson.{js,py}`,
  `qjson{-,_}sql.{js,py}`, `qjson_query.py`, `qjq.py`) and their tests
  → parked in `attic/src/` and `attic/test/`. Hosts must call
  foundation, not reimplement format/projection/comparison.
- **`qjq` C host** → parked in `attic/native/qjq.c` because it embeds
  the legacy JS parser. To reactivate: replace the embed with FFI to
  libqjson. See `PLAN-qjq.md`.

## Constraints

- **Bedrock and foundation must agree.** If the spec says one thing
  and the C says another, fix one of them — never both quietly.
- **Hosts call foundation, not the spec directly.** No language should
  reimplement parsing, projection, or comparison; it should bind to libqjson.
- **`where`, `and`, `or`, `not`, `in` are reserved keywords** — quote them
  when used as data: `{"in": true}`.

## License

MIT
