#!/usr/bin/env python3
"""Projection equivalence tests — libbf baseline vs Python vs PostgreSQL.

The C libbf directed rounding (BF_RNDD/BF_RNDU) is the authoritative
projection.  This test suite verifies:

  1. Python round_down/round_up match libbf (via qjson_round_down/up SQL fns)
  2. SQLite stores bit-exact libbf projections
  3. PostgreSQL stores bit-exact libbf projections (--postgres)
  4. Interval invariants hold (lo <= hi, 1-ULP brackets, etc.)
  5. Negation symmetry: round_down(-x) == -round_up(x)

Usage:
  python3 test/test_projection_equivalence.py              # libbf + Python + SQLite
  python3 test/test_projection_equivalence.py --postgres   # + PostgreSQL cross-check

Prerequisites:
  make                          # builds qjson_ext with libbf
  pip install psycopg2-binary   # for --postgres
  docker compose up -d postgres # for --postgres
"""

import sys
import os
import struct
import math

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson_sql import (qjson_sql_adapter, round_down, round_up,
                        _project_numeric, _classify_value)
from qjson import BigInt, BigFloat, Unbound
from decimal import Decimal


# ── IEEE 754 bit-exact comparison ─────────────────────────────

def double_bits(x):
    """Return the 64-bit IEEE 754 representation as bytes."""
    return struct.pack('<d', x)


def assert_bit_equal(a, b, label):
    """Assert two doubles are bit-identical (not just ==)."""
    if a is None and b is None:
        return
    ba = double_bits(a)
    bb = double_bits(b)
    assert ba == bb, (
        '%s: bit mismatch — %.17g (0x%s) vs %.17g (0x%s)'
        % (label, a, ba.hex(), b, bb.hex()))


# ── Load libbf extension ─────────────────────────────────────

def _load_ext(conn):
    """Load qjson_ext into a SQLite connection.  Returns True if loaded."""
    try:
        conn.enable_load_extension(True)
        conn.load_extension('./qjson_ext')
        return True
    except Exception:
        return False


# ── Decimal string test vectors ───────────────────────────────
# These are the raw decimal strings that libbf projects.
# Covers: exact doubles, inexact decimals, boundary cases,
# overflow, underflow, subnormals, high precision, large/small.

