#!/usr/bin/env python3
"""Tests for qjson_sql adapter — SQLite and PostgreSQL.

Usage:
  python3 test/test_qjson_sql.py              # SQLite only
  python3 test/test_qjson_sql.py --postgres   # SQLite + PostgreSQL

PostgreSQL requires:
  pip install psycopg2-binary
  docker compose up -d postgres
"""

import sys
import os
import struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson_sql import (qjson_sql_adapter, round_down, round_up,
                        _project_numeric, _classify_value)
from qjson import BigInt, BigFloat, Blob, Unbound
from decimal import Decimal


# ── round_down / round_up ───────────────────────────────────

def test_rounding():
    # 0.1 is inexact
    lo = round_down('0.1')
    hi = round_up('0.1')
    assert lo < hi, '0.1 should bracket'
    assert lo == 0.09999999999999999
    assert hi == 0.1

    # 42 is exact
    assert round_down('42') == 42.0
    assert round_up('42') == 42.0

    # 2^53+1 is inexact
    lo = round_down('9007199254740993')
    hi = round_up('9007199254740993')
    assert lo == 9007199254740992.0
    assert hi == 9007199254740994.0

    # negative inexact
    lo = round_down('-0.1')
    hi = round_up('-0.1')
    assert lo < hi
    assert lo == -0.1
    assert hi == -0.09999999999999999

    # 0.5 is exact
    assert round_down('0.5') == 0.5
    assert round_up('0.5') == 0.5

    print('  round_down/round_up: OK')


def test_project():
    lo, s, hi = _project_numeric('0.1')
    assert s == '0.1'
    assert lo < hi

    lo, s, hi = _project_numeric('42')
    assert s is None
    assert lo == hi == 42.0

    print('  _project_numeric: OK')


def test_classify():
    assert _classify_value(None) == ('null', None)
    assert _classify_value(True) == ('true', None)
    assert _classify_value(False) == ('false', None)
    assert _classify_value(42) == ('number', None)
    assert _classify_value(3.14) == ('number', None)
    assert _classify_value(BigInt(42)) == ('bigint', '42')
    assert _classify_value(Decimal('0.1')) == ('bigdec', '0.1')
    assert _classify_value(BigFloat('3.14')) == ('bigfloat', '3.14')
    assert _classify_value('hello') == ('string', None)
    assert _classify_value(Blob(b'\x01')) == ('blob', None)
    assert _classify_value([1, 2]) == ('array', None)
    assert _classify_value({'a': 1}) == ('object', None)
    assert _classify_value(Unbound('X')) == ('unbound', 'X')
    assert _classify_value(Unbound('')) == ('unbound', '')
    assert _classify_value(Unbound('_')) == ('unbound', '_')
    print('  _classify_value: OK')


# ── Adapter round-trip tests ────────────────────────────────

