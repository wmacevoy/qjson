# Plan: Lemon parser + unified type system

## Motivation

The hand-written recursive descent parser in `native/qjson.c` (989 lines)
works, but it mixes tokenization, parsing, and semantic actions in one pass.
Lemon is SQLite's own parser generator — production-hardened, re-entrant,
zero global state.  Replacing the recursive descent with Lemon gives us:

- **The grammar file is the spec.** One `.y` file that can be diffed
  against `docs/qjson.md` line-by-line.
- **Conflict detection at build time.** If a grammar change is ambiguous,
  `lemon` rejects it before any code runs.
- **Battle-tested error recovery.** SQLite parses adversarial SQL
  all day with the same machinery.
- **Cleaner separation.** Lexer handles bytes.  Parser handles structure.
  Semantic actions handle values.  No interleaving.

Separately, the `libbfxx` C++ library now defines a bitmask-based
`qjson_type` enum that encodes type groups in the bit pattern.  This
replaces the sequential C enum and should be the single source of truth
for both projects.

## Shared type enum

The type IDs live in a C-compatible header included by both projects:

```c
/* qjson_types.h — shared between qjson (C) and libbfxx (C++) */

typedef enum {
    QJSON_NULL       = 0x000,  /* 0b0'0000'0000 */
    QJSON_FALSE      = 0x010,  /* 0b0'0001'0000 */
    QJSON_TRUE       = 0x011,  /* 0b0'0001'0001 */
    QJSON_NUMBER     = 0x021,  /* 0b0'0010'0001 */
    QJSON_BIGINT     = 0x022,  /* 0b0'0010'0010 */
    QJSON_BIGFLOAT   = 0x024,  /* 0b0'0010'0100 */
    QJSON_BIGDECIMAL = 0x028,  /* 0b0'0010'1000 */
    QJSON_BLOB       = 0x040,  /* 0b0'0100'0000 */
    QJSON_STRING     = 0x081,  /* 0b0'1000'0001 */
    QJSON_ARRAY      = 0x101,  /* 0b1'0000'0001 */
    QJSON_OBJECT     = 0x102,  /* 0b1'0000'0010 */
    QJSON_UNBOUND    = 0x1FF,  /* 0b1'1111'1111 */

    /* Group masks */
    QJSON_BOOLEAN    = 0x010,
    QJSON_NUMERIC    = 0x020,
    QJSON_CONTAINER  = 0x100
} qjson_type;

/* Group tests:
 *   (type & QJSON_BOOLEAN)   — FALSE or TRUE
 *   (type & QJSON_NUMERIC)   — NUMBER, BIGINT, BIGDECIMAL, BIGFLOAT
 *   (type & QJSON_CONTAINER) — ARRAY or OBJECT
 *   (type & 0x01) on BOOLEAN — truth value
 *   UNBOUND & any_type == any_type (matches everything)
 */
```

### Migration from current enum

Current `qjson.h` uses sequential values (`QJSON_NULL=0`, `QJSON_TRUE=1`,
etc.).  The new bitmask values are a breaking change to the C ABI.
Migration:

1. Add `qjson_types.h` with the bitmask enum.
2. Update `qjson.h` to include it (replacing the inline enum).
3. Update `qjson.c` — all `switch` statements on `type` get `default:`
   cases since the values are no longer contiguous.
4. Update `qjson_sqlite_ext.c` — type string mapping (`"number"`,
   `"bigint"`, `"bigdec"`, `"bigfloat"`, etc.) uses the new enum values.
5. Run all existing tests to verify equivalence.

The JS and Python implementations are unaffected (they use string type
names, not integer IDs).

## Parser architecture

```
  bytes
    │
    ▼
┌─────────┐   tokens   ┌─────────┐   qjson_val*
│  lexer  │ ─────────► │  lemon  │ ──────────►  arena-allocated tree
│ (hand-  │            │  parser │
│ written)│            │ (.y → .c│
└─────────┘            └─────────┘
```

### Lexer (hand-written C)

Responsibility: byte stream -> tokens.  This is where QJSON's
character-level complexity lives.

**Token types:**

```c
enum {
    TK_NULL, TK_TRUE, TK_FALSE,
    TK_NUMBER, TK_NUMBER_N, TK_NUMBER_M, TK_NUMBER_L,
    TK_STRING, TK_IDENT, TK_BLOB, TK_UNBOUND,
    TK_LBRACKET, TK_RBRACKET, TK_LBRACE, TK_RBRACE,
    TK_COMMA, TK_COLON,
    TK_EOF
};
```