def _raw_test_vectors():
    """Return list of (label, raw_decimal_string) pairs."""
    vecs = []

    # Exact binary fractions
    vecs.append(('zero', '0'))
    vecs.append(('one', '1'))
    vecs.append(('neg_one', '-1'))
    vecs.append(('half', '0.5'))
    vecs.append(('quarter', '0.25'))
    vecs.append(('eighth', '0.125'))
    vecs.append(('neg_half', '-0.5'))
    vecs.append(('1.5', '1.5'))
    vecs.append(('0.0625', '0.0625'))

    # Exact integers
    vecs.append(('42', '42'))
    vecs.append(('neg_42', '-42'))
    vecs.append(('1000', '1000'))
    vecs.append(('max_safe', '9007199254740991'))
    vecs.append(('neg_max_safe', '-9007199254740991'))
    vecs.append(('pow2_52', str(2**52)))
    vecs.append(('pow2_53', str(2**53)))

    # Inexact: beyond 2^53
    vecs.append(('pow2_53+1', '9007199254740993'))
    vecs.append(('pow2_53+2', '9007199254740994'))
    vecs.append(('pow2_53+3', '9007199254740995'))
    vecs.append(('neg_pow2_53+1', '-9007199254740993'))
    vecs.append(('pow2_54+1', str(2**54 + 1)))
    vecs.append(('pow2_64', str(2**64)))
    vecs.append(('pow2_100', str(2**100)))
    vecs.append(('10^18', str(10**18)))
    vecs.append(('10^20', str(10**20)))
    vecs.append(('10^30', str(10**30)))
    vecs.append(('10^50', str(10**50)))
    vecs.append(('neg_10^30', str(-(10**30))))

    # Inexact: common decimals
    vecs.append(('0.1', '0.1'))
    vecs.append(('0.2', '0.2'))
    vecs.append(('0.3', '0.3'))
    vecs.append(('0.6', '0.6'))
    vecs.append(('0.7', '0.7'))
    vecs.append(('0.9', '0.9'))
    vecs.append(('neg_0.1', '-0.1'))
    vecs.append(('neg_0.3', '-0.3'))
    vecs.append(('0.01', '0.01'))
    vecs.append(('0.001', '0.001'))
    vecs.append(('0.0001', '0.0001'))

    # High precision
    vecs.append(('pi_30', '3.141592653589793238462643383279'))
    vecs.append(('e_30', '2.718281828459045235360287471352'))
    vecs.append(('third_30', '0.333333333333333333333333333333'))
    vecs.append(('seventh_30', '0.142857142857142857142857142857'))
    vecs.append(('large_frac', '123456789.123456789'))
    vecs.append(('neg_large_frac', '-987654321.000000001'))

    # Near underflow (smallest normal: ~2.2250738585072014e-308)
    vecs.append(('1e-307', '1E-307'))
    vecs.append(('1e-308', '1E-308'))
    vecs.append(('smallest_normal', '2.2250738585072014E-308'))
    vecs.append(('subnormal_1e-310', '1E-310'))
    vecs.append(('subnormal_1e-320', '1E-320'))
    vecs.append(('neg_1e-308', '-1E-308'))

    # Near overflow (largest finite: ~1.7976931348623157e+308)
    vecs.append(('near_max', '1.7976931348623157E+308'))
    vecs.append(('over_max', '1.8E+308'))
    vecs.append(('neg_near_max', '-1.7976931348623157E+308'))
    vecs.append(('neg_over_max', '-1.8E+308'))

    # Large integers near overflow
    vecs.append(('10^300', str(10**300)))
    vecs.append(('10^308', str(10**308)))
    vecs.append(('10^309', str(10**309)))
    vecs.append(('neg_10^309', str(-(10**309))))

    # Powers of 10
    for exp in [1, 2, 3, 6, 9, 12, 15, 18]:
        vecs.append(('1e+%d' % exp, '1E+%d' % exp))

    # Negative zero
    vecs.append(('neg_zero', '-0'))
    vecs.append(('neg_0.0', '-0.0'))

    return vecs


# ── QJSON value test cases (for adapter store/load) ──────────

def _value_test_cases():
    """Generate (label, value) pairs for adapter-level testing."""
    cases = []

    # Plain numbers (exact IEEE doubles)
    cases.append(('num_zero', 0))
    cases.append(('num_one', 1))
    cases.append(('num_pi', 3.14))
    cases.append(('num_half', 0.5))
    cases.append(('num_max_safe', 9007199254740991))

    # BigInt
    cases.append(('bigint_42', BigInt(42)))
    cases.append(('bigint_pow2_53+1', BigInt(2**53 + 1)))
    cases.append(('bigint_10^30', BigInt(10**30)))
    cases.append(('bigint_neg_10^30', BigInt(-(10**30))))
    cases.append(('bigint_10^309', BigInt(10**309)))

    # BigDecimal
    cases.append(('bigdec_0.5', Decimal('0.5')))
    cases.append(('bigdec_0.1', Decimal('0.1')))
    cases.append(('bigdec_pi_30', Decimal('3.141592653589793238462643383279')))
    cases.append(('bigdec_large_frac', Decimal('123456789.123456789')))
    cases.append(('bigdec_1e-310', Decimal('1E-310')))
    cases.append(('bigdec_near_max', Decimal('1.7976931348623157E+308')))

    # BigFloat
    cases.append(('bigfloat_0.5', BigFloat('0.5')))
    cases.append(('bigfloat_0.1', BigFloat('0.1')))
    cases.append(('bigfloat_pi', BigFloat('3.14159265358979323846')))

    # Unbound
    cases.append(('unbound_X', Unbound('X')))
    cases.append(('unbound_anon', Unbound('')))

    return cases


