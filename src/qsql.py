# ============================================================
# qsql.py — QSQL: Per-name typed SQLite adapter with interval
# arithmetic for exact BigNum comparisons.
#
# QJSON values → per-name typed SQLite tables with interval
# arithmetic for exact BigNum comparisons.
#
#   price(btc, 67432.50M)  →  table "qjson_price_2"
#     arg0 = 'btc'              (atom → text, no interval)
#     arg1 = '67432.50'         (value as string)
#     arg1_lo = 67432.5         (ieee_double_round_down)
#     arg1_hi = 67432.5         (ieee_double_round_up)
#
# Each numeric arg → [round_down(x), x, round_up(x)]:
#   arg    = value as string (exact representation)
#   arg_lo = largest IEEE double ≤ exact value
#   arg_hi = smallest IEEE double ≥ exact value
#
# Exact doubles (most numbers): lo == hi → point interval.
# Non-exact (rare): lo + 1 ULP == hi → 1-ULP bracket.
# ============================================================

import json
import re
import struct
import math
from decimal import Decimal


def _safe_name(name):
    return re.sub(r'[^a-zA-Z0-9_]', '_', name)


def _table_name(prefix, name, arity):
    return "%s%s_%d" % (prefix, _safe_name(name), arity)


# ── IEEE 754 nextUp / nextDown ───────────────────────────────

def _next_up(x):
    if hasattr(math, 'nextafter'):
        return math.nextafter(x, float('inf'))
    if x != x or x == float('inf'):
        return x
    if x == 0.0:
        return 5e-324
    if x == float('-inf'):
        return -1.7976931348623157e+308
    buf = struct.pack('<d', x)
    bits = struct.unpack('<Q', buf)[0]
    if x > 0:
        bits += 1
    else:
        bits -= 1
    return struct.unpack('<d', struct.pack('<Q', bits))[0]


def _next_down(x):
    if hasattr(math, 'nextafter'):
        return math.nextafter(x, float('-inf'))
    return -_next_up(-x)


# ── Rounding direction detection ──────────────────────────────

def _rounding_dir(v, raw):
    """Determine rounding direction of double v vs exact decimal raw.

    Returns: 0 (exact), 1 (v > exact), -1 (v < exact).
    Language-agnostic: identical results to JS implementation.
    """
    # Overflow: Infinity > any finite exact value
    if v == float('inf'):
        return 1
    if v == float('-inf'):
        return -1
    # Underflow to zero
    if v == 0.0:
        stripped = raw.lstrip('+-').replace('.', '').replace('0', '')
        if stripped == '':
            return 0
        return 1 if raw.startswith('-') else -1
    # General case: exact decimal comparison
    d_exact = Decimal(raw)
    d_double = Decimal(v)
    if d_double == d_exact:
        return 0
    elif d_double > d_exact:
        return 1
    else:
        return -1


# ── Value conversion ─────────────────────────────────────────

def _arg_val(arg):
    """Primary value for the arg column (backward compat utility)."""
    if arg is None:
        return None
    t = arg.get("t")
    if t == "a":
        return arg["n"]
    if t == "n":
        v = arg["v"]
        if isinstance(v, (int, float)):
            return v
        return str(v)
    return json.dumps(arg, separators=(',', ':'))


def _arg_interval(arg):
    """Interval [str, lo, hi] for a serialized arg.

    atom:       (name,  None,          None         )
    exact num:  (None,  v,             v            )  str NULL (lo IS the value)
    inexact BN: (raw,   round_down(v), round_up(v)  )  1-ULP bracket

    str is None when lo == hi — the double IS the exact value.
    round_down = largest IEEE double <= exact value
    round_up   = smallest IEEE double >= exact value
    """
    if arg is None:
        return (None, None, None)
    t = arg.get("t")
    if t == "a":
        return (arg["n"], None, None)
    if t == "n":
        v = arg["v"]
        if not isinstance(v, (int, float)):
            raw_s = str(v)
            for suffix in "NMLnml":
                raw_s = raw_s.rstrip(suffix)
            v = float(raw_s)
        r = arg.get("r")
        if not r:
            # Plain number — exact, str NULL
            fv = float(v) if isinstance(v, int) else v
            return (None, fv, fv)
        # BigNum with repr — determine tightest interval
        raw = re.sub(r'[NMLnml]$', '', r)
        fv = float(v) if isinstance(v, int) else v
        d = _rounding_dir(fv, raw)
        if d == 0:
            return (None, fv, fv)             # exact double, str NULL
        elif d == 1:
            return (raw, _next_down(fv), fv)  # v > exact
        else:
            return (raw, fv, _next_up(fv))    # v < exact
    # compound/other
    return (json.dumps(arg, separators=(',', ':')), None, None)


