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
                # JOIN object → object_item with subquery key lookup
                o = self._alias("o")
                oi = self._alias("oi")
                self._joins.append(
                    'JOIN %s %s ON %s.value_id = %s' % (
                        self._t("object"), o, o, current_vid))
                self._joins.append(
                    'JOIN %s %s ON %s.object_id = %s.id'
                    ' AND %s.key_id IN (SELECT value_id FROM %s WHERE value = %s)' % (
                        self._t("object_item"), oi, oi, o,
                        oi, self._t("string"), self.P))
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
                    k = self._var_aliases[step_val]
                    current_vid = "%s.dst_vid" % k
                else:
                    # UNION handles both arrays (element value_id)
                    # and objects/sets (key_id)
                    k = self._alias("k")
                    self._joins.append(
                        'JOIN ('
                        'SELECT a.value_id AS src_vid, ai.value_id AS dst_vid'
                        ' FROM %s a JOIN %s ai ON ai.array_id = a.id'
                        ' UNION ALL'
                        ' SELECT o.value_id AS src_vid, oi.key_id AS dst_vid'
                        ' FROM %s o JOIN %s oi ON oi.object_id = o.id'
                        ') %s ON %s.src_vid = %s' % (
                            self._t("array"), self._t("array_item"),
                            self._t("object"), self._t("object_item"),
                            k, k, current_vid))
                    self._var_aliases[step_val] = k
                    current_vid = "%s.dst_vid" % k

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


# ── AST for expression trees ─────────────────────────────────

class _Lit:
    """Literal number."""
    __slots__ = ('val',)
    def __init__(self, val): self.val = val
    def to_sql(self, b): return "'%s'" % self.val
    def find_unbound(self, checker): return None

class _Path:
    """Path reference to a stored value."""
    __slots__ = ('path',)
    def __init__(self, path): self.path = path
    def to_sql(self, b):
        steps = parse_path(self.path)
        vid = b.resolve_path(steps, "root.id")
        n = b._alias("ne")
        b._joins.append('JOIN %s %s ON %s.value_id = %s' % (b._t("number"), n, n, vid))
        return "COALESCE(%s.str, CAST(%s.lo AS TEXT))" % (n, n)
    def find_unbound(self, checker):
        if checker(self.path):
            return self
        return None

class _BinOp:
    """Binary operation: left OP right."""
    __slots__ = ('op', 'left', 'right', 'fn')
    _FN = {'+': 'qjson_add', '-': 'qjson_sub', '*': 'qjson_mul',
           '/': 'qjson_div', '**': 'qjson_pow'}
    def __init__(self, op, left, right):
        self.op = op; self.left = left; self.right = right
        self.fn = self._FN[op]
    def to_sql(self, b):
        return "%s(%s, %s)" % (self.fn, self.left.to_sql(b), self.right.to_sql(b))
    def find_unbound(self, checker):
        return self.left.find_unbound(checker) or self.right.find_unbound(checker)

class _Func:
    """Function call: fn(args)."""
    __slots__ = ('name', 'args', 'sql_fn')
    _FN = {'POWER': 'qjson_pow', 'SQRT': 'qjson_sqrt', 'EXP': 'qjson_exp',
           'LOG': 'qjson_log', 'SIN': 'qjson_sin', 'COS': 'qjson_cos',
           'TAN': 'qjson_tan', 'ATAN': 'qjson_atan', 'ASIN': 'qjson_asin',
           'ACOS': 'qjson_acos', 'PI': 'qjson_pi'}
    def __init__(self, name, args):
        self.name = name; self.args = args; self.sql_fn = self._FN[name]
    def to_sql(self, b):
        return "%s(%s)" % (self.sql_fn, ', '.join(a.to_sql(b) for a in self.args))
    def find_unbound(self, checker):
        for a in self.args:
            r = a.find_unbound(checker)
            if r: return r
        return None


