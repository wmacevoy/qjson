# QJSON

[![Tests](https://github.com/wmacevoy/qjson/actions/workflows/test.yml/badge.svg)](https://github.com/wmacevoy/qjson/actions/workflows/test.yml)

JSON superset with exact numerics, pattern matching, and SQL storage.

```bash
pip install qjson
npm install qjson
```

## Example: order tracking

```python
from qjson import parse, stringify, Unbound, is_bound
from qjson.sql import adapter
from qjson.query import select
from decimal import Decimal
import sqlite3

conn = sqlite3.connect(':memory:')
db = adapter(conn)
db['setup']()

# Store orders with exact prices
db['store']({'orders': [
    {'id': 1, 'item': 'book',   'price': Decimal('9.99'),   'delivered': True},
    {'id': 2, 'item': 'laptop', 'price': Decimal('999.99'), 'delivered': False},
    {'id': 3, 'item': 'pen',    'price': Decimal('1.50'),   'delivered': True},
]})
db['commit']()

# All delivered orders
select(conn, 1, '.orders[K]', '.orders[K].delivered == true')

# Orders over $10 (exact decimal comparison, no floating-point error)
select(conn, 1, '.orders[K]', '.orders[K].price > 10M')

# What's the status of order 2?
select(conn, 1, '.orders[K].delivered', '.orders[K].id == 2')
```

The same query runs inside the database — no application code needed:

```sql
-- SQLite (with qjson_ext loaded)
SELECT qjson FROM qjson_select
WHERE root_id = 1 AND select_path = '.orders[K]'
  AND where_expr = '.orders[K].delivered == true';

-- PostgreSQL
SELECT * FROM qjson_select(1, '.orders[K]',
    '.orders[K].delivered == true');
```

## What QJSON adds to JSON

```javascript
{
  price: 67432.50M,        // BigDecimal — exact base-10
  count: 9007199254740993N, // BigInt — beyond 2^53
  pi: 3.14159265358979L,   // BigFloat — arbitrary precision
  key: 0jSGVsbG8,          // blob (JS64 binary)
  query: ?X,               // unbound variable — matches anything
  /* nested /* block */ comments */
}
```

Valid JSON is valid QJSON.

## How it works

Every number projects to a `[lo, str, hi]` IEEE double interval.
`lo` and `hi` are indexed — 99.999% of comparisons resolve with
a single index scan.  The rest use libbf (SQLite) or NUMERIC
(PostgreSQL) for exact resolution.

```
42        → [42.0,  NULL, 42.0]        exact double
0.1M      → [0.099…, "0.1", 0.100…]   1-ULP bracket
```

Unbound variables (`?X`) project to `[-Inf, "?X", +Inf]` — they
match everything.  Same-name unbounds bind together: `?X == ?X`
is true, `?X != ?X` is false.

## Install

| Platform | Package | Encrypted storage |
|----------|---------|-------------------|
| Python | `pip install qjson` | `adapter('db', key='...')` |
| Node.js | `npm install qjson` | `qjsonSqlAdapter(db, {key:'...'})` |
| PostgreSQL | `docker pull ghcr.io/wmacevoy/qjson-postgres` | encrypted volume |
| Browser | [WASM example](examples/wasm/) | SQLCipher + OPFS |
| C | `#include "qjson.h"` | link sqlcipher |

## Tests

```bash
make test                                  # C + Python + SQLite
python3 test/test_qjson_sql.py --postgres  # + PostgreSQL
```

## Docs

[docs/qjson.md](docs/qjson.md) — full spec: types, grammar,
canonical form, SQL schema, query language, comparison semantics,
encryption at rest.

## License

MIT
