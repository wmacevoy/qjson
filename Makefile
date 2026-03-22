CC = cc
CFLAGS = -O2 -std=c11 -fPIC -DQJSON_USE_LIBBF -Inative -Inative/libbf
LIBBF_SRC = native/libbf/libbf.c native/libbf/cutils.c

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  SHARED = -dynamiclib -undefined dynamic_lookup
  EXT_SUFFIX = .dylib
else
  SHARED = -shared
  EXT_SUFFIX = .so
endif

SQLCIPHER_DIR ?= ../sqlcipher-libressl

.PHONY: all clean test test-postgres

all: qjson_ext$(EXT_SUFFIX)

# SQLite loadable extension (with libbf for exact comparison)
qjson_ext$(EXT_SUFFIX): native/qjson_sqlite_ext.c native/qjson.c $(LIBBF_SRC)
	$(CC) $(CFLAGS) $(SHARED) -o $@ $^ -lm

# SQLCipher loadable extension (same code, linked against sqlcipher headers)
qjson_ext_sqlcipher$(EXT_SUFFIX): native/qjson_sqlite_ext.c native/qjson.c $(LIBBF_SRC)
	$(CC) $(CFLAGS) $(SHARED) -I$(SQLCIPHER_DIR)/src -o $@ $^ -lm

# C test binary
test_qjson: test/test_qjson.c native/qjson.c $(LIBBF_SRC)
	$(CC) $(CFLAGS) -frounding-math -o $@ $^ -lm

test: all test_qjson
	./test_qjson
	python3 test/test_qjson.py
	python3 test/test_qjson_sql.py

test-postgres: all
	docker compose up -d postgres
	@echo "Waiting for PostgreSQL..."
	@sleep 3
	python3 test/test_qjson_sql.py --postgres
	docker compose down

clean:
	rm -f qjson_ext$(EXT_SUFFIX) test_qjson