def _decompose_to_constraints(lhs_node, rhs_node, conn, root_id, prefix, dialect):
    """Decompose LHS == RHS into 3-term qjson_solve_* constraints.

    Walks the expression tree, creates anonymous unbound value_ids for
    intermediate nodes, and returns a list of (sql, params) constraints
    ready for leaf-folding propagation.
    """
    from qjson_sql import qjson_sql_adapter
    from qjson import Unbound
    from qjson_query import qjson_select

    P = '?' if dialect == 'sqlite' else '%s'

    def _exec(sql, params=None):
        if dialect == 'postgres':
            cur = conn.cursor()
            cur.execute(sql, params or ())
            return cur
        return conn.execute(sql, params or ())

    # Get adapter for storing temporaries
    adapter = qjson_sql_adapter.__wrapped__ if hasattr(qjson_sql_adapter, '__wrapped__') else None

    def _store_temp():
        """Create an anonymous unbound temporary value."""
        _exec('INSERT INTO "%svalue" (type) VALUES (%s)' % (prefix, P), ('unbound',))
        if dialect == 'postgres':
            cur = conn.cursor()
            cur.execute("SELECT lastval()")
            vid = cur.fetchone()[0]
        else:
            vid = conn.execute("SELECT last_insert_rowid()").fetchone()[0]
        _exec('INSERT INTO "%snumber" (value_id, lo, str, hi) VALUES (%s,%s,%s,%s)' % (
            prefix, P, P, P, P), (vid, float('-inf'), '?', float('inf')))
        return vid

    def _resolve_path_vid(path_expr):
        """Get the value_id for a path."""
        results = qjson_select(conn, root_id, path_expr, prefix=prefix, dialect=dialect, has_ext=True)
        return results[0][0] if results else None

    def _node_to_vid(node):
        """Convert an AST node to a value_id. Literals get stored, paths get resolved,
        compound nodes get decomposed into constraints + temporary vid."""
        if isinstance(node, _Lit):
            # Store as a number value
            from qjson_sql import round_down, round_up
            lo = round_down(node.val)
            hi = round_up(node.val)
            s = None if lo == hi else node.val
            _exec('INSERT INTO "%svalue" (type) VALUES (%s)' % (prefix, P), ('number',))
            vid = conn.execute("SELECT last_insert_rowid()").fetchone()[0] if dialect == 'sqlite' else None
            if dialect == 'postgres':
                cur = conn.cursor(); cur.execute("SELECT lastval()"); vid = cur.fetchone()[0]
            _exec('INSERT INTO "%snumber" (value_id, lo, str, hi) VALUES (%s,%s,%s,%s)' % (
                prefix, P, P, P, P), (vid, lo, s, hi))
            return vid

        if isinstance(node, _Path):
            return _resolve_path_vid(node.path)

        if isinstance(node, _BinOp):
            left_vid = _node_to_vid(node.left)
            right_vid = _node_to_vid(node.right)
            result_vid = _store_temp()
            op_map = {'+': 'add', '-': 'sub', '*': 'mul', '/': 'div', '**': 'pow'}
            fn = 'qjson_solve_' + op_map[node.op]
            constraints.append(("SELECT %s(%s, %s, %s)" % (fn, P, P, P),
                               (left_vid, right_vid, result_vid)))
            return result_vid

        if isinstance(node, _Func):
            if node.name == 'POWER' and len(node.args) == 2:
                base_vid = _node_to_vid(node.args[0])
                exp_vid = _node_to_vid(node.args[1])
                result_vid = _store_temp()
                constraints.append(("SELECT qjson_solve_pow(%s, %s, %s)" % (P, P, P),
                                   (base_vid, exp_vid, result_vid)))
                return result_vid
            # For single-arg functions, use the 2-arg solve form:
            # qjson_solve_exp(a, b) means exp(a) = b
            # These aren't implemented yet as solve functions, so fall back
            # to evaluating directly (forward only for now)
            arg_vid = _node_to_vid(node.args[0]) if node.args else None
            result_vid = _store_temp()
            # TODO: qjson_solve_sin, qjson_solve_exp, etc.
            # For now, store as a forward-only computation constraint
            constraints.append(("SELECT qjson_solve_pow(%s, '1', %s)" % (P, P),
                               (arg_vid, result_vid)))  # placeholder
            return result_vid

        raise ValueError("Cannot decompose: %r" % node)

    constraints = []
    lhs_vid = _node_to_vid(lhs_node)
    rhs_vid = _node_to_vid(rhs_node)

    # The top-level constraint: LHS == RHS (as an add with 0: lhs + 0 = rhs? No...)
    # Actually: qjson_solve_add(lhs, '0', rhs) means lhs + 0 = rhs, i.e. lhs = rhs
    # Or simpler: just record that lhs_vid and rhs_vid must be equal.
    # Use solve_add(lhs, literal_0, rhs) as identity constraint.
    _exec('INSERT INTO "%svalue" (type) VALUES (%s)' % (prefix, P), ('number',))
    zero_vid = conn.execute("SELECT last_insert_rowid()").fetchone()[0] if dialect == 'sqlite' else None
    if dialect == 'postgres':
        cur = conn.cursor(); cur.execute("SELECT lastval()"); zero_vid = cur.fetchone()[0]
    _exec('INSERT INTO "%snumber" (value_id, lo, str, hi) VALUES (%s,%s,%s,%s)' % (
        prefix, P, P, P, P), (zero_vid, 0.0, None, 0.0))
    constraints.append(("SELECT qjson_solve_add(%s, %s, %s)" % (P, P, P),
                        (lhs_vid, zero_vid, rhs_vid)))

    return constraints


