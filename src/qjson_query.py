# ============================================================
# qjson_query.py — QJSON path expressions → SQL query compiler.
#
# Compiles jq-like path expressions and WHERE predicates into
# SQL JOIN chains against the normalized QJSON schema.
#
# Path syntax:
#   .key        object child by key
#   [n]         array index (literal integer)
#   [K]         array index (variable, uppercase = binding)
#   .[]         all array elements
#
# WHERE predicates:
#   path == value     equality (exact projection match)
#   path != value     inequality
#   path < value      ordering (interval pushdown + libbf fallback)
#   path <= value
#   path > value
#   path >= value
#   pred AND pred     conjunction
#   pred OR pred      disjunction
#
# Examples:
#   SELECT .items[K] WHERE .items[K].t > 30
#   SELECT .name WHERE .age >= 18M
#   UPDATE .items[K].y = 3 WHERE .items[K].t > 30
# ============================================================

import re
from qjson_sql import _project_numeric, _classify_value


# ── Path tokenizer ──────────────────────────────────────────

# Token types
_TOK_DOT_KEY = 'dot_key'       # .name
_TOK_INDEX = 'index'           # [3]
_TOK_VAR = 'var'               # [K]
_TOK_ITER = 'iter'             # []  or .[]

_PATH_RE = re.compile(r"""
    \.\[([A-Z][A-Za-z0-9_]*)\]  # .[Var]
  | \.\[\]                      # .[] iterate all
  | \.([a-zA-Z_][a-zA-Z0-9_]*)  # .key
  | \[([0-9]+)\]                # [n] index
  | \[([A-Z][A-Za-z0-9_]*)\]    # [K] variable
  | \[\]                        # [] iterate all
""", re.VERBOSE)


def parse_path(expr):
    """Parse a jq-like path expression into a list of steps.

    Each step is (type, value):
      ('dot_key', 'name')
      ('index', 3)
      ('var', 'K')
      ('iter', None)
    """
    steps = []
    pos = 0
    while pos < len(expr):
        m = _PATH_RE.match(expr, pos)
        if not m:
            raise ValueError("Invalid path at position %d: %r" % (pos, expr[pos:]))
        matched = m.group(0)
        if matched == '.[]' or matched == '[]':
            steps.append((_TOK_ITER, None))
        elif m.group(1) is not None:  # .[Var]
            steps.append((_TOK_VAR, m.group(1)))
        elif m.group(2) is not None:  # .key
            steps.append((_TOK_DOT_KEY, m.group(2)))
        elif m.group(3) is not None:  # [n]
            steps.append((_TOK_INDEX, int(m.group(3))))
        elif m.group(4) is not None:  # [K]
            steps.append((_TOK_VAR, m.group(4)))
        pos = m.end()
    return steps


# ── SQL query builder ────────────────────────────────────────

