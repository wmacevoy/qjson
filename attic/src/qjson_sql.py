# ============================================================
# qjson_sql.py — normalized relational storage for QJSON values.
#
# Stores any QJSON value (null, boolean, number, bigint, bigdec,
# bigfloat, string, blob, array, object) in a normalized schema
# with interval projection for exact numeric comparisons.
#
# Supports SQLite and PostgreSQL.  Dialect is auto-detected from
# the connection object, or can be set explicitly.
#
# Schema (prefix default "qjson_"):
#   {prefix}value        — root table, every value gets an id + type
#   {prefix}number       — [lo, str, hi] interval for all numeric types
#   {prefix}string       — string values
#   {prefix}blob         — binary data
#   {prefix}array        — array containers
#   {prefix}array_item   — array elements (idx, value_id)
#   {prefix}object       — object containers
#   {prefix}object_item  — object entries (key, value_id)
# ============================================================

import struct
import math
from decimal import Decimal

# Import QJSON types for classification
from qjson import BigInt, BigFloat, Blob, Unbound, QMap


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
    """
    if v == float('inf'):
        return 1
    if v == float('-inf'):
        return -1
    if v == 0.0:
        stripped = raw.lstrip('+-').replace('.', '').replace('0', '')
        if stripped == '':
            return 0
        return 1 if raw.startswith('-') else -1
    d_exact = Decimal(raw)
    d_double = Decimal(v)
    if d_double == d_exact:
        return 0
    elif d_double > d_exact:
        return 1
    else:
        return -1


# ── Directed rounding: decimal string → IEEE double ──────────
#
# round_down(raw): largest IEEE double <= exact(raw)
# round_up(raw):   smallest IEEE double >= exact(raw)
#
# Equivalent to the C qjson_project() which uses BF_RNDD/BF_RNDU
# (libbf) or FE_DOWNWARD/FE_UPWARD (fesetround).  All three
# implementations produce identical results for all inputs.

def round_down(raw):
    """Largest IEEE 754 double <= exact decimal value of raw."""
    v = float(raw)
    d = _rounding_dir(v, raw)
    if d <= 0:
        return v        # v <= exact → v is round-down
    return _next_down(v)  # v > exact → step down one ULP


def round_up(raw):
    """Smallest IEEE 754 double >= exact decimal value of raw."""
    v = float(raw)
    d = _rounding_dir(v, raw)
    if d >= 0:
        return v        # v >= exact → v is round-up
    return _next_up(v)    # v < exact → step up one ULP


# ── Numeric interval projection ──────────────────────────────

def _project_numeric(raw):
    """Compute [lo, str, hi] interval for a decimal string.

    raw — exact decimal string

    Returns (lo, str_or_None, hi).
    lo  = round_down(raw)
    hi  = round_up(raw)
    str = raw when lo != hi, None when lo == hi (exact double)
    """
    lo = round_down(raw)
    hi = round_up(raw)
    s = None if lo == hi else raw
    return (lo, s, hi)


# ── Value type classification ────────────────────────────────

def _classify_value(value):
    """Classify a native Python value into its QJSON type.

    Returns (type_str, raw_decimal_str_or_None).
    """
    if value is None:
        return ("null", None)
    if value is True:
        return ("true", None)
    if value is False:
        return ("false", None)
    if isinstance(value, Unbound):
        return ("unbound", value.name)
    if isinstance(value, Blob):
        return ("blob", None)
    if isinstance(value, BigFloat):
        return ("bigfloat", value._raw)
    if isinstance(value, BigInt):
        return ("bigint", str(int(value)))
    if isinstance(value, Decimal):
        return ("bigdec", str(value))
    if isinstance(value, float):
        return ("number", None)
    if isinstance(value, int):
        return ("number", None)
    if isinstance(value, str):
        return ("string", None)
    if isinstance(value, (list, tuple)):
        return ("array", None)
    if isinstance(value, QMap):
        return ("object", None)
    if isinstance(value, dict):
        return ("object", None)
    return ("string", None)


# ── Dialect detection ────────────────────────────────────────

def _detect_dialect(conn):
    """Auto-detect SQL dialect from connection object.

    Returns 'sqlite' or 'postgres'.
    """
    mod = type(conn).__module__
    if 'psycopg' in mod or 'pgdb' in mod or 'pg8000' in mod:
        return 'postgres'
    return 'sqlite'


# SQL dialect differences:
#   SQLite:     REAL (8-byte), BLOB, INTEGER PRIMARY KEY, ?, lastrowid
#   PostgreSQL: DOUBLE PRECISION (8-byte), BYTEA, SERIAL PRIMARY KEY,
#               %s, RETURNING id

_DIALECTS = {
    'sqlite': {
        'float_type': 'REAL',
        'blob_type': 'BLOB',
        'serial_pk': 'INTEGER PRIMARY KEY',
        'param': '?',
        'returning': False,
    },
    'postgres': {
        'float_type': 'DOUBLE PRECISION',
        'blob_type': 'BYTEA',
        'serial_pk': 'SERIAL PRIMARY KEY',
        'param': '%s',
        'returning': True,
    },
}


# ── Adapter Factory ──────────────────────────────────────────

def _load_sqlite_ext(conn, ext_path=None):
    """Load the qjson_ext SQLite extension if available.

    Registers qjson_decimal_cmp() and qjson_cmp() SQL functions
    backed by libbf for exact arbitrary-precision comparison.
    """
    import os
    if ext_path is None:
        # Search relative to this file, then project root
        here = os.path.dirname(os.path.abspath(__file__))
        for candidate in [
            os.path.join(here, '..', 'qjson_ext'),
            os.path.join(here, '..', 'qjson_ext.dylib'),
            os.path.join(here, '..', 'qjson_ext.so'),
        ]:
            base = candidate.rsplit('.', 1)[0] if '.' in os.path.basename(candidate) else candidate
            if os.path.exists(base + '.dylib') or os.path.exists(base + '.so'):
                ext_path = base
                break
    if ext_path is None:
        return False
    try:
        conn.enable_load_extension(True)
        conn.load_extension(ext_path)
        return True
    except Exception:
        return False


def qjson_sql_adapter(db, prefix="qjson_", dialect=None, ext_path=None, key=None):
    """Create a QJSON SQL adapter with normalized relational storage.

    db       — sqlite3/sqlcipher connection, psycopg2 connection, or file path string
    prefix   — table name prefix (default: "qjson_")
    dialect  — 'sqlite' or 'postgres' (auto-detected if None)
    ext_path — path to qjson_ext shared library (auto-detected if None)
    key      — SQLCipher encryption key (optional, sqlite dialect only)
    """
    if isinstance(db, str):
        import sqlite3
        conn = sqlite3.connect(db)
        dialect = 'sqlite'
    else:
        conn = db
        if dialect is None:
            dialect = _detect_dialect(conn)

    if dialect == 'sqlite':
        # SQLCipher encryption key (must be set before any other operation)
        if key is not None:
            conn.execute("PRAGMA key = '%s'" % key.replace("'", "''"))
        conn.execute("PRAGMA journal_mode=WAL")

    # Load libbf extension for exact comparison in SQL
    _has_ext = False
    if dialect == 'sqlite':
        _has_ext = _load_sqlite_ext(conn, ext_path)

    d = _DIALECTS[dialect]
    P = d['param']
    FT = d['float_type']
    BT = d['blob_type']
    SPK = d['serial_pk']
    USE_RETURNING = d['returning']

    t_value = prefix + "value"
    t_number = prefix + "number"
    t_string = prefix + "string"
    t_blob = prefix + "blob"
    t_array = prefix + "array"
    t_array_item = prefix + "array_item"
    t_object = prefix + "object"
    t_object_item = prefix + "object_item"

    # psycopg2 connections don't have .execute(); need a cursor.
    # sqlite3 connections do, but cursor works for both.
    _cur = conn.cursor()

    def _exec(sql, params=None):
        if params:
            _cur.execute(sql, params)
        else:
            _cur.execute(sql)
        return _cur

    def _insert_returning(sql, params=None):
        """Insert and return the new row id."""
        if USE_RETURNING:
            cur = _exec(sql + ' RETURNING id', params)
            return cur.fetchone()[0]
        else:
            cur = _exec(sql, params)
            return cur.lastrowid

    def _fetchone(sql, params=None):
        return _exec(sql, params).fetchone()

    def _fetchall(sql, params=None):
        return _exec(sql, params).fetchall()

    def _setup():
        _exec(
            'CREATE TABLE IF NOT EXISTS "%s" '
            '(id %s, type TEXT NOT NULL)' % (t_value, SPK))
        _exec(
            'CREATE TABLE IF NOT EXISTS "%s" '
            '(id %s, value_id INTEGER REFERENCES "%s"(id), '
            'lo %s, str TEXT, hi %s)' % (t_number, SPK, t_value, FT, FT))
        _exec(
            'CREATE TABLE IF NOT EXISTS "%s" '
            '(id %s, value_id INTEGER REFERENCES "%s"(id), '
            'value TEXT)' % (t_string, SPK, t_value))
        _exec(
            'CREATE TABLE IF NOT EXISTS "%s" '
            '(id %s, value_id INTEGER REFERENCES "%s"(id), '
            'value %s)' % (t_blob, SPK, t_value, BT))
        _exec(
            'CREATE TABLE IF NOT EXISTS "%s" '
            '(id %s, value_id INTEGER REFERENCES "%s"(id))'
            % (t_array, SPK, t_value))
        _exec(
            'CREATE TABLE IF NOT EXISTS "%s" '
            '(id %s, array_id INTEGER REFERENCES "%s"(id), '
            'idx INTEGER, value_id INTEGER REFERENCES "%s"(id))'
            % (t_array_item, SPK, t_array, t_value))
        _exec(
            'CREATE TABLE IF NOT EXISTS "%s" '
            '(id %s, value_id INTEGER REFERENCES "%s"(id))'
            % (t_object, SPK, t_value))
        _exec(
            'CREATE TABLE IF NOT EXISTS "%s" '
            '(id %s, object_id INTEGER REFERENCES "%s"(id), '
            'key_id INTEGER REFERENCES "%s"(id), '
            'value_id INTEGER REFERENCES "%s"(id))'
            % (t_object_item, SPK, t_object, t_value, t_value))

        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_vid" ON "%s"(value_id)' % (t_number, t_number))
        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_lo" ON "%s"(lo)' % (t_number, t_number))
        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_hi" ON "%s"(hi)' % (t_number, t_number))
        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_vid" ON "%s"(value_id)' % (t_string, t_string))
        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_vid" ON "%s"(value_id)' % (t_blob, t_blob))
        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_vid" ON "%s"(value_id)' % (t_array, t_array))
        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_aid" ON "%s"(array_id)' % (t_array_item, t_array_item))
        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_vid" ON "%s"(value_id)' % (t_object, t_object))
        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_oid" ON "%s"(object_id)' % (t_object_item, t_object_item))
        _exec('CREATE INDEX IF NOT EXISTS "ix_%s_kid" ON "%s"(key_id)' % (t_object_item, t_object_item))
        conn.commit()

    def _store(value):
        type_str, raw = _classify_value(value)
        vid = _insert_returning(
            'INSERT INTO "%s" (type) VALUES (%s)' % (t_value, P),
            (type_str,))

        if type_str in ("null", "true", "false"):
            return vid

        if type_str == "number":
            fv = float(value)
            _exec(
                'INSERT INTO "%s" (value_id, lo, str, hi) VALUES (%s, %s, %s, %s)'
                % (t_number, P, P, P, P), (vid, fv, None, fv))
            return vid

        if type_str in ("bigint", "bigdec", "bigfloat"):
            lo, s, hi = _project_numeric(raw)
            _exec(
                'INSERT INTO "%s" (value_id, lo, str, hi) VALUES (%s, %s, %s, %s)'
                % (t_number, P, P, P, P), (vid, lo, s, hi))
            return vid

        if type_str == "unbound":
            _exec(
                'INSERT INTO "%s" (value_id, lo, str, hi) VALUES (%s, %s, %s, %s)'
                % (t_number, P, P, P, P), (vid, float('-inf'), "?" + raw, float('inf')))
            return vid

        if type_str == "string":
            _exec(
                'INSERT INTO "%s" (value_id, value) VALUES (%s, %s)'
                % (t_string, P, P), (vid, value))
            return vid

        if type_str == "blob":
            data = value.data if isinstance(value.data, bytes) else bytes(value.data)
            if dialect == 'postgres':
                import psycopg2
                data = psycopg2.Binary(data)
            _exec(
                'INSERT INTO "%s" (value_id, value) VALUES (%s, %s)'
                % (t_blob, P, P), (vid, data))
            return vid

        if type_str == "array":
            array_id = _insert_returning(
                'INSERT INTO "%s" (value_id) VALUES (%s)' % (t_array, P),
                (vid,))
            items = value if isinstance(value, list) else list(value)
            for i, item in enumerate(items):
                item_vid = _store(item)
                _exec(
                    'INSERT INTO "%s" (array_id, idx, value_id) VALUES (%s, %s, %s)'
                    % (t_array_item, P, P, P), (array_id, i, item_vid))
            return vid

        if type_str == "object":
            object_id = _insert_returning(
                'INSERT INTO "%s" (value_id) VALUES (%s)' % (t_object, P),
                (vid,))
            entries = value.items() if isinstance(value, (dict, QMap)) else value.items()
            for k, v in entries:
                key_vid = _store(k)
                item_vid = _store(v)
                _exec(
                    'INSERT INTO "%s" (object_id, key_id, value_id) VALUES (%s, %s, %s)'
                    % (t_object_item, P, P, P), (object_id, key_vid, item_vid))
            return vid

        return vid

    def _load(vid):
        row = _fetchone(
            'SELECT type FROM "%s" WHERE id = %s' % (t_value, P), (vid,))
        if not row:
            return None
        type_str = row[0]

        if type_str == "null":
            return None
        if type_str == "true":
            return True
        if type_str == "false":
            return False

        if type_str in ("number", "bigint", "bigdec", "bigfloat"):
            nr = _fetchone(
                'SELECT lo, str, hi FROM "%s" WHERE value_id = %s'
                % (t_number, P), (vid,))
            if not nr:
                return None
            lo, s, hi = nr
            if type_str == "number":
                return lo
            raw = s if s is not None else str(int(lo)) if lo == int(lo) else str(lo)
            if type_str == "bigint":
                return BigInt(int(raw))
            if type_str == "bigdec":
                return Decimal(raw)
            if type_str == "bigfloat":
                return BigFloat(raw)

        if type_str == "unbound":
            nr = _fetchone(
                'SELECT str FROM "%s" WHERE value_id = %s'
                % (t_number, P), (vid,))
            name = nr[0] if nr and nr[0] else "?"
            if name.startswith("?"):
                name = name[1:]
            return Unbound(name)

        if type_str == "string":
            sr = _fetchone(
                'SELECT value FROM "%s" WHERE value_id = %s'
                % (t_string, P), (vid,))
            return sr[0] if sr else ""

        if type_str == "blob":
            br = _fetchone(
                'SELECT value FROM "%s" WHERE value_id = %s'
                % (t_blob, P), (vid,))
            if not br:
                return Blob(b"")
            data = br[0]
            if isinstance(data, memoryview):
                data = bytes(data)
            return Blob(data)

        if type_str == "array":
            ar = _fetchone(
                'SELECT id FROM "%s" WHERE value_id = %s'
                % (t_array, P), (vid,))
            if not ar:
                return []
            items = _fetchall(
                'SELECT value_id FROM "%s" WHERE array_id = %s ORDER BY idx'
                % (t_array_item, P), (ar[0],))
            return [_load(item[0]) for item in items]

        if type_str == "object":
            ob = _fetchone(
                'SELECT id FROM "%s" WHERE value_id = %s'
                % (t_object, P), (vid,))
            if not ob:
                return {}
            items = _fetchall(
                'SELECT key_id, value_id FROM "%s" WHERE object_id = %s'
                % (t_object_item, P), (ob[0],))
            entries = []
            all_string_keys = True
            for item in items:
                k = _load(item[0])
                v = _load(item[1])
                if not isinstance(k, str):
                    all_string_keys = False
                entries.append((k, v))
            if all_string_keys:
                result = {}
                for k, v in entries:
                    result[k] = v
                return result
            return QMap(entries)

        return None

    def _remove(vid):
        row = _fetchone(
            'SELECT type FROM "%s" WHERE id = %s' % (t_value, P), (vid,))
        if not row:
            return
        type_str = row[0]

        if type_str in ("number", "bigint", "bigdec", "bigfloat", "unbound"):
            _exec('DELETE FROM "%s" WHERE value_id = %s' % (t_number, P), (vid,))
        elif type_str == "string":
            _exec('DELETE FROM "%s" WHERE value_id = %s' % (t_string, P), (vid,))
        elif type_str == "blob":
            _exec('DELETE FROM "%s" WHERE value_id = %s' % (t_blob, P), (vid,))
        elif type_str == "array":
            ar = _fetchone(
                'SELECT id FROM "%s" WHERE value_id = %s'
                % (t_array, P), (vid,))
            if ar:
                items = _fetchall(
                    'SELECT value_id FROM "%s" WHERE array_id = %s'
                    % (t_array_item, P), (ar[0],))
                child_vids = [item[0] for item in items]
                # Delete join rows first (FK constraints)
                _exec('DELETE FROM "%s" WHERE array_id = %s'
                      % (t_array_item, P), (ar[0],))
                _exec('DELETE FROM "%s" WHERE id = %s' % (t_array, P), (ar[0],))
                for cv in child_vids:
                    _remove(cv)
        elif type_str == "object":
            ob = _fetchone(
                'SELECT id FROM "%s" WHERE value_id = %s'
                % (t_object, P), (vid,))
            if ob:
                items = _fetchall(
                    'SELECT key_id, value_id FROM "%s" WHERE object_id = %s'
                    % (t_object_item, P), (ob[0],))
                child_vids = []
                for item in items:
                    child_vids.append(item[0])  # key_id
                    child_vids.append(item[1])  # value_id
                # Delete join rows first (FK constraints)
                _exec('DELETE FROM "%s" WHERE object_id = %s'
                      % (t_object_item, P), (ob[0],))
                _exec('DELETE FROM "%s" WHERE id = %s' % (t_object, P), (ob[0],))
                for cv in child_vids:
                    _remove(cv)

        _exec('DELETE FROM "%s" WHERE id = %s' % (t_value, P), (vid,))

    return {
        "setup": _setup,
        "store": _store,
        "load": _load,
        "remove": _remove,
        "commit": lambda: conn.commit(),
        "close": lambda: conn.close(),
    }
