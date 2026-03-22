# QJSON — JSON + exact numerics + binary blobs + comments

QJSON is a superset of JSON.  Every valid JSON document is valid
QJSON.  The extensions add what JSON lacks for configuration,
financial data, and embedded systems: exact numbers, binary data,
and human-friendly syntax.

## Types

| Type | JSON | QJSON extension | Example |
|------|------|-----------------|---------|
| null | `null` | — | `null` |
| boolean | `true`, `false` | — | `true` |
| number | `3.14` | — | `3.14` |
| string | `"hello"` | — | `"hello"` |
| array | `[1, 2]` | trailing comma | `[1, 2,]` |
| object | `{"a": 1}` | unquoted keys, trailing comma | `{a: 1,}` |
| BigInt | — | `N` suffix | `42N` |
| BigDecimal | — | `M` suffix | `67432.50M` |
| BigFloat | — | `L` suffix | `3.14159265358979L` |
| blob | — | `0j` prefix (JS64) | `0jSGVsbG8` |
| unbound | — | `?` prefix | `?X`, `?"Bob's Last Memo"` |

## Numbers

Plain numbers are IEEE 754 doubles.  Exact when representable
(integers up to 2^53, binary fractions like 0.5).  Inexact
otherwise (0.1, most decimals).

### Suffixed numbers: N, M, L

A suffix after a numeric literal marks it as exact:

```
42N                  // BigInt — arbitrary precision integer
67432.50M            // BigDecimal — exact base-10 decimal
3.14159265358979L    // BigFloat — arbitrary precision float
```

Lowercase accepted, canonicalized to uppercase on output.
The suffix must not be followed by an alphanumeric character
(`42N` is BigInt, `42Name` is a parse error).

## Blobs: `0j` prefix

Binary data encoded with JS64 — a base-64 encoding that uses
the 64 legal JavaScript identifier characters as its alphabet.

```
0jSGVsbG8          // 5 bytes: "Hello"
0j0012f580deb4     // raw binary (SHA-256 fragment, key material, etc.)
```

### JS64 alphabet

```
$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz
```

64 characters: `$`, `0-9`, `A-Z`, `_`, `a-z`.  Sorted by
ASCII code point.  Each character encodes 6 bits.

### Encoding

JS64 packs bits LSB-first into 6-bit chunks mapped to the
alphabet.  The full JS64 encoding produces a leading `$`
(zero bits).  The `0j` prefix replaces this leading `$`:

| Full JS64 | QJSON literal |
|-----------|---------------|
| `$SGVsbG8` | `0jSGVsbG8` |
| `$AAAA` | `0jAAAA` |

To decode a `0j` literal: prepend `$`, then JS64-decode.
To encode a blob for QJSON: JS64-encode, strip leading `$`,
prepend `0j`.

Note that leading `$` are significant — even though they represent six zero bits, the length of the JS64 encoding defines the size of the blob: `size(blob) = floor(6*length(encoding)/8)`. `0j` is a 0-length (empty) blob.

### Why not base64?

Standard base64 uses `+` and `/` which aren't valid in
identifiers or URLs without escaping.  JS64 uses only
identifier-safe characters.  A JS64 blob is a valid token
in JavaScript, Prolog, SQL, and shell without quoting.

### Why `0j`?

Follows the `0b`/`0o`/`0x` convention for literal prefixes.
`j` for JS64.  Case-insensitive (`0j` = `0J`).  Doesn't
conflict with `${}` template interpolation.

## Unbound variables: `?` prefix

```
?X                    bare identifier
?_                    anonymous (same as bare ?)
?myVar_1              alphanumeric + underscore
?"Bob's Last Memo"    quoted string — any content
```

An unbound variable represents "match anything".  It is a leaf
value like a number or string.  The name after `?` can be a bare
identifier or a quoted string (same quoting rules as object keys).
`?` alone is shorthand for `?_` (anonymous).

### Projection

Unbound projects to `[-Inf, "?name", +Inf]`:

| Column | Value |
|--------|-------|
| lo | `-Infinity` |
| str | `"?X"` (the `?` prefix + name) |
| hi | `+Infinity` |

The universal interval passes through all range comparisons
without constraining — an unbound variable matches everything.

### Use case

Prolog facts are QJSON arrays.  Unbound slots make patterns:

```
[reading, sensor1, temp, 35]       // fact
[reading, ?From, temp, ?Val]       // pattern — matches the fact
```

Two unbounds with the same name must bind to the same value.
Two unbounds with different names are independent.

## Comments

```
// line comment (to end of line)

/* block comment */

/* nested /* block */ comments */
```

Block comments nest.  This is intentional — you can comment
out a region that already contains comments.

## Trailing commas

```
[1, 2, 3,]          // OK
{a: 1, b: 2,}       // OK
```

## Unquoted keys

Object keys that are valid identifiers don't need quotes:

```
{
    name: "alice",
    age: 30,
    _internal: true,
}
```

Valid unquoted key: starts with `[a-zA-Z_$]`, followed by
`[a-zA-Z0-9_$]`.

## QJSON type enum (C API)

```c
typedef enum {
    QJSON_NULL, QJSON_TRUE, QJSON_FALSE,
    QJSON_NUM,          // IEEE 754 double
    QJSON_BIGINT,       // raw string, suffix N
    QJSON_BIGDEC,       // raw string, suffix M
    QJSON_BIGFLOAT,     // raw string, suffix L
    QJSON_BLOB,         // JS64-decoded byte array
    QJSON_STRING,
    QJSON_ARRAY,
    QJSON_OBJECT,
    QJSON_UNBOUND       // ?name — unbound variable, str holds name
} qjson_type;
```

## Implementations

| Language | File | Notes |
|----------|------|-------|
| C | `native/qjson.h`, `native/qjson.c` | Canonical. Arena-allocated, zero malloc, 3.5M msg/sec. |
| JavaScript | `src/qjson.js` | ES5-compatible. Node, Bun, Deno, QuickJS, Duktape. |
| Python | `src/qjson.py` | Pure Python 3. |

All three implementations parse and stringify the same format.
The C implementation is the reference.

QJSON objects support key-intersection matching: `{user: Name}`
matches `{user: alice, age: 30}` binding `Name = alice`.

## Grammar

JSON defines its representation in terms of characters (Unicode
codepoints), leaving the byte encoding up to the implementation,
and notably limiting `\u` escapes to 4 hex digits (BMP only).

QJSON defines its grammar in terms of bytes.  A language may
keep this representation as a string, but QJSON is primarily
about serialization — transport and storage are sequences of
bytes.

```
value     = ws (null | boolean | number | string | blob
               | array | object) ws

null      = 'null'
boolean   = 'false' | 'true'

number    = '-'? digits ('.' digits)? (('e'|'E') ('+'|'-')? digits)?
            ('N'|'n'|'M'|'m'|'L'|'l')?
digits    = [0-9]+

blob      = '0' ('j'|'J') js64*
js64      = [$0-9A-Z_a-z]

string    = '"' character* '"'
character = <any UTF-8 byte sequence except '"' and '\'>
          | '\"' | '\\' | '\/' | '\b' | '\f' | '\n' | '\r' | '\t'
          | '\u' hex hex hex hex
          | '\u{' hex+ '}'
hex       = [0-9A-Fa-f]

array     = '[' (value (',' value)* ','?)? ']'
object    = '{' (pair (',' pair)* ','?)? '}'
pair      = (string | ident) ':' value
ident     = [a-zA-Z_$] [a-zA-Z0-9_$]*

comment   = '//' <to end of line>
          | '/*' (comment | <any>)* '*/'
ws        = (space | tab | newline | comment)*
```

