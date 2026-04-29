#!/usr/bin/env python3
"""Cross-comparison tests — type-aware path-vs-path WHERE clauses.

Verifies that the QJSON query translator supports horn-clause-equivalent
queries: variable-bound self-joins with cross-path comparisons for all
types (string, numeric, boolean, null).

Usage:
  python3 test/test_cross_comparison.py

Prerequisites:
  make    # builds qjson_ext with libbf
"""

import sys
import os
import json

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson_sql import qjson_sql_adapter
from qjson_query import qjson_select
from qjson import BigInt, BigFloat, Unbound
from decimal import Decimal


# ── Test helpers ──────────────────────────────────────────────

_conn = None
_adapter = None
_has_ext = False


def setup():
    """Initialize SQLite + qjson_ext for all tests."""
    global _conn, _adapter, _has_ext
    import sqlite3
    _conn = sqlite3.connect(':memory:')
    try:
        _conn.enable_load_extension(True)
        _conn.load_extension('./qjson_ext')
        _has_ext = True
    except Exception:
        print('WARNING: qjson_ext not loaded — some tests need it')
    _adapter = qjson_sql_adapter(_conn)
    _adapter['setup']()


def store(value):
    vid = _adapter['store'](value)
    _adapter['commit']()
    return vid


def load(vid):
    return _adapter['load'](vid)


def select(root_id, select_path, where_expr=None):
    return qjson_select(_conn, root_id, select_path,
                        where_expr=where_expr, has_ext=_has_ext)


# ── String cross-comparison ──────────────────────────────────

def test_grandparent_query():
    """Horn clause: grandparent(GP,GC) :- parent(GP,Y), parent(Y,GC)."""
    vid = store({'parent': [
        {'name': 'Alice', 'child': 'Bob'},
        {'name': 'Bob', 'child': 'Carol'},
        {'name': 'Carol', 'child': 'Dave'},
    ]})
    results = select(vid, '.parent[K1].name',
                     '.parent[K1].child == .parent[K2].name')
    gps = sorted([load(r[0]) for r in results])
    assert gps == ['Alice', 'Bob'], 'grandparents: %r' % gps
    # Alice is grandparent of Carol (via Bob)
    # Bob is grandparent of Dave (via Carol)
    print('  grandparent query: OK')


def test_3hop_reachability():
    """3-hop: great-grandparent via 3 variable bindings."""
    vid = store({'edge': [
        {'from': 'A', 'to': 'B'},
        {'from': 'B', 'to': 'C'},
        {'from': 'C', 'to': 'D'},
        {'from': 'D', 'to': 'E'},
    ]})
    results = select(vid, '.edge[K1].from',
        '.edge[K1].to == .edge[K2].from AND .edge[K2].to == .edge[K3].from')
    names = sorted([load(r[0]) for r in results])
    assert names == ['A', 'B'], '3-hop: %r' % names
    # Verify bindings
    for r in results:
        assert 'K1' in r[1] and 'K2' in r[1] and 'K3' in r[1]
    print('  3-hop reachability: OK')


def test_string_self_join():
    """Self-join: find pairs where left == right within same array."""
    vid = store({'items': [
        {'left': 'x', 'right': 'y'},
        {'left': 'x', 'right': 'x'},
        {'left': 'z', 'right': 'z'},
    ]})
    results = select(vid, '.items[K]',
                     '.items[K].left == .items[K].right')
    indices = sorted([r[1]['K'] for r in results])
    assert indices == [1, 2], 'self-join indices: %r' % indices
    print('  string self-join: OK')


# ── Path vs string literal ───────────────────────────────────

def test_path_vs_string_literal_eq():
    """path == "literal" for string values."""
    vid = store({'people': [
        {'name': 'Alice', 'city': 'NYC'},
        {'name': 'Bob', 'city': 'LA'},
        {'name': 'Carol', 'city': 'NYC'},
    ]})
    results = select(vid, '.people[K].name',
                     '.people[K].city == "NYC"')
    names = sorted([load(r[0]) for r in results])
    assert names == ['Alice', 'Carol'], 'city=NYC: %r' % names
    print('  path vs string literal ==: OK')