class _QueryBuilder:
    """Builds SQL JOIN chains from parsed path steps."""

    def __init__(self, prefix="qjson_", dialect="sqlite"):
        self.prefix = prefix
        self.dialect = dialect
        self.P = '?' if dialect == 'sqlite' else '%s'
        self._alias_counter = 0
        self._joins = []
        self._params = []
        self._var_aliases = {}  # variable name → alias for array_item
        self._cte = None

    def _alias(self, base):
        self._alias_counter += 1
        return "%s_%d" % (base, self._alias_counter)

    def _t(self, name):
        return '"%s%s"' % (self.prefix, name)

    def resolve_path(self, steps, root_vid_expr="root.id"):
        """Resolve a path from a root value, returning the final value_id expression.

        root_vid_expr: SQL expression for the starting value_id
        Returns: SQL expression for the resolved value_id
        """
        current_vid = root_vid_expr

        for step_type, step_val in steps:
            if step_type == _TOK_DOT_KEY:
                # JOIN object → object_item WHERE key = ?
                o = self._alias("o")
                oi = self._alias("oi")
                self._joins.append(
                    'JOIN %s %s ON %s.value_id = %s' % (
                        self._t("object"), o, o, current_vid))
                self._joins.append(
                    'JOIN %s %s ON %s.object_id = %s.id AND %s.key = %s' % (
                        self._t("object_item"), oi, oi, o, oi, self.P))
                self._params.append(step_val)
                current_vid = "%s.value_id" % oi

            elif step_type == _TOK_INDEX:
                # JOIN array → array_item WHERE idx = ?
                a = self._alias("a")
                ai = self._alias("ai")
                self._joins.append(
                    'JOIN %s %s ON %s.value_id = %s' % (
                        self._t("array"), a, a, current_vid))
                self._joins.append(
                    'JOIN %s %s ON %s.array_id = %s.id AND %s.idx = %s' % (
                        self._t("array_item"), ai, ai, a, ai, self.P))
                self._params.append(step_val)
                current_vid = "%s.value_id" % ai

            elif step_type == _TOK_VAR:
                # Variable binding: reuse alias if same variable
                if step_val in self._var_aliases:
                    ai = self._var_aliases[step_val]
                    current_vid = "%s.value_id" % ai
                else:
                    a = self._alias("a")
                    ai = self._alias("ai")
                    self._joins.append(
                        'JOIN %s %s ON %s.value_id = %s' % (
                            self._t("array"), a, a, current_vid))
                    self._joins.append(
                        'JOIN %s %s ON %s.array_id = %s.id' % (
                            self._t("array_item"), ai, ai, a))
                    self._var_aliases[step_val] = ai
                    current_vid = "%s.value_id" % ai

            elif step_type == _TOK_ITER:
                # Iterate all array elements
                a = self._alias("a")
                ai = self._alias("ai")
                self._joins.append(
                    'JOIN %s %s ON %s.value_id = %s' % (
                        self._t("array"), a, a, current_vid))
                self._joins.append(
                    'JOIN %s %s ON %s.array_id = %s.id' % (
                        self._t("array_item"), ai, ai, a))
                current_vid = "%s.value_id" % ai

        return current_vid


# ── Predicate compiler ───────────────────────────────────────

# Tokenize a WHERE expression
_WHERE_RE = re.compile(r"""
    (==|!=|<=|>=|<|>)           # comparison operators
  | \b(AND|OR|NOT)\b           # logical operators
  | (\(|\))                    # grouping
  | ((?:\.[a-zA-Z_]\w*|\[\w+\]|\.\[\]|\.\.)+)  # path expression
  | (-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?[NMLnml]?) # numeric literal
  | (true|false|null)          # boolean/null literals
  | ("(?:[^"\\]|\\.)*")        # string literal
  | \s+                        # whitespace (skip)
""", re.VERBOSE)


def _tokenize_where(expr):
    """Tokenize a WHERE clause into a list of (type, value) tokens."""
    tokens = []
    pos = 0
    while pos < len(expr):
        m = _WHERE_RE.match(expr, pos)
        if not m:
            raise ValueError("Invalid WHERE at position %d: %r" % (pos, expr[pos:]))
        if m.group(1):
            tokens.append(('op', m.group(1)))
        elif m.group(2):
            tokens.append(('logic', m.group(2)))
        elif m.group(3):
            tokens.append(('paren', m.group(3)))
        elif m.group(4):
            tokens.append(('path', m.group(4)))
        elif m.group(5):
            tokens.append(('number', m.group(5)))
        elif m.group(6):
            tokens.append(('literal', m.group(6)))
        elif m.group(7):
            tokens.append(('string', m.group(7)))
        pos = m.end()
    return tokens


def compile_where(where_expr, builder, root_vid_expr="root.id", has_ext=False):
    """Compile a WHERE expression into SQL WHERE clause + params.

    Returns (sql_fragment, params_list).
    """
    tokens = _tokenize_where(where_expr)
    sql_parts = []
    params = []

    i = 0
    while i < len(tokens):
        tok_type, tok_val = tokens[i]

        if tok_type == 'logic':
            sql_parts.append(tok_val)
            i += 1

        elif tok_type == 'paren':
            sql_parts.append(tok_val)
            i += 1

        elif tok_type == 'path':
            # Next should be an operator, then a value
            if i + 2 >= len(tokens):
                raise ValueError("Incomplete predicate at %r" % tok_val)
            op_type, op_val = tokens[i + 1]
            val_type, val_val = tokens[i + 2]
            if op_type != 'op':
                raise ValueError("Expected operator after path, got %r" % op_val)

            path_steps = parse_path(tok_val)
            vid_expr = builder.resolve_path(path_steps, root_vid_expr)

            # Project the comparison value
            cmp_sql, cmp_params = _compile_comparison(
                vid_expr, op_val, val_type, val_val, builder, has_ext)
            sql_parts.append(cmp_sql)
            params.extend(cmp_params)
            i += 3

        else:
            raise ValueError("Unexpected token: %r" % (tokens[i],))

    return ' '.join(sql_parts), params