# ── Test 1: libbf vs Python round_down/round_up ──────────────

def test_libbf_vs_python(conn):
    """Compare libbf qjson_round_down/up against Python round_down/round_up."""
    vecs = _raw_test_vectors()
    passed = 0
    failures = []

    for label, raw in vecs:
        # libbf baseline
        row = conn.execute(
            'SELECT qjson_round_down(?), qjson_round_up(?)',
            (raw, raw)).fetchone()
        bf_lo, bf_hi = row

        # Python
        py_lo = round_down(raw)
        py_hi = round_up(raw)

        ok = True
        try:
            assert_bit_equal(py_lo, bf_lo, '%s lo' % label)
        except AssertionError as e:
            failures.append(str(e))
            ok = False
        try:
            assert_bit_equal(py_hi, bf_hi, '%s hi' % label)
        except AssertionError as e:
            failures.append(str(e))
            ok = False

        if ok:
            passed += 1

    if failures:
        print('  libbf vs Python: %d/%d passed, %d FAILURES:'
              % (passed, len(vecs), len(failures)))
        for f in failures:
            print('    FAIL: %s' % f)
        return False
    else:
        print('  libbf vs Python: %d/%d OK' % (passed, len(vecs)))
        return True


# ── Test 2: libbf vs SQLite adapter storage ───────────────────

def test_libbf_vs_sqlite_adapter(conn):
    """Verify adapter-stored intervals match libbf for all value types."""
    prefix = 'eq_sq_'
    a = qjson_sql_adapter(conn, prefix=prefix)
    a['setup']()

    cases = _value_test_cases()
    passed = 0
    failures = []

    for label, value in cases:
        type_str, raw = _classify_value(value)
        vid = a['store'](value)
        a['commit']()

        t_number = prefix + 'number'
        row = conn.execute(
            'SELECT lo, str, hi FROM "%s" WHERE value_id = ?' % t_number,
            (vid,)).fetchone()

        if row is None:
            continue

        db_lo, db_str, db_hi = row

        if type_str == 'number':
            # Plain doubles: exact, no projection needed
            exp_lo = exp_hi = float(value)
            exp_str = None
        elif type_str in ('bigint', 'bigdec', 'bigfloat'):
            # Compare against libbf
            bf_row = conn.execute(
                'SELECT qjson_round_down(?), qjson_round_up(?)',
                (raw, raw)).fetchone()
            exp_lo, exp_hi = bf_row
            exp_str = None if exp_lo == exp_hi else raw
        elif type_str == 'unbound':
            exp_lo = float('-inf')
            exp_str = '?' + raw
            exp_hi = float('inf')
        else:
            continue

        ok = True
        try:
            assert_bit_equal(db_lo, exp_lo, '%s lo' % label)
        except AssertionError as e:
            failures.append(str(e))
            ok = False
        try:
            assert_bit_equal(db_hi, exp_hi, '%s hi' % label)
        except AssertionError as e:
            failures.append(str(e))
            ok = False
        if db_str != exp_str:
            failures.append('%s str: stored=%r expected=%r' % (label, db_str, exp_str))
            ok = False

        if ok:
            passed += 1

    if failures:
        print('  libbf vs SQLite adapter: %d/%d passed, %d FAILURES:'
              % (passed, len(cases), len(failures)))
        for f in failures:
            print('    FAIL: %s' % f)
        return False
    else:
        print('  libbf vs SQLite adapter: %d/%d OK' % (passed, len(cases)))
        return True


# ── Test 3: libbf vs PostgreSQL adapter storage ───────────────

