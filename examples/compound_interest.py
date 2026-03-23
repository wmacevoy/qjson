#!/usr/bin/env python3
"""Compound interest — exact arithmetic via QJSON constraint solver.

    FV = PV × (1 + r)^n

Store 3 of 4 values, leave one as ? (Unbound).
The solver fills it in — any direction.

Each unbound must appear in exactly one constraint for the
solver to isolate it. The compound interest formula decomposes
into 3 constraints with no fan-out:

    .opr    == 1 + .rate       r appears once
    .factor == .opr ^ .periods opr, periods appear once
    .future == .present * .factor   present, factor appear once

Requires: make  (builds qjson_ext with libbf)
"""

import sys, os, sqlite3
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson import parse, stringify, Unbound, is_bound
from qjson_sql import qjson_sql_adapter
from qjson_query import qjson_select

FORMULA = [
    ("SELECT qjson_solve_add('1', ?, ?)", 'rate', 'opr'),
    ("SELECT qjson_solve_pow(?, ?, ?)",   'opr', 'periods', 'factor'),
    ("SELECT qjson_solve_mul(?, ?, ?)",   'present', 'factor', 'future'),
]


def propagate(conn, constraints):
    """Leaf-fold: solve constraints with exactly 1 unknown, repeat."""
    pending = list(constraints)
    while True:
        remaining, solved = [], False
        for sql, params in pending:
            r = conn.execute(sql, params).fetchone()[0]
            if r == 1: solved = True
            elif r == 3: remaining.append((sql, params))
            elif r == 0: return False  # inconsistent
        if not solved: break
        pending = remaining
    return len(remaining) == 0


def compound_interest(conn, db, **kwargs):
    """Store a compound interest problem, solve, return result.

    Pass exactly 3 of: present, rate, periods, future.
    The 4th should be omitted or None — it becomes Unbound.
    """
    fields = ['present', 'rate', 'periods', 'future']
    intermediates = ['opr', 'factor']

    doc = {}
    for f in fields:
        v = kwargs.get(f)
        doc[f] = v if v is not None else Unbound(f)
    for f in intermediates:
        doc[f] = Unbound(f)

    root = db['store'](doc)
    db['commit']()

    # Resolve field value_ids
    def vid(field):
        rows = qjson_select(conn, root, '.' + field, has_ext=True)
        return rows[0][0] if rows else None

    ids = {f: vid(f) for f in fields + intermediates}

    # Build constraints with value_ids
    constraints = [
        ("SELECT qjson_solve_add('1', ?, ?)", (ids['rate'], ids['opr'])),
        ("SELECT qjson_solve_pow(?, ?, ?)", (ids['opr'], ids['periods'], ids['factor'])),
        ("SELECT qjson_solve_mul(?, ?, ?)", (ids['present'], ids['factor'], ids['future'])),
    ]

    ok = propagate(conn, constraints)
    return db['load'](root), ok


def main():
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)
    conn.load_extension(os.path.join(os.path.dirname(__file__), '..', 'qjson_ext'))

    db = qjson_sql_adapter(conn)
    db['setup']()

    print("=== Compound Interest: FV = PV × (1+r)^n ===\n")

    # Solve for future value
    print("1. What will $10,000 be worth in 10 years at 5%?")
    r, ok = compound_interest(conn, db,
        present=parse('10000M'), rate=parse('0.05M'), periods=10)
    print("   FV = $%s  (solved: %s)\n" % (str(r['future'])[:10], ok))

    # Solve for present value
    print("2. How much to invest now for $20,000 in 10 years at 5%?")
    r, ok = compound_interest(conn, db,
        future=parse('20000M'), rate=parse('0.05M'), periods=10)
    print("   PV = $%s  (solved: %s)\n" % (str(r['present'])[:10], ok))

    # Solve for rate
    print("3. What rate doubles $10,000 to $20,000 in 10 years?")
    r, ok = compound_interest(conn, db,
        present=parse('10000M'), future=parse('20000M'), periods=10)
    print("   rate = %s  (solved: %s)\n" % (str(r['rate'])[:10], ok))

    # Solve for periods
    print("4. How long to grow $10,000 to $20,000 at 7%?")
    r, ok = compound_interest(conn, db,
        present=parse('10000M'), future=parse('20000M'), rate=parse('0.07M'))
    print("   periods = %s  (solved: %s)\n" % (str(r['periods'])[:10], ok))

    conn.close()


if __name__ == '__main__':
    main()