def _compile_comparison(vid_expr, op, val_type, val_val, builder, has_ext):
    """Compile a single comparison: path <op> value → SQL."""
    P = builder.P
    t_number = builder._t("number")
    t_value = builder._t("value")

    if val_type == 'literal':
        # true, false, null — compare on type column
        v = builder._alias("v")
        builder._joins.append(
            'JOIN %s %s ON %s.id = %s' % (t_value, v, v, vid_expr))
        if op == '==':
            return "%s.type = %s" % (v, P), [val_val]
        elif op == '!=':
            return "%s.type != %s" % (v, P), [val_val]
        else:
            raise ValueError("Cannot use %s with %s" % (op, val_val))

    if val_type == 'string':
        # String comparison on string_value table
        t_string = builder._t("string")
        sv = builder._alias("sv")
        builder._joins.append(
            'JOIN %s %s ON %s.value_id = %s' % (t_string, sv, sv, vid_expr))
        if op == '==':
            return "%s.value = %s" % (sv, P), [val_val[1:-1]]  # strip quotes
        elif op == '!=':
            return "%s.value != %s" % (sv, P), [val_val[1:-1]]
        else:
            raise ValueError("Cannot use %s with strings" % op)

    if val_type == 'number':
        # Numeric comparison with interval pushdown
        raw = val_val
        # Strip suffix for projection
        suffix = ''
        if raw and raw[-1] in 'NMLnml':
            suffix = raw[-1]
            raw_num = raw[:-1]
        else:
            raw_num = raw

        q_lo = float(raw_num)
        q_hi = float(raw_num)
        q_str = None

        # Check if it needs interval projection
        from qjson_sql import round_down, round_up
        q_lo = round_down(raw_num)
        q_hi = round_up(raw_num)
        if q_lo != q_hi:
            q_str = raw_num

        # Join to number table
        n = builder._alias("n")
        builder._joins.append(
            'JOIN %s %s ON %s.value_id = %s' % (t_number, n, n, vid_expr))

        return _interval_comparison(n, op, q_lo, q_hi, q_str, P, has_ext)

    raise ValueError("Unknown value type: %r" % val_type)


