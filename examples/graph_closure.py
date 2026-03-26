#!/usr/bin/env python3
"""Graph reachability — transitive closure via WITH RECURSIVE.

Given a set of edges {[a,b], [b,c], [c,d]}, compute all
reachable pairs using qjson_closure().

Requires: make  (builds qjson_ext with libbf)
"""

import sys, os, sqlite3
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson_sql import qjson_sql_adapter
from qjson_query import qjson_closure, qjson_select
from qjson import parse


def main():
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)
    conn.load_extension(os.path.join(os.path.dirname(__file__), '..', 'qjson_ext'))

    db = qjson_sql_adapter(conn)
    db['setup']()

    # ── Store a directed graph as a set of edge tuples ────

    print("=== Graph: a -> b -> c -> d, a -> e -> d ===\n")

    root = db['store']({
        'edge': parse('{[a, b], [b, c], [c, d], [a, e], [e, d]}')
    })
    db['commit']()

    # ── Full transitive closure ───────────────────────────

    print("1. All reachable pairs:")
    pairs = qjson_closure(conn, root, '.edge')
    for f, t in sorted(pairs):
        print("   %s -> %s" % (f, t))
    print("   (%d pairs)\n" % len(pairs))

    # ── Reachable from 'a' ────────────────────────────────

    print("2. Reachable from 'a':")
    pairs = qjson_closure(conn, root, '.edge', where_from='a')
    for f, t in sorted(pairs):
        print("   %s -> %s" % (f, t))
    print()

    # ── What reaches 'd'? ────────────────────────────────

    print("3. What reaches 'd'?")
    pairs = qjson_closure(conn, root, '.edge', where_to='d')
    for f, t in sorted(pairs):
        print("   %s -> %s" % (f, t))
    print()

    # ── C extension: closure as QJSON set ─────────────────

    print("4. C extension — qjson_closure() returns a QJSON set:")
    result = conn.execute(
        'SELECT qjson_closure(?, ?, ?)',
        (root, '.edge', 'qjson_')).fetchone()[0]
    print("   %s\n" % result)

    # ── Set iteration with [K] ────────────────────────────

    print("5. Iterate edge facts with [K]:")
    rows = qjson_select(conn, root, '.edge[K]', has_ext=True)
    for r in rows:
        print("   %s" % db['load'](r[0]))
    print()

    # ── Grandparent join ──────────────────────────────────

    print("6. Grandparent join: .edge[K1][1] == .edge[K2][0]")
    rows = qjson_select(conn, root, '.edge[K1][0]',
        where_expr='.edge[K1][1] == .edge[K2][0]',
        has_ext=True)
    for r in rows:
        gp = db['load'](r[0])
        print("   %s is a 2-hop ancestor" % gp)

    conn.close()


if __name__ == '__main__':
    main()
