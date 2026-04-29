CC = cc
FACTS_SRC = vendor/facts/src/facts.c
FACTS_INC = -Ivendor/facts/include
CFLAGS = -O2 -std=c11 -fPIC -DQJSON_USE_LIBBF -Inative -Inative/libbf $(FACTS_INC)
LIBBF_SRC = native/libbf/libbf.c native/libbf/cutils.c
QJSON_SRC = native/qjson.c native/qjson_lex.c native/qjson_parse.c

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  SHARED = -dynamiclib -undefined dynamic_lookup
  EXT_SUFFIX = .dylib
else
  SHARED = -shared
  EXT_SUFFIX = .so
endif

SQLCIPHER_DIR ?= ../sqlcipher-libressl

.PHONY: all clean test

all: qjson_ext$(EXT_SUFFIX)

# Lemon parser generator
native/lemon/lemon: native/lemon/lemon.c
	$(CC) -O2 -o $@ $<

# Generated parser from grammar
native/qjson_parse.c native/qjson_parse.h: native/qjson_parse.y native/lemon/lemon
	native/lemon/lemon -Tnative/lemon/lempar.c native/qjson_parse.y

# SQLite loadable extension (with libbf for exact comparison)
qjson_ext$(EXT_SUFFIX): native/qjson_sqlite_ext.c $(QJSON_SRC) $(LIBBF_SRC)
	$(CC) $(CFLAGS) $(SHARED) -o $@ $^ -lm

# SQLCipher loadable extension (same code, linked against sqlcipher headers)
qjson_ext_sqlcipher$(EXT_SUFFIX): native/qjson_sqlite_ext.c $(QJSON_SRC) $(LIBBF_SRC)
	$(CC) $(CFLAGS) $(SHARED) -I$(SQLCIPHER_DIR)/src -o $@ $^ -lm

# C test binary
test_qjson: test/test_qjson.c $(QJSON_SRC) $(LIBBF_SRC) $(FACTS_SRC)
	$(CC) $(CFLAGS) -frounding-math -o $@ $^ -lm

test: all test_qjson
	./test_qjson

clean:
	rm -f qjson_ext$(EXT_SUFFIX) test_qjson native/lemon/lemon
	rm -f native/qjson_parse.c native/qjson_parse.h native/qjson_parse.out