def test_libbf_vs_postgres_adapter(bf_conn, pg_conn):
    """Verify PostgreSQL adapter stores bit-exact libbf projections."""
    pg_prefix = 'eq_pg_'
    pg = qjson_sql_adapter(pg_conn, prefix=pg_prefix)
    pg['setup']()
    pg['commit']()

    cases = _value_test_cases()
    passed = 0
    failures = []

    for label, value in cases:
        type_str, raw = _classify_value(value)
        vid = pg['store'](value)
        pg['commit']()

        t_number = pg_prefix + 'number'
        cur = pg_conn.cursor()
        cur.execute(
            'SELECT lo, str, hi FROM "%s" WHERE value_id = %%s' % t_number,
            (vid,))
        row = cur.fetchone()

        if row is None:
            continue

        pg_lo, pg_str, pg_hi = row

        if type_str == 'number':
            exp_lo = exp_hi = float(value)
            exp_str = None
        elif type_str in ('bigint', 'bigdec', 'bigfloat'):
            bf_row = bf_conn.execute(
                'SELECT qjson_round_down(?), qjson_round_up(?)',
                (raw, raw)).fetchone()
            exp_lo, exp_hi = bf_row
            exp_str = None if exp_lo == exp_hi else raw
        elif type_str == 'unbound':
            exp_lo = float('-inf')
            exp_str = '?' + raw
            exp_hi = float('inf')
        else:
            continue

        ok = True
        try:
            assert_bit_equal(pg_lo, exp_lo, '%s lo' % label)
        except AssertionError as e:
            failures.append(str(e))
            ok = False
        try:
            assert_bit_equal(pg_hi, exp_hi, '%s hi' % label)
        except AssertionError as e:
            failures.append(str(e))
            ok = False
        if pg_str != exp_str:
            failures.append('%s str: stored=%r expected=%r' % (label, pg_str, exp_str))
            ok = False

        if ok:
            passed += 1

    # Clean up
    cur = pg_conn.cursor()
    for table in ('object_item', 'object', 'array_item', 'array',
                  'blob', 'string', 'number', 'value'):
        try:
            cur.execute('DROP TABLE IF EXISTS "%s%s" CASCADE' % (pg_prefix, table))
        except Exception:
            pg_conn.rollback()
    pg_conn.commit()

    if failures:
        print('  libbf vs PostgreSQL adapter: %d/%d passed, %d FAILURES:'
              % (passed, len(cases), len(failures)))
        for f in failures:
            print('    FAIL: %s' % f)
        return False
    else:
        print('  libbf vs PostgreSQL adapter: %d/%d OK' % (passed, len(cases)))
        return True


# ── Test 4: Interval invariants ───────────────────────────────

def test_interval_invariants(conn):
    """Verify libbf projection invariants for all raw vectors.

    For every [lo, hi]:
      - lo <= hi
      - Neither is NaN
      - If lo == hi, value is exact
      - If lo != hi, nextafter(lo, +inf) == hi  (1-ULP bracket)
        unless overflow (lo = DBL_MAX, hi = +inf) or similar
    """
    vecs = _raw_test_vectors()
    passed = 0
    failures = []

    for label, raw in vecs:
        row = conn.execute(
            'SELECT qjson_round_down(?), qjson_round_up(?)',
            (raw, raw)).fetchone()
        lo, hi = row

        if math.isnan(lo) or math.isnan(hi):
            failures.append('%s: NaN in projection' % label)
            continue

        if lo > hi:
            failures.append('%s: lo > hi (%.17g > %.17g)' % (label, lo, hi))
            continue

        if lo != hi:
            # Inexact: check 1-ULP bracket
            if not math.isinf(lo) and not math.isinf(hi):
                next_lo = math.nextafter(lo, float('inf'))
                if next_lo != hi:
                    failures.append(
                        '%s: not 1-ULP — lo=%.17g next=%.17g hi=%.17g'
                        % (label, lo, next_lo, hi))
                    continue

        passed += 1

    if failures:
        print('  Interval invariants: %d/%d passed, %d FAILURES:'
              % (passed, len(vecs), len(failures)))
        for f in failures:
            print('    FAIL: %s' % f)
        return False
    else:
        print('  Interval invariants: %d/%d OK' % (passed, len(vecs)))
        return True