def _interval_comparison(n_alias, op, q_lo, q_hi, q_str, P, has_ext):
    """Generate interval-pushdown SQL for a numeric comparison.

    Uses 3-tier comparison:
      1. [brackets] on lo/hi (indexed)
      2. {braces} when both exact
      3. qjson_decimal_cmp() for overlap zone (if extension loaded)
    """
    n = n_alias

    if op == '==':
        # Equality: all three columns must match
        sql = ("(%s.lo = %s AND %s.hi = %s AND "
               "((%s.str IS NULL AND %s IS NULL) OR %s.str = %s))"
               % (n, P, n, P, n, P, n, P))
        return sql, [q_lo, q_hi, q_str, q_str]

    if op == '!=':
        sql = ("NOT (%s.lo = %s AND %s.hi = %s AND "
               "((%s.str IS NULL AND %s IS NULL) OR %s.str = %s))"
               % (n, P, n, P, n, P, n, P))
        return sql, [q_lo, q_hi, q_str, q_str]

    # Ordering comparisons: interval pushdown
    if has_ext:
        # Full 3-tier with libbf fallback
        if op == '>':
            sql = ("(%s.hi > %s AND (%s.lo > %s OR "
                   "qjson_cmp(%s.lo, %s.hi, %s.str, %s, %s, %s) > 0))"
                   % (n, P, n, P, n, n, n, P, P, P))
            return sql, [q_lo, q_hi, q_lo, q_hi, q_str]
        if op == '>=':
            sql = ("(%s.hi >= %s AND (%s.lo >= %s OR "
                   "qjson_cmp(%s.lo, %s.hi, %s.str, %s, %s, %s) >= 0))"
                   % (n, P, n, P, n, n, n, P, P, P))
            return sql, [q_lo, q_hi, q_lo, q_hi, q_str]
        if op == '<':
            sql = ("(%s.lo < %s AND (%s.hi < %s OR "
                   "qjson_cmp(%s.lo, %s.hi, %s.str, %s, %s, %s) < 0))"
                   % (n, P, n, P, n, n, n, P, P, P))
            return sql, [q_hi, q_lo, q_lo, q_hi, q_str]
        if op == '<=':
            sql = ("(%s.lo <= %s AND (%s.hi <= %s OR "
                   "qjson_cmp(%s.lo, %s.hi, %s.str, %s, %s, %s) <= 0))"
                   % (n, P, n, P, n, n, n, P, P, P))
            return sql, [q_hi, q_lo, q_lo, q_hi, q_str]
    else:
        # 2-tier without extension (no exact fallback)
        if op == '>':
            sql = ("(%s.hi > %s AND (%s.lo > %s"
                   " OR (%s.str IS NOT NULL AND %s IS NOT NULL)))"
                   % (n, P, n, P, n, P))
            return sql, [q_lo, q_hi, q_str]
        if op == '>=':
            sql = ("(%s.hi >= %s AND (%s.lo >= %s"
                   " OR (%s.str IS NOT NULL AND %s IS NOT NULL)))"
                   % (n, P, n, P, n, P))
            return sql, [q_lo, q_hi, q_str]
        if op == '<':
            sql = ("(%s.lo < %s AND (%s.hi < %s"
                   " OR (%s.str IS NOT NULL AND %s IS NOT NULL)))"
                   % (n, P, n, P, n, P))
            return sql, [q_hi, q_lo, q_str]
        if op == '<=':
            sql = ("(%s.lo <= %s AND (%s.hi <= %s"
                   " OR (%s.str IS NOT NULL AND %s IS NOT NULL)))"
                   % (n, P, n, P, n, P))
            return sql, [q_hi, q_lo, q_str]

    raise ValueError("Unknown operator: %s" % op)


# ── High-level query API ─────────────────────────────────────

def qjson_select(conn, root_id, select_path, where_expr=None,
                 prefix="qjson_", dialect=None, has_ext=False):
    """SELECT values by path with optional WHERE filtering.

    conn        — database connection
    root_id     — value_id of the root value
    select_path — jq path expression for the result (e.g. ".items[K]")
    where_expr  — optional WHERE predicate (e.g. ".items[K].t > 30")
    prefix      — table prefix
    dialect     — 'sqlite' or 'postgres'
    has_ext     — True if qjson_ext is loaded (enables exact comparison)

    Returns list of (value_id, bindings_dict) tuples.
    """
    if dialect is None:
        dialect = 'sqlite'
    P = '?' if dialect == 'sqlite' else '%s'

    builder = _QueryBuilder(prefix=prefix, dialect=dialect)
    t_value = builder._t("value")

    # Resolve SELECT path
    select_steps = parse_path(select_path)
    select_vid = builder.resolve_path(select_steps, "root.id")

    # Build variable select columns
    var_selects = []
    for var_name, ai_alias in builder._var_aliases.items():
        var_selects.append("%s.idx AS %s" % (ai_alias, var_name))

    # Compile WHERE
    where_sql = ""
    where_params = []
    if where_expr:
        where_sql, where_params = compile_where(
            where_expr, builder, "root.id", has_ext)

    # Assemble query
    select_cols = ["%s AS result_vid" % select_vid]
    select_cols.extend(var_selects)

    sql = "SELECT %s\nFROM %s root\n%s" % (
        ", ".join(select_cols),
        t_value,
        "\n".join(builder._joins))

    # Params order must match SQL: JOIN params first, then WHERE
    all_params = list(builder._params)
    sql += "\nWHERE root.id = %s" % P
    all_params.append(root_id)

    if where_sql:
        sql += "\n  AND (%s)" % where_sql
        all_params.extend(where_params)

    # Execute
    if dialect == 'postgres':
        cur = conn.cursor()
        cur.execute(sql, tuple(all_params))
        rows = cur.fetchall()
    else:
        rows = conn.execute(sql, tuple(all_params)).fetchall()

    # Parse results
    results = []
    n_vars = len(var_selects)
    for row in rows:
        vid = row[0]
        bindings = {}
        for i, (var_name, _) in enumerate(builder._var_aliases.items()):
            bindings[var_name] = row[1 + i]
        results.append((vid, bindings))

    return results


