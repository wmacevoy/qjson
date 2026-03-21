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
    QJSON_OBJECT
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
Null and boolean need no child table — the `type` column
carries the full value.  All numeric types share one table
with `[lo, str, hi]` interval projection.

```sql
CREATE TABLE value (
    id   INTEGER PRIMARY KEY,
    type TEXT NOT NULL
    -- 'null', 'true', 'false',
    -- 'number', 'bigint', 'bigdec', 'bigfloat',
    -- 'string', 'blob', 'array', 'object'
);

-- All numeric types: [lo, str, hi] projection.
-- str is NULL when lo == hi (exact double — 99.999% of rows).
-- value.type distinguishes number/bigint/bigdec/bigfloat.
CREATE TABLE number_value (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES value(id),
    lo       REAL,    -- round_down_ieee754(exact_value)
    str      TEXT,    -- exact string repr, NULL when lo == hi
    hi       REAL     -- round_up_ieee754(exact_value)
);

CREATE TABLE string_value (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES value(id),
    value    TEXT
);

CREATE TABLE blob_value (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES value(id),
    value    BLOB
);

CREATE TABLE array_value (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES value(id)
);

CREATE TABLE array_item (
    id       INTEGER PRIMARY KEY,
    array_id INTEGER REFERENCES array_value(id),
    idx      INTEGER,
    value_id INTEGER REFERENCES value(id)
);

CREATE TABLE object_value (
    id       INTEGER PRIMARY KEY,
    value_id INTEGER REFERENCES value(id)
);

CREATE TABLE object_item (
    id        INTEGER PRIMARY KEY,
    object_id INTEGER REFERENCES object_value(id),
    key       TEXT,
    value_id  INTEGER REFERENCES value(id)
);
```

The `number_value.str` optimization: when `lo == hi`, the IEEE
double IS the exact value — no string needed.

| Value | type | lo | str | hi |
|-------|------|----|-----|----|
| `42` | number | 42.0 | NULL | 42.0 |
| `67432.50M` | bigdec | 67432.5 | NULL | 67432.5 |
| `0.1M` | bigdec | round_down(0.1) | `"0.1"` | round_up(0.1) |
| `9007199254740993N` | bigint | round_down(9e15+1) | `"9007199254740993"` | round_up(9e15+1) |

`round_down` = largest IEEE double ≤ exact value.
`round_up` = smallest IEEE double ≥ exact value.
When the exact value IS an IEEE double: `round_down = round_up = value`.

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
int qjson_cmp(a_lo, a_hi, a_str, a_len, b_lo, b_hi, b_str, b_len) {
    if (a_hi < b_lo) return -1;                  // [brackets]: separated
    if (a_lo > b_hi) return  1;                  // [brackets]: separated
    if (a_lo == a_hi && b_lo == b_hi) return 0;  // {braces}: both exact
    return qjson_decimal_cmp(a_str, a_len, b_str, b_len);  // val() decode
}
```

All ordering operators: `qjson_cmp(...) <op> 0`.
Equality: compare `[lo, str, hi]` columns directly.
