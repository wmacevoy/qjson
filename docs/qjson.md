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
?                     anonymous (empty name)
?X                    bare identifier
?_                    named "_" (not the same as bare ?)
?myVar_1              alphanumeric + underscore
?"Bob's Last Memo"    quoted string — any content
```

An unbound variable represents "match anything".  It is a leaf
value like a number or string.  The name after `?` can be a bare
identifier or a quoted string (same quoting rules as object keys).
`?` alone is anonymous — it has an empty name and never binds
to another `?`.  `?_` is a named variable (name is `_`).

### Projection

Unbound projects to `[-Inf, "?name", +Inf]`:

| Column | Value |
|--------|-------|
| lo | `-Infinity` |
| str | `"?"` (anonymous) or `"?X"` (named) |
| hi | `+Infinity` |

The universal interval passes through all range comparisons
without constraining — an unbound variable matches everything.

### Use cases

**CRUD pattern matching.**  QJSON values double as query patterns.
Unbound slots are the "holes" — fill in what you know, leave `?`
for what you want:

```
// Data
{id: 1, item: "book",   price: 9.99M,   delivered: true}
{id: 2, item: "laptop", price: 999.99M, delivered: false}

// "all delivered orders"
{id: ?, item: ?, price: ?, delivered: true}

// "delivery status of order 2"
{id: 2, delivered: ?}

// "orders over $10"
WHERE .orders[K].price > 10M
```

**Prolog-style facts.**  Unbound slots in arrays make patterns:

```
[reading, sensor1, temp, 35]       // fact
[reading, ?From, temp, ?Val]       // pattern — matches the fact
[reading, ?, temp, ?]              // anonymous — matches but doesn't bind
```

Two named unbounds with the same name must bind to the same value.
Two named unbounds with different names are independent.
Anonymous unbounds (`?`) never bind — each one is independent.

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

### qjson_cmp_[op] (C API)

Six operator-specific functions, each returning 0 (false) or 1 (true):

```c
int qjson_cmp_lt(a_type, a_lo, a_str, a_len, a_hi,
                 b_type, b_lo, b_str, b_len, b_hi);  // a <  b
int qjson_cmp_le(...)   // a <= b
int qjson_cmp_eq(...)   // a == b
int qjson_cmp_ne(...)   // a != b
int qjson_cmp_gt(...)   // a >  b
int qjson_cmp_ge(...)   // a >= b
```

Unbound semantics:

| Case | lt | le | eq | ne | gt | ge |
|------|----|----|----|----|----|----|
| `?X` vs `?X` (same name) | 0 | 1 | 1 | 0 | 0 | 1 |
| `?X` vs `?Y` (diff name) | 1 | 1 | 1 | 1 | 1 | 1 |
| `?X` vs `42` (unbound vs concrete) | 1 | 1 | 1 | 1 | 1 | 1 |

Same-name unbounds behave as equal (bound to same value).
Different-name or unbound-vs-concrete: all operators return 1
(unknown — could satisfy any relation).

### Database extensions

The `qjson_cmp_[op]` and `qjson_decimal_cmp` functions are
available as database extensions for exact comparison inside SQL:

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

Generated SQL:

```sql
WHERE n.hi > 3.141592653589793                          -- [brackets]: index scan
  AND qjson_cmp_gt(v.type, n.lo, n.str, n.hi,
                   'bigfloat', 3.141592653589793,
                   '3.14159265358979323846',
                   3.1415926535897936) = 1               -- exact fallback
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

### Cross-path comparison (relational joins)

WHERE predicates can compare two paths — not just path vs literal.
Combined with multiple variable bindings, this gives full
relational-join power over arrays stored as QJSON values.

```
path == path        type-aware equality (string, numeric, bool, null)
path != path        type-aware inequality
path < path         numeric ordering (both paths must resolve to numbers)
path > path
path <= path
path >= path
```

**Type dispatch.** `path == path` checks that both values have the
same type, then compares within that type:

- **string**: SQL string equality on the `string` table
- **numeric** (number, bigint, bigdec, bigfloat): exact comparison
  via `qjson_decimal_cmp`
- **boolean/null**: type column equality (true == true, null == null)
- **mixed types**: never equal (number 1 != string "1")

**Variable bindings as self-joins.** Each `[K]` variable creates an
unconstrained JOIN to `array_item` — iterating all elements.  Two
variables `[K1]`, `[K2]` on the same array create a cross product,
filtered by the WHERE clause.  This is a SQL self-join.

### Relational query examples

**Grandparent (2-hop join).**  The Prolog clause
`grandparent(GP, GC) :- parent(GP, Y), parent(Y, GC)` becomes:

```python
# Data
root = store({'parent': [
    {'name': 'Alice', 'child': 'Bob'},
    {'name': 'Bob',   'child': 'Carol'},
    {'name': 'Carol', 'child': 'Dave'},
]})

# Query: find all grandparents
results = qjson_select(conn, root, '.parent[K1].name',
    where_expr='.parent[K1].child == .parent[K2].name')
# → Alice (grandparent of Carol, via Bob)
# → Bob   (grandparent of Dave, via Carol)
```

Each `[K1]` and `[K2]` iterate independently over the `parent` array.
The WHERE clause `.parent[K1].child == .parent[K2].name` acts as the
equijoin condition — it only passes when K1's child equals K2's name.

**Great-grandparent (3-hop).** Add a third binding:

```python
results = qjson_select(conn, root, '.parent[K1].name',
    where_expr='.parent[K1].child == .parent[K2].name '
               'AND .parent[K2].child == .parent[K3].name')
# → Alice (great-grandparent of Dave)
```

**Mixed-type filtering.** Combine string, numeric, and boolean
comparisons with AND/OR:

```python
results = qjson_select(conn, root, '.emp[K].name',
    where_expr='.emp[K].dept == "eng" '
               'AND .emp[K].salary > 100000 '
               'AND .emp[K].active == true')
```

**Cross-path numeric comparison.** Find pairs in the same department
where one earns more than another:

```python
results = qjson_select(conn, root, '.emp[K1].name',
    where_expr='.emp[K1].dept == .emp[K2].dept '
               'AND .emp[K1].salary > .emp[K2].salary')
```

**Arithmetic in cross-comparisons.** Expressions like
`path * path > path` work — the arithmetic side is evaluated via
libbf exact functions, then compared to the path's projected value:

```python
results = qjson_select(conn, root, '.order[K].item',
    where_expr='.order[K].price * .order[K].qty > .order[K].budget')
```

### Horn clause equivalence

| Prolog | QJSON query |
|--------|-------------|
| Body predicate `parent(X, Y)` | `.parent[K]` (variable-bound iteration) |
| Shared variable `Y` | `WHERE .parent[K1].child == .parent[K2].name` |
| Conjunction `,` | `AND` |
| Disjunction `;` | `OR` |
| Head `grandparent(GP, GC)` | SELECT path + result bindings |
| CLP(R) constraints | `qjson_solve` arithmetic propagation |

Every Datalog rule (non-recursive horn clause) can be written as a
QJSON query.  The array is the relation, `[K]` bindings are tuple
iteration, and WHERE equalities are the join conditions.

## Exact arithmetic

17 SQL functions backed by libbf (arbitrary precision).
All take and return TEXT decimal strings.

| Function | Operation |
|----------|-----------|
| `qjson_add(a, b)` | a + b |
| `qjson_sub(a, b)` | a - b |
| `qjson_mul(a, b)` | a * b |
| `qjson_div(a, b [, prec])` | a / b |
| `qjson_pow(a, b [, prec])` | a ^ b |
| `qjson_neg(a)` | -a |
| `qjson_abs(a)` | |a| |
| `qjson_sqrt(a [, prec])` | √a |
| `qjson_exp(a [, prec])` | e^a |
| `qjson_log(a [, prec])` | ln(a) |
| `qjson_sin/cos/tan(a)` | trigonometric |
| `qjson_asin/acos/atan(a)` | inverse trig |
| `qjson_pi([prec])` | π |

Default precision: 113 bits (~34 decimal digits, IEEE 754 quad).
Optional `prec` parameter specifies decimal digits.

```sql
SELECT qjson_add('0.1', '0.2');        -- → '0.3' (exact)
SELECT qjson_div('1', '3', 50);        -- → '0.33333...' (50 digits)
SELECT qjson_pi(100);                  -- → π to 100 digits
```

## Constraint solver

`qjson_solve(root_id, formula)` — store a document with one
unknown (`?`), write the formula, the solver fills it in.
One call, any direction.

```sql
SELECT qjson_solve(root_id,
    '.future == .present * POWER(1 + .rate, .periods)');
```

### How it works

The solver:
1. Parses the formula into an expression tree
2. Decomposes the tree into 3-term constraints with
   anonymous temporaries (the user never sees them)
3. Runs leaf-folding propagation: constraints with exactly
   1 unknown fire first, newly solved values unlock more
4. Each constraint uses inverse operations automatically
   (division for multiplication, logarithm for power, etc.)

All arithmetic is exact via libbf (SQLite) or NUMERIC (PostgreSQL).

### Example: compound interest

FV = PV × (1 + r)<sup>n</sup>

```python
from qjson import Unbound
from decimal import Decimal

# Store with one unknown
root = db['store']({
    'present': Decimal('10000'), 'rate': Decimal('0.05'),
    'periods': Decimal('10'), 'future': Unbound('FV'),
})

# One call — the database does the math
conn.execute("SELECT qjson_solve(?, ?)",
    (root, '.future == .present * POWER(1 + .rate, .periods)'))

print(db['load'](root)['future'])  # → 16288.9462...
```

Change which field is `?` — the same formula solves in
any direction:

| Unknown | Result | Inverse operation |
|---------|--------|-------------------|
| future | $16,288.95 | `present * (1+rate)^periods` |
| present | $12,278.27 | `future / (1+rate)^periods` |
| rate | 7.18% | nth root then subtract 1 |
| periods | 10.24 years | logarithm |

```sql
-- Same formula, different unknown — pure SQL
SELECT qjson_solve(root_id,
    '.future == .present * POWER(1 + .rate, .periods)');
SELECT qjson_reconstruct(root_id);
```

### Expressions in WHERE

Arithmetic works in SELECT queries too (read-only, no solving):

```sql
SELECT qjson FROM qjson_select
WHERE root_id = 1 AND select_path = '.present'
  AND where_expr = '.present * POWER(1 + .rate, .periods) > 16000';
```

Operators: `+`, `-`, `*`, `/`, `**` (power).
Functions: `POWER`, `SQRT`, `EXP`, `LOG`, `SIN`, `COS`,
`TAN`, `ATAN`, `ASIN`, `ACOS`, `PI`.

### Internal mechanism

The solver decomposes expressions into 3-term constraints
internally using `qjson_solve_add/sub/mul/div/pow`.  These
are also available directly for advanced use:

| Function | Constraint | Returns |
|----------|-----------|---------|
| `qjson_solve_add(a, b, c)` | a + b = c | 0/1/2/3 |
| `qjson_solve_mul(a, b, c)` | a * b = c | 0/1/2/3 |
| `qjson_solve_pow(a, b, c)` | a ** b = c | 0/1/2/3 |

Arguments: INTEGER (value_id, can be solved if unbound) or
TEXT (literal, always bound).  Return codes: 0=inconsistent,
1=solved, 2=consistent, 3=underdetermined.
