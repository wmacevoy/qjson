#!/usr/bin/env python3
"""Relational queries — SQL self-joins via QJSON variable bindings.

Variable bindings [K1], [K2], ... in path expressions create
unconstrained JOINs to array elements. Combined with WHERE
cross-comparisons, this gives horn-clause-equivalent power
using familiar SQL-like syntax.

    grandparent(GP, GC) :- parent(GP, Y), parent(Y, GC).

becomes:

    SELECT .parent[K1].name
    WHERE  .parent[K1].child == .parent[K2].name

No Prolog needed — just paths, variables, and WHERE.

Requires: make  (builds qjson_ext with libbf)
"""

import sys, os, sqlite3
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson_sql import qjson_sql_adapter
from qjson_query import qjson_select
from decimal import Decimal


def main():
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)
    conn.load_extension(os.path.join(os.path.dirname(__file__), '..', 'qjson_ext'))

    db = qjson_sql_adapter(conn)
    db['setup']()

    # ── Family tree ──────────────────────────────────────────

    print("=== Family Tree ===\n")
    print("Data: Alice->Bob->Carol->Dave->Eve\n")

    family = db['store']({'parent': [
        {'name': 'Alice', 'child': 'Bob'},
        {'name': 'Bob',   'child': 'Carol'},
        {'name': 'Carol', 'child': 'Dave'},
        {'name': 'Dave',  'child': 'Eve'},
    ]})
    db['commit']()

    # Grandparent: 2-hop join
    print("1. Grandparents (2-hop):")
    print("   SELECT .parent[K1].name")
    print("   WHERE  .parent[K1].child == .parent[K2].name\n")

    results = qjson_select(conn, family, '.parent[K1].name',
        where_expr='.parent[K1].child == .parent[K2].name',
        has_ext=True)
    for r in results:
        gp = db['load'](r[0])
        k2 = r[1]['K2']
        gc_vid = qjson_select(conn, family, '.parent[K2].child',
            has_ext=True)
        # Get the grandchild from K2 binding
        gc_results = qjson_select(conn, family,
            '.parent[K1].child',
            where_expr='.parent[K1].child == .parent[K2].name',
            has_ext=True)
        print("   %s is grandparent (K1=%d, K2=%d)" % (gp, r[1]['K1'], k2))

    # Great-grandparent: 3-hop join
    print("\n2. Great-grandparents (3-hop):")
    print("   WHERE .parent[K1].child == .parent[K2].name")
    print("     AND .parent[K2].child == .parent[K3].name\n")

    results = qjson_select(conn, family, '.parent[K1].name',
        where_expr='.parent[K1].child == .parent[K2].name '
                    'AND .parent[K2].child == .parent[K3].name',
        has_ext=True)
    for r in results:
        ggp = db['load'](r[0])
        print("   %s is great-grandparent (K1=%d, K2=%d, K3=%d)"
              % (ggp, r[1]['K1'], r[1]['K2'], r[1]['K3']))

    # ── Graph reachability ───────────────────────────────────

    print("\n=== Flight Routes ===\n")
    print("Data: SFO->DEN, DEN->ORD, ORD->JFK, SFO->LAX, LAX->JFK\n")

    flights = db['store']({'route': [
        {'from': 'SFO', 'to': 'DEN'},
        {'from': 'DEN', 'to': 'ORD'},
        {'from': 'ORD', 'to': 'JFK'},
        {'from': 'SFO', 'to': 'LAX'},
        {'from': 'LAX', 'to': 'JFK'},
    ]})
    db['commit']()

    print("3. Two-hop connections from SFO:")
    print("   WHERE .route[K1].from == \"SFO\"")
    print("     AND .route[K1].to == .route[K2].from\n")

    results = qjson_select(conn, flights, '.route[K2].to',
        where_expr='.route[K1].from == "SFO" '
                    'AND .route[K1].to == .route[K2].from',
        has_ext=True)
    for r in results:
        dest = db['load'](r[0])
        via_idx = r[1]['K1']
        # Look up the via city
        via_results = qjson_select(conn, flights, '.route[K1].to',
            has_ext=True)
        print("   SFO -> ... -> %s (K1=%d, K2=%d)"
              % (dest, r[1]['K1'], r[1]['K2']))

    # ── Employee queries with mixed types ────────────────────

    print("\n=== Employee Database ===\n")

    employees = db['store']({'emp': [
        {'name': 'Alice', 'dept': 'eng',   'salary': Decimal('130000'), 'active': True},
        {'name': 'Bob',   'dept': 'eng',   'salary': Decimal('120000'), 'active': True},
        {'name': 'Carol', 'dept': 'sales', 'salary': Decimal('110000'), 'active': True},
        {'name': 'Dave',  'dept': 'eng',   'salary': Decimal('95000'),  'active': False},
        {'name': 'Eve',   'dept': 'sales', 'salary': Decimal('140000'), 'active': True},
    ]})
    db['commit']()

    print("4. Active engineers earning > 100,000:")
    print("   WHERE .emp[K].dept == \"eng\"")
    print("     AND .emp[K].salary > 100000")
    print("     AND .emp[K].active == true\n")

    results = qjson_select(conn, employees, '.emp[K].name',
        where_expr='.emp[K].dept == "eng" '
                    'AND .emp[K].salary > 100000 '
                    'AND .emp[K].active == true',
        has_ext=True)
    for r in results:
        print("   %s" % db['load'](r[0]))

    print("\n5. Find pairs where one earns more than another in same dept:")
    print("   WHERE .emp[K1].dept == .emp[K2].dept")
    print("     AND .emp[K1].salary > .emp[K2].salary\n")

    results = qjson_select(conn, employees, '.emp[K1].name',
        where_expr='.emp[K1].dept == .emp[K2].dept '
                    'AND .emp[K1].salary > .emp[K2].salary',
        has_ext=True)
    for r in results:
        higher = db['load'](r[0])
        # Get the lower-paid person
        lower_results = qjson_select(conn, employees, '.emp[K2].name',
            where_expr='.emp[K1].dept == .emp[K2].dept '
                        'AND .emp[K1].salary > .emp[K2].salary',
            has_ext=True)
        for lr in lower_results:
            if lr[1]['K1'] == r[1]['K1'] and lr[1]['K2'] == r[1]['K2']:
                lower = db['load'](lr[0])
                print("   %s earns more than %s" % (higher, lower))

    # ── Arithmetic in cross-comparison ───────────────────────

    print("\n=== Budget Check ===\n")

    orders = db['store']({'order': [
        {'item': 'widget',  'price': Decimal('25'), 'qty': 4, 'budget': Decimal('90')},
        {'item': 'gadget',  'price': Decimal('50'), 'qty': 3, 'budget': Decimal('200')},
        {'item': 'gizmo',   'price': Decimal('15'), 'qty': 10, 'budget': Decimal('100')},
    ]})
    db['commit']()

    print("6. Orders where price * qty exceeds budget:")
    print("   WHERE .order[K].price * .order[K].qty > .order[K].budget\n")

    results = qjson_select(conn, orders, '.order[K].item',
        where_expr='.order[K].price * .order[K].qty > .order[K].budget',
        has_ext=True)
    for r in results:
        print("   %s is over budget" % db['load'](r[0]))

    conn.close()


if __name__ == '__main__':
    main()