class _ExprResult:
    """Tagged expression result from the SQL builder.

    kind:
      'numeric'        — SQL expression evaluating to a TEXT decimal string
      'path'           — resolved path with type info for cross-comparison
      'string_literal' — a quoted string literal (e.g. '"hello"')
      'type_literal'   — true, false, or null
    """
    __slots__ = ('kind', 'sql', 'vid_expr', 'type_expr',
                 'num_str_expr', 'str_expr', 'n_alias', 'v_alias')

    def __init__(self, kind, sql, vid_expr=None, type_expr=None,
                 num_str_expr=None, str_expr=None, n_alias=None, v_alias=None):
        self.kind = kind
        self.sql = sql
        self.vid_expr = vid_expr
        self.type_expr = type_expr
        self.num_str_expr = num_str_expr
        self.str_expr = str_expr
        self.n_alias = n_alias
        self.v_alias = v_alias


class _ExprCompiler:
    """Recursive-descent compiler: expressions → AST and SQL."""

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

    # ── AST builders (return _Lit/_Path/_BinOp/_Func nodes) ──

    def ast_expr(self):
        left = self.ast_term()
        while True:
            tok = self.eat('addsub')
            if not tok: break
            left = _BinOp(tok[1], left, self.ast_term())
        return left

    def ast_term(self):
        left = self.ast_factor()
        while True:
            tok = self.eat('muldiv')
            if not tok: break
            left = _BinOp(tok[1], left, self.ast_factor())
        return left

    def ast_factor(self):
        left = self.ast_atom()
        tok = self.eat('pow')
        if tok:
            left = _BinOp('**', left, self.ast_atom())
        return left

    def ast_atom(self):
        t, v = self.peek()
        if t == 'func':
            self.pos += 1
            fn_name = v.upper()
            if not self.eat('paren'): raise ValueError("Expected '(' after %s" % fn_name)
            args = []
            if fn_name != 'PI':
                args.append(self.ast_expr())
                while self.eat('comma'): args.append(self.ast_expr())
            if not self.eat('paren'): raise ValueError("Expected ')'")
            if fn_name == 'POWER' and len(args) == 2:
                return _BinOp('**', args[0], args[1])
            return _Func(fn_name, args)
        if t == 'paren' and v == '(':
            self.pos += 1
            r = self.ast_expr()
            if not self.eat('paren'): raise ValueError("Expected ')'")
            return r
        if t == 'number':
            self.pos += 1
            raw = v
            if raw and raw[-1] in 'NMLnml': raw = raw[:-1]
            return _Lit(raw)
        if t == 'path':
            self.pos += 1
            return _Path(v)
        raise ValueError("Expected value, got %r" % (self.peek(),))

    # ── SQL builders ─────────────────────────────────────────
    # expr/term/factor/atom return _ExprResult for type-aware comparison.

    def expr(self):
        """expr = term (('+' | '-') term)*"""
        left = self.term()
        while True:
            tok = self.eat('addsub')
            if not tok:
                break
            right = self.term()
            fn = 'qjson_add' if tok[1] == '+' else 'qjson_sub'
            left = _ExprResult('numeric', "%s(%s, %s)" % (fn, left.sql, right.sql))
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
            left = _ExprResult('numeric', "%s(%s, %s)" % (fn, left.sql, right.sql))
        return left

    def factor(self):
        """factor = atom ('**' atom)?"""
        left = self.atom()
        tok = self.eat('pow')
        if tok:
            right = self.atom()
            left = _ExprResult('numeric', "qjson_pow(%s, %s)" % (left.sql, right.sql))
        return left

    def atom(self):
        """atom = path | number | string | literal | func(args) | '(' expr ')'"""
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
            return _ExprResult('numeric',
                "%s(%s)" % (sql_fn, ', '.join(a.sql for a in args)))

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
            return _ExprResult('numeric', "'%s'" % raw)

        if t == 'string':
            self.pos += 1
            # Strip quotes for the literal value
            return _ExprResult('string_literal', v)

        if t == 'literal':
            self.pos += 1
            return _ExprResult('type_literal', v)

        if t == 'path':
            self.pos += 1
            return self._resolve_path(v)

        raise ValueError("Expected value, got %r" % (self.peek(),))

    def _resolve_path_to_str(self, path_expr):
        """Resolve a path to a SQL expression for the exact decimal value.
        Uses COALESCE(n.str, CAST(n.lo AS TEXT)) to get the exact string.
        Used for arithmetic expressions (number-only context)."""
        P = self.builder.P
        t_number = self.builder._t("number")
        path_steps = parse_path(path_expr)
        vid_expr = self.builder.resolve_path(path_steps, self.root_vid)
        n = self.builder._alias("ne")
        self.builder._joins.append(
            'JOIN %s %s ON %s.value_id = %s' % (t_number, n, n, vid_expr))
        return "COALESCE(%s.str, CAST(%s.lo AS TEXT))" % (n, n)

    def _resolve_path(self, path_expr):
        """Resolve a path for comparison context.

        Returns _ExprResult with kind='path' and metadata for type-aware
        comparison: value_id expression, type alias, number alias (LEFT JOIN),
        string alias (LEFT JOIN).
        """
        b = self.builder
        P = b.P
        path_steps = parse_path(path_expr)
        vid_expr = b.resolve_path(path_steps, self.root_vid)

        # Always JOIN to value table for type
        v = b._alias("cv")
        b._joins.append(
            'JOIN %s %s ON %s.id = %s' % (b._t("value"), v, v, vid_expr))

        # LEFT JOIN to number table
        n = b._alias("cn")
        b._joins.append(
            'LEFT JOIN %s %s ON %s.value_id = %s' % (b._t("number"), n, n, vid_expr))

        # LEFT JOIN to string table
        s = b._alias("cs")
        b._joins.append(
            'LEFT JOIN %s %s ON %s.value_id = %s' % (b._t("string"), s, s, vid_expr))

        num_sql = "COALESCE(%s.str, CAST(%s.lo AS TEXT))" % (n, n)

        return _ExprResult('path', num_sql,
                           vid_expr=vid_expr, type_expr="%s.type" % v,
                           num_str_expr=num_sql, str_expr="%s.value" % s,
                           n_alias=n, v_alias=v)

    # ── Predicate: expr cmp expr ────────────────────────────

    def comparison(self):
        """predicate = expr cmp expr — type-aware dispatch."""
        left = self.expr()
        tok = self.eat('cmp')
        if not tok:
            raise ValueError("Expected comparison operator, got %r" % (self.peek(),))
        op = tok[1]
        right = self.expr()

        return _compile_cross_comparison(left, op, right, self.builder, self.has_ext)

    # Also handle 'string' and 'literal' tokens in predicates start set
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
            elif t in ('path', 'number', 'paren', 'string', 'literal', 'func'):
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


