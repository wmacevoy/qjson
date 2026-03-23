# QJSON

[![Tests](https://github.com/wmacevoy/qjson/actions/workflows/test.yml/badge.svg)](https://github.com/wmacevoy/qjson/actions/workflows/test.yml)

JSON superset with exact numerics, pattern matching, and SQL storage.

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
import sqlite3

conn = sqlite3.connect(':memory:')
conn.enable_load_extension(True)
conn.load_extension('./qjson_ext')

db = adapter(conn)
db['setup']()

# All fields start unbound
doc = db['store']({
    'present': Unbound('PV'), 'rate': Unbound('r'),
    'periods': Unbound('n'),  'future': Unbound('FV'),
    'opr': Unbound('opr'),    'factor': Unbound('f'),
})

# User fills in 3 of 4
qjson_update(doc, '.present == 10000')
qjson_update(doc, '.rate == 0.05')
qjson_update(doc, '.periods == 10')

# Formula — same every time, works in any direction
qjson_update(doc, """
    .opr == 1 + .rate
    AND .factor == .opr ^ .periods
    AND .future == .present * .factor
""")

if is_bound(db['load'](doc)):
    print(db['load'](doc)['future'])  # → 16288.9462...
```

Each unbound value must appear in exactly one constraint
for the solver to isolate it.  The three constraints above
have no fan-out — every variable appears once — so any
single unknown can be solved:

| Unknown | Inverse operation |
|---------|-------------------|
| future | `present × factor` |
| present | `future / factor` |
| rate | `factor^(1/n)` then `opr - 1` (nth root) |
| periods | `log(factor) / log(opr)` (logarithm) |

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