def run_adapter_tests(adapter, label):
    a = adapter
    a['setup']()

    # null
    vid = a['store'](None)
    assert a['load'](vid) is None

    # booleans
    vid = a['store'](True)
    assert a['load'](vid) is True
    vid = a['store'](False)
    assert a['load'](vid) is False

    # number (exact IEEE double)
    vid = a['store'](42)
    assert a['load'](vid) == 42.0
    vid = a['store'](3.14)
    assert a['load'](vid) == 3.14

    # BigInt — exact
    vid = a['store'](BigInt(42))
    v = a['load'](vid)
    assert isinstance(v, BigInt) and int(v) == 42

    # BigInt — inexact (> 2^53)
    vid = a['store'](BigInt(9007199254740993))
    v = a['load'](vid)
    assert isinstance(v, BigInt) and int(v) == 9007199254740993

    # BigInt — large
    vid = a['store'](BigInt(10**30))
    v = a['load'](vid)
    assert isinstance(v, BigInt) and int(v) == 10**30

    # BigInt — negative inexact
    vid = a['store'](BigInt(-9007199254740993))
    v = a['load'](vid)
    assert isinstance(v, BigInt) and int(v) == -9007199254740993

    # BigDecimal — exact
    vid = a['store'](Decimal('0.5'))
    v = a['load'](vid)
    assert isinstance(v, Decimal) and v == Decimal('0.5')

    # BigDecimal — inexact
    vid = a['store'](Decimal('0.1'))
    v = a['load'](vid)
    assert isinstance(v, Decimal) and v == Decimal('0.1')

    # BigDecimal — very precise
    vid = a['store'](Decimal('3.141592653589793238462643383279'))
    v = a['load'](vid)
    assert isinstance(v, Decimal) and v == Decimal('3.141592653589793238462643383279')

    # BigFloat
    vid = a['store'](BigFloat('3.14'))
    v = a['load'](vid)
    assert isinstance(v, BigFloat) and str(v) == '3.14'

    # String
    vid = a['store']('hello world')
    assert a['load'](vid) == 'hello world'
    vid = a['store']('')
    assert a['load'](vid) == ''

    # Blob
    vid = a['store'](Blob(b'\x01\x02\x03'))
    v = a['load'](vid)
    assert isinstance(v, Blob) and v.data == b'\x01\x02\x03'

    # Array (nested)
    vid = a['store']([1, 'two', None, [3, 4]])
    v = a['load'](vid)
    assert v == [1.0, 'two', None, [3.0, 4.0]]

    # Object (nested)
    vid = a['store']({'a': 1, 'b': {'c': 'deep'}})
    v = a['load'](vid)
    assert v == {'a': 1.0, 'b': {'c': 'deep'}}

    # Complex nested
    vid = a['store']({
        'price': Decimal('67432.50'),
        'items': [BigInt(1), BigInt(2)],
        'meta': {'ok': True},
    })
    v = a['load'](vid)
    assert v['meta'] == {'ok': True}
    assert isinstance(v['price'], Decimal)
    assert isinstance(v['items'][0], BigInt)

    # Unbound — named
    vid = a['store'](Unbound('X'))
    v = a['load'](vid)
    assert isinstance(v, Unbound) and v.name == 'X'

    # Unbound — anonymous (empty name)
    vid = a['store'](Unbound(''))
    v = a['load'](vid)
    assert isinstance(v, Unbound) and v.name == ''

    # Unbound — named "_" (distinct from anonymous)
    vid = a['store'](Unbound('_'))
    v = a['load'](vid)
    assert isinstance(v, Unbound) and v.name == '_'

    # Unbound — in array (Prolog-style pattern)
    vid = a['store'](['reading', Unbound('Sensor'), Unbound('Val')])
    v = a['load'](vid)
    assert isinstance(v[1], Unbound) and v[1].name == 'Sensor'
    assert isinstance(v[2], Unbound) and v[2].name == 'Val'

    # Unbound — in object
    vid = a['store']({'query': Unbound('X'), 'fixed': 42})
    v = a['load'](vid)
    assert isinstance(v['query'], Unbound) and v['query'].name == 'X'
    assert v['fixed'] == 42.0

    # Remove
    vid = a['store']([1, 2, 3])
    a['remove'](vid)
    assert a['load'](vid) is None

    a['commit']()
    print('  %s round-trip: OK' % label)


def test_sqlite_interval_storage():
    """Verify the actual SQL values stored in SQLite."""
    import sqlite3
    conn = sqlite3.connect(':memory:')
    a = qjson_sql_adapter(conn)
    a['setup']()

    # Inexact BigDecimal
    vid = a['store'](Decimal('0.1'))
    lo, s, hi = conn.execute(
        'SELECT lo, str, hi FROM qjson_number WHERE value_id = ?',
        (vid,)).fetchone()
    assert s == '0.1'
    assert lo == round_down('0.1')
    assert hi == round_up('0.1')
    assert lo < hi

    # Exact BigInt
    vid = a['store'](BigInt(42))
    lo, s, hi = conn.execute(
        'SELECT lo, str, hi FROM qjson_number WHERE value_id = ?',
        (vid,)).fetchone()
    assert s is None
    assert lo == 42.0 and hi == 42.0

    # Inexact BigInt
    vid = a['store'](BigInt(9007199254740993))
    lo, s, hi = conn.execute(
        'SELECT lo, str, hi FROM qjson_number WHERE value_id = ?',
        (vid,)).fetchone()
    assert s == '9007199254740993'
    assert lo == 9007199254740992.0
    assert hi == 9007199254740994.0

    # Verify SQLite types are REAL (8-byte)
    row = conn.execute(
        'SELECT typeof(lo), typeof(hi) FROM qjson_number LIMIT 1').fetchone()
    assert row[0] == 'real' and row[1] == 'real'

    conn.close()
    print('  SQLite interval storage: OK')