Notes:
- `ws` is implicit between all tokens.
- Block comments nest (`/* outer /* inner */ still */`).
- Trailing commas are permitted in arrays and objects.
- `\u{hex+}` extends JSON's `\uXXXX` to all Unicode codepoints.
- No suffix after a number means plain IEEE 754 double.
- A number with suffix is a distinct type (`42` ≠ `42N` ≠ `42M`).
- `0j` is unambiguous: no legal number has `j`/`J` after `0`.

## Canonical representation

The canonical form is a deterministic byte sequence for each
value.  Goal: `SHA256(canon(x)) == SHA256(canon(y))` iff
`x` and `y` represent the same value.

### Encoding

UTF-8 bytes.  No BOM.

### Whitespace and comments

None.  No spaces, tabs, newlines, or comments.

### null, boolean

`null`, `true`, `false` — lowercase, no variants.

### Number (no suffix)

Plain numbers are IEEE 754 doubles.  `1` and `1.0` are the
same value (same double).

Canonical form follows the ECMAScript `Number.toString()` rules:
the shortest decimal string that round-trips to the same double.

| Value | Canonical | Not canonical |
|-------|-----------|---------------|
| forty-two | `42` | `42.0`, `042`, `4.2e1` |
| one-tenth | `0.1` | `.1`, `0.10` |
| negative zero | `0` | `-0` |
| 10^20 | `100000000000000000000` | `1e20` |
| 10^21 | `1e+21` | `1000000000000000000000` |
| 5 × 10^-7 | `5e-7` | `0.0000005` |

Scientific notation is used when the exponent is ≥ 21 or ≤ -7
(ECMAScript rules).

### BigInt (N suffix)

Canonical: minimal integer, no leading zeros, no `+` sign,
uppercase suffix.

| Value | Canonical | Not canonical |
|-------|-----------|---------------|
| forty-two | `42N` | `042N`, `42n`, `+42N` |
| zero | `0N` | `00N` |

### BigDecimal (M suffix)

Canonical: strip trailing fractional zeros, strip unnecessary
decimal point, no leading zeros (except `0.x`), no `+` sign,
no scientific notation, uppercase suffix.

QuickJS BigDecimal (libbf) normalizes internally —
`0.50m === 0.5m` is `true`.  Same value, same canonical form.

| Value | Canonical | Not canonical |
|-------|-----------|---------------|
| 67432.5 | `67432.5M` | `67432.50M`, `067432.5M` |
| forty-two | `42M` | `42.0M`, `42.00M` |
| one-tenth | `0.1M` | `00.1M`, `0.10M` |

### BigFloat (L suffix)

Same rules as BigDecimal.  Uppercase `L` suffix.

### Blob

`0j` prefix (lowercase), followed by the JS64 body.
JS64 encoding is deterministic for a given byte sequence.

### String

- Delimited by `"`
- Escape only what is required:
  - `\"` and `\\` (must escape)
  - Control characters 0x00–0x1F as `\uXXXX` (lowercase hex)
- Everything else: literal UTF-8 bytes
- No `\/` escape (literal `/` instead)
- No `\u{...}` in output (accepted on input, emitted as
  literal UTF-8)
- No surrogate pairs in output (use literal UTF-8 for
  codepoints above U+FFFF)

No Unicode normalization — bytes are compared raw.  If you
need NFC equivalence, normalize before serialization.

### Array

Elements in order.  No trailing commas.  No whitespace.

```
[1,2,3]
```

### Object

Keys sorted by UTF-8 byte order (ascending).  Always quoted
(even valid identifiers).  No trailing commas.  No whitespace.
No duplicate keys.

```
{"a":1,"b":2}
```

### Value identity vs text identity

QJSON has two notions of equality:

**Text identity** (canonical form, for document hashing):
the type suffix is part of the text.  `"42"` and `"42N"` are
different QJSON strings → different SHA256.

**Value identity** (SQL, for queries):  the type suffix is
representation metadata.  `42`, `42N`, `42M` are all "five
times eight plus two" → same numeric value → same `[lo, str, hi]`
projection.

