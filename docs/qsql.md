# QSQL — Typed SQL storage for QJSON values

QSQL bridges QJSON values to SQL tables with typed columns
and interval arithmetic.  Each named collection gets its own table.
Arguments become indexed columns.  Exact QJSON numerics survive
the round-trip through IEEE 754 doubles via `[lo, str, hi]`
projection.

## From QJSON values to SQL rows

A record `price(btc, 67432.50M, 1710000000N)` becomes:

```sql
INSERT INTO "qjson_price_3" VALUES (
  '{"t":"c","f":"price","a":[...]}',      -- _key (full record, for restore)
  'btc',    NULL,       NULL,              -- arg0: atom
  '67432.50', 67432.5,    67432.5,         -- arg1: [lo, str, hi]
  '1710000000', 1710000000, 1710000000     -- arg2: [lo, str, hi]
);
```

The `_key` column holds the complete serialized record — QSQL
never needs to reconstruct a value from its columns.  The typed
columns exist purely for **indexed query pushdown**.

## Schema

Table name: `{prefix}{name}_{arity}`.  Default prefix: `qjson_`.

Per argument: 3 columns.

| Column | Type | Content |
|--------|------|---------|
| `arg{i}` | TEXT | value as string (atom name, exact numeric repr, blob) |
| `arg{i}_lo` | REAL | `ieee_double_round_down(exact_value)`, NULL for atoms |
| `arg{i}_hi` | REAL | `ieee_double_round_up(exact_value)`, NULL for atoms |

Indexes on `arg{i}` (equality) and `arg{i}_lo` (range).

```sql
CREATE TABLE "qjson_price_3" (
  _key     TEXT PRIMARY KEY,
  arg0     TEXT,  arg0_lo REAL, arg0_hi REAL,
  arg1     TEXT,  arg1_lo REAL, arg1_hi REAL,
  arg2     TEXT,  arg2_lo REAL, arg2_hi REAL
);
```

## Projection: `[lo, str, hi]`

Every numeric argument projects to three values:

- **str** — the exact string representation (`"67432.50"`,
  `"0.1"`, `"9007199254740993"`).  Always authoritative.
- **lo** — largest IEEE double <= exact value.
- **hi** — smallest IEEE double >= exact value.

| Value | lo | str | hi |
|-------|----|-----|----|
| `42` (exact double) | 42.0 | `"42"` | 42.0 |
| `67432.50M` (exact) | 67432.5 | `"67432.50"` | 67432.5 |
| `0.1M` (inexact) | round_down(0.1) | `"0.1"` | round_up(0.1) |
| `9007199254740993N` | 2^53 | `"9007199254740993"` | 2^53+2 |
| `2e308M` (overflow) | DBL_MAX | `"2e308"` | +Infinity |
| atom `btc` | NULL | `"btc"` | NULL |

Canonical implementation: `qjson_project()` in C using
`fesetround` + `strtod`, or libbf for exact directed rounding.
See `docs/qjson.md` for the type system.

## Comparison: `qjson_cmp`

```c
int qjson_cmp(a_lo, a_hi, a_str, a_len, b_lo, b_hi, b_str, b_len) {
    if (a_hi < b_lo) return -1;                  // intervals prove a < b
    if (a_lo > b_hi) return  1;                  // intervals prove a > b
    if (a_lo == a_hi && b_lo == b_hi) return 0;  // both exact, same double
    return qjson_decimal_cmp(a_str, a_len, b_str, b_len);
}
```

All six operators: `qjson_cmp(...) <op> 0`.

For SQL WHERE clauses, expand inline for index usage:

| Op | SQL expansion |
|----|---------------|
| `a < b`  | `(a_hi < b_lo) OR ((a_lo < b_hi) AND cmp(a,b) < 0)` |
| `a <= b` | `(a_hi <= b_lo) OR ((a_lo <= b_hi) AND cmp(a,b) <= 0)` |
| `a > b`  | `(a_lo > b_hi) OR ((a_hi > b_lo) AND cmp(a,b) > 0)` |
| `a >= b` | `(a_lo >= b_hi) OR ((a_hi >= b_lo) AND cmp(a,b) >= 0)` |
| `a == b` | `(a_hi >= b_lo AND b_hi >= a_lo) AND cmp(a,b) = 0` |
| `a != b` | `(a_hi < b_lo OR b_hi < a_lo) OR cmp(a,b) != 0` |

The interval branches use indexed REAL columns (99.999%).
`qjson_decimal_cmp` only fires in the overlap zone (~0.001%).

## Configurable prefix

The adapter takes a `prefix` option (default `"qjson_"`):

```javascript
qsqlAdapter(db, { prefix: "prices_" })
// → tables: prices_btc_3, prices_eth_3, ...

qsqlAdapter(db)
// → tables: qjson_price_3, qjson_threshold_4, ...
```

## Full round-trip

```
1. Application: store record price(btc, 67432.50M, 1710000000N)
2. Serialize:   record → _key JSON string
3. QSQL:        extract args → project [lo, str, hi] per arg
4. SQLite:      INSERT INTO qjson_price_3 (typed columns + _key)
5. Restart:     SELECT _key FROM qjson_price_3
6. Restore:     deserialize _key → original record
7. Query:       price(X, Y, Z) → results with repr preserved
8. Print:       67432.50M  (not 67432.499999... or 67432.500001...)
```

The exact QJSON representation survives the entire cycle.
SQL stores the doubles for fast indexed queries.  The
string column preserves what the user actually wrote.