def test_postgres_interval_storage(conn):
    """Verify the actual SQL values stored in PostgreSQL."""
    cur = conn.cursor()
    a = qjson_sql_adapter(conn, prefix='pgtest_')
    a['setup']()
    a['commit']()

    # Inexact BigDecimal
    vid = a['store'](Decimal('0.1'))
    a['commit']()
    cur.execute(
        'SELECT lo, str, hi FROM pgtest_number WHERE value_id = %s',
        (vid,))
    lo, s, hi = cur.fetchone()
    assert s == '0.1', 'str should be 0.1, got %r' % s
    assert lo == round_down('0.1'), 'lo mismatch: %r != %r' % (lo, round_down('0.1'))
    assert hi == round_up('0.1'), 'hi mismatch: %r != %r' % (hi, round_up('0.1'))
    assert lo < hi

    # Exact BigInt
    vid = a['store'](BigInt(42))
    a['commit']()
    cur.execute(
        'SELECT lo, str, hi FROM pgtest_number WHERE value_id = %s',
        (vid,))
    lo, s, hi = cur.fetchone()
    assert s is None
    assert lo == 42.0 and hi == 42.0

    # Inexact BigInt — verify PG DOUBLE PRECISION preserves IEEE bits
    vid = a['store'](BigInt(9007199254740993))
    a['commit']()
    cur.execute(
        'SELECT lo, str, hi FROM pgtest_number WHERE value_id = %s',
        (vid,))
    lo, s, hi = cur.fetchone()
    assert s == '9007199254740993'
    assert lo == 9007199254740992.0, 'PG lo: %r' % lo
    assert hi == 9007199254740994.0, 'PG hi: %r' % hi

    # Verify PostgreSQL column type is double precision (not real!)
    cur.execute("""
        SELECT data_type FROM information_schema.columns
        WHERE table_name = 'pgtest_number' AND column_name = 'lo'
    """)
    row = cur.fetchone()
    assert row[0] == 'double precision', 'Expected double precision, got %r' % row[0]

    # Verify IEEE 754 bit-exact round-trip
    # Store a value where 4-byte float would lose precision
    test_val = 0.09999999999999999  # round_down('0.1')
    vid = a['store'](Decimal('0.1'))
    a['commit']()
    cur.execute(
        'SELECT lo FROM pgtest_number WHERE value_id = %s', (vid,))
    pg_lo = cur.fetchone()[0]
    assert pg_lo == test_val, \
        'IEEE 754 bit mismatch: stored %.17g got %.17g' % (test_val, pg_lo)

    print('  PostgreSQL interval storage: OK')
    print('  PostgreSQL DOUBLE PRECISION verified (not REAL)')


def test_sqlite_custom_prefix():
    a = qjson_sql_adapter(':memory:', prefix='custom_')
    a['setup']()
    vid = a['store'](42)
    assert a['load'](vid) == 42.0
    a['close']()
    print('  SQLite custom prefix: OK')


