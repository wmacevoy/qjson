# Plan: Migrate compute + crypto from qjson to wyatt

## Context

QJSON v1.1.4 drew a clean boundary:

```
Database layer (qjson — SQLite + PostgreSQL identical):
  store, load, reconstruct, select, compare

Application layer (wyatt — has libbf + LibreSSL):
  arithmetic, solver, closure, projection, crypto
```

Wyatt currently vendors qjson v1.1.3 and already loads the
SQLite extension for cross-path comparison.  The compute
functions (solver, closure) and crypto functions already exist
in qjson's C code — wyatt just needs to own them.

## What moves

### From qjson to wyatt

| Function group | Source in qjson | Destination in wyatt |
|---------------|-----------------|---------------------|
| Arithmetic (17) | `native/qjson_sqlite_ext.c` | stays in qjson_ext (SQLite-only) |
| Solver (14) | `native/qjson_sqlite_ext.c` | stays in qjson_ext (SQLite-only) |
| Closure | `native/qjson_sqlite_ext.c` + `src/qjson_query.py` | stays in qjson_ext + Python |
| Crypto (15) | `native/qjson_crypto.c` | `wyatt/native/` or `wyatt/vendor/qjson/` |
| Projection | `src/qjson_sql.py` (`round_down`/`round_up`) | already in wyatt via vendor |

Nothing physically moves — the C code stays in qjson's `native/`
directory.  Wyatt just vendors the updated qjson and links the
extension with LibreSSL when crypto is needed.

### What wyatt already does right

- `y8_datalog.py` already calls `qjson_select` and `qjson_closure`
- `y8-datalog.js` already loads `qjson_ext`
- The solver (`qjson_solve`) is called via SQL — it works because
  wyatt uses SQLite with the extension loaded
- Projection (`round_down`/`round_up`) happens in the Python/JS
  adapter before SQL INSERT — already application-layer

### What needs changing

1. **Update vendored qjson to v1.1.4+**
   - `vendor/qjson/` → new `key_id` schema (was `key TEXT`)
   - Complex keys, set shorthand, bare identifiers
   - `[K]` set iteration, cross-path WHERE in C
   - Bidirectional transcendental solvers
   - All tests pass with `make test`

2. **Expose crypto via native hooks**
   ```prolog
   % Wyatt native hooks for crypto
   native(sha256(Data), Hash).
   native(encrypt(Plaintext, Key), Ciphertext).
   native(decrypt(Ciphertext, Key), Plaintext).
   native(hmac(Data, Key), Mac).
   native(jwt_sign(Payload, Secret), Token).
   native(jwt_verify(Token, Secret), Payload).
   native(random_bytes(N), Bytes).
   native(hkdf(Ikm, Salt, Info, Len), Key).
   native(shamir_split(Secret, M, N), Shares).
   native(shamir_recover(Indices, Keys), Secret).
   ```

   Implementation: register these as native hooks in y8_datalog
   that call the C functions in `qjson_crypto.c`.

3. **Build the crypto extension**
   ```makefile
   # In wyatt's Makefile:
   qjson_ext_crypto$(EXT):
       $(CC) $(CFLAGS) -DQJSON_USE_CRYPTO -DQJSON_USE_LIBBF \
           -I$(LIBRESSL)/include \
           vendor/qjson/native/qjson_sqlite_ext.c \
           vendor/qjson/native/qjson.c \
           vendor/qjson/native/qjson_crypto.c \
           vendor/qjson/native/libbf/libbf.c \
           vendor/qjson/native/libbf/cutils.c \
           $(LIBRESSL)/lib/libcrypto.a -lm
   ```

4. **PostgreSQL path**
   - Wyatt uses Python `qjson_sql.py` adapter for PG
   - Solver: wyatt computes via libbf, projects with
     `round_down`/`round_up`, stores via adapter
   - Crypto: wyatt calls C functions directly (not SQL)
   - Closure: wyatt calls Python `qjson_closure()` which
     generates `WITH RECURSIVE` SQL — works on any backend

## Artifacts to find

### In qjson (current, post-v1.1.4)

| What | Where |
|------|-------|
| Crypto C code | `native/qjson_crypto.c`, `native/qjson_crypto.h` |
| Crypto SQL wrappers | `native/qjson_sqlite_ext.c` (`#ifdef QJSON_USE_CRYPTO`) |
| Solver (binary ops) | `native/qjson_sqlite_ext.c` (`_sql_solve`, `SOLVE_ADD..POW`) |
| Solver (unary ops) | `native/qjson_sqlite_ext.c` (`_sql_solve_unary`, `USOLVE_*`) |
| Expression decomposition | `native/qjson_sqlite_ext.c` (`_ast_to_vid`) |
| Python solver | `src/qjson_query.py` (`qjson_solve`, `_decompose_to_constraints`) |
| Python closure | `src/qjson_query.py` (`qjson_closure`) |
| Shamir (ported from strata) | `native/qjson_crypto.c` (`qjson_shamir_split/recover`) |
| Shamir original | `../strata/src/shamir.c` (libsodium version) |
| Makefile crypto target | `Makefile` (`qjson_ext_crypto` target) |

### In qjson git history (if needed)

| What | Where to find |
|------|---------------|
| PG crypto functions | `git show v1.1.4:sql/qjson_pg.sql` (removed in post-v1.1.4) |
| PG solver | `git show v1.1.3:sql/qjson_pg.sql` (removed in v1.1.4) |
| AES-GCM version | `git show v1.1.4~3:native/qjson_crypto.c` (before CBC switch) |
| Original qjson_kv struct | `git show v1.1.1:native/qjson.h` (before key_id) |

## Migration steps

### Phase 1: Update vendor (no new features)

```bash
cd wyatt
git submodule update vendor/qjson  # or copy
make test  # verify existing tests pass
```

The `key_id` schema change means existing test databases will
need recreation.  The adapter's `setup()` creates fresh tables.

### Phase 2: Add crypto native hooks

1. Add LibreSSL as a build dependency (already in CI for SQLCipher)
2. Build `qjson_ext_crypto` instead of `qjson_ext`
3. Register native hooks for crypto in y8_datalog.py/js
4. Add examples: JWT auth, encrypted storage, Shamir key split

### Phase 3: Expose solver via native hooks (optional)

The solver already works via the SQLite extension.  But if wyatt
wants to expose it as Prolog native hooks:

```prolog
native(solve(RootId, Formula), Result).
```

This just calls `qjson_solve()` which is already in the extension.

### Phase 4: PostgreSQL backend

For PG deployments, wyatt needs to:
1. Use `qjson_sql.py` adapter (already does)
2. Call solver via Python (not SQL) — `qjson_query.qjson_solve()`
3. Call closure via Python — `qjson_query.qjson_closure()`
4. Call crypto via C FFI or Python ctypes

The Python adapter already handles projection correctly.
The solver and closure Python implementations already work
with any database backend.

## Not in scope

- qjq CLI (separate plan, PLAN-qjq.md)
- QuickJS integration (qjq plan phase 1)
- WASM crypto (needs Emscripten build of qjson_crypto.c)
