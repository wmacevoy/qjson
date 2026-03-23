#!/usr/bin/env python3
"""Compound interest — exact arithmetic via QJSON constraint solver.

    FV = PV × (1 + r) ** n

Store 3 of 4 values, leave one as ? (Unbound).
Write the formula as constraints with AND.
The solver fills in unknowns via leaf folding.

Each unbound must be isolated in exactly one constraint.
Decompose complex expressions into steps:
    .opr == 1 + .rate
    .factor == POWER(.opr, .periods)
    .future == .present * .factor

Requires: make  (builds qjson_ext with libbf)
"""

import sys, os, sqlite3
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson import parse, stringify, Unbound, is_bound
from qjson_sql import qjson_sql_adapter
from qjson_query import qjson_solve

FORMULA = """
    .opr == 1 + .rate
    AND .factor == POWER(.opr, .periods)
    AND .future == .present * .factor
"""


def compound_interest(conn, db, **kwargs):
    """Store a compound interest problem, solve, return result."""
    fields = ['present', 'rate', 'periods', 'future']
    doc = {}
    for f in fields:
        v = kwargs.get(f)
        doc[f] = v if v is not None else Unbound(f)
    # Intermediates for bidirectional solving
    doc['opr'] = Unbound('opr')
    doc['factor'] = Unbound('factor')

    root = db['store'](doc)
    db['commit']()

    ok = qjson_solve(conn, root, FORMULA, has_ext=True)
    return db['load'](root), ok


def main():
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)
    conn.load_extension(os.path.join(os.path.dirname(__file__), '..', 'qjson_ext'))

    db = qjson_sql_adapter(conn)
    db['setup']()

    print("=== Compound Interest: FV = PV × (1+r)**n ===")
    print()
    print("Formula:")
    print("    .opr == 1 + .rate")
    print("    .factor == POWER(.opr, .periods)")
    print("    .future == .present * .factor")
    print()

    # Solve for future value
    print("1. What will $10,000 be worth in 10 years at 5%?")
    r, ok = compound_interest(conn, db,
        present=parse('10000M'), rate=parse('0.05M'), periods=parse('10M'))
    print("   FV = $%s  (solved: %s)\n" % (str(r['future'])[:10], ok))

    # Solve for present value
    print("2. How much to invest now for $20,000 in 10 years at 5%?")
    r, ok = compound_interest(conn, db,
        future=parse('20000M'), rate=parse('0.05M'), periods=parse('10M'))
    print("   PV = $%s  (solved: %s)\n" % (str(r['present'])[:10], ok))

    # Solve for rate
    print("3. What rate doubles $10,000 to $20,000 in 10 years?")
    r, ok = compound_interest(conn, db,
        present=parse('10000M'), future=parse('20000M'), periods=parse('10M'))
    print("   rate = %s  (solved: %s)\n" % (str(r['rate'])[:10], ok))

    # Solve for periods
    print("4. How long to grow $10,000 to $20,000 at 7%?")
    r, ok = compound_interest(conn, db,
        present=parse('10000M'), future=parse('20000M'), rate=parse('0.07M'))
    print("   periods = %s  (solved: %s)\n" % (str(r['periods'])[:10], ok))

    conn.close()


if __name__ == '__main__':
    main()