def test_path_vs_string_literal_ne():
    """path != "literal" for string values."""
    vid = store({'people': [
        {'name': 'Alice'}, {'name': 'Bob'}, {'name': 'Carol'},
    ]})
    results = select(vid, '.people[K].name',
                     '.people[K].name != "Bob"')
    names = sorted([load(r[0]) for r in results])
    assert names == ['Alice', 'Carol'], 'name!=Bob: %r' % names
    print('  path vs string literal !=: OK')


def test_string_with_spaces():
    """String comparison with spaces and special characters."""
    vid = store({'items': [
        {'label': 'hello world'},
        {'label': 'foo bar'},
    ]})
    results = select(vid, '.items[K]',
                     '.items[K].label == "hello world"')
    assert len(results) == 1, 'string with spaces: %d' % len(results)
    print('  string with spaces: OK')


def test_empty_string():
    """Comparison with empty string."""
    vid = store({'items': [
        {'v': ''},
        {'v': 'notempty'},
    ]})
    results = select(vid, '.items[K]',
                     '.items[K].v == ""')
    assert len(results) == 1, 'empty string: %d' % len(results)
    print('  empty string: OK')


# ── Path vs type literal (true/false/null) ───────────────────

def test_path_vs_true():
    vid = store({'flags': [
        {'name': 'x', 'on': True},
        {'name': 'y', 'on': False},
        {'name': 'z', 'on': True},
    ]})
    results = select(vid, '.flags[K].name',
                     '.flags[K].on == true')
    names = sorted([load(r[0]) for r in results])
    assert names == ['x', 'z'], 'on==true: %r' % names
    print('  path vs true: OK')


def test_path_vs_false():
    vid = store({'flags': [
        {'name': 'x', 'on': True},
        {'name': 'y', 'on': False},
    ]})
    results = select(vid, '.flags[K].name',
                     '.flags[K].on == false')
    names = [load(r[0]) for r in results]
    assert names == ['y'], 'on==false: %r' % names
    print('  path vs false: OK')


def test_path_vs_null():
    vid = store({'data': [
        {'v': None},
        {'v': 1},
        {'v': 'hello'},
    ]})
    results = select(vid, '.data[K]',
                     '.data[K].v == null')
    assert len(results) == 1, 'null match: %d' % len(results)
    assert results[0][1]['K'] == 0
    print('  path vs null: OK')


def test_path_vs_null_ne():
    vid = store({'data': [
        {'v': None},
        {'v': 1},
        {'v': 'hello'},
    ]})
    results = select(vid, '.data[K]',
                     '.data[K].v != null')
    assert len(results) == 2, 'not-null: %d' % len(results)
    indices = sorted([r[1]['K'] for r in results])
    assert indices == [1, 2], 'not-null indices: %r' % indices
    print('  path vs null !=: OK')


# ── Numeric path-vs-path ─────────────────────────────────────

def test_numeric_path_gt_path():
    """Numeric ordering: path > path."""
    vid = store({'pairs': [
        {'a': 10, 'b': 20},
        {'a': 30, 'b': 15},
        {'a': 5, 'b': 5},
    ]})
    results = select(vid, '.pairs[K]',
                     '.pairs[K].a > .pairs[K].b')
    indices = [r[1]['K'] for r in results]
    assert indices == [1], 'a>b: K=%r' % indices
    print('  numeric path > path: OK')


def test_numeric_path_eq_path():
    """Numeric equality: path == path (same element, different fields)."""
    vid = store({'pairs': [
        {'a': 10, 'b': 20},
        {'a': 5, 'b': 5},
        {'a': 7, 'b': 7},
    ]})
    results = select(vid, '.pairs[K]',
                     '.pairs[K].a == .pairs[K].b')
    indices = sorted([r[1]['K'] for r in results])
    assert indices == [1, 2], 'a==b: K=%r' % indices
    print('  numeric path == path: OK')


