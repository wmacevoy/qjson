CC = cc
FACTS_SRC = vendor/facts/src/facts.c
FACTS_INC = -Ivendor/facts/include
CFLAGS = -O2 -std=c11 -fPIC -DQJSON_USE_LIBBF -Inative -Inative/libbf $(FACTS_INC)
LIBBF_SRC = native/libbf/libbf.c native/libbf/cutils.c
QJSON_SRC = native/qjson.c native/qjson_lex.c native/qjson_parse.c

# QuickJS (use its cutils instead of libbf's to avoid duplicates)
QJS_DIR = vendor/quickjs
QJS_SRC = $(QJS_DIR)/quickjs.c $(QJS_DIR)/quickjs-libc.c \
          $(QJS_DIR)/cutils.c $(QJS_DIR)/libregexp.c \
          $(QJS_DIR)/libunicode.c $(QJS_DIR)/dtoa.c
QJS_CFLAGS = -O2 -DCONFIG_VERSION=\"2025-09-13\" -I$(QJS_DIR)
# libbf without its own cutils (QuickJS cutils is a superset)
LIBBF_SRC_NOCU = native/libbf/libbf.c

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  SHARED = -dynamiclib -undefined dynamic_lookup
  EXT_SUFFIX = .dylib
else
  SHARED = -shared
  EXT_SUFFIX = .so
endif

SQLCIPHER_DIR ?= ../sqlcipher-libressl
LIBRESSL_DIR ?= $(HOME)/libressl

.PHONY: all clean test

all: qjson_ext$(EXT_SUFFIX)

# Lemon parser generator
native/lemon/lemon: native/lemon/lemon.c
	$(CC) -O2 -o $@ $<

# Generated parser from grammar
native/qjson_parse.c native/qjson_parse.h: native/qjson_parse.y native/lemon/lemon
	native/lemon/lemon -Tnative/lemon/lempar.c native/qjson_parse.y

# Embedded qjson.js for qjq
native/qjson_js.h: src/qjson.js native/embed_js.sh
	sh native/embed_js.sh src/qjson.js native/qjson_js.h qjson_js_source

# SQLite loadable extension (with libbf for exact comparison)
qjson_ext$(EXT_SUFFIX): native/qjson_sqlite_ext.c $(QJSON_SRC) $(LIBBF_SRC)
	$(CC) $(CFLAGS) $(SHARED) -o $@ $^ -lm

# SQLCipher loadable extension (same code, linked against sqlcipher headers)
qjson_ext_sqlcipher$(EXT_SUFFIX): native/qjson_sqlite_ext.c $(QJSON_SRC) $(LIBBF_SRC)
	$(CC) $(CFLAGS) $(SHARED) -I$(SQLCIPHER_DIR)/src -o $@ $^ -lm

# Extension with crypto functions (requires LibreSSL)
qjson_ext_crypto$(EXT_SUFFIX): native/qjson_sqlite_ext.c $(QJSON_SRC) native/qjson_crypto.c $(LIBBF_SRC)
	$(CC) $(CFLAGS) -DQJSON_USE_CRYPTO -I$(LIBRESSL_DIR)/include \
		$(SHARED) -o $@ $^ $(LIBRESSL_DIR)/lib/libcrypto.a -lm

# qjq — QJSON query tool (QuickJS + QJSON + libbf)
# QuickJS renamed dbuf_putc → __dbuf_putc; libbf_shim.c bridges.
# libbf.o must be compiled against QuickJS cutils.h, not its own.
native/libbf_qjs.o: native/libbf/libbf.c
	$(CC) -O2 -c -I$(QJS_DIR) -o $@ $<

native/libbf_shim.o: native/libbf_shim.c
	$(CC) -O2 -c -Inative -o $@ $<

qjq: native/qjq.c native/qjson_js.h $(QJSON_SRC) native/libbf_qjs.o native/libbf_shim.o $(QJS_SRC)
	$(CC) $(QJS_CFLAGS) -DQJSON_USE_LIBBF -Inative -Inative/libbf -I$(QJS_DIR) \
		-o $@ native/qjq.c $(QJSON_SRC) native/libbf_qjs.o \
		native/libbf_shim.o $(QJS_SRC) -lm -lpthread

# C test binary
test_qjson: test/test_qjson.c $(QJSON_SRC) $(LIBBF_SRC) $(FACTS_SRC)
	$(CC) $(CFLAGS) -frounding-math -o $@ $^ -lm

test: all test_qjson
	./test_qjson
	python3 test/test_qjson.py
	python3 test/test_qjson_sql.py

clean:
	rm -f qjson_ext$(EXT_SUFFIX) test_qjson qjq native/lemon/lemon
	rm -f native/qjson_parse.c native/qjson_parse.h native/qjson_parse.out
	rm -f native/qjson_js.h