# ── Test 5: Negation symmetry ────────────────────────────────

def test_negation_symmetry(conn):
    """Verify round_down(-x) == -round_up(x) via libbf."""
    raw_values = [
        '0.1', '0.3', '0.7', '0.9',
        '3.141592653589793238462643383279',
        '9007199254740993',
        '1E-308', '1E+308',
        '123456789.123456789',
        str(2**100),
        '1E-320',
    ]
    passed = 0
    failures = []

    for raw in raw_values:
        row_pos = conn.execute(
            'SELECT qjson_round_down(?), qjson_round_up(?)',
            (raw, raw)).fetchone()
        row_neg = conn.execute(
            'SELECT qjson_round_down(?), qjson_round_up(?)',
            ('-' + raw, '-' + raw)).fetchone()

        bf_lo_pos, bf_hi_pos = row_pos
        bf_lo_neg, bf_hi_neg = row_neg

        ok = True
        # round_down(-x) should be -round_up(x)
        try:
            assert_bit_equal(bf_lo_neg, -bf_hi_pos,
                             'negation lo for %s' % raw)
        except AssertionError as e:
            failures.append(str(e))
            ok = False
        # round_up(-x) should be -round_down(x)
        try:
            assert_bit_equal(bf_hi_neg, -bf_lo_pos,
                             'negation hi for %s' % raw)
        except AssertionError as e:
            failures.append(str(e))
            ok = False

        if ok:
            passed += 1

    if failures:
        print('  Negation symmetry: %d/%d passed, %d FAILURES:'
              % (passed, len(raw_values), len(failures)))
        for f in failures:
            print('    FAIL: %s' % f)
        return False
    else:
        print('  Negation symmetry: %d/%d OK' % (passed, len(raw_values)))
        return True


# ── Main ─────────────────────────────────────────────────────

if __name__ == '__main__':
    import sqlite3

    print('Projection equivalence tests (libbf baseline):')
    print()

    # Open a SQLite connection with libbf extension
    conn = sqlite3.connect(':memory:')
    has_ext = _load_ext(conn)
    if not has_ext:
        print('ERROR: qjson_ext not found — run `make` first')
        sys.exit(1)

    all_ok = True

    print('1. Interval invariants (libbf):')
    all_ok &= test_interval_invariants(conn)

    print()
    print('2. Negation symmetry (libbf):')
    all_ok &= test_negation_symmetry(conn)

    print()
    print('3. libbf vs Python round_down/round_up:')
    all_ok &= test_libbf_vs_python(conn)

    print()
    print('4. libbf vs SQLite adapter:')
    all_ok &= test_libbf_vs_sqlite_adapter(conn)

    if '--postgres' in sys.argv:
        print()
        print('5. libbf vs PostgreSQL adapter:')
        try:
            import psycopg2
        except ImportError:
            print('  SKIP: psycopg2 not installed (pip install psycopg2-binary)')
            sys.exit(0)

        pg_host = os.environ.get('PGHOST', 'localhost')
        pg_port = int(os.environ.get('PGPORT', '5433'))
        try:
            pg_conn = psycopg2.connect(
                host=pg_host, port=pg_port,
                dbname='qjson_test', user='qjson', password='qjson')
            pg_conn.autocommit = False
        except Exception as e:
            print('  SKIP: cannot connect to PostgreSQL (%s)' % e)
            print('  Run: docker compose up -d postgres')
            sys.exit(0)

        all_ok &= test_libbf_vs_postgres_adapter(conn, pg_conn)
        pg_conn.close()

    conn.close()

    print()
    if all_ok:
        print('ALL PROJECTION EQUIVALENCE TESTS PASSED')
    else:
        print('SOME TESTS FAILED')
        sys.exit(1)
