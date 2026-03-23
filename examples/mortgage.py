#!/usr/bin/env python3
"""Mortgage calculator — exact arithmetic via QJSON constraint solver.

Store a mortgage with one unknown field (?). The constraint propagator
solves it by leaf-folding: find solvable constraints (1 unbound),
solve them, repeat until no more progress. Each constraint fires at
most once.

Requires: make  (builds qjson_ext with libbf)
"""

import sys, os, sqlite3
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson import parse, stringify, Unbound, is_bound
from qjson_sql import qjson_sql_adapter
from qjson_query import qjson_select


def propagate(conn, constraints):
    """Solve constraints by leaf folding.

    Return codes from qjson_solve_*:
      0 = inconsistent    1 = solved (updated one value)
      2 = consistent      3 = underdetermined (skipped)

    Each constraint fires at most twice: once skipped (3), once solved (1).
    """
    pending = list(constraints)
    while True:
        remaining = []
        solved_any = False
        for sql, params in pending:
            r = conn.execute(sql, params).fetchone()[0]
            if r == 1:
                solved_any = True   # made progress
            elif r == 3:
                remaining.append((sql, params))  # try again later
            elif r == 0:
                return False  # inconsistent
            # r == 2: already consistent, done with this constraint
        if not solved_any:
            break
        pending = remaining
    return len(remaining) == 0  # True if fully determined


def mortgage(conn, db, principal, rate, months, payment):
    """Store a mortgage, solve for the one unknown, return the result."""

    root = db['store']({
        'principal': principal,
        'rate': rate,
        'months': months,
        'payment': payment,
    })
    db['commit']()

    def vid(field):
        rows = qjson_select(conn, root, field, has_ext=True)
        return rows[0][0] if rows else None

    p_id = vid('.principal')
    r_id = vid('.rate')
    n_id = vid('.months')
    pay_id = vid('.payment')

    # Intermediates
    mr_id  = db['store'](Unbound('mr'));  db['commit']()
    opr_id = db['store'](Unbound('opr')); db['commit']()
    f_id   = db['store'](Unbound('f'));   db['commit']()
    d_id   = db['store'](Unbound('d'));   db['commit']()
    pr_id  = db['store'](Unbound('pr'));  db['commit']()
    num_id = db['store'](Unbound('num')); db['commit']()

    # P = M * r/12 * (1+r/12)^n / ((1+r/12)^n - 1)
    constraints = [
        ("SELECT qjson_solve_div(?, '12', ?)",  (r_id, mr_id)),
        ("SELECT qjson_solve_add('1', ?, ?)",   (mr_id, opr_id)),
        ("SELECT qjson_solve_pow(?, ?, ?)",     (opr_id, n_id, f_id)),
        ("SELECT qjson_solve_sub(?, '1', ?)",   (f_id, d_id)),
        ("SELECT qjson_solve_mul(?, ?, ?)",     (p_id, mr_id, pr_id)),
        ("SELECT qjson_solve_mul(?, ?, ?)",     (pr_id, f_id, num_id)),
        ("SELECT qjson_solve_div(?, ?, ?)",     (num_id, d_id, pay_id)),
    ]

    determined = propagate(conn, constraints)
    return db['load'](root), determined


def main():
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)
    conn.load_extension(os.path.join(os.path.dirname(__file__), '..', 'qjson_ext'))

    db = qjson_sql_adapter(conn)
    db['setup']()

    print("=== Mortgage Calculator (exact arithmetic) ===\n")

    # Solve for payment
    print("1. What's the monthly payment?")
    print("   $250,000 @ 6.5% for 30 years")
    result, ok = mortgage(conn, db, parse('250000M'), parse('0.065M'), 360, Unbound('payment'))
    print("   → $%s/month  (solved: %s)\n" % (str(result['payment'])[:10], ok))

    # Solve for principal
    print("2. How much can I borrow?")
    print("   $1,580.17/month @ 6.5% for 30 years")
    result, ok = mortgage(conn, db, Unbound('principal'), parse('0.065M'), 360, parse('1580.17M'))
    print("   → $%s  (solved: %s)\n" % (str(result['principal'])[:12], ok))

    # Consistency check — exact means exact
    print("3. Is $1,580.17 the exact payment?")
    result, ok = mortgage(conn, db, parse('250000M'), parse('0.065M'), 360, parse('1580.17M'))
    print("   → %s (the exact payment is $1,580.17005..., not $1,580.17)\n" % (
        "consistent" if ok else "INCONSISTENT"))

    # Underdetermined
    print("4. Rate unknown (nonlinear — can't solve with linear constraints)")
    result, ok = mortgage(conn, db, parse('250000M'), Unbound('rate'), 360, parse('1500M'))
    print("   → determined: %s" % ok)
    if not is_bound(result['rate']):
        print("   rate still unbound (needs iterative solver)")

    conn.close()


if __name__ == '__main__':
    main()
