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


# ── Predicate compiler with arithmetic expressions ───────────
#
# Grammar:
#   predicate  = expr ('=='|'!='|'<'|'<='|'>'|'>=') expr
#   predicates = predicate (('AND'|'OR') predicate)* | 'NOT' predicate | '(' predicates ')'
#   expr       = term (('+' | '-') term)*
#   term       = factor (('*' | '/') factor)*
#   factor     = atom ('**' atom)?
#   atom       = path | number | func '(' args ')' | '(' expr ')'

_TOKEN_RE = re.compile(r"""
    (==|!=|<=|>=|<|>)                              # comparison
  | (\+|-(?!\d))                                   # add/sub (minus not before digit)
  | (\*\*|\*|/)                                    # power(**) or mul/div
  | \b(AND|OR|NOT)\b                               # logic
  | \b(POWER|SQRT|EXP|LOG|SIN|COS|TAN|ATAN|ASIN|ACOS|PI)\b  # functions
  | (\(|\))                                        # parens
  | (,)                                            # comma (function args)
  | ((?:\.[a-zA-Z_]\w*|\[\w+\]|\.\[\])+)          # path
  | (-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?[NMLnml]?)   # number
  | (true|false|null)                              # literal
  | ("(?:[^"\\]|\\.)*")                            # string
  | \s+                                            # whitespace
""", re.VERBOSE)


def _tokenize(expr):
    tokens = []
    pos = 0
    while pos < len(expr):
        m = _TOKEN_RE.match(expr, pos)
        if not m:
            raise ValueError("Invalid at position %d: %r" % (pos, expr[pos:]))
        if m.group(1): tokens.append(('cmp', m.group(1)))
        elif m.group(2): tokens.append(('addsub', m.group(2)))
        elif m.group(3):
            if m.group(3) == '**': tokens.append(('pow', '**'))
            else: tokens.append(('muldiv', m.group(3)))
        elif m.group(4): tokens.append(('logic', m.group(4)))
        elif m.group(5): tokens.append(('func', m.group(5)))
        elif m.group(6): tokens.append(('paren', m.group(6)))
        elif m.group(7): tokens.append(('comma', ','))
        elif m.group(8): tokens.append(('path', m.group(8)))
        elif m.group(9): tokens.append(('number', m.group(9)))
        elif m.group(10): tokens.append(('literal', m.group(10)))
        elif m.group(11): tokens.append(('string', m.group(11)))
        pos = m.end()
    return tokens