_NUM_TYPES = "('number','bigint','bigdec','bigfloat')"


def _compile_cross_comparison(left, op, right, builder, has_ext):
    """Type-aware comparison dispatch for two _ExprResult values.

    Handles:
      numeric vs numeric  — qjson_decimal_cmp
      path vs path        — type-dispatched (string eq, numeric cmp, type match)
      path vs literal     — type/string/numeric dispatch
      path vs string_lit  — string table equality
    """
    P = builder.P
    lk, rk = left.kind, right.kind

    # ── Both numeric (arithmetic expressions or number literals) ──
    if lk == 'numeric' and rk == 'numeric':
        return _numeric_cmp(left.sql, right.sql, op)

    # ── path vs path ──
    if lk == 'path' and rk == 'path':
        return _path_vs_path(left, op, right, P)

    # ── path vs string literal ──
    if lk == 'path' and rk == 'string_literal':
        return _path_vs_string(left, op, right.sql, P)
    if lk == 'string_literal' and rk == 'path':
        return _path_vs_string(right, _flip_op(op), left.sql, P)

    # ── path vs type literal (true/false/null) ──
    if lk == 'path' and rk == 'type_literal':
        return _path_vs_type(left, op, right.sql, P)
    if lk == 'type_literal' and rk == 'path':
        return _path_vs_type(right, _flip_op(op), left.sql, P)

    # ── path vs numeric expression ──
    if lk == 'path' and rk == 'numeric':
        return _numeric_cmp(left.num_str_expr, right.sql, op)
    if lk == 'numeric' and rk == 'path':
        return _numeric_cmp(left.sql, right.num_str_expr, op)

    # ── string literal vs string literal ──
    if lk == 'string_literal' and rk == 'string_literal':
        if op == '==':
            return "%s = %s" % (left.sql, right.sql)
        elif op == '!=':
            return "%s != %s" % (left.sql, right.sql)

    raise ValueError("Cannot compare %s %s %s" % (lk, op, rk))


