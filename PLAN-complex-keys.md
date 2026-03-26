# Plan: Complex keys + set shorthand in QJSON ✓ (implemented v1.1.2)

## Context

QJSON object keys are currently strings.  If keys can be any
QJSON value, objects become full maps and sets need no new type:

```javascript
// Set shorthand: {key, key, key} → {key: true, key: true, ...}
{[alice, bob], [bob, carol]}

// Full map with complex keys:
{[alice, bob]: true, [bob, carol]: true}
```

No new type.  Sets are objects where values are `true`.
The `{a, b, c}` syntax is sugar for `{a: true, b: true, c: true}`.

This enables Datalog: predicates are sets of fact tuples
(see PLAN-datalog.md).

## 1. Schema change

### Current

```sql
CREATE TABLE qjson_object_item (
  id        INTEGER PRIMARY KEY,
  object_id INTEGER REFERENCES qjson_object(id),
  key       TEXT,
  value_id  INTEGER REFERENCES qjson_value(id)
);
```

### New

```sql
CREATE TABLE qjson_object_item (
  id        INTEGER PRIMARY KEY,
  object_id INTEGER REFERENCES qjson_object(id),
  key_id    INTEGER REFERENCES qjson_value(id),
  value_id  INTEGER REFERENCES qjson_value(id),
  UNIQUE(object_id, key_id)
);
CREATE INDEX ix_oi_key ON qjson_object_item(key_id);
```

`key` TEXT → `key_id` INTEGER (foreign key to qjson_value).
Keys are full QJSON values with their own `[lo, str, hi]`
projection.  UNIQUE constraint gives set semantics (no
duplicate keys per object).

### Migration

Existing text keys → create qjson_string values, populate
key_id.  Then drop the text `key` column.

### Files

- `src/qjson_sql.py` — schema, store, load, remove
- `src/qjson-sql.js` — same
- `native/qjson_sqlite_ext.c` — reconstruct, qjson_select
- `sql/qjson_pg.sql` — PostgreSQL

## 2. Parser: complex keys + set shorthand

### Grammar

```
object := '{' '}'
       |  '{' entries '}'

entries := entry (',' entry)* ','?

entry := expr ':' expr        // map: key: value
       | expr                 // set: key (sugar for key: true)
```

After first `expr`, peek:
- `:` → map entry, parse value
- `,` or `}` → set entry, value = `true`

### Key expressions

Any QJSON value can be a key:

```javascript
{name: alice}              // string key (backward compat)
{42: answer}               // number key
{[1, 2]: pair}             // array key
{?X: 42}                   // unbound key (pattern matching)
{{a: 1}: nested}           // object key
```

### Set shorthand

```javascript
{alice, bob, carol}        // set of strings
{[alice, bob], [bob, carol]}  // set of tuples (Datalog facts)
{1, 2, 3}                 // set of numbers
```

### Serialization

If all values are `true` → emit set shorthand: `{a, b, c}`.
Otherwise emit map syntax: `{a: 1, b: 2}`.

### Files

- `src/qjson.py` — parser, serializer
- `src/qjson.js` — parser, serializer
- `native/qjson.c` — C parser, stringifier
- Tests in all three languages

## 3. Unification efficiency

### Current: O(n*m)

For each key in A, scan all keys in B.

### With indexed keys: O(n log n)

In SQL, object unification = JOIN on key_id:

```sql
SELECT A.value_id, B.value_id
FROM qjson_object_item A
JOIN qjson_object_item B ON A.key_id = B.key_id
WHERE A.object_id = ? AND B.object_id = ?
```

Indexed.  Key intersection is a merge join.

## 4. Query path: `{K}` for object iteration

### Current

`.key` resolves string keys.

### New

`{K}` iterates object entries (key-value pairs):

```python
# Iterate all facts in parent set
qjson_select(conn, root, '.parent{K}')

# Match on key structure
qjson_select(conn, root, '.parent{K}',
    where_expr='.parent{K}[1] == "bob"')
```

`{K}` binds to key_id.  Navigate into the key with
`{K}[0]`, `{K}[1]`, `{K}.field`, etc.

### Files

- `src/qjson_query.py` — path parser, query builder
- `native/qjson_sqlite_ext.c` — path resolution
- `docs/qjson.md` — query language

## Backward compatibility

- `{key: value}` with string keys: unchanged
- `{}`: empty object (= empty set)
- `.key` path: unchanged for string keys
- Existing data: migrated (text keys → string value_ids)