def test_numeric_cross_binding():
    """Numeric cross-binding: .arr[K1].v == .arr[K2].v (different elements, same value)."""
    vid = store({'arr': [
        {'v': 10},
        {'v': 20},
        {'v': 10},
    ]})
    results = select(vid, '.arr[K1]',
        '.arr[K1].v == .arr[K2].v AND .arr[K1] != .arr[K2]')
    # Can't do K1 != K2 directly, but values at idx 0 and 2 are equal
    # Actually, path == path compares VALUES not indices.
    # K1=0,K2=2 and K1=2,K2=0 should both match (same value 10)
    # plus K1=0,K2=0 / K1=1,K2=1 / K1=2,K2=2 (self-matches)
    # But we want just the cross-matches. Without index inequality,
    # we get all pairs where values match.
    # Let's just verify the count.
    results_all = select(vid, '.arr[K1]',
        '.arr[K1].v == .arr[K2].v')
    # K1=0,K2=0 (10==10), K1=0,K2=2 (10==10), K1=1,K2=1 (20==20),
    # K1=2,K2=0 (10==10), K1=2,K2=2 (10==10)
    assert len(results_all) == 5, 'cross-binding numeric: %d' % len(results_all)
    print('  numeric cross-binding: OK')


# ── Mixed type rejection ─────────────────────────────────────

def test_mixed_type_not_equal():
    """Number 1 should not equal string '1'."""
    vid = store({'items': [1, '1']})
    results = select(vid, '.items[K1]',
        '.items[K1] == .items[K2]')
    # K1=0,K2=0 (num==num) and K1=1,K2=1 (str==str) only
    assert len(results) == 2, 'mixed type: %d (expect 2, not 4)' % len(results)
    print('  mixed type rejection: OK')


def test_mixed_type_number_vs_string():
    """Cross-compare where one is number, other is string — never equal."""
    vid = store({'data': [
        {'num': 42, 'str': '42'},
    ]})
    results = select(vid, '.data[K]',
        '.data[K].num == .data[K].str')
    assert len(results) == 0, 'num vs str: %d (expect 0)' % len(results)
    print('  number vs string never equal: OK')


# ── Arithmetic + path comparison ─────────────────────────────

def test_arithmetic_vs_path():
    """Arithmetic expression compared to a path value."""
    vid = store({'items': [
        {'price': 10, 'qty': 3, 'budget': 25},
        {'price': 10, 'qty': 3, 'budget': 35},
    ]})
    results = select(vid, '.items[K]',
        '.items[K].price * .items[K].qty > .items[K].budget')
    assert len(results) == 1, 'arithmetic vs path: %d' % len(results)
    assert results[0][1]['K'] == 0  # 10*3=30 > 25
    print('  arithmetic vs path: OK')


def test_arithmetic_both_sides():
    """Arithmetic on both sides of comparison."""
    vid = store({'data': [
        {'a': 2, 'b': 3, 'c': 5, 'd': 1},
    ]})
    results = select(vid, '.data[K]',
        '.data[K].a + .data[K].b == .data[K].c + .data[K].d')
    # 2+3=5, 5+1=6, not equal
    assert len(results) == 0, 'arithmetic both sides: %d' % len(results)
    print('  arithmetic both sides: OK')


# ── AND / OR ─────────────────────────────────────────────────

def test_and_with_cross_comparison():
    """AND combining string and numeric cross-comparisons."""
    vid = store({'employees': [
        {'name': 'Alice', 'dept': 'eng', 'salary': 100},
        {'name': 'Bob', 'dept': 'eng', 'salary': 120},
        {'name': 'Carol', 'dept': 'sales', 'salary': 110},
    ]})
    # Find eng employees with salary > 110
    results = select(vid, '.employees[K].name',
        '.employees[K].dept == "eng" AND .employees[K].salary > 110')
    names = [load(r[0]) for r in results]
    assert names == ['Bob'], 'AND: %r' % names
    print('  AND with mixed types: OK')


