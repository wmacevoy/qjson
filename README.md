# QJSON

[![Tests](https://github.com/wmacevoy/qjson/actions/workflows/test.yml/badge.svg)](https://github.com/wmacevoy/qjson/actions/workflows/test.yml)

JSON superset with exact numerics and interval-projected SQL storage.

```bash
pip install qjson          # Python
npm install qjson          # JavaScript
docker pull ghcr.io/wmacevoy/qjson-postgres  # PostgreSQL with QJSON
```

```javascript
{
  name: "thermocouple-7",       // unquoted keys
  offset: 0.003M,               // BigDecimal — exact base-10
  nonce: 42N,                   // BigInt
  calibration: 3.14159L,        // BigFloat — full precision
  key: 0jSGVsbG8,              // blob (JS64 binary)
  readings: [22.5, 23.1,],      // trailing commas
  /* nested /* block */ comments */
}
```

Valid JSON is valid QJSON.

## Four layers

**Format** — parse and stringify QJSON (C, JavaScript, Python).

**Projection** — map any QJSON value to `[lo, str, hi]` IEEE double
interval.  `roundDown` and `roundUp` bracket the exact value.
`str` preserves the exact representation when it's not an exact double.

**SQL adapter** — store any QJSON value tree in SQLite or PostgreSQL.
Normalized schema with configurable prefix (default `qjson_`).
Indexed interval columns for fast range queries.

**Query translator** — jq-like path expressions compiled to SQL
JOIN chains.  SELECT, UPDATE, WHERE with interval pushdown and
exact comparison via libbf (SQLite) or NUMERIC (PostgreSQL).

## Quick start

### Parse and stringify

```javascript
import { qjson_parse, qjson_stringify } from 'qjson';       // or './src/qjson.js'

var obj = qjson_parse('{ price: 67432.50M, ts: 1710000000N }');
qjson_stringify(obj);  // '{"price":"67432.50M","ts":1710000000N}'
```

```python
from qjson import parse, stringify

obj = parse('{ price: 67432.50M, ts: 1710000000N }')
stringify(obj)  # '{"price":67432.50M,"ts":1710000000N}'
```

### Store and query

```python
from qjson.sql import adapter
from qjson.query import select, update
from decimal import Decimal
import sqlite3

conn = sqlite3.connect(':memory:')
a = adapter(conn)
a['setup']()

# Store a document
root_id = a['store']({
    'name': 'sensor-array',
    'items': [
        {'t': 10, 'y': 1.5, 'label': 'cold'},
        {'t': 25, 'y': Decimal('2.718281828'), 'label': 'warm'},
        {'t': 35, 'y': 3.5, 'label': 'hot'},
        {'t': 50, 'y': 5.0, 'label': 'fire'},
    ]
})
a['commit']()

# SELECT — returns (value_id, bindings) tuples
results = select(conn, root_id, '.items[K]',
                 where_expr='.items[K].t > 30')
for vid, bindings in results:
    print(a['load'](vid))
# {'label': 'hot', 't': 35.0, 'y': 3.5}
# {'label': 'fire', 't': 50.0, 'y': 5.0}

# UPDATE — set y = 3 where t > 30
update(conn, root_id, '.items[K].y', 3,
       where_expr='.items[K].t > 30')

# AND / OR / NOT
results = select(conn, root_id, '.items[K].label',
    where_expr='.items[K].t > 20 AND .items[K].t < 40')
# → 'warm', 'hot'
```

### PostgreSQL (embedded translator)

```sql
-- Install the QJSON functions
\i sql/qjson_pg.sql

-- Query with QJSON output
SELECT qjson FROM qjson_select(1, '.items[K]', '.items[K].t > 30');
-- → '{"label":"hot","t":35,"y":3.5}'
-- → '{"label":"fire","t":50,"y":5}'

-- Get a specific field
SELECT qjson FROM qjson_select(1, '.items[K].label', '.items[K].t == 25');
-- → '"warm"'

-- AND / OR
SELECT qjson FROM qjson_select(1, '.items[K]',
    '.items[K].t > 20 AND .items[K].t < 40');

-- Reconstruct any stored value
SELECT qjson_reconstruct(1, 'qjson_');

-- Exact comparison (libbf-equivalent via NUMERIC)
SELECT qjson_decimal_cmp('3.141592653589793238', '3.141592653589793239');
-- → -1
```

### SQLite extension

```bash
make  # build qjson_ext.dylib/.so with libbf
```

```python
import sqlite3
conn = sqlite3.connect('mydb.sqlite')
conn.enable_load_extension(True)
conn.load_extension('./qjson_ext')

# Now qjson_cmp() and qjson_decimal_cmp() are available in SQL
conn.execute("""
    SELECT * FROM qjson_number
    WHERE qjson_cmp(lo, hi, str, ?, ?, ?) > 0
""", (query_lo, query_hi, query_str))
```

### SQLCipher (encrypted storage)

```python
# Python — pass key to the adapter
from qjson.sql import adapter

a = adapter('encrypted.db', key='my-secret-key')
a['setup']()
vid = a['store']({'sensitive': True, 'balance': Decimal('1000000.50')})
a['commit']()
# Data encrypted at rest, queries work identically
```

```javascript
// JavaScript — better-sqlite3-sqlcipher or similar
var adapter = qjsonSqlAdapter(db, { key: "my-secret-key" });
adapter.setup();
adapter.store({ sensitive: true });
```

```bash
# Build extension for SQLCipher
make qjson_ext_sqlcipher.dylib  # or .so on Linux
```

Compatible with [sqlcipher-libressl](https://github.com/nickoala/sqlcipher-libressl)
for native and WASM (browser) encrypted storage with OPFS or IndexedDB persistence.

### C API

```c
#include "qjson.h"

char buf[8192];
qjson_arena a;
qjson_arena_init(&a, buf, sizeof(buf));

qjson_val *v = qjson_parse(&a, text, len);
double lo, hi;
qjson_project("0.1", 3, &lo, &hi);  // [0.0999..., 0.1000...]

int cmp = qjson_cmp(a_lo, a_hi, a_str, a_len,
                     b_lo, b_hi, b_str, b_len);
```

## QJSON types

| Suffix | Type | Example | JS type | Python type |
|--------|------|---------|---------|-------------|
| (none) | JSON number | `42`, `3.14` | `number` | `int`/`float` |
| `N` | BigInt | `1710000000N` | `BigInt` | `BigInt` |
| `M` | BigDecimal | `67432.50M` | string fallback | `Decimal` |
| `L` | BigFloat | `3.14159L` | string fallback | `BigFloat` |
| `0j` | Blob | `0jSGVsbG8` | `{$qjson:"blob"}` | `Blob` |

Plus: line/block/nested comments, trailing commas, unquoted keys.

## Query language

jq-like path expressions → SQL JOIN chains.

| Syntax | Meaning |
|--------|---------|
| `.key` | object child by key |
| `[n]` | array index |
| `[K]` | variable binding (uppercase, shared across SELECT/WHERE) |
| `.[]` | all array elements |

WHERE: `==`, `!=`, `>`, `>=`, `<`, `<=`, `AND`, `OR`, `NOT`, `()`.

Results include both `value_id` (for further SQL operations) and
reconstructed QJSON text.

## Project layout

```
src/
  qjson.js / qjson.py          QJSON parser + serializer
  qjson-sql.js / qjson_sql.py  SQL adapter (SQLite + PostgreSQL)
  qjson_query.py                Query translator (path → SQL)

native/
  qjson.h / qjson.c            C: parse + stringify + project + cmp
  qjson_sqlite_ext.c            SQLite extension (libbf)
  libbf/                        Vendored (exact directed rounding)

sql/
  qjson_pg.sql                  PostgreSQL: translator + reconstruct

test/
  test-qjson.js                 JS format tests
  test_qjson.py                 Python format tests
  test_qjson.c                  C format + projection tests
  test_qjson_sql.py             SQL adapter tests (SQLite + PG)

examples/
  wasm/                         Browser demo: QJSON + encrypted SQLCipher WASM
```

## Run tests

```bash
# Format tests
node test/test-qjson.js
python3 test/test_qjson.py

# SQL adapter (SQLite)
python3 test/test_qjson_sql.py

# SQL adapter (SQLite + PostgreSQL)
docker compose up -d postgres
python3 test/test_qjson_sql.py --postgres
docker compose down

# Build SQLite extension + C tests
make test

# C only
cc -O2 -std=c11 -frounding-math -o test_qjson test/test_qjson.c native/qjson.c -lm && ./test_qjson
```

## Encryption at rest

| Platform | Encryption | How |
|----------|-----------|-----|
| **SQLite / embedded** | SQLCipher (page-level) | `key='...'` parameter |
| **Browser** | SQLCipher WASM + OPFS/IndexedDB | `QJSONDatabase.open({key: '...'})` |
| **PostgreSQL** | Encrypted volume (LUKS, cloud EBS/PD) | Container with encrypted data dir |

Column-level encryption (pgcrypto) must **not** be used — it would
break interval indexing.  Encryption must be below the SQL layer.

```bash
# Production PostgreSQL with encrypted volume
docker compose up -d postgres-encrypted
```

## Release artifacts

Tagged releases (`v*`) produce:

| Artifact | Contents |
|----------|----------|
| `qjson_ext-{platform}.so/dylib` | SQLite extension (linux-x64, linux-arm64, macos-arm64, macos-x64) |
| PyPI `qjson` | `pip install qjson` — parser, SQL adapter, query translator |
| npm `qjson` | `npm install qjson` — ES5 parser + SQL adapter |
| `ghcr.io/.../qjson-postgres` | Docker image — PG 16 with QJSON functions pre-installed |
| `qjson-c.tar.gz` | C library + libbf + Makefile |
| `qjson_pg.sql` | PostgreSQL functions (translator + reconstruct) |
| `qjson-wasm.tar.gz` | Browser adapter for SQLCipher WASM |

## Documentation

| Doc | Scope |
|-----|-------|
| `docs/qjson.md` | Full spec: types, grammar, canonical form, SQL schema, query language, encryption |

## License

MIT