def qjson_update(conn, root_id, update_path, value, where_expr=None,
                 prefix="qjson_", dialect=None, has_ext=False):
    """UPDATE values by path with optional WHERE filtering.

    conn        — database connection
    root_id     — value_id of the root value
    update_path — jq path to the target value (e.g. ".items[K].y")
    value       — new value (Python native: int, float, str, Decimal, etc.)
    where_expr  — optional WHERE predicate (e.g. ".items[K].t > 30")
    prefix      — table prefix
    dialect     — 'sqlite' or 'postgres'
    has_ext     — True if qjson_ext is loaded

    Returns number of values updated.
    """
    from qjson_sql import (qjson_sql_adapter, _classify_value,
                           _project_numeric, round_down, round_up)

    if dialect is None:
        dialect = 'sqlite'
    P = '?' if dialect == 'sqlite' else '%s'

    # Find all target value_ids via SELECT
    targets = qjson_select(conn, root_id, update_path, where_expr,
                           prefix=prefix, dialect=dialect, has_ext=has_ext)

    if not targets:
        return 0

    # Classify the new value
    new_type, new_raw = _classify_value(value)

    t_value = '"%s%s"' % (prefix, "value")
    t_number = '"%s%s"' % (prefix, "number")
    t_string = '"%s%s"' % (prefix, "string")
    t_blob = '"%s%s"' % (prefix, "blob")

    def _exec(sql, params):
        if dialect == 'postgres':
            cur = conn.cursor()
            cur.execute(sql, params)
        else:
            conn.execute(sql, params)

    def _fetchone(sql, params):
        if dialect == 'postgres':
            cur = conn.cursor()
            cur.execute(sql, params)
            return cur.fetchone()
        else:
            return conn.execute(sql, params).fetchone()

    count = 0
    for target_vid, _bindings in targets:
        # Get current type of target
        row = _fetchone('SELECT type FROM %s WHERE id = %s' % (t_value, P),
                        (target_vid,))
        if not row:
            continue
        old_type = row[0]

        # Delete old child row
        if old_type in ('number', 'bigint', 'bigdec', 'bigfloat'):
            _exec('DELETE FROM %s WHERE value_id = %s' % (t_number, P),
                  (target_vid,))
        elif old_type == 'string':
            _exec('DELETE FROM %s WHERE value_id = %s' % (t_string, P),
                  (target_vid,))
        elif old_type == 'blob':
            _exec('DELETE FROM %s WHERE value_id = %s' % (t_blob, P),
                  (target_vid,))
        # Note: updating to/from array/object not supported (structural change)

        # Update type
        _exec('UPDATE %s SET type = %s WHERE id = %s' % (t_value, P, P),
              (new_type, target_vid))

        # Insert new child row
        if new_type == 'number':
            fv = float(value)
            _exec('INSERT INTO %s (value_id, lo, str, hi) VALUES (%s, %s, %s, %s)'
                  % (t_number, P, P, P, P), (target_vid, fv, None, fv))
        elif new_type in ('bigint', 'bigdec', 'bigfloat'):
            lo, s, hi = _project_numeric(new_raw)
            _exec('INSERT INTO %s (value_id, lo, str, hi) VALUES (%s, %s, %s, %s)'
                  % (t_number, P, P, P, P), (target_vid, lo, s, hi))
        elif new_type == 'string':
            _exec('INSERT INTO %s (value_id, value) VALUES (%s, %s)'
                  % (t_string, P, P), (target_vid, value))
        elif new_type == 'blob':
            from qjson import Blob
            data = value.data if isinstance(value.data, bytes) else bytes(value.data)
            if dialect == 'postgres':
                import psycopg2
                data = psycopg2.Binary(data)
            _exec('INSERT INTO %s (value_id, value) VALUES (%s, %s)'
                  % (t_blob, P, P), (target_vid, data))
        # null, true, false: type column is the full value, no child row needed

        count += 1

    return count
