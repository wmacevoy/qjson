# QJSON

[![Tests](https://github.com/wmacevoy/qjson/actions/workflows/test.yml/badge.svg)](https://github.com/wmacevoy/qjson/actions/workflows/test.yml)

JSON superset with arbitrary-precision numerics (N/M/L), pattern matching, and SQL storage.

```bash
pip install qjson
npm install qjson
```

## Example: compound interest

FV = PV × (1 + r)<sup>n</sup> — store 3 of 4 values, leave one
as `?` (unbound).  The constraint solver fills it in.

```python
from qjson import parse, Unbound, is_bound
from qjson.sql import adapter
from qjson.query import select
from decimal import Decimal
import sqlite3

conn = sqlite3.connect(':memory:')
conn.enable_load_extension(True)
conn.load_extension('./qjson_ext')

db = adapter(conn)
db['setup']()

# Store with one unknown
root = db['store']({
    'present': Decimal('10000'), 'rate': Decimal('0.05'),
    'periods': 10, 'future': Unbound('FV'),
})
db['commit']()

# Query with arithmetic in WHERE
results = select(conn, root, '.present',
    where_expr='.present * POWER(1 + .rate, .periods) > 16000',
    has_ext=True)
# → returns row (10000 * 1.05^10 = 16288.95 > 16000)
```

Arithmetic in WHERE expressions:
```
.future == .present * (1 + .rate) ** .periods
.circumference == 2 * PI() * .radius
SQRT(.area) > 10
```

Functions: `POWER`, `SQRT`, `EXP`, `LOG`, `SIN`, `COS`, `TAN`,
`ATAN`, `ASIN`, `ACOS`, `PI`. Arbitrary precision via libbf.

## What QJSON adds to JSON

```javascript
{
  price: 67432.50M,        // BigDecimal — exact base-10
  count: 9007199254740993N, // BigInt — beyond 2^53
  pi: 3.14159265358979L,   // BigFloat — arbitrary precision
  key: 0jSGVsbG8,          // blob (JS64 binary)
  query: ?X,               // unbound — matches anything
  /* nested /* block */ comments */
}

// Complex keys — any value can be a key
{42: "answer", [1, 2]: "pair"}

// Set shorthand — sugar for {a: true, b: true, ...}
{alice, bob, carol}

// Datalog facts — sets of tuples
{[alice, bob], [bob, carol]}
```

Valid JSON is valid QJSON.

## Install

| Platform | Package | Encrypted storage |
|----------|---------|-------------------|
| Python | `pip install qjson` | `adapter('db', key='...')` |
| Node.js | `npm install qjson` | `qjsonSqlAdapter(db, {key:'...'})` |
| PostgreSQL | `docker pull ghcr.io/wmacevoy/qjson-postgres` | encrypted volume |
| Browser | [WASM](examples/wasm/) | SQLCipher + OPFS |
| C | `#include "qjson.h"` | link sqlcipher |

## Tests

```bash
make test                                  # C + Python + SQLite
python3 test/test_qjson_sql.py --postgres  # + PostgreSQL
python3 examples/compound_interest.py      # solver demo
```

## Docs

[docs/qjson.md](docs/qjson.md) — types, grammar, SQL schema,
query language, exact arithmetic, constraint solver, encryption.

## License

MIT
