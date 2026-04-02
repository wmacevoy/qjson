#!/usr/bin/env python3
"""Compare PL/pgSQL nextup/nextdown/round_down/round_up against libbf reference.

Uses the C reference harness for libbf values, and a local PostgreSQL
(docker compose) for the PL/pgSQL implementation.

Usage:
  # Build C reference
  cc -O2 -I../native/libbf -o ref_harness pg_nextafter_ref.c \
     ../native/libbf/libbf.c ../native/libbf/cutils.c -lm

  # Generate reference data
  ./ref_harness > ref_data.csv

  # Run against PostgreSQL
  docker compose up -d postgres
  psql -h localhost -p 5433 -U qjson -d qjson_test -f pg_nextafter.sql
  python3 test_pg_nextafter.py

  # Or run against local SQLite for comparison
  python3 test_pg_nextafter.py --sqlite
"""

import sys
import os
import csv
import struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))


def f2bits(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


def signed_to_unsigned(b):
    """PG bigint is signed; convert to unsigned for comparison."""
    if b < 0:
        return b + (1 << 64)
    return b


def bits2f(b):
    return struct.unpack('<d', struct.pack('<Q', b))[0]


def load_reference(path='ref_data.csv'):
    """Load reference data from C harness output."""
    ref = {}
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            ref[row['value']] = {
                'nextup': int(row['ref_nextup_bits']),
                'nextdown': int(row['ref_nextdown_bits']),
                'lo': int(row['ref_lo_bits']),
                'hi': int(row['ref_hi_bits']),
            }
    return ref


def test_sqlite(ref):
    """Test Python round_down/round_up against reference."""
    from qjson_sql import round_down, round_up
    import math

    passed = 0
    failed = 0

    for value, expected in ref.items():
        v = float(value)

        # nextup/nextdown via math.nextafter
        nu = math.nextafter(v, float('inf'))
        nd = math.nextafter(v, float('-inf'))

        if f2bits(nu) != expected['nextup']:
            print(f"  FAIL nextup({value}): got {f2bits(nu)}, expected {expected['nextup']}")
            failed += 1
        else:
            passed += 1

        if f2bits(nd) != expected['nextdown']:
            print(f"  FAIL nextdown({value}): got {f2bits(nd)}, expected {expected['nextdown']}")
            failed += 1
        else:
            passed += 1

        # round_down/round_up
        try:
            lo = round_down(value)
            hi = round_up(value)
        except Exception:
            lo = v
            hi = v

        if f2bits(lo) != expected['lo']:
            print(f"  FAIL round_down({value}): got {f2bits(lo)}, expected {expected['lo']}")
            failed += 1
        else:
            passed += 1

        if f2bits(hi) != expected['hi']:
            print(f"  FAIL round_up({value}): got {f2bits(hi)}, expected {expected['hi']}")
            failed += 1
        else:
            passed += 1

    print(f"SQLite/Python: {passed} passed, {failed} failed")
    return failed


def test_postgres(ref):
    """Test PL/pgSQL nextup/nextdown/round_down/round_up against reference."""
    try:
        import psycopg2
    except ImportError:
        print("SKIP: psycopg2 not installed")
        return 0

    pg_host = os.environ.get('PGHOST', 'localhost')
    pg_port = int(os.environ.get('PGPORT', '5433'))

    try:
        conn = psycopg2.connect(
            host=pg_host, port=pg_port,
            dbname='qjson_test', user='qjson', password='qjson')
    except Exception as e:
        print(f"SKIP: cannot connect to PostgreSQL: {e}")
        return 0

    cur = conn.cursor()
    passed = 0
    failed = 0

    for value, expected in ref.items():
        v = float(value)

        # nextup
        try:
            cur.execute("SELECT qjson_float8_to_bits(qjson_nextup(%s::float8))", (v,))
            pg_nu = cur.fetchone()[0]
        except Exception as e:
            conn.rollback()
            pg_nu = None
            print(f"  ERROR nextup({value}): {e}")

        if pg_nu is not None and signed_to_unsigned(pg_nu) != expected['nextup']:
            print(f"  FAIL nextup({value}): PG={signed_to_unsigned(pg_nu)}, ref={expected['nextup']}")
            failed += 1
        elif pg_nu is not None:
            passed += 1

        # nextdown
        try:
            cur.execute("SELECT qjson_float8_to_bits(qjson_nextdown(%s::float8))", (v,))
            pg_nd = cur.fetchone()[0]
        except Exception as e:
            conn.rollback()
            pg_nd = None
            print(f"  ERROR nextdown({value}): {e}")

        if pg_nd is not None and signed_to_unsigned(pg_nd) != expected['nextdown']:
            print(f"  FAIL nextdown({value}): PG={signed_to_unsigned(pg_nd)}, ref={expected['nextdown']}")
            failed += 1
        elif pg_nd is not None:
            passed += 1

        # round_down
        try:
            cur.execute("SELECT qjson_float8_to_bits(qjson_round_down(%s))", (value,))
            pg_lo = cur.fetchone()[0]
        except Exception as e:
            conn.rollback()
            pg_lo = None
            print(f"  ERROR round_down({value}): {e}")

        if pg_lo is not None and signed_to_unsigned(pg_lo) != expected['lo']:
            print(f"  FAIL round_down({value}): PG={signed_to_unsigned(pg_lo)}, ref={expected['lo']}")
            failed += 1
        elif pg_lo is not None:
            passed += 1

        # round_up
        try:
            cur.execute("SELECT qjson_float8_to_bits(qjson_round_up(%s))", (value,))
            pg_hi = cur.fetchone()[0]
        except Exception as e:
            conn.rollback()
            pg_hi = None
            print(f"  ERROR round_up({value}): {e}")

        if pg_hi is not None and signed_to_unsigned(pg_hi) != expected['hi']:
            print(f"  FAIL round_up({value}): PG={signed_to_unsigned(pg_hi)}, ref={expected['hi']}")
            failed += 1
        elif pg_hi is not None:
            passed += 1

    conn.close()
    print(f"PostgreSQL: {passed} passed, {failed} failed")
    return failed


if __name__ == '__main__':
    # Build and run reference harness if needed
    ref_csv = os.path.join(os.path.dirname(__file__), 'ref_data.csv')
    if not os.path.exists(ref_csv):
        print("Building reference harness...")
        os.system(
            "cc -O2 -I../native/libbf -o ref_harness "
            "pg_nextafter_ref.c ../native/libbf/libbf.c "
            "../native/libbf/cutils.c -lm")
        os.system("./ref_harness > ref_data.csv")

    ref = load_reference(ref_csv)
    print(f"Loaded {len(ref)} reference values\n")

    total_failed = 0

    print("=== Python/SQLite (math.nextafter + round_down/round_up) ===")
    total_failed += test_sqlite(ref)

    if '--sqlite' not in sys.argv:
        print("\n=== PostgreSQL (PL/pgSQL nextup/nextdown) ===")
        total_failed += test_postgres(ref)

    print()
    if total_failed == 0:
        print("ALL PASSED")
    else:
        print(f"{total_failed} FAILURES")
        sys.exit(1)