**What the lexer handles:**
- Whitespace + comment skipping (line and nested block)
- Number parsing: `-? digits (. digits)? ((e|E) (+|-)? digits)?`
  followed by suffix detection -> `TK_NUMBER` / `TK_NUMBER_N` / `_M` / `_L`
- String parsing: `"..."` with all escapes (`\uXXXX`, `\u{...}`, etc.)
- Blob parsing: `0j` prefix -> JS64 decode
- Unbound: `?` + optional ident or `"quoted"`
- Bare identifiers: `[a-zA-Z_$][a-zA-Z0-9_$]*`
  - `true`/`false`/`null` -> keyword tokens
  - everything else -> `TK_IDENT`

**Token value:** Each token carries a pointer into the source buffer
(or arena-allocated decoded string for blobs/escapes) plus length.
Zero-copy where possible.

The existing lexer logic in `qjson.c` (lines ~50-400) migrates here
almost unchanged — just stripped of the recursive parse calls.

### Grammar (`native/qjson.y`)

```
%token_type { qjson_token }
%extra_argument { qjson_parse_ctx *ctx }

value ::= TK_NULL.                        { qjson_push_null(ctx); }
value ::= TK_TRUE.                        { qjson_push_true(ctx); }
value ::= TK_FALSE.                       { qjson_push_false(ctx); }
value ::= TK_NUMBER(T).                   { qjson_push_number(ctx, T); }
value ::= TK_NUMBER_N(T).                 { qjson_push_bigint(ctx, T); }
value ::= TK_NUMBER_M(T).                 { qjson_push_bigdecimal(ctx, T); }
value ::= TK_NUMBER_L(T).                 { qjson_push_bigfloat(ctx, T); }
value ::= TK_STRING(T).                   { qjson_push_string(ctx, T); }
value ::= TK_BLOB(T).                     { qjson_push_blob(ctx, T); }
value ::= TK_UNBOUND(T).                  { qjson_push_unbound(ctx, T); }
value ::= TK_IDENT(T).                    { qjson_push_ident(ctx, T); }
value ::= array.
value ::= object.

array ::= TK_LBRACKET TK_RBRACKET.       { qjson_push_array(ctx, 0); }
array ::= TK_LBRACKET elements
           opt_comma TK_RBRACKET.         { qjson_push_array(ctx, ctx->count); }

elements ::= value.                       { ctx->count = 1; }
elements ::= elements TK_COMMA value.     { ctx->count++; }

object ::= TK_LBRACE TK_RBRACE.          { qjson_push_object(ctx, 0); }
object ::= TK_LBRACE entries
            opt_comma TK_RBRACE.          { qjson_push_object(ctx, ctx->count); }

entries ::= entry.                        { ctx->count = 1; }
entries ::= entries TK_COMMA entry.       { ctx->count++; }

entry ::= value TK_COLON value.           { qjson_push_kv(ctx); }
entry ::= value.                          { qjson_push_set_entry(ctx); }

opt_comma ::= .
opt_comma ::= TK_COMMA.
```

That's ~30 productions for the full QJSON grammar.  Semantic actions
push arena-allocated `qjson_val` nodes onto a value stack.

### Parse context

```c
typedef struct {
    qjson_arena *arena;        /* arena allocator (existing) */
    qjson_val  **stack;        /* value stack */
    int          stack_top;
    int          count;        /* element counter for current container */
    const char  *error;        /* first error message, or NULL */
} qjson_parse_ctx;
```

The value stack replaces the implicit recursion stack of the current
recursive descent parser.  Arena allocation is unchanged.

## Phases

### Phase 0: Vendor Lemon

- Copy `lemon.c` and `lempar.c` from SQLite/SQLCipher source tree
  into `native/lemon/`.
- Add Makefile target: `native/lemon/lemon` (build the tool).
- Verify it builds and runs: `./native/lemon/lemon --help`.
- Lemon is a single C file (~5000 lines).  No dependencies.

### Phase 1: Shared type header

- Create `native/qjson_types.h` with the bitmask enum.
- Update `qjson.h` to `#include "qjson_types.h"` replacing the
  inline enum.
- Update all `switch` statements in `qjson.c` and
  `qjson_sqlite_ext.c` for non-contiguous values.
- Update type-string mappings (`"number"`, `"bigint"`, etc.).
- Run all existing C tests.  Must pass unchanged.
- Update `libbfxx/include/numeric.hpp` to include the shared header
  (or keep in sync — same values, verified by a static_assert).

### Phase 2: Extract lexer

