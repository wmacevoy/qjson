# QJSON

JSON superset with exact numerics and interval-projected SQL storage.

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

## Three layers

**Format** — parse and stringify QJSON (C, JavaScript, Python).

**Projection** — map any QJSON value to `[lo, str, hi]` IEEE double
interval.  `lo` and `hi` bracket the exact value.  `str` preserves
the exact representation.  `qjson_cmp` compares two projected values
in 4 lines.

**SQL adapter** — store projected values in SQLite, SQLCipher, or
PostgreSQL.  Configurable table prefix (default `qjson_`).  Indexed
interval columns for fast range queries.  Exact numerics survive the
full round-trip.

## Quick start

### Parse and stringify

```javascript
import { qjson_parse, qjson_stringify } from './src/qjson.js';

var obj = qjson_parse('{ price: 67432.50M, ts: 1710000000N }');
qjson_stringify(obj);  // '{"price":67432.50M,"ts":1710000000N}'
```

```python
from qjson import parse, stringify

obj = parse('{ price: 67432.50M, ts: 1710000000N }')
stringify(obj)  # '{"price":67432.50M,"ts":1710000000N}'
```

### Interval projection

```javascript
import { _qsql_argInterval } from './src/qsql.js';

_qsql_argInterval({t:"n", v:0.1, r:"0.1M"})
// → ["0.1", 0.09999999999999999, 0.10000000000000002]
//   1-ULP bracket: exact value is between lo and hi

_qsql_argInterval({t:"n", v:42})
// → [null, 42, 42]
//   point interval: 42 is exactly representable
```

### SQL storage

```javascript
import { qsqlAdapter } from './src/qsql.js';

var adapter = qsqlAdapter(db);  // any better-sqlite3-compatible db
adapter.setup();
adapter.insert(key, "price", 3);  // → CREATE TABLE qjson_price_3 (...)
```

```javascript
// Custom prefix
var adapter = qsqlAdapter(db, { prefix: "my_" });
// → tables: my_price_3, my_threshold_4, ...
```

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
| `M` | BigDecimal | `67432.50M` | `{type:"M",value:...}` | `Decimal` |
| `L` | BigFloat | `3.14159L` | `{type:"L",value:...}` | `BigFloat` |
| `0j` | Blob | `0jSGVsbG8` | `Blob` | `Blob` |

Plus: line/block/nested comments, trailing commas, unquoted keys.

## Project layout

```
src/
  qjson.js / qjson.py     QJSON parser + serializer
  qsql.js / qsql.py       Interval projection + SQL adapter

native/
  qjson.h / qjson.c       C: parse + stringify + project + cmp
  libbf/                   Vendored (exact directed rounding)

docs/
  qjson.md                 Format spec
  qsql.md                  Storage model

test/
  test-qjson.js            JS format tests
  test_qjson.py            Python format tests
  test_qjson.c             C format + projection tests
```

## Run tests

```bash
# JavaScript
node test/test-qjson.js

# Python
python3 test/test_qjson.py

# C
cc -O2 -std=c11 -frounding-math -o test_qjson test/test_qjson.c native/qjson.c -lm && ./test_qjson
```

## Documentation

| Doc | Scope |
|-----|-------|
| `docs/qjson.md` | QJSON spec: types, grammar, canonical form, JS64 blobs |
| `docs/qsql.md` | SQL storage: `[lo, str, hi]` projection, comparison, WHERE efficiency |

## License

MIT
