#!/bin/bash
set -e

# Build SQLCipher WASM with QJSON extension (libbf exact arithmetic).
#
# Prerequisites:
#   - Emscripten SDK (emsdk) activated
#   - sqlcipher-libressl checked out at $SQLCIPHER_DIR
#   - LibreSSL built for native ($HOME/libressl) and WASM ($HOME/libressl-wasm)
#
# Usage:
#   ./wasm/build.sh [sqlcipher-dir]
#
# Output:
#   wasm/dist/sqlcipher.js
#   wasm/dist/sqlcipher.wasm
#   wasm/dist/sqlcipher-worker.js
#   wasm/dist/sqlcipher-api.js
#   wasm/dist/qjson-wasm.js
#   wasm/dist/mortgage.html

QJSON_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SQLCIPHER_DIR="${1:-$QJSON_DIR/../sqlcipher-libressl}"
LIBRESSL_NATIVE="${LIBRESSL_NATIVE:-$HOME/libressl}"
LIBRESSL_WASM="${LIBRESSL_WASM:-$HOME/libressl-wasm}"

echo "QJSON:    $QJSON_DIR"
echo "SQLCipher: $SQLCIPHER_DIR"
echo "LibreSSL:  $LIBRESSL_NATIVE (native) / $LIBRESSL_WASM (wasm)"

# Ensure amalgamation exists
if [ ! -f "$SQLCIPHER_DIR/sqlite3.c" ]; then
    echo "Building SQLCipher amalgamation..."
    cd "$SQLCIPHER_DIR"
    ./configure --with-tempstore=yes \
        CFLAGS="-DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_OPENSSL \
                -DSQLITE_EXTRA_INIT=sqlcipher_extra_init \
                -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown \
                -I$LIBRESSL_NATIVE/include" \
        LDFLAGS="$LIBRESSL_NATIVE/lib/libcrypto.a"
    make sqlite3.c
fi

# Prepare dist
mkdir -p "$QJSON_DIR/wasm/dist"
cd "$QJSON_DIR/wasm/dist"

# Copy sources
cp "$SQLCIPHER_DIR/sqlite3.c" .
cp "$SQLCIPHER_DIR/sqlite3.h" .
cp "$SQLCIPHER_DIR/wasm/sqlcipher_wasm.c" .
cp "$SQLCIPHER_DIR/wasm/opfs_vfs.c" .
cp "$QJSON_DIR/wasm/qjson_wasm_init.c" .
cp "$QJSON_DIR/native/qjson.h" .
cp "$QJSON_DIR/native/qjson.c" .
cp "$QJSON_DIR/native/qjson_sqlite_ext.c" .
cp "$QJSON_DIR/native/libbf/libbf.h" .
cp "$QJSON_DIR/native/libbf/libbf.c" .
cp "$QJSON_DIR/native/libbf/cutils.h" .
cp "$QJSON_DIR/native/libbf/cutils.c" .

EXPORTED='["_wasm_db_open","_wasm_db_close","_wasm_db_exec","_wasm_db_errmsg","_wasm_db_changes","_wasm_db_total_changes","_wasm_db_key","_wasm_db_prepare","_wasm_stmt_finalize","_wasm_stmt_reset","_wasm_stmt_clear_bindings","_wasm_stmt_step","_wasm_stmt_bind_text","_wasm_stmt_bind_int","_wasm_stmt_bind_double","_wasm_stmt_bind_null","_wasm_stmt_bind_parameter_count","_wasm_stmt_columns","_wasm_stmt_colname","_wasm_stmt_coltype","_wasm_stmt_int","_wasm_stmt_double","_wasm_stmt_text","_sqlite3_opfs_init","_malloc","_free"]'
RUNTIME='["cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8"]'

echo "Compiling WASM..."
emcc sqlcipher_wasm.c qjson_wasm_init.c qjson.c libbf.c cutils.c \
    -I. -I"$LIBRESSL_NATIVE/include" \
    -L"$LIBRESSL_WASM/lib" \
    -O2 \
    -DSQLITE_HAS_CODEC \
    -DSQLCIPHER_CRYPTO_OPENSSL \
    -DSQLITE_EXTRA_INIT=sqlcipher_extra_init \
    -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown \
    -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_THREADSAFE=1 \
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
    -s ENVIRONMENT='web,worker,node' \
    -s FILESYSTEM=1 \
    "$LIBRESSL_WASM/lib/libcrypto.a" \
    -o sqlcipher.js

# Clean up source copies
rm -f sqlite3.c sqlite3.h sqlcipher_wasm.c opfs_vfs.c \
      qjson_wasm_init.c qjson.h qjson.c qjson_sqlite_ext.c \
      libbf.h libbf.c cutils.h cutils.c

# Copy JS files
cp "$SQLCIPHER_DIR/wasm/sqlcipher-worker.js" .
cp "$SQLCIPHER_DIR/wasm/sqlcipher-api.js" .
cp "$QJSON_DIR/examples/wasm/qjson-wasm.js" .

ls -lh sqlcipher.js sqlcipher.wasm
echo "Done. Output in wasm/dist/"