def _flip_op(op):
    """Flip a comparison operator for swapped operands."""
    return {'<': '>', '>': '<', '<=': '>=', '>=': '<=',
            '==': '==', '!=': '!='}.get(op, op)


def _numeric_cmp(left_sql, right_sql, op):
    """Emit qjson_decimal_cmp for two numeric SQL expressions."""
    _op_map = {'==': '= 0', '!=': '!= 0', '>': '> 0',
               '>=': '>= 0', '<': '< 0', '<=': '<= 0'}
    return "qjson_decimal_cmp(%s, %s) %s" % (left_sql, right_sql, _op_map[op])


def _path_vs_path(left, op, right, P):
    """Type-dispatched comparison of two resolved paths.

    For == / !=: checks type match, then dispatches to string eq or numeric cmp.
    For ordering: numeric only (both must be numeric types).
    """
    lt, rt = left.type_expr, right.type_expr
    ln, rn = left.num_str_expr, right.num_str_expr
    ls, rs = left.str_expr, right.str_expr

    if op == '==':
        return ("(%s = %s AND ("
                "(%s IN %s AND qjson_decimal_cmp(%s, %s) = 0)"
                " OR (%s = 'string' AND %s = %s)"
                " OR (%s IN ('null','true','false'))"
                "))" % (lt, rt,
                        lt, _NUM_TYPES, ln, rn,
                        lt, ls, rs,
                        lt))
    elif op == '!=':
        return ("(%s != %s OR ("
                "(%s IN %s AND qjson_decimal_cmp(%s, %s) != 0)"
                " OR (%s = 'string' AND %s != %s)"
                "))" % (lt, rt,
                        lt, _NUM_TYPES, ln, rn,
                        lt, ls, rs))
    else:
        # Ordering: numeric types only
        return ("(%s IN %s AND %s IN %s AND qjson_decimal_cmp(%s, %s) %s)"
                % (lt, _NUM_TYPES, rt, _NUM_TYPES,
                   ln, rn, {'<': '< 0', '<=': '<= 0',
                            '>': '> 0', '>=': '>= 0'}[op]))


