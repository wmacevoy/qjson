#!/usr/bin/env python3
"""qjq — QJSON query tool.

Like jq, but with arbitrary-precision arithmetic, constraint
solving, and transitive closure.  Backed by SQLite + libbf.

Usage:
  echo '{name: alice, age: 30}' | qjq
  echo '{items: [1, 2, 3]}' | qjq '.items[K]'
  echo '{edge: {[a,b],[b,c],[c,d]}}' | qjq --closure .edge
  qjq --solve '.future == .present * POWER(1 + .rate, .periods)' <<< '{present: 10000M, rate: ?, periods: 10}'
  qjq --eval 'qjson_add("0.1", "0.2")'
"""

import sys
import os
import argparse
import sqlite3

# Find src/ relative to this script
_src = os.path.dirname(os.path.abspath(__file__))
if _src not in sys.path:
    sys.path.insert(0, _src)

from qjson import parse, stringify
from qjson_sql import qjson_sql_adapter


def _load_ext(conn):
    """Try to load qjson_ext. Returns True if loaded."""
    try:
        conn.enable_load_extension(True)
    except Exception:
        return False
    # Search relative to this file, then cwd
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(here, '..', 'qjson_ext'),
        os.path.join(os.getcwd(), 'qjson_ext'),
    ]
    for c in candidates:
        try:
            conn.load_extension(c)
            return True
        except Exception:
            pass
    return False


def cmd_format(args):
    """Parse stdin, emit canonical QJSON."""
    text = sys.stdin.read()
    value = parse(text)
    if args.indent:
        print(stringify(value, indent=int(args.indent)))
    else:
        print(stringify(value))


def cmd_query(args):
    """Store stdin, query with path expression."""
    from qjson_query import qjson_select

    text = sys.stdin.read()
    value = parse(text)

    conn = sqlite3.connect(':memory:')
    has_ext = _load_ext(conn)
    db = qjson_sql_adapter(conn)
    db['setup']()

    root = db['store'](value)
    db['commit']()

    rows = qjson_select(conn, root, args.path,
                        where_expr=args.where,
                        has_ext=has_ext)
    for vid, bindings in rows:
        v = db['load'](vid)
        print(stringify(v))
    conn.close()


def cmd_solve(args):
    """Store stdin, solve constraint, emit result."""
    text = sys.stdin.read()
    value = parse(text)

    conn = sqlite3.connect(':memory:')
    has_ext = _load_ext(conn)
    if not has_ext:
        print("qjq: solver requires qjson_ext (run 'make')", file=sys.stderr)
        sys.exit(1)

    db = qjson_sql_adapter(conn)
    db['setup']()

    root = db['store'](value)
    db['commit']()

    r = conn.execute('SELECT qjson_solve(?, ?)',
                     (root, args.formula)).fetchone()[0]
    if r:
        result = db['load'](root)
        print(stringify(result))
    else:
        print("qjq: no solution", file=sys.stderr)
        sys.exit(1)
    conn.close()


def cmd_closure(args):
    """Store stdin, compute transitive closure."""
    from qjson_query import qjson_closure

    text = sys.stdin.read()
    value = parse(text)

    conn = sqlite3.connect(':memory:')
    has_ext = _load_ext(conn)
    db = qjson_sql_adapter(conn)
    db['setup']()

    root = db['store'](value)
    db['commit']()

    pairs = qjson_closure(conn, root, args.set_path,
                          where_from=args.from_val,
                          where_to=args.to_val)
    for f, t in pairs:
        print("%s -> %s" % (f, t))
    conn.close()


def cmd_eval(args):
    """Evaluate a SQL expression (arbitrary-precision arithmetic)."""
    conn = sqlite3.connect(':memory:')
    has_ext = _load_ext(conn)
    if not has_ext:
        print("qjq: --eval requires qjson_ext (run 'make')", file=sys.stderr)
        sys.exit(1)

    result = conn.execute('SELECT %s' % args.expr).fetchone()[0]
    print(result)
    conn.close()


def _get_key(args):
    """Get encryption key from env or file. Never from CLI args."""
    key = os.environ.get('QJSON_KEY')
    if key:
        return key.encode() if len(key) != 32 else key
    kf = os.environ.get('QJSON_KEY_FILE') or getattr(args, 'key_file', None)
    if kf:
        with open(kf, 'rb') as f:
            return f.read(32)
    return None


def main():
    p = argparse.ArgumentParser(
        prog='qjq',
        description='QJSON query tool — parse, query, solve, closure',
        epilog='Keys: set QJSON_KEY or QJSON_KEY_FILE env var. '
               'Never pass keys as CLI arguments.')

    p.add_argument('path', nargs='?',
                   help='jq-style path expression (e.g. .items[K])')
    p.add_argument('-w', '--where',
                   help='WHERE predicate for queries')
    p.add_argument('-s', '--solve', dest='formula',
                   help='solve constraint formula')
    p.add_argument('-c', '--closure', dest='set_path',
                   help='transitive closure of a set path')
    p.add_argument('--from', dest='from_val',
                   help='closure filter: starting node')
    p.add_argument('--to', dest='to_val',
                   help='closure filter: ending node')
    p.add_argument('-e', '--eval', dest='expr',
                   help='evaluate SQL expression')
    p.add_argument('-i', '--indent', default=None,
                   help='indent width for pretty-printing')
    p.add_argument('--db', default=None,
                   help='persistent database file (default: :memory:)')
    p.add_argument('--key-file', default=None,
                   help='read encryption key from file (or set QJSON_KEY)')

    args = p.parse_args()

    if args.expr:
        cmd_eval(args)
    elif args.formula:
        cmd_solve(args)
    elif args.set_path:
        cmd_closure(args)
    elif args.path:
        cmd_query(args)
    else:
        cmd_format(args)


if __name__ == '__main__':
    main()