# ── Adapter Factory ──────────────────────────────────────────

def qsql_adapter(path, parse_fn=None, prefix="qjson_"):
    """Create a QSQL adapter with interval arithmetic.

    path      — file path or ":memory:"
    parse_fn  — optional QJSON parse function (default: json.loads)
    prefix    — table name prefix (default: "qjson_")
    """
    import sqlite3
    conn = sqlite3.connect(path)
    conn.execute("PRAGMA journal_mode=WAL")

    _parse = parse_fn or json.loads
    _known = {}
    _meta_table = prefix + "meta"

    def _ensure_table(functor, arity):
        fa = "%s/%d" % (functor, arity)
        if fa in _known:
            return
        tbl = _table_name(prefix, functor, arity)
        cols = []
        for i in range(arity):
            cols.append("arg%d TEXT" % i)
            cols.append("arg%d_lo REAL" % i)
            cols.append("arg%d_hi REAL" % i)
        col_str = ", ".join(cols)
        ddl = 'CREATE TABLE IF NOT EXISTS "%s" (_key TEXT PRIMARY KEY%s)' % (
            tbl, (", " + col_str) if col_str else "")
        conn.execute(ddl)
        for i in range(arity):
            conn.execute(
                'CREATE INDEX IF NOT EXISTS "%s_ix_%d" ON "%s"(arg%d)' %
                (tbl, i, tbl, i))
            conn.execute(
                'CREATE INDEX IF NOT EXISTS "%s_ix_%d_lo" ON "%s"(arg%d_lo)' %
                (tbl, i, tbl, i))
        conn.execute(
            'INSERT OR IGNORE INTO "%s" VALUES (?, ?)' % _meta_table,
            (functor, arity))
        conn.commit()
        _known[fa] = True

    def _setup():
        conn.execute(
            'CREATE TABLE IF NOT EXISTS "%s" '
            '(functor TEXT, arity INTEGER, PRIMARY KEY(functor, arity))' %
            _meta_table)
        conn.commit()
        for row in conn.execute('SELECT functor, arity FROM "%s"' % _meta_table):
            _known["%s/%d" % (row[0], row[1])] = True

    def _insert(key, functor=None, arity=None):
        if functor is None:
            return
        _ensure_table(functor, arity)
        obj = _parse(key)
        values = [key]
        if obj.get("t") == "c" and "a" in obj:
            for i in range(arity):
                if i < len(obj["a"]):
                    iv = _arg_interval(obj["a"][i])
                else:
                    iv = (None, None, None, None)
                values.extend(iv)
        tbl = _table_name(prefix, functor, arity)
        ph = ", ".join("?" for _ in values)
        conn.execute(
            'INSERT OR IGNORE INTO "%s" VALUES (%s)' % (tbl, ph),
            tuple(values))

    def _remove(key):
        try:
            obj = _parse(key)
        except (ValueError, TypeError):
            return
        if obj.get("t") == "c":
            functor = obj["f"]
            arity = len(obj.get("a", []))
        elif obj.get("t") == "a":
            functor = obj["n"]
            arity = 0
        else:
            return
        fa = "%s/%d" % (functor, arity)
        if fa not in _known:
            return
        tbl = _table_name(prefix, functor, arity)
        conn.execute('DELETE FROM "%s" WHERE _key = ?' % tbl, (key,))

    def _all(predicates=None):
        results = []
        if predicates:
            metas = []
            for pred in predicates:
                parts = pred.split("/")
                metas.append((parts[0], int(parts[1])))
        else:
            metas = conn.execute(
                'SELECT functor, arity FROM "%s"' % _meta_table).fetchall()
        for functor, arity in metas:
            fa = "%s/%d" % (functor, arity)
            if fa not in _known:
                continue
            tbl = _table_name(prefix, functor, arity)
            try:
                rows = conn.execute(
                    'SELECT _key FROM "%s"' % tbl).fetchall()
                for row in rows:
                    results.append(row[0])
            except Exception:
                pass
        return results

    return {
        "setup": _setup,
        "insert": _insert,
        "remove": _remove,
        "all": _all,
        "commit": lambda: conn.commit(),
        "close": lambda: conn.close(),
    }
