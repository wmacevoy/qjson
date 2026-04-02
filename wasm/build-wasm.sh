#!/bin/bash
set -e

# Build SQLCipher + QJSON WASM bundle inside Docker.
#
# Usage: ./wasm/build-wasm.sh
# Output: wasm/dist/sqlcipher.{js,wasm} + JS helpers + qjson.js
#
# Requires: docker, ../sqlcipher-libressl

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QJSON_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SQLCIPHER_DIR="$(cd "${SQLCIPHER_DIR:-$QJSON_DIR/../sqlcipher-libressl}" && pwd)"

echo "QJSON:     $QJSON_DIR"
echo "SQLCipher: $SQLCIPHER_DIR"

if [ ! -d "$SQLCIPHER_DIR/src" ]; then
    echo "Error: sqlcipher-libressl not found at $SQLCIPHER_DIR"
    exit 1
fi

echo "=== Building WASM builder image (first time takes ~5min) ==="
docker build -t qjson-wasm-builder "$SCRIPT_DIR"

echo "=== Compiling WASM ==="
mkdir -p "$QJSON_DIR/wasm/dist"

docker run --rm \
    -v "$QJSON_DIR:/qjson:ro" \
    -v "$SQLCIPHER_DIR:/sqlcipher" \
    -v "$QJSON_DIR/wasm/dist:/output" \
    qjson-wasm-builder \
    bash -c '
set -e

# ── Build SQLCipher amalgamation ──
cd /sqlcipher
if [ ! -f sqlite3.c ]; then
    echo "==> Building amalgamation"
    ./configure --with-tempstore=yes \
      CFLAGS="-DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_OPENSSL \
              -DSQLITE_EXTRA_INIT=sqlcipher_extra_init \
              -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown \
              -I/opt/libressl-native/include" \
      LDFLAGS="/opt/libressl-native/lib/libcrypto.a"
    make sqlite3.c
fi

# ── Compile WASM ──
cd /build
cp /sqlcipher/sqlite3.c /sqlcipher/sqlite3.h .
cp /sqlcipher/src/sqlite3ext.h .
cp /sqlcipher/wasm/sqlcipher_wasm.c /sqlcipher/wasm/opfs_vfs.c .
cp /qjson/wasm/qjson_wasm_init.c .
cp /qjson/native/qjson.h /qjson/native/qjson_types.h .
cp /qjson/native/qjson.c .
cp /qjson/native/qjson_lex.h /qjson/native/qjson_lex.c .
cp /qjson/native/qjson_parse.h /qjson/native/qjson_parse.c .
cp /qjson/native/qjson_parse_ctx.h .
cp /qjson/native/qjson_sqlite_ext.c .
cp /qjson/native/libbf/libbf.h /qjson/native/libbf/libbf.c .
cp /qjson/native/libbf/cutils.h /qjson/native/libbf/cutils.c .

EXPORTED='"'"'["_wasm_db_open","_wasm_db_close","_wasm_db_exec","_wasm_db_errmsg","_wasm_db_changes","_wasm_db_total_changes","_wasm_db_key","_wasm_db_prepare","_wasm_stmt_finalize","_wasm_stmt_reset","_wasm_stmt_clear_bindings","_wasm_stmt_step","_wasm_stmt_bind_text","_wasm_stmt_bind_int","_wasm_stmt_bind_double","_wasm_stmt_bind_null","_wasm_stmt_bind_parameter_count","_wasm_stmt_columns","_wasm_stmt_colname","_wasm_stmt_coltype","_wasm_stmt_int","_wasm_stmt_double","_wasm_stmt_text","_sqlite3_opfs_init","_malloc","_free"]'"'"'
RUNTIME='"'"'["cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8"]'"'"'

echo "==> emcc compile"
emcc sqlcipher_wasm.c opfs_vfs.c qjson_wasm_init.c \
     qjson.c qjson_lex.c qjson_parse.c \
     libbf.c cutils.c \
    -I. -I/opt/libressl-wasm/include \
    -O2 \
    -DSQLITE_HAS_CODEC \
    -DSQLCIPHER_CRYPTO_OPENSSL \
    -DSQLITE_EXTRA_INIT=sqlcipher_extra_init \
    -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown \
    -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_THREADSAFE=0 \
    -DSQLITE_TEMP_STORE=2 \
    -DSQLITE_ENABLE_FTS5 \
    -DSQLITE_ENABLE_JSON1 \
    -DQJSON_USE_LIBBF \
    -s WASM=1 \
    -s EXPORTED_FUNCTIONS="$EXPORTED" \
    -s EXPORTED_RUNTIME_METHODS="$RUNTIME" \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=16777216 \
    -s MODULARIZE=1 \
    -s EXPORT_NAME="initSqlcipher" \
    -s ENVIRONMENT="web,worker" \
    /opt/libressl-wasm/lib/libcrypto.a \
    -o /output/sqlcipher.js

echo "==> Done"
ls -lh /output/sqlcipher.*
'

# Copy JS files
cp "$SQLCIPHER_DIR/wasm/sqlcipher-api.js" "$QJSON_DIR/wasm/dist/" 2>/dev/null || true
cp "$SQLCIPHER_DIR/wasm/sqlcipher-worker.js" "$QJSON_DIR/wasm/dist/" 2>/dev/null || true
cp "$QJSON_DIR/src/qjson.js" "$QJSON_DIR/wasm/dist/"

echo "=== Build complete ==="
ls -lh "$QJSON_DIR/wasm/dist/"