| Expression | Text equal? | Value equal? |
|-----------|-------------|-------------|
| `42` vs `42` | yes | yes |
| `1` vs `1.0` | yes (same canonical form) | yes |
| `42` vs `42N` | **no** | **yes** (same number) |
| `42N` vs `42M` | **no** | **yes** (same number) |
| `0.5M` vs `0.50M` | **no** (`0.5M` vs `0.50M`) | **yes** (same projection) |
| `"hello"` vs `"hello"` | yes | yes |
| `0jSGVsbG8` vs `0jSGVsbG8` | yes | yes |

### Summary

| Type | Canonical form |
|------|---------------|
| null | `null` |
| boolean | `true` or `false` |
| number | shortest round-trip decimal (ECMAScript rules) |
| BigInt | minimal integer + `N` |
| BigDecimal | normalized decimal + `M` (no trailing zeros) |
| BigFloat | normalized decimal + `L` (no trailing zeros) |
| blob | `0j` + JS64 body |
| string | `"..."` minimal escapes, literal UTF-8 |
| array | `[v,v,v]` |
| object | `{"k":v,"k":v}` sorted keys, quoted |

## SQL representation

Normalized schema for storing arbitrary QJSON values in SQL.
All table names share a configurable prefix (default `qjson_`).
Null and boolean need no child table — the `type` column
carries the full value.  All numeric types share one table
with `[lo, str, hi]` interval projection.

lo/hi columns must be 8-byte IEEE 754 doubles.  The SQL type
name differs by database:

| Database | 8-byte double | Binary data | Auto-increment PK | Param |
|----------|---------------|-------------|-------------------|-------|
| SQLite | `REAL` | `BLOB` | `INTEGER PRIMARY KEY` | `?` |
| SQLCipher | `REAL` | `BLOB` | `INTEGER PRIMARY KEY` | `?` |
| PostgreSQL | `DOUBLE PRECISION` | `BYTEA` | `SERIAL PRIMARY KEY` | `%s` |

SQLCipher uses the same dialect as SQLite (identical SQL, same
`REAL` = 8-byte IEEE 754).  The only difference is the encryption
key set via `PRAGMA key` before any other operation.

### Encryption at rest

QJSON's interval columns (`lo`, `hi`) must remain indexable for
the 3-tier comparison to work.  Encryption must happen **below**
the SQL layer (page-level or disk-level), not at the column level.

| Platform | Encryption | Indexes work | Key model |
|----------|-----------|-------------|-----------|
| SQLite + SQLCipher | Page-level | Yes | User-supplied per-database |
| SQLCipher WASM | Page-level + OPFS/IndexedDB | Yes | User-supplied in browser |
| PostgreSQL + FDE | Disk-level (LUKS, cloud EBS) | Yes | Infrastructure-managed |
| PostgreSQL container | Encrypted volume mount | Yes | Container/host-managed |

Column-level encryption (e.g. pgcrypto) must **not** be used on
the `lo`/`hi` columns — it would make them unindexable and break
interval pushdown.

For PostgreSQL in production, use a container with an encrypted
volume for the data directory:

```yaml
# docker-compose.yml
services:
  postgres:
    image: postgres:16-alpine
    volumes:
      - pgdata:/var/lib/postgresql/data       # encrypted volume
      - ./sql/qjson_pg.sql:/docker-entrypoint-initdb.d/01-qjson.sql:ro
volumes:
  pgdata:  # back with LUKS, encrypted EBS/PD, or host FDE
```

**SQLite**:

```sql
-- prefix default: "qjson_"

CREATE TABLE qjson_value (
    id   INTEGER PRIMARY KEY,
    type TEXT NOT NULL
    -- 'null', 'true', 'false',
    -- 'number', 'bigint', 'bigdec', 'bigfloat',
    -- 'string', 'blob', 'array', 'object'
);

CREATE TABLE qjson_number (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id),
    lo       REAL,    -- roundDown(exact_value)
    str      TEXT,    -- exact string repr, NULL when lo == hi
    hi       REAL     -- roundUp(exact_value)
);

CREATE TABLE qjson_string (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id),
    value    TEXT
);

CREATE TABLE qjson_blob (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id),
    value    BLOB
);

CREATE TABLE qjson_array (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id)
);

CREATE TABLE qjson_array_item (
    id       INTEGER PRIMARY KEY,
    array_id INTEGER REFERENCES qjson_array(id),
    idx      INTEGER,
    value_id INTEGER REFERENCES qjson_value(id)
);

CREATE TABLE qjson_object (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id)
);

CREATE TABLE qjson_object_item (
    id        INTEGER PRIMARY KEY,
    object_id INTEGER REFERENCES qjson_object(id),
    key       TEXT,
    value_id  INTEGER REFERENCES qjson_value(id)
);
```

**PostgreSQL**:

```sql
CREATE TABLE qjson_value (
    id   SERIAL PRIMARY KEY,
    type TEXT NOT NULL
);

CREATE TABLE qjson_number (
    id       SERIAL PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id),
    lo       DOUBLE PRECISION,
    str      TEXT,
    hi       DOUBLE PRECISION
);

CREATE TABLE qjson_string (
    id       SERIAL PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id),
    value    TEXT
);

CREATE TABLE qjson_blob (
    id       SERIAL PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id),
    value    BYTEA
);

CREATE TABLE qjson_array (
    id       SERIAL PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id)
);

CREATE TABLE qjson_array_item (
    id       SERIAL PRIMARY KEY,
    array_id INTEGER REFERENCES qjson_array(id),
    idx      INTEGER,
    value_id INTEGER REFERENCES qjson_value(id)
);

CREATE TABLE qjson_object (
    id       SERIAL PRIMARY KEY,
    value_id INTEGER REFERENCES qjson_value(id)
);

CREATE TABLE qjson_object_item (
    id        SERIAL PRIMARY KEY,
    object_id INTEGER REFERENCES qjson_object(id),
    key       TEXT,
    value_id  INTEGER REFERENCES qjson_value(id)
);
```

The `qjson_number.str` optimization: when `lo == hi`, the IEEE
double IS the exact value — no string needed.

| Value | type | lo | str | hi |
|-------|------|----|-----|----|
| `42` | number | 42.0 | NULL | 42.0 |
| `67432.50M` | bigdec | 67432.5 | NULL | 67432.5 |
| `0.1M` | bigdec | roundDown(0.1) | `"0.1"` | roundUp(0.1) |
| `9007199254740993N` | bigint | roundDown(9e15+1) | `"9007199254740993"` | roundUp(9e15+1) |

`roundDown` = largest IEEE double ≤ exact value.
`roundUp` = smallest IEEE double ≥ exact value.
When the exact value IS an IEEE double: `roundDown = roundUp = value`.

## WHERE efficiency

The `[lo, str, hi]` projection splits comparisons into two
categories: ordering (number-line) and equality (data identity).

### Equality and inequality (data question)

The projection IS the value.  Two numbers are equal iff their
projections match — no interval arithmetic, no string decode:

```sql
-- x == y: all three columns match
x == y ≡ lo(x) = lo(y) AND hi(x) = hi(y)
          AND ((str(x) IS NULL AND str(y) IS NULL)
               OR str(x) = str(y))

-- x != y: NOT of the above
x != y ≡ NOT (x == y)
```

This lands entirely in the database.  Indexed columns, no
application-side decode.  Type suffix doesn't matter — `5`,
`5N`, `5M` all project to `[5.0, NULL, 5.0]` → equal.

### Ordering (number-line question)

Three tiers, each avoiding work for the next:

- **`[brackets]`** — indexed WHERE on `lo`/`hi` REAL columns.
  Necessary condition.  Does 99.999% of the filtering.
- **`{braces}`** — both values are exact doubles (`lo == hi`).
  Avoids string decode.  Resolves 99.999% of the remainder.
- **`val(x) <op> val(y)`** — full comparison for the ~0.001%
  overlap zone.