def _path_vs_string(path_result, op, str_literal_sql, P):
    """Compare a resolved path against a string literal.

    The string literal SQL is a quoted string like '"hello"'.
    We strip the outer quotes for SQL comparison.
    """
    # Strip the JSON-style double quotes from the literal for SQL
    # str_literal_sql is e.g. '"hello"' — we need 'hello' for SQL
    inner = str_literal_sql[1:-1]  # strip outer quotes
    st = path_result.str_expr
    tt = path_result.type_expr
    if op == '==':
        return "(%s = 'string' AND %s = '%s')" % (tt, st, inner.replace("'", "''"))
    elif op == '!=':
        return "(%s != 'string' OR %s != '%s')" % (tt, st, inner.replace("'", "''"))
    else:
        raise ValueError("Cannot use %s with string comparison" % op)


def _path_vs_type(path_result, op, type_literal, P):
    """Compare a resolved path against true/false/null."""
    tt = path_result.type_expr
    if op == '==':
        return "%s = '%s'" % (tt, type_literal)
    elif op == '!=':
        return "%s != '%s'" % (tt, type_literal)
    else:
        raise ValueError("Cannot use %s with %s" % (op, type_literal))


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

    # Compile WHERE (may introduce new variable bindings)
    where_sql = ""
    where_params = []
    if where_expr:
        where_sql, where_params = compile_where(
            where_expr, builder, "root.id", has_ext)

    # Build variable select columns AFTER where (captures all bindings)
    var_selects = []
    for var_name, ai_alias in builder._var_aliases.items():
        var_selects.append("%s.dst_vid AS %s" % (ai_alias, var_name))

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


def qjson_closure(conn, root_id, set_path,
                  where_from=None, where_to=None,
                  prefix="qjson_", dialect=None):
    """Compute transitive closure of a binary relation (set of 2-tuples).

    set_path    — path to a set of 2-element tuples (e.g. ".edge")
    where_from  — optional: only paths starting from this value (string)
    where_to    — optional: only paths ending at this value (string)

    Returns list of (from_text, to_text) pairs as QJSON strings.

    Example:
        # edge = {[a, b], [b, c], [c, d]}
        qjson_closure(conn, root, '.edge')
        # → [('a','b'), ('a','c'), ('a','d'), ('b','c'), ('b','d'), ('c','d')]

        qjson_closure(conn, root, '.edge', where_from='a')
        # → [('a','b'), ('a','c'), ('a','d')]
    """
    if dialect is None:
        dialect = 'sqlite'
    P = '?' if dialect == 'sqlite' else '%s'
    qi = lambda name: '"%s%s"' % (prefix, name)

    # Resolve set_path to the set's object_id
    builder = _QueryBuilder(prefix=prefix, dialect=dialect)
    steps = parse_path(set_path)
    vid_expr = builder.resolve_path(steps, "root.id")

    resolve_sql = ("SELECT o.id FROM %s root %s"
                   " JOIN %s o ON o.value_id = %s"
                   " WHERE root.id = %s" % (
                       qi("value"), "\n".join(builder._joins),
                       qi("object"), vid_expr, P))
    params = list(builder._params) + [root_id]

    if dialect == 'postgres':
        cur = conn.cursor()
        cur.execute(resolve_sql, tuple(params))
        row = cur.fetchone()
    else:
        row = conn.execute(resolve_sql, tuple(params)).fetchone()
    if not row:
        return []
    obj_id = row[0]

    # Build WITH RECURSIVE query
    cte_sql = """
WITH RECURSIVE closure(from_vid, to_vid) AS (
    SELECT ai0.value_id, ai1.value_id
    FROM {oi} oi
    JOIN {arr} a ON a.value_id = oi.key_id
    JOIN {ai} ai0 ON ai0.array_id = a.id AND ai0.idx = 0
    JOIN {ai} ai1 ON ai1.array_id = a.id AND ai1.idx = 1
    WHERE oi.object_id = {P}

    UNION

    SELECT c.from_vid, ai1.value_id
    FROM closure c
    JOIN {oi} oi ON oi.object_id = {P}
    JOIN {arr} a ON a.value_id = oi.key_id
    JOIN {ai} ai0 ON ai0.array_id = a.id AND ai0.idx = 0
    JOIN {ai} ai1 ON ai1.array_id = a.id AND ai1.idx = 1
    WHERE qjson_reconstruct(ai0.value_id, {P}) = qjson_reconstruct(c.to_vid, {P})
)
SELECT qjson_reconstruct(from_vid, {P}), qjson_reconstruct(to_vid, {P})
FROM closure""".format(
        oi=qi("object_item"), arr=qi("array"), ai=qi("array_item"),
        P=P)

    cte_params = [obj_id, obj_id, prefix, prefix, prefix, prefix]

    # Optional filters
    filters = []
    if where_from is not None:
        filters.append(
            "from_vid IN (SELECT value_id FROM %s WHERE value = %s)" % (qi("string"), P))
        cte_params.append(where_from)
    if where_to is not None:
        filters.append(
            "to_vid IN (SELECT value_id FROM %s WHERE value = %s)" % (qi("string"), P))
        cte_params.append(where_to)

    if filters:
        cte_sql += "\nWHERE " + " AND ".join(filters)

    if dialect == 'postgres':
        cur = conn.cursor()
        cur.execute(cte_sql, tuple(cte_params))
        rows = cur.fetchall()
    else:
        rows = conn.execute(cte_sql, tuple(cte_params)).fetchall()

    return [(r[0], r[1]) for r in rows]