def test_large_keys_and_deep_paths():
    """Stress test: large key names, deep nesting, long values."""
    import sqlite3 as _sqlite3
    import json

    conn = _sqlite3.connect(':memory:')
    has_ext = False
    try:
        conn.enable_load_extension(True)
        conn.load_extension('./qjson_ext')
        has_ext = True
    except Exception:
        pass

    a = qjson_sql_adapter(conn)
    a['setup']()

    # Large key name (500 chars)
    long_key = 'a' * 500
    vid = a['store']({long_key: 42})
    a['commit']()
    v = a['load'](vid)
    assert v[long_key] == 42.0

    if has_ext:
        from qjson_query import qjson_select
        r = conn.execute('SELECT qjson_reconstruct(?, ?)',
                         (vid, 'qjson_')).fetchone()[0]
        assert long_key in r
        rows = conn.execute(
            'SELECT qjson FROM qjson_select WHERE root_id=? AND select_path=?',
            (vid, '.' + long_key)).fetchall()
        assert len(rows) == 1

    # Deep nesting (20 levels)
    deep = {'v': 'leaf'}
    for _ in range(20):
        deep = {'d': deep}
    vid = a['store'](deep)
    a['commit']()

    if has_ext:
        path = '.d' * 20 + '.v'
        rows = conn.execute(
            'SELECT qjson FROM qjson_select WHERE root_id=? AND select_path=?',
            (vid, path)).fetchall()
        assert len(rows) == 1 and rows[0][0] == '"leaf"'

    # Large array (50 elements) with WHERE
    big_arr = [{'idx': i, 'val': i * 0.1} for i in range(50)]
    vid = a['store']({'data': big_arr})
    a['commit']()

    if has_ext:
        rows = conn.execute(
            'SELECT qjson FROM qjson_select WHERE root_id=? AND select_path=? AND where_expr=?',
            (vid, '.data[K]', '.data[K].idx > 45')).fetchall()
        assert len(rows) == 4  # idx 46,47,48,49

    # Long string value (1200 chars)
    long_str = 'hello ' * 200
    vid = a['store']({'msg': long_str})
    a['commit']()
    v = a['load'](vid)
    assert v['msg'] == long_str

    # Deep path + AND WHERE
    doc = {'l1': {'l2': {'l3': {'items': [
        {'x': 1, 'y': 10},
        {'x': 2, 'y': 20},
        {'x': 3, 'y': 30},
    ]}}}}
    vid = a['store'](doc)
    a['commit']()

    if has_ext:
        rows = conn.execute(
            'SELECT qjson FROM qjson_select WHERE root_id=? AND select_path=? AND where_expr=?',
            (vid, '.l1.l2.l3.items[K]',
             '.l1.l2.l3.items[K].x > 1 AND .l1.l2.l3.items[K].y < 30')).fetchall()
        assert len(rows) == 1
        obj = json.loads(rows[0][0])
        assert obj['x'] == 2 and obj['y'] == 20

    conn.close()
    ext_note = ' (with embedded translator)' if has_ext else ''
    print('  Large keys, deep paths, long values%s: OK' % ext_note)


# ── Main ─────────────────────────────────────────────────────

if __name__ == '__main__':
    print('Unit tests:')
    test_rounding()
    test_project()
    test_classify()

    print('\nSQLite tests:')
    run_adapter_tests(qjson_sql_adapter(':memory:'), 'SQLite')
    test_sqlite_interval_storage()
    test_sqlite_custom_prefix()
    test_large_keys_and_deep_paths()

    if '--postgres' in sys.argv:
        print('\nPostgreSQL tests:')
        try:
            import psycopg2
        except ImportError:
            print('  SKIP: psycopg2 not installed (pip install psycopg2-binary)')
            sys.exit(0)

        pg_host = os.environ.get('PGHOST', 'localhost')
        pg_port = int(os.environ.get('PGPORT', '5433'))
        try:
            conn = psycopg2.connect(
                host=pg_host, port=pg_port,
                dbname='qjson_test', user='qjson', password='qjson')
            conn.autocommit = False
        except Exception as e:
            print('  SKIP: cannot connect to PostgreSQL (%s)' % e)
            print('  Run: docker compose up -d postgres')
            sys.exit(0)

        run_adapter_tests(
            qjson_sql_adapter(conn, prefix='rt_'), 'PostgreSQL')
        conn.commit()
        test_postgres_interval_storage(conn)

        # Clean up test tables
        cur = conn.cursor()
        for prefix in ('qjson_', 'rt_', 'pgtest_'):
            for table in ('object_item', 'object', 'array_item', 'array',
                          'blob', 'string', 'number', 'value'):
                try:
                    cur.execute('DROP TABLE IF EXISTS "%s%s" CASCADE'
                                % (prefix, table))
                except Exception:
                    conn.rollback()
            conn.commit()
        conn.close()

    print('\nALL TESTS PASSED')