```
val(x) = lo(x) if lo(x) == hi(x) else decode(str(x))
```

```
x <  y ≡ [lo(x) < hi(y)]  AND ({hi(x) < lo(y)}  OR val(x) < val(y))
x <= y ≡ [lo(x) <= hi(y)] AND ({hi(x) <= lo(y)} OR val(x) <= val(y))
x >  y ≡ [hi(x) > lo(y)]  AND ({lo(x) > hi(y)}  OR val(x) > val(y))
x >= y ≡ [hi(x) >= lo(y)] AND ({lo(x) >= hi(y)} OR val(x) >= val(y))
```

The `[brackets]` are the SQL WHERE clause — indexed, fast,
eliminates most rows.  The `{braces}` check avoids string
decode when both values are exact doubles.  `val()` only
fires when intervals overlap and at least one value is
non-exact.

### qjson_cmp (C API)

```c
int qjson_cmp(a_type, a_lo, a_str, a_str_len, a_hi,
              b_type, b_lo, b_str, b_str_len, b_hi) {
    // Unbound: compare names if both unbound, else matches any
    if (a_type == QJSON_UNBOUND || b_type == QJSON_UNBOUND) ...
    if (a_hi < b_lo) return -1;                  // [brackets]: separated
    if (a_lo > b_hi) return  1;                  // [brackets]: separated
    if (a_lo == a_hi && b_lo == b_hi) return 0;  // {braces}: both exact
    // Overlap: resolve exact value using type
    // If lo == hi → exact double (val = lo); else exact(type, str) via libbf
    ...
}
```

The `type` parameter is the `qjson_type` enum value.  Needed to
resolve exact values when one side is an exact double (str=NULL)
and the other has a decimal string, and to handle unbound variables.

All ordering operators: `qjson_cmp(...) <op> 0`.
Equality: compare `[lo, str, hi]` columns directly.

### Database extensions

The `qjson_cmp` and `qjson_decimal_cmp` functions are available
as database extensions for exact comparison inside SQL:

| Database | Mechanism | Exact math |
|----------|-----------|------------|
| SQLite | Loadable extension (`qjson_ext.dylib/.so`) | libbf (arbitrary precision) |
| PostgreSQL | PL/pgSQL functions (`sql/qjson_pg.sql`) | `NUMERIC` type |

Build the SQLite extension:

```bash
make   # produces qjson_ext.dylib (macOS) or qjson_ext.so (Linux)
```

## Query language

jq-like path expressions compiled to SQL JOIN chains.
Each path step is a fixed JOIN — no recursive CTEs,
pure translation.

### Path syntax

| Syntax | Meaning | SQL |
|--------|---------|-----|
| `.key` | object child by key | JOIN `object` + `object_item WHERE key = 'key'` |
| `[n]` | array index | JOIN `array` + `array_item WHERE idx = n` |
| `[K]` | variable binding | JOIN `array` + `array_item` (K binds to idx) |
| `.[]` | all array elements | JOIN `array` + `array_item` (all rows) |

Variables are uppercase identifiers.  A variable used in both
the SELECT path and WHERE clause refers to the same array
element — it becomes a shared table alias in the generated SQL.

### WHERE predicates

```
path == value       equality (exact projection match)
path != value       inequality
path < value        ordering (interval pushdown + exact fallback)
path <= value
path > value
path >= value
pred AND pred       conjunction
pred OR pred        disjunction
NOT pred            negation
(pred)              grouping
```

Values: numbers (`42`, `0.1M`, `3.14L`), strings (`"hello"`),
literals (`true`, `false`, `null`).

### SELECT

Returns matching values as `(value_id, qjson_text, bindings)`.