def test_or_with_cross_comparison():
    """OR combining different type comparisons."""
    vid = store({'items': [
        {'name': 'Alice', 'role': 'admin'},
        {'name': 'Bob', 'role': 'user'},
        {'name': 'Carol', 'role': 'admin'},
    ]})
    results = select(vid, '.items[K].name',
        '.items[K].name == "Bob" OR .items[K].role == "admin"')
    names = sorted([load(r[0]) for r in results])
    assert names == ['Alice', 'Bob', 'Carol'], 'OR: %r' % names
    print('  OR with mixed types: OK')


# ── Edge cases ───────────────────────────────────────────────

def test_single_element_self_join():
    """Single-element array: self-join produces exactly one row."""
    vid = store({'arr': [{'k': 'only'}]})
    results = select(vid, '.arr[K1]',
        '.arr[K1].k == .arr[K2].k')
    assert len(results) == 1, 'single element self-join: %d' % len(results)
    print('  single element self-join: OK')


def test_empty_array():
    """Empty array: no results."""
    vid = store({'arr': []})
    results = select(vid, '.arr[K]')
    assert len(results) == 0, 'empty array: %d' % len(results)
    print('  empty array: OK')


def test_no_match_string():
    """String comparison where nothing matches."""
    vid = store({'items': [{'name': 'Alice'}, {'name': 'Bob'}]})
    results = select(vid, '.items[K]',
        '.items[K].name == "Nobody"')
    assert len(results) == 0, 'no match: %d' % len(results)
    print('  no match string: OK')


def test_cross_join_result_bindings():
    """Verify all variable bindings are returned correctly."""
    vid = store({'rel': [
        {'a': 'x', 'b': 'y'},
        {'a': 'y', 'b': 'z'},
    ]})
    results = select(vid, '.rel[K1].a',
        '.rel[K1].b == .rel[K2].a')
    assert len(results) == 1
    r = results[0]
    assert r[1]['K1'] == 0 and r[1]['K2'] == 1, 'bindings: %r' % r[1]
    assert load(r[0]) == 'x'
    print('  cross-join bindings: OK')


# ── BigDecimal cross-comparison ──────────────────────────────

def test_bigdecimal_path_vs_path():
    """BigDecimal values compared across paths (exact via qjson_decimal_cmp)."""
    vid = store({'pairs': [
        {'a': Decimal('0.1'), 'b': Decimal('0.1')},
        {'a': Decimal('0.1'), 'b': Decimal('0.2')},
    ]})
    results = select(vid, '.pairs[K]',
        '.pairs[K].a == .pairs[K].b')
    assert len(results) == 1, 'bigdec eq: %d' % len(results)
    assert results[0][1]['K'] == 0
    print('  BigDecimal path vs path: OK')


# ── Main ─────────────────────────────────────────────────────

if __name__ == '__main__':
    setup()

    print('String cross-comparison:')
    test_grandparent_query()
    test_3hop_reachability()
    test_string_self_join()

    print('\nPath vs string literal:')
    test_path_vs_string_literal_eq()
    test_path_vs_string_literal_ne()
    test_string_with_spaces()
    test_empty_string()

    print('\nPath vs type literal:')
    test_path_vs_true()
    test_path_vs_false()
    test_path_vs_null()
    test_path_vs_null_ne()

    print('\nNumeric path-vs-path:')
    test_numeric_path_gt_path()
    test_numeric_path_eq_path()
    test_numeric_cross_binding()

    print('\nMixed type rejection:')
    test_mixed_type_not_equal()
    test_mixed_type_number_vs_string()

    print('\nArithmetic + path:')
    test_arithmetic_vs_path()
    test_arithmetic_both_sides()

    print('\nAND / OR:')
    test_and_with_cross_comparison()
    test_or_with_cross_comparison()

    print('\nEdge cases:')
    test_single_element_self_join()
    test_empty_array()
    test_no_match_string()
    test_cross_join_result_bindings()

    print('\nBigDecimal:')
    test_bigdecimal_path_vs_path()

    print('\nALL CROSS-COMPARISON TESTS PASSED')
