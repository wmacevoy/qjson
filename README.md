# QJSON

[![Tests](https://github.com/wmacevoy/qjson/actions/workflows/test.yml/badge.svg)](https://github.com/wmacevoy/qjson/actions/workflows/test.yml)

A JSON superset where `0.1 + 0.2 = 0.3`.

```bash
pip install qjson
npm install qjson
```

## The idea

JSON numbers are IEEE 754 doubles.  `0.1 + 0.2` gives
`0.30000000000000004`.  Financial data, sensor readings,
anything that needs decimal precision — JSON can't represent
it faithfully.

QJSON fixes this with **three suffixes**:

```javascript
67432.50M            // BigDecimal — exact base-10 decimal
9007199254740993N    // BigInt — arbitrary-precision integer
3.14159265358979L    // BigFloat — arbitrary-precision binary float
```

No new syntax to learn.  Valid JSON is valid QJSON.

## The trick

Every QJSON number — even one with 300 digits — gets stored as
a **three-column interval**: `[lo, str, hi]`.

- `lo` = largest IEEE double ≤ the exact value
- `hi` = smallest IEEE double ≥ the exact value
- `str` = the exact decimal string (NULL when `lo == hi`)

The SQL index works on the doubles. 99.999% of comparisons
resolve from the index alone.  The 0.001% that land in the
overlap zone fall through to exact string comparison.

**Arbitrary precision at indexed speed.**

## Solve backwards

Store 3 of 4 values, leave one as `?`.  The solver fills it in:

```
present = 10000M, rate = 0.05M, periods = 10, future = ?
  → future = 16288.946267774414M

present = ?, rate = 0.05M, periods = 10, future = 16288.95M
  → present = 10000.002290952...M
```

Same formula, any direction.  Works with SQRT, EXP, LOG,
SIN, COS, TAN and their inverses — all arbitrary precision
via [libbf](https://bellard.org/libbf/).

## Datalog in JSON

Sets are objects where all values are `true`:

```javascript
{
  parent: {[alice, bob], [bob, carol], [carol, dave]},
  edge:   {[a, b], [b, c], [c, d]}
}
```

Query with path expressions and variable bindings:

```python
# All parents
qjson_select(conn, root, '.parent[K]')

# Grandparent join: parent(X,Z) ∧ parent(Z,Y)
qjson_select(conn, root, '.parent[K1][0]',
    where_expr='.parent[K1][1] == .parent[K2][0]')

# Transitive closure: all reachable pairs
qjson_closure(conn, root, '.edge')
# → a→b, a→c, a→d, b→c, b→d, c→d
```

`[K]` iterates both arrays and sets.  `qjson_closure` uses
`WITH RECURSIVE` for fixpoint computation.  Handles cycles.

## Built-in crypto

With LibreSSL linked (SQLCipher build), 15 SQL functions:

```sql
-- Hash and verify
SELECT qjson_sha256('data');
SELECT qjson_hmac('data', 'key');

-- Encrypt individual fields (AES-256-GCM)
SELECT qjson_encrypt(ssn, key);
SELECT qjson_decrypt(encrypted_ssn, key);

-- Key management
SELECT qjson_random(32);                       -- crypto random key
SELECT qjson_hkdf('password', 'salt', '', 32); -- derive from password
SELECT qjson_shamir_split(secret_hex, 2, 3);   -- 2-of-3 threshold
SELECT qjson_shamir_recover('[1,3]', keys);    -- recover from any 2

-- JWT
SELECT qjson_jwt_sign('{"sub":"alice"}', secret);
SELECT qjson_jwt_verify(jwt, secret);          -- payload or NULL
```

Also: `qjson_base64_encode/decode`, `qjson_base64url_encode/decode`.

## What else

```javascript
{
  key: 0jSGVsbG8,          // blob (JS64 binary encoding)
  query: ?X,               // unbound variable — matches anything
  42: "answer",            // any value as object key
  /* nested /* block */ comments */
}
```

Bare identifiers are strings: `{name: alice}` is
`{"name": "alice"}`.

## Install

| Platform | Package | Encrypted storage |
|----------|---------|-------------------|
| Python | `pip install qjson` | `adapter('db', key='...')` |
| Node.js | `npm install qjson` | `qjsonSqlAdapter(db, {key:'...'})` |
| PostgreSQL | `docker pull ghcr.io/wmacevoy/qjson-postgres` | encrypted volume |
| Browser | [WASM](examples/wasm/) | SQLCipher + OPFS |
| C | `#include "qjson.h"` | link sqlcipher |

## Docs

- [Interactive tutorial](docs/tutorial.html) — WASM playground
- [Full reference](docs/qjson.md) — types, grammar, SQL schema, queries, solver
- [Graph closure example](examples/graph_closure.py) — Datalog-style reachability
- [Relational query example](examples/relational_query.py) — cross-path joins

## Tests

```bash
make test                                  # C + Python + SQLite
python3 test/test_qjson_sql.py --postgres  # + PostgreSQL
python3 examples/compound_interest.py      # solver demo
python3 examples/graph_closure.py          # closure demo
```

## License

MIT
