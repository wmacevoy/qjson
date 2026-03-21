# CLAUDE.md

## Project

QJSON — JSON superset with exact numerics (N/M/L suffixes, 0j blobs)
and interval-projected SQL storage.  Three layers: format (parse/stringify),
projection (`[lo, str, hi]` intervals), and SQL adapter (SQLite/SQLCipher/PG).

Implementations in C, JavaScript, and Python.  All produce identical results.

## Commands

```bash
# JavaScript tests
node test/test-qjson.js

# Python tests
python3 test/test_qjson.py

# C tests
cc -O2 -std=c11 -frounding-math -o test_qjson test/test_qjson.c native/qjson.c -lm && ./test_qjson

# C with libbf (exact directed rounding)
cc -O2 -std=c11 -DQJSON_USE_LIBBF -o test_qjson test/test_qjson.c native/qjson.c native/libbf/libbf.c native/libbf/cutils.c -lm && ./test_qjson
```

## Architecture

```
Format:      parse/stringify QJSON text ↔ in-memory values
Projection:  value → [lo, str, hi] IEEE double interval
Comparison:  qjson_cmp (4 lines: brackets → exact → decimal fallback)
SQL adapter: create/insert/query tables with projected columns
```

### Source modules

| Module | Role |
|--------|------|
| `src/qjson.js` / `src/qjson.py` | QJSON parser + serializer |
| `src/qsql.js` / `src/qsql.py` | Interval projection + SQL adapter |
| `native/qjson.h` / `native/qjson.c` | C: parse + stringify + project + cmp |
| `native/libbf/` | Vendored libbf (exact directed rounding) |

### Key concepts

**QJSON types**: N = BigInt, M = BigDecimal, L = BigFloat, 0j = blob (JS64).
Valid JSON is valid QJSON.

**Interval projection `[lo, str, hi]`**:
- `str` — exact string representation, NULL when lo == hi (exact double)
- `lo` — largest IEEE double <= exact value
- `hi` — smallest IEEE double >= exact value
- Exact doubles: lo == hi (point interval, 99.999% of values)
- Inexact: lo + 1 ULP == hi (1-ULP bracket)

**Comparison `qjson_cmp`**:
```c
if (a_hi < b_lo) return -1;                  // intervals separated
if (a_lo > b_hi) return  1;                  // intervals separated
if (a_lo == a_hi && b_lo == b_hi) return 0;  // both exact doubles
return qjson_decimal_cmp(a_str, a_len, b_str, b_len);
```

**SQL adapter**: configurable table prefix (default `qjson_`).
Tables: `{prefix}{name}_{arity}`.  Per arg: `arg{i}` TEXT,
`arg{i}_lo` REAL, `arg{i}_hi` REAL.

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