class _ExprCompiler:
    """Recursive-descent compiler: expressions → SQL with qjson_* arithmetic."""

    def __init__(self, tokens, builder, root_vid_expr, has_ext):
        self.tokens = tokens
        self.pos = 0
        self.builder = builder
        self.root_vid = root_vid_expr
        self.has_ext = has_ext
        self.params = []

    def peek(self):
        if self.pos < len(self.tokens):
            return self.tokens[self.pos]
        return (None, None)

    def eat(self, *types):
        t, v = self.peek()
        if t in types:
            self.pos += 1
            return (t, v)
        return None

    # ── Arithmetic expressions → SQL ────────────────────────
    # Each returns a SQL expression string that evaluates to a TEXT decimal.
    # Paths resolve to their str column (exact value) from the number table.
    # Arithmetic wraps in qjson_add/sub/mul/div/pow calls.

    def expr(self):
        """expr = term (('+' | '-') term)*"""
        left = self.term()
        while True:
            tok = self.eat('addsub')
            if not tok:
                break
            right = self.term()
            fn = 'qjson_add' if tok[1] == '+' else 'qjson_sub'
            left = "%s(%s, %s)" % (fn, left, right)
        return left

    def term(self):
        """term = factor (('*' | '/') factor)*"""
        left = self.factor()
        while True:
            tok = self.eat('muldiv')
            if not tok:
                break
            right = self.factor()
            fn = 'qjson_mul' if tok[1] == '*' else 'qjson_div'
            left = "%s(%s, %s)" % (fn, left, right)
        return left

    def factor(self):
        """factor = atom ('**' atom)?"""
        left = self.atom()
        tok = self.eat('pow')
        if tok:
            right = self.atom()
            left = "qjson_pow(%s, %s)" % (left, right)
        return left

    def atom(self):
        """atom = path | number | func(args) | '(' expr ')'"""
        t, v = self.peek()

        # Function call: POWER(a,b), SQRT(a), etc.
        if t == 'func':
            self.pos += 1
            fn_name = v.upper()
            if not self.eat('paren'):  # opening (
                raise ValueError("Expected '(' after %s" % fn_name)
            _fn_map = {
                'POWER': ('qjson_pow', 2), 'SQRT': ('qjson_sqrt', 1),
                'EXP': ('qjson_exp', 1), 'LOG': ('qjson_log', 1),
                'SIN': ('qjson_sin', 1), 'COS': ('qjson_cos', 1),
                'TAN': ('qjson_tan', 1), 'ATAN': ('qjson_atan', 1),
                'ASIN': ('qjson_asin', 1), 'ACOS': ('qjson_acos', 1),
                'PI': ('qjson_pi', 0),
            }
            sql_fn, nargs = _fn_map.get(fn_name, (None, 0))
            if not sql_fn:
                raise ValueError("Unknown function: %s" % fn_name)
            args = []
            if nargs > 0:
                args.append(self.expr())
                while self.eat('comma'):
                    args.append(self.expr())
            if not self.eat('paren'):  # closing )
                raise ValueError("Expected ')' after %s arguments" % fn_name)
            return "%s(%s)" % (sql_fn, ', '.join(args))

        # Parenthesized expression
        if t == 'paren' and v == '(':
            self.pos += 1
            result = self.expr()
            if not self.eat('paren'):
                raise ValueError("Expected ')'")
            return result

        if t == 'number':
            self.pos += 1
            raw = v
            if raw and raw[-1] in 'NMLnml':
                raw = raw[:-1]
            return "'%s'" % raw

        if t == 'path':
            self.pos += 1
            return self._resolve_path_to_str(v)

        raise ValueError("Expected value, got %r" % (self.peek(),))

    def _resolve_path_to_str(self, path_expr):
        """Resolve a path to a SQL expression for the exact decimal value.
        Uses COALESCE(n.str, CAST(n.lo AS TEXT)) to get the exact string."""
        P = self.builder.P
        t_number = self.builder._t("number")
        path_steps = parse_path(path_expr)
        vid_expr = self.builder.resolve_path(path_steps, self.root_vid)
        n = self.builder._alias("ne")
        self.builder._joins.append(
            'JOIN %s %s ON %s.value_id = %s' % (t_number, n, n, vid_expr))
        return "COALESCE(%s.str, CAST(%s.lo AS TEXT))" % (n, n)

    # ── Predicate: expr cmp expr ────────────────────────────

    def comparison(self):
        """predicate = expr cmp expr"""
        left_sql = self.expr()
        tok = self.eat('cmp')
        if not tok:
            raise ValueError("Expected comparison operator, got %r" % (self.peek(),))
        op = tok[1]
        right_sql = self.expr()

        # All comparisons use qjson_decimal_cmp for exact decimal semantics
        # (text comparison fails: '7' != '7.0')
        if op == '==':
            return "qjson_decimal_cmp(%s, %s) = 0" % (left_sql, right_sql)
        elif op == '!=':
            return "qjson_decimal_cmp(%s, %s) != 0" % (left_sql, right_sql)
        elif op == '>':
            return "qjson_decimal_cmp(%s, %s) > 0" % (left_sql, right_sql)
        elif op == '>=':
            return "qjson_decimal_cmp(%s, %s) >= 0" % (left_sql, right_sql)
        elif op == '<':
            return "qjson_decimal_cmp(%s, %s) < 0" % (left_sql, right_sql)
        elif op == '<=':
            return "qjson_decimal_cmp(%s, %s) <= 0" % (left_sql, right_sql)

    def predicates(self):
        """predicates = comparison (logic comparison)* | NOT predicates | ( predicates )"""
        parts = []
        while self.pos < len(self.tokens):
            t, v = self.peek()
            if t == 'logic' and v == 'NOT':
                self.pos += 1
                parts.append("NOT (%s)" % self.predicates())
            elif t == 'paren' and v == '(':
                # Could be arithmetic grouping or predicate grouping.
                # Look ahead: if this contains a comparison op, it's a predicate group.
                # Otherwise it's the start of an expression (handled by comparison→expr).
                # Try as predicate group first.
                save = self.pos
                self.pos += 1
                try:
                    inner = self.predicates()
                    if self.eat('paren'):  # closing )
                        parts.append("(%s)" % inner)
                    else:
                        raise ValueError("backtrack")
                except:
                    self.pos = save
                    parts.append(self.comparison())
            elif t in ('path', 'number', 'paren'):
                parts.append(self.comparison())
            else:
                break

            # Check for AND/OR
            t2, v2 = self.peek()
            if t2 == 'logic' and v2 in ('AND', 'OR'):
                self.pos += 1
                parts.append(v2)
            else:
                break

        return ' '.join(parts)


