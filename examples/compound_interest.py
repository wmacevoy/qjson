#!/usr/bin/env python3
"""Compound interest — exact arithmetic via embedded QJSON solver.

    FV = PV × (1 + r) ** n

Store 3 of 4 values, leave one as ? (Unbound).
One SQL call solves for the unknown — any direction.

Requires: make  (builds qjson_ext with libbf)
"""

import sys, os, sqlite3
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson import parse, stringify, Unbound, is_bound
from qjson_sql import qjson_sql_adapter
from decimal import Decimal

FORMULA = '.future == .present * POWER(1 + .rate, .periods)'


def solve(conn, db, **kwargs):
    """Store a compound interest problem, solve via SQL, return result."""
    fields = ['present', 'rate', 'periods', 'future']
    doc = {f: kwargs.get(f, Unbound(f)) for f in fields}

    root = db['store'](doc)
    db['commit']()

    # One SQL call — the database does the math
    conn.execute("SELECT qjson_solve(?, ?)", (root, FORMULA))

    return db['load'](root)


def main():
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)
    conn.load_extension(os.path.join(os.path.dirname(__file__), '..', 'qjson_ext'))

    db = qjson_sql_adapter(conn)
    db['setup']()

    print("=== Compound Interest: FV = PV × (1+r)**n ===")
    print("Formula: %s\n" % FORMULA)

    print("1. $10,000 at 5%% for 10 years?")
    r = solve(conn, db, present=Decimal('10000'), rate=Decimal('0.05'), periods=Decimal('10'))
    print("   FV = $%s\n" % str(r['future'])[:10])

    print("2. How much to invest for $20,000 in 10 years at 5%%?")
    r = solve(conn, db, future=Decimal('20000'), rate=Decimal('0.05'), periods=Decimal('10'))
    print("   PV = $%s\n" % str(r['present'])[:10])

    print("3. What rate doubles money in 10 years?")
    r = solve(conn, db, present=Decimal('10000'), future=Decimal('20000'), periods=Decimal('10'))
    print("   rate = %s%%\n" % str(float(str(r['rate'])) * 100)[:6])

    print("4. How long to double at 7%%?")
    r = solve(conn, db, present=Decimal('10000'), future=Decimal('20000'), rate=Decimal('0.07'))
    print("   periods = %s years\n" % str(r['periods'])[:5])

    conn.close()


if __name__ == '__main__':
    main()
