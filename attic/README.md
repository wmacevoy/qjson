# attic — reference material for the datalog repo

Code parked here on 2026-04-29 as part of refocusing qjson on its three layers:

- **bedrock**: format spec, grammar, SQL projection (`docs/qjson.md`)
- **foundation**: C reference impl (parse/stringify/project, libbf arithmetic, comparison, SQLite extension)
- **living space**: hosts (qjq, Python, JS) that bind to foundation

The query language — pattern matching, unification, equation solving,
assert/retract/signal, reactive watchers — is its own layer with its
own repo: [`../datalog`](https://github.com/wmacevoy/datalog) (sibling
checkout typically at `../datalog/`).

That work was prototyped here first. The prototypes are kept as a
reference while the datalog repo's implementations stabilize. They
are **not built, not tested, and not part of the qjson API**.

## What's here

| File | Role | Successor |
|------|------|-----------|
| `native/qjson_resolve.{c,h}` | In-memory unification + pattern matching | `../datalog/native/datalog.c` |
| `native/qjson_db.{c,h}` | Reactive store: assert/retract/signal/freeze, watchers | `../datalog/native/datalog.c` (engine) |
| `native/qjson_solve_ext.c` | SQLite functions: `qjson_solve_{add,sub,mul,div,pow,sqrt,exp,log,sin,cos,tan,asin,acos,atan}` + `qjson_solve` (expression parser & propagator) | datalog resolver calls qjson arithmetic primitives |
| `test/test_resolve.c` | Resolver tests (grandparent, joins) | datalog test suite |
| `test/test_brain.c` | Ephemeral / freeze / signal tests | datalog test suite |
| `test/test_reactive.c` | watch/unwatch notification tests | datalog test suite |
| `test/test_solve.c` | Constraint-solver tests (compound interest, inverse ops) | datalog test suite |

## When to delete

When `../datalog` covers the same ground with tests passing, this
directory can go. Until then, it's a working reference for whoever
ports the ideas — types, function signatures, edge cases handled,
tricks for libbf type widening across the solve operations.

Do not link against any of this from the foundation or living space.