def compile_where(where_expr, builder, root_vid_expr="root.id", has_ext=False):
    """Compile a WHERE expression into SQL WHERE clause + params.

    Supports arithmetic in expressions:
      .future == .present * (1 + .rate) ^ .periods
    """
    tokens = _tokenize(where_expr)
    compiler = _ExprCompiler(tokens, builder, root_vid_expr, has_ext)
    sql = compiler.predicates()
    return sql, compiler.params


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

        # Join to number table + value table (for type)
        n = builder._alias("n")
        builder._joins.append(
            'JOIN %s %s ON %s.value_id = %s' % (t_number, n, n, vid_expr))
        vt = builder._alias("vt")
        builder._joins.append(
            'JOIN %s %s ON %s.id = %s' % (t_value, vt, vt, vid_expr))

        # Query literal type
        q_type = 'number'
        if suffix in ('N', 'n'):
            q_type = 'bigint'
        elif suffix in ('M', 'm'):
            q_type = 'bigdec'
        elif suffix in ('L', 'l'):
            q_type = 'bigfloat'

        return _interval_comparison(n, vt, op, q_lo, q_hi, q_str, q_type, P, has_ext)

    raise ValueError("Unknown value type: %r" % val_type)


def _interval_comparison(n_alias, vt_alias, op, q_lo, q_hi, q_str, q_type, P, has_ext):
    """Generate interval-pushdown SQL for a numeric comparison.

    n_alias  — alias for the number table
    vt_alias — alias for the value table (provides stored type)
    q_type   — query literal's type string ('number', 'bigdec', etc.)
    """
    n = n_alias

    if op == '==':
        sql = ("(%s.lo = %s AND %s.hi = %s AND "
               "((%s.str IS NULL AND %s IS NULL) OR %s.str = %s))"
               % (n, P, n, P, n, P, n, P))
        return sql, [q_lo, q_hi, q_str, q_str]

    if op == '!=':
        sql = ("NOT (%s.lo = %s AND %s.hi = %s AND "
               "((%s.str IS NULL AND %s IS NULL) OR %s.str = %s))"
               % (n, P, n, P, n, P, n, P))
        return sql, [q_lo, q_hi, q_str, q_str]

    # Ordering: bracket pre-filter + qjson_cmp_[op] exact fallback
    op_map = {'>': 'qjson_cmp_gt', '>=': 'qjson_cmp_ge',
              '<': 'qjson_cmp_lt', '<=': 'qjson_cmp_le'}
    fn_name = op_map.get(op)
    if not fn_name:
        raise ValueError("Unknown operator: %s" % op)

    if has_ext:
        if op in ('>', '>='):
            bracket = "%s.hi %s %s" % (n, op, P)
            bracket_params = [q_lo]
        else:
            bracket = "%s.lo %s %s" % (n, op, P)
            bracket_params = [q_hi]

        # qjson_cmp_XX(a_type, a_lo, a_str, a_hi, b_type, b_lo, b_str, b_hi) → 0/1
        sql = ("(%s AND %s(%s.type, %s.lo, %s.str, %s.hi, %s, %s, %s, %s) = 1)"
               % (bracket, fn_name, vt_alias, n, n, n, P, P, P, P))
        return sql, bracket_params + [q_type, q_lo, q_str, q_hi]
    else:
        # 2-tier without extension
        if op in ('>', '>='):
            sql = ("(%s.hi %s %s AND (%s.lo %s %s"
                   " OR (%s.str IS NOT NULL AND %s IS NOT NULL)))"
                   % (n, op, P, n, op, P, n, P))
            return sql, [q_lo, q_hi, q_str]
        else:
            sql = ("(%s.lo %s %s AND (%s.hi %s %s"
                   " OR (%s.str IS NOT NULL AND %s IS NOT NULL)))"
                   % (n, op, P, n, op, P, n, P))
            return sql, [q_hi, q_lo, q_str]


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
