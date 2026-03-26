# Plan: Set iteration + recursive queries in QJSON ✓ (implemented v1.1.3)

## Context

QJSON v1.1.2 has complex keys, set shorthand, cross-path
comparison, and variable bindings.  Predicates are sets:

```javascript
{
  parent: {[alice, bob], [bob, carol], [carol, dave]},
  edge:   {[a, b], [b, c], [c, d]}
}
```

Two things are missing for Datalog-style queries over sets:

## 1. `[K]` on sets

`[K]` already iterates array elements.  Extend it to iterate
set members (object keys) when the target is an object.

```python
# Iterate parent facts
qjson_select(conn, root, '.parent[K]')

# Filter: parent(?, bob)
qjson_select(conn, root, '.parent[K]',
    where_expr='.parent[K][1] == "bob"')

# Join: grandparent via shared variable
qjson_select(conn, root, '.parent[K1]',
    where_expr='.parent[K1][1] == .parent[K2][0]')
```

### Implementation

In `_QueryBuilder.resolve_path`, when step is `_TOK_VAR`:

- On array: JOIN `qjson_array_item` (existing)
- On object/set: JOIN `qjson_object_item`, bind K to `key_id`

The key_id IS the set member.  `[K][0]` navigates into the
key's array structure via the existing path resolution.

### Files

- `src/qjson_query.py` — `[K]` dispatches on value type
- `native/qjson_sqlite_ext.c` — same in C path resolver
- Tests: set iteration, filtering, cross-path joins over sets

## 2. Recursive queries (WITH RECURSIVE)

```python
# Transitive closure: all nodes reachable from 'a'
qjson_select(conn, root, '.path[K]',
    where_expr='.path[K][0] == "a"',
    recursive={
      'path': {
        'base': '.edge[K]',
        'step': '.edge[K1][1] == .path[K2][0]'
      }
    })
```

Or detect recursion from stored rules:

```javascript
{
  edge: {[a, b], [b, c], [c, d]},
  rules: {
    path: {
      [[path, ?X, ?Y], [edge, ?X, ?Y]],
      [[path, ?X, ?Y], [edge, ?X, ?Z], [path, ?Z, ?Y]]
    }
  }
}
```

Compiles to:

```sql
WITH RECURSIVE path_results AS (
  SELECT key_id FROM qjson_object_item
  WHERE object_id = (edge set)

  UNION

  SELECT E.key_id
  FROM qjson_object_item E
  JOIN path_results P ON (E arg1) == (P arg0)
  WHERE E.object_id = (edge set)
)
SELECT * FROM path_results WHERE (pattern)
```

UNION eliminates duplicates (fixpoint).

### Detection

Body goal references head predicate → recursive → WITH RECURSIVE.

### Files

- `src/qjson_query.py` — recursive query compilation
- `native/qjson_sqlite_ext.c` — CTE support in SQL generation
- Tests: graph reachability, transitive closure

## That's it

Everything else is already in qjson (store, update, query,
cross-path joins, constraint solver) or lives in wyatt
(Prolog parser, assert/retract semantics, ephemeral, react,
native, send, fossilize).