def qjson_solve(conn, root_id, constraint_expr,
                prefix="qjson_", dialect=None, has_ext=True):
    """Solve constraints by decomposing into 3-term qjson_solve_* calls.

    constraint_expr — equality constraints joined by AND:
        '.future == .present * POWER(1 + .rate, .periods)'

    The expression tree is decomposed into 3-term constraints with
    anonymous temporaries. qjson_solve_add/sub/mul/div/pow handle
    inversion automatically. Leaf-folding propagation runs until
    no more progress.

    Returns True if fully determined, False otherwise.
    """
    if dialect is None:
        dialect = 'sqlite'
    P = '?' if dialect == 'sqlite' else '%s'

    # Parse each LHS == RHS constraint into AST pairs
    parts = [c.strip() for c in re.split(r'\bAND\b', constraint_expr)]
    all_constraints = []

    for part in parts:
        if '==' not in part:
            raise ValueError("Constraint must use ==: %r" % part)
        lhs_str, rhs_str = part.split('==', 1)

        # Parse both sides to AST
        tokens_l = _tokenize(lhs_str.strip())
        comp_l = _ExprCompiler(tokens_l, None, None, has_ext)
        lhs_ast = comp_l.ast_expr()

        tokens_r = _tokenize(rhs_str.strip())
        comp_r = _ExprCompiler(tokens_r, None, None, has_ext)
        rhs_ast = comp_r.ast_expr()

        # Decompose into 3-term constraints
        new_constraints = _decompose_to_constraints(
            lhs_ast, rhs_ast, conn, root_id, prefix, dialect)
        all_constraints.extend(new_constraints)

    # Leaf-folding propagation using qjson_solve_*
    pending = list(all_constraints)
    while True:
        remaining = []
        solved_any = False
        for sql, params in pending:
            r = conn.execute(sql, params).fetchone()[0]
            if r == 1:    solved_any = True  # solved one
            elif r == 3:  remaining.append((sql, params))  # underdetermined
            elif r == 0:  return False  # inconsistent
            # r == 2: consistent, done
        if not solved_any:
            break
        pending = remaining

    return len(pending) == 0


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