```sql
-- PostgreSQL (embedded translator)
SELECT * FROM qjson_select(1, '.name');
-- → (2, '"sensor-array"', {})

SELECT * FROM qjson_select(1, '.items[K]', '.items[K].t > 30');
-- → (15, '{"label":"hot","t":35,"y":3.5}',    {})
-- → (19, '{"label":"fire","t":50,"y":5}',      {})

SELECT qjson FROM qjson_select(1, '.items[K].label',
    '.items[K].t > 20 AND .items[K].t < 40');
-- → '"warm"'
-- → '"hot"'
```

```python
# Python (external translator)
from qjson_query import qjson_select

results = qjson_select(conn, root_id, '.items[K]',
                        where_expr='.items[K].t > 30')
for vid, bindings in results:
    print(adapter['load'](vid))
# {'label': 'hot', 't': 35.0, 'y': 3.5}
# {'label': 'fire', 't': 50.0, 'y': 5.0}
```

### UPDATE

Modifies scalar values in place.  Finds targets via the same
path + WHERE mechanism, then replaces the child row.

```python
from qjson_query import qjson_update

# Set y = 3 for all items where t > 30
qjson_update(conn, root_id, '.items[K].y', 3,
             where_expr='.items[K].t > 30')

# Change label to "medium" where 20 < t < 40
qjson_update(conn, root_id, '.items[K].label', 'medium',
             where_expr='.items[K].t > 20 AND .items[K].t < 40')

# Set to BigDecimal
from decimal import Decimal
qjson_update(conn, root_id, '.items[0].y', Decimal('99.99'))
```

### Reconstruction

Any `value_id` can be reconstructed as canonical QJSON text.

```sql
-- PostgreSQL
SELECT qjson_reconstruct(42, 'qjson_');
-- → '{"label":"hot","t":35,"y":3.5}'
```

```python
# Python
adapter['load'](42)
# → {'label': 'hot', 't': 35.0, 'y': 3.5}

from qjson import stringify
stringify(adapter['load'](42))
# → '{"label":"hot","t":35,"y":3.5}'
```

### Interval pushdown example

Query: `.items[K].value > 3.14159265358979323846L`

The query literal is pre-projected:

```
roundDown("3.14159265358979323846") = 3.141592653589793
roundUp("3.14159265358979323846")   = 3.1415926535897936
str = "3.14159265358979323846"
```

Generated SQL (3-tier comparison):

```sql
WHERE n.hi > 3.141592653589793                          -- [brackets]: index scan
  AND (n.lo > 3.1415926535897936                        -- {braces}: both exact
       OR qjson_cmp(v.type_id, n.lo, n.str, n.hi,
                    5, 3.141592653589793,
                    '3.14159265358979323846',
                    3.1415926535897936) > 0)             -- val(): exact fallback
```

The indexed `hi` column eliminates most rows.  The `lo` check
resolves 99.999% of the remainder.  `qjson_cmp` (backed by libbf
in SQLite or NUMERIC in PostgreSQL) fires only for the rare
overlap zone where two inexact values share the same IEEE double
brackets.

### Deep path example

Paths and WHERE clauses can be arbitrarily deep.  Each `.key`
step adds exactly 2 JOINs — the cost is predictable.

```python
# Store deeply nested document
doc = {'config': {'sensors': {'array': [
    {'id': 'tc-7', 'cal': Decimal('0.003'), 'readings': [22.5, 23.1]},
    {'id': 'tc-8', 'cal': Decimal('0.007'), 'readings': [30.0, 31.2]},
]}}}

# Query 4 levels deep with AND
results = select(conn, root_id, '.config.sensors.array[K]',
    where_expr='.config.sensors.array[K].cal > 0.005M')
# → tc-8 element
```

```sql
-- Same query in SQLite (embedded translator)
SELECT qjson FROM qjson_select
WHERE root_id = 1
  AND select_path = '.config.sensors.array[K]'
  AND where_expr = '.config.sensors.array[K].cal > 0.005M';

-- PostgreSQL (embedded translator)
SELECT * FROM qjson_select(1,
    '.config.sensors.array[K]',
    '.config.sensors.array[K].cal > 0.005M');
```

Key names and string values have no length limit — all
allocations are dynamic.