- Create `native/qjson_lex.h` / `native/qjson_lex.c`.
- Move all tokenization logic from `qjson.c` into the lexer:
  `skip_ws()`, number scanning, string scanning, blob decoding,
  unbound parsing, ident parsing, keyword detection.
- Expose `qjson_lex(src, len, &pos) -> token`.
- The existing `qjson.c` parser calls the lexer instead of
  doing inline byte scanning.
- All tests still pass — this is a pure refactor.

### Phase 3: Lemon grammar

- Write `native/qjson.y` (the grammar file above).
- Makefile: `lemon native/qjson.y` -> `native/qjson.c` (generated)
  and `native/qjson.h` (token definitions).
- Implement semantic actions (`qjson_push_*`) using the existing
  arena allocator.
- The generated parser + hand-written lexer + semantic actions
  replace the recursive descent parser.
- `qjson_parse()` entry point calls `qjson_lex()` in a loop,
  feeding tokens to the Lemon state machine.
- All tests still pass.

### Phase 4: Stringify stays

The serializer (`qjson_stringify`) is not parser-generated.
It stays as hand-written C — it's simple traversal with
suffix emission.  No changes needed beyond the type enum update
from Phase 1.

### Phase 5: Test vectors from libbfxx

- Add a `test/vectors/` directory.
- Generate numeric comparison test vectors from libbfxx's 444
  tests: pairs of QJSON literals + expected comparison results
  for all 6 operators.
- C tests load and validate these vectors.
- Python and JS tests do the same.
- Cross-language equivalence is now mechanically verified from
  a single source of truth (the C++ numeric layer).

## File inventory (done)

```
native/
  lemon/
    lemon.c          NEW  — vendored Lemon parser generator
    lempar.c         NEW  — Lemon parser template
  qjson_types.h      NEW  — shared bitmask type enum
  qjson_lex.h        NEW  — lexer API
  qjson_lex.c        NEW  — extracted tokenizer (~250 lines)
  qjson_parse.y      NEW  — Lemon grammar (~30 productions)
  qjson_parse.c      GEN  — Lemon-generated parser
  qjson_parse.h      GEN  — Lemon-generated token IDs
  qjson_parse_ctx.h  NEW  — parse context: value stack + semantic actions
  qjson.h            MOD  — #include qjson_types.h, remove inline enum
  qjson.c            MOD  — recursive descent replaced with Lemon driver
  qjson_sqlite_ext.c MOD  — QJSON_NUMBER/QJSON_BIGDECIMAL rename

test/
  vectors/           TODO — generated from libbfxx (Phase 5)
```

JS, Python, and SQL implementations are unchanged (they don't use
the C type enum).

## Risks and mitigations

**Grammar conflicts.** The QJSON grammar is simple enough that
LALR(1) conflicts are unlikely.  The only subtlety is `entry ::= value`
vs `entry ::= value COLON value` — Lemon resolves this with a 1-token
lookahead (`:` or `,`/`}`).  If conflicts appear, `%left`/`%right`
directives handle them.

**Bare identifiers.** `true`, `false`, `null` are keywords *and*
identifier prefixes (`truthy`, `nullable`).  The lexer handles this
(word boundary check), not the parser.  No grammar impact.

**Arena allocation.** Lemon's generated parser uses `malloc` for its
own state stack.  This is a small fixed-size allocation (~200 bytes)
separate from the value arena.  Acceptable — the arena contract
(zero malloc for values) is preserved.

**Error messages.** The current recursive descent gives precise
error locations.  Lemon's `%syntax_error` directive gives token
position.  Combined with the lexer's byte offset tracking, error
quality should be equivalent or better.

## Non-goals

- **Rewriting the JS or Python parsers.** They stay as recursive descent.
  They're correct, tested, and portable.  Lemon is for the C deployment
  target only.
- **Changing the QJSON grammar.** This plan replaces the implementation,
  not the language.  Same inputs, same outputs.
- **C++ in qjson.**  The C project stays C.  libbfxx stays C++.
  They share `qjson_types.h` and test vectors, not code.

## Future work

**PostgreSQL as C extension.**  The current `sql/qjson_pg.sql` is a
PL/pgSQL reimplementation of comparison, reconstruction, and query
translation — archived, not maintained.  When PostgreSQL support returns,
the right approach is a C extension (`CREATE FUNCTION ... LANGUAGE C`)
that links the same `qjson.c` / `qjson_lex.c` / `qjson_parse.c` code.
Same binary, different host.  The SQL reimplementation shares zero code
with the C layer and cannot benefit from libbf, Lemon, or the bitmask
type system.
