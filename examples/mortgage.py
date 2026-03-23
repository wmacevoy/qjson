#!/usr/bin/env python3
"""Mortgage calculator — exact arithmetic via QJSON constraint solver.

Store a mortgage with one unknown field (?). The solver fills it in.
Change which field is ? to solve in any direction.

Requires: make  (builds qjson_ext with libbf)
"""

import sys, os, sqlite3
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from qjson import parse, stringify, Unbound, is_bound
from qjson_sql import qjson_sql_adapter
from qjson_query import qjson_select

def mortgage(conn, db, principal, rate, months, payment):
    """Store a mortgage, solve for the one unknown, return the result."""

    # Store the problem — one field should be Unbound
    root = db['store']({
        'principal': principal,
        'rate': rate,
        'months': months,
        'payment': payment,
    })
    db['commit']()

    # Find the value_ids for each field
    def vid(field):
        rows = qjson_select(conn, root, field, has_ext=True)
        return rows[0][0] if rows else None

    p_id = vid('.principal')
    r_id = vid('.rate')
    n_id = vid('.months')
    pay_id = vid('.payment')

    # Intermediates (all start as unbound)
    mr_id  = db['store'](Unbound('mr'));  db['commit']()  # monthly rate
    opr_id = db['store'](Unbound('opr')); db['commit']()  # 1 + monthly rate
    f_id   = db['store'](Unbound('f'));   db['commit']()  # (1+r)^n
    d_id   = db['store'](Unbound('d'));   db['commit']()  # f - 1
    pr_id  = db['store'](Unbound('pr'));  db['commit']()  # principal * monthly_rate
    num_id = db['store'](Unbound('num')); db['commit']()  # pr * f

    # Constraints: P = M * r/12 * (1+r/12)^n / ((1+r/12)^n - 1)
    # Run multiple passes — each pass propagates one more solved value.
    # Converges in at most N passes where N = number of intermediates.
    constraints = [
        ("SELECT qjson_solve_div(?, '12', ?)", (r_id, mr_id)),
        ("SELECT qjson_solve_add('1', ?, ?)", (mr_id, opr_id)),
        ("SELECT qjson_solve_pow(?, ?, ?)", (opr_id, n_id, f_id)),
        ("SELECT qjson_solve_sub(?, '1', ?)", (f_id, d_id)),
        ("SELECT qjson_solve_mul(?, ?, ?)", (p_id, mr_id, pr_id)),
        ("SELECT qjson_solve_mul(?, ?, ?)", (pr_id, f_id, num_id)),
        ("SELECT qjson_solve_div(?, ?, ?)", (num_id, d_id, pay_id)),
    ]
    for _pass in range(len(constraints)):
        for sql, params in constraints:
            conn.execute(sql, params)

    # Load the solved document
    return db['load'](root)


def main():
    conn = sqlite3.connect(':memory:')
    conn.enable_load_extension(True)

    ext = os.path.join(os.path.dirname(__file__), '..', 'qjson_ext')
    conn.load_extension(ext)

    db = qjson_sql_adapter(conn)
    db['setup']()

    print("=== Mortgage Calculator (exact arithmetic) ===\n")

    # Solve for payment
    print("1. What's the monthly payment?")
    print("   Principal: $250,000  Rate: 6.5%  Term: 30 years")
    result = mortgage(conn, db, parse('250000M'), parse('0.065M'), 360, Unbound('payment'))
    print("   Payment: $%s/month" % str(result['payment'])[:10])
    print()

    # Solve for principal
    print("2. How much can I borrow?")
    print("   Payment: $1,580.17  Rate: 6.5%  Term: 30 years")
    result = mortgage(conn, db, Unbound('principal'), parse('0.065M'), 360, parse('1580.17M'))
    print("   Principal: $%s" % str(result['principal'])[:12])
    print()

    # Consistency check (all fields filled)
    print("3. Consistency check:")
    print("   Principal: $250,000  Rate: 6.5%  Term: 30 years  Payment: $1,580.17")
    result = mortgage(conn, db, parse('250000M'), parse('0.065M'), 360, parse('1580.17M'))
    all_bound = all(is_bound(v) for v in result.values())
    print("   All fields bound: %s" % all_bound)
    print("   Result: %s" % stringify(result)[:80])
    print()

    # Solve for rate (requires backward propagation — this is harder
    # because rate appears in multiple places in the formula)
    print("4. What interest rate gives $1,500/month on $250K for 30 years?")
    print("   (Note: rate appears nonlinearly — constraint solver fills")
    print("    intermediates but can't solve the rate directly from payment)")
    result = mortgage(conn, db, parse('250000M'), Unbound('rate'), 360, parse('1500M'))
    if is_bound(result['rate']):
        print("   Rate: %s" % result['rate'])
    else:
        print("   Rate: underdetermined (nonlinear — needs iterative solver)")

    conn.close()


if __name__ == '__main__':
    main()
