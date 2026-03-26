-- ============================================================
-- qjson_pg.sql — QJSON query translator for PostgreSQL.
--
-- Installs PL/pgSQL functions that translate jq-like path
-- expressions + WHERE predicates into SQL JOIN chains and
-- execute them against the normalized QJSON schema.
--
-- Functions:
--   qjson_select(root_id, select_path, where_expr, prefix)
--   qjson_update(root_id, update_path, new_value, new_type, where_expr, prefix)
--   qjson_decimal_cmp(a, b)  — exact decimal string comparison
--
-- Path syntax:
--   .key    object child by key
--   [n]     array index (literal integer)
--   [K]     array index (variable, uppercase = binding)
--   .[]     all array elements
--
-- Usage:
--   docker compose up -d postgres
--   psql -h localhost -p 5433 -U qjson -d qjson_test -f sql/qjson_pg.sql
-- ============================================================

-- ── Exact decimal comparison ────────────────────────────────
-- Pure SQL fallback (no C extension needed).
-- Compares two decimal strings numerically.

CREATE OR REPLACE FUNCTION qjson_decimal_cmp(a TEXT, b TEXT)
RETURNS INTEGER AS $$
DECLARE
    da NUMERIC;
    db NUMERIC;
BEGIN
    IF a IS NULL OR b IS NULL THEN RETURN NULL; END IF;
    da := a::NUMERIC;
    db := b::NUMERIC;
    IF da < db THEN RETURN -1;
    ELSIF da > db THEN RETURN 1;
    ELSE RETURN 0;
    END IF;
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

-- ── Internal: raw ordering for bound values ─────────────────

CREATE OR REPLACE FUNCTION _qjson_cmp_ord(
    a_lo DOUBLE PRECISION, a_hi DOUBLE PRECISION, a_str TEXT,
    b_lo DOUBLE PRECISION, b_hi DOUBLE PRECISION, b_str TEXT
) RETURNS INTEGER AS $$
BEGIN
    IF a_hi < b_lo THEN RETURN -1; END IF;
    IF a_lo > b_hi THEN RETURN  1; END IF;
    IF a_lo = a_hi AND b_lo = b_hi THEN RETURN 0; END IF;
    RETURN qjson_decimal_cmp(a_str, b_str);
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- ── Internal: check if two unbound names match ──────────────

CREATE OR REPLACE FUNCTION _qjson_unbound_same(a_str TEXT, b_str TEXT)
RETURNS BOOLEAN AS $$
BEGIN
    RETURN COALESCE(a_str, '') = COALESCE(b_str, '');
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- ── Six comparison functions (each returns 0 or 1) ──────────
-- a_type/b_type: text type name from qjson_value.type column.
-- Unbound rules:
--   same-name unbounds → behave as equal
--   different-name or unbound-vs-concrete → always 1

CREATE OR REPLACE FUNCTION qjson_cmp_eq(
    a_type TEXT, a_lo DOUBLE PRECISION, a_str TEXT, a_hi DOUBLE PRECISION,
    b_type TEXT, b_lo DOUBLE PRECISION, b_str TEXT, b_hi DOUBLE PRECISION
) RETURNS INTEGER AS $$
BEGIN
    IF a_type = 'unbound' OR b_type = 'unbound' THEN RETURN 1; END IF;
    IF _qjson_cmp_ord(a_lo, a_hi, a_str, b_lo, b_hi, b_str) = 0 THEN RETURN 1; END IF;
    RETURN 0;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE OR REPLACE FUNCTION qjson_cmp_ne(
    a_type TEXT, a_lo DOUBLE PRECISION, a_str TEXT, a_hi DOUBLE PRECISION,
    b_type TEXT, b_lo DOUBLE PRECISION, b_str TEXT, b_hi DOUBLE PRECISION
) RETURNS INTEGER AS $$
BEGIN
    IF a_type = 'unbound' AND b_type = 'unbound' THEN
        IF _qjson_unbound_same(a_str, b_str) THEN RETURN 0; END IF;
        RETURN 1;
    END IF;
    IF a_type = 'unbound' OR b_type = 'unbound' THEN RETURN 1; END IF;
    IF _qjson_cmp_ord(a_lo, a_hi, a_str, b_lo, b_hi, b_str) != 0 THEN RETURN 1; END IF;
    RETURN 0;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE OR REPLACE FUNCTION qjson_cmp_lt(
    a_type TEXT, a_lo DOUBLE PRECISION, a_str TEXT, a_hi DOUBLE PRECISION,
    b_type TEXT, b_lo DOUBLE PRECISION, b_str TEXT, b_hi DOUBLE PRECISION
) RETURNS INTEGER AS $$
BEGIN
    IF a_type = 'unbound' AND b_type = 'unbound' THEN
        IF _qjson_unbound_same(a_str, b_str) THEN RETURN 0; END IF;
        RETURN 1;
    END IF;
    IF a_type = 'unbound' OR b_type = 'unbound' THEN RETURN 1; END IF;
    IF _qjson_cmp_ord(a_lo, a_hi, a_str, b_lo, b_hi, b_str) < 0 THEN RETURN 1; END IF;
    RETURN 0;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE OR REPLACE FUNCTION qjson_cmp_le(
    a_type TEXT, a_lo DOUBLE PRECISION, a_str TEXT, a_hi DOUBLE PRECISION,
    b_type TEXT, b_lo DOUBLE PRECISION, b_str TEXT, b_hi DOUBLE PRECISION
) RETURNS INTEGER AS $$
BEGIN
    IF a_type = 'unbound' OR b_type = 'unbound' THEN RETURN 1; END IF;
    IF _qjson_cmp_ord(a_lo, a_hi, a_str, b_lo, b_hi, b_str) <= 0 THEN RETURN 1; END IF;
    RETURN 0;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE OR REPLACE FUNCTION qjson_cmp_gt(
    a_type TEXT, a_lo DOUBLE PRECISION, a_str TEXT, a_hi DOUBLE PRECISION,
    b_type TEXT, b_lo DOUBLE PRECISION, b_str TEXT, b_hi DOUBLE PRECISION
) RETURNS INTEGER AS $$
BEGIN
    IF a_type = 'unbound' AND b_type = 'unbound' THEN
        IF _qjson_unbound_same(a_str, b_str) THEN RETURN 0; END IF;
        RETURN 1;
    END IF;
    IF a_type = 'unbound' OR b_type = 'unbound' THEN RETURN 1; END IF;
    IF _qjson_cmp_ord(a_lo, a_hi, a_str, b_lo, b_hi, b_str) > 0 THEN RETURN 1; END IF;
    RETURN 0;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE OR REPLACE FUNCTION qjson_cmp_ge(
    a_type TEXT, a_lo DOUBLE PRECISION, a_str TEXT, a_hi DOUBLE PRECISION,
    b_type TEXT, b_lo DOUBLE PRECISION, b_str TEXT, b_hi DOUBLE PRECISION
) RETURNS INTEGER AS $$
BEGIN
    IF a_type = 'unbound' OR b_type = 'unbound' THEN RETURN 1; END IF;
    IF _qjson_cmp_ord(a_lo, a_hi, a_str, b_lo, b_hi, b_str) >= 0 THEN RETURN 1; END IF;
    RETURN 0;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- ── QJSON reconstructor ─────────────────────────────────────
-- Walks the normalized schema and emits canonical QJSON text.

CREATE OR REPLACE FUNCTION qjson_reconstruct(
    p_vid    INTEGER,
    p_prefix TEXT DEFAULT 'qjson_'
) RETURNS TEXT AS $$
DECLARE
    vtype    TEXT;
    result   TEXT;
    n_lo     DOUBLE PRECISION;
    n_str    TEXT;
    n_hi     DOUBLE PRECISION;
    s_val    TEXT;
    b_val    BYTEA;
    arr_id   INTEGER;
    obj_id   INTEGER;
    rec      RECORD;
    parts    TEXT[];
    raw      TEXT;
    i        INTEGER;
    ch       TEXT;
    code     INTEGER;
BEGIN
    -- Look up type
    EXECUTE format('SELECT type FROM %I WHERE id = $1', p_prefix || 'value')
        INTO vtype USING p_vid;
    IF vtype IS NULL THEN RETURN 'null'; END IF;

    -- Scalars
    IF vtype = 'null'  THEN RETURN 'null';  END IF;
    IF vtype = 'true'  THEN RETURN 'true';  END IF;
    IF vtype = 'false' THEN RETURN 'false'; END IF;

    -- Numeric types
    IF vtype IN ('number', 'bigint', 'bigdec', 'bigfloat') THEN
        EXECUTE format('SELECT lo, str, hi FROM %I WHERE value_id = $1',
                       p_prefix || 'number')
            INTO n_lo, n_str, n_hi USING p_vid;

        IF vtype = 'number' THEN
            -- Format as shortest round-trip decimal
            raw := n_lo::TEXT;
            IF position('.' IN raw) > 0 THEN
                raw := trim(trailing '0' FROM raw);
                raw := trim(trailing '.' FROM raw);
            END IF;
            RETURN raw;
        END IF;

        -- BigInt/BigDecimal/BigFloat: use str if present, else lo
        IF n_str IS NOT NULL THEN
            raw := n_str;
        ELSE
            -- Exact double — reconstruct from lo
            IF vtype = 'bigint' THEN
                raw := trunc(n_lo)::BIGINT::TEXT;
            ELSE
                raw := n_lo::TEXT;
                IF position('.' IN raw) > 0 THEN
                    raw := trim(trailing '0' FROM raw);
                    raw := trim(trailing '.' FROM raw);
                END IF;
            END IF;
        END IF;

        IF vtype = 'bigint'    THEN RETURN raw || 'N'; END IF;
        IF vtype = 'bigdec'    THEN RETURN raw || 'M'; END IF;
        IF vtype = 'bigfloat'  THEN RETURN raw || 'L'; END IF;
    END IF;

    -- String
    IF vtype = 'string' THEN
        EXECUTE format('SELECT value FROM %I WHERE value_id = $1',
                       p_prefix || 'string')
            INTO s_val USING p_vid;
        IF s_val IS NULL THEN s_val := ''; END IF;
        -- Escape for QJSON canonical form
        result := '"';
        FOR i IN 1..length(s_val) LOOP
            ch := substr(s_val, i, 1);
            code := ascii(ch);
            IF ch = '"'  THEN result := result || '\"';
            ELSIF ch = E'\\' THEN result := result || '\\';
            ELSIF ch = E'\n' THEN result := result || '\n';
            ELSIF ch = E'\r' THEN result := result || '\r';
            ELSIF ch = E'\t' THEN result := result || '\t';
            ELSIF code < 32  THEN result := result || '\u' || lpad(to_hex(code), 4, '0');
            ELSE result := result || ch;
            END IF;
        END LOOP;
        RETURN result || '"';
    END IF;

    -- Blob
    IF vtype = 'blob' THEN
        EXECUTE format('SELECT value FROM %I WHERE value_id = $1',
                       p_prefix || 'blob')
            INTO b_val USING p_vid;
        IF b_val IS NULL THEN RETURN '0j'; END IF;
        -- Encode as hex blob (0j prefix with hex digits)
        -- Full JS64 encoding would be ideal but hex is simpler and valid
        RETURN '0j' || encode(b_val, 'hex');
    END IF;

    -- Array
    IF vtype = 'array' THEN
        EXECUTE format('SELECT id FROM %I WHERE value_id = $1',
                       p_prefix || 'array')
            INTO arr_id USING p_vid;
        IF arr_id IS NULL THEN RETURN '[]'; END IF;

        parts := '{}';
        FOR rec IN EXECUTE format(
            'SELECT value_id FROM %I WHERE array_id = $1 ORDER BY idx',
            p_prefix || 'array_item')
            USING arr_id
        LOOP
            parts := parts || qjson_reconstruct(rec.value_id, p_prefix);
        END LOOP;
        RETURN '[' || array_to_string(parts, ',') || ']';
    END IF;

    -- Object
    IF vtype = 'object' THEN
        EXECUTE format('SELECT id FROM %I WHERE value_id = $1',
                       p_prefix || 'object')
            INTO obj_id USING p_vid;
        IF obj_id IS NULL THEN RETURN '{}'; END IF;

        parts := '{}';
        FOR rec IN EXECUTE format(
            'SELECT key_id, value_id FROM %I WHERE object_id = $1',
            p_prefix || 'object_item')
            USING obj_id
        LOOP
            parts := parts || (qjson_reconstruct(rec.key_id, p_prefix) || ':' ||
                               qjson_reconstruct(rec.value_id, p_prefix));
        END LOOP;
        RETURN '{' || array_to_string(parts, ',') || '}';
    END IF;

    RETURN 'null';
END;
$$ LANGUAGE plpgsql;

-- ── Path parser (returns table of steps) ────────────────────

CREATE TYPE qjson_path_step AS (
    step_type TEXT,   -- 'key', 'index', 'var', 'iter'
    step_val  TEXT    -- key name, index number, variable name, or NULL
);

CREATE OR REPLACE FUNCTION qjson_parse_path(path_expr TEXT)
RETURNS qjson_path_step[] AS $$
DECLARE
    steps qjson_path_step[] := '{}';
    pos INTEGER := 1;
    ch TEXT;
    buf TEXT;
    expr_len INTEGER;
BEGIN
    expr_len := length(path_expr);
    WHILE pos <= expr_len LOOP
        ch := substr(path_expr, pos, 1);

        IF ch = '.' THEN
            -- Check for .[] (iterate)
            IF substr(path_expr, pos, 3) = '.[]' THEN
                steps := steps || ROW('iter', NULL)::qjson_path_step;
                pos := pos + 3;
            ELSE
                -- .key
                pos := pos + 1;
                buf := '';
                WHILE pos <= expr_len AND substr(path_expr, pos, 1) ~ '[a-zA-Z0-9_]' LOOP
                    buf := buf || substr(path_expr, pos, 1);
                    pos := pos + 1;
                END LOOP;
                IF buf = '' THEN
                    RAISE EXCEPTION 'Invalid path at position %', pos;
                END IF;
                steps := steps || ROW('key', buf)::qjson_path_step;
            END IF;

        ELSIF ch = '[' THEN
            pos := pos + 1;
            -- Check for [] (iterate)
            IF substr(path_expr, pos, 1) = ']' THEN
                steps := steps || ROW('iter', NULL)::qjson_path_step;
                pos := pos + 1;
            ELSE
                buf := '';
                WHILE pos <= expr_len AND substr(path_expr, pos, 1) != ']' LOOP
                    buf := buf || substr(path_expr, pos, 1);
                    pos := pos + 1;
                END LOOP;
                pos := pos + 1;  -- skip ]
                -- Variable (uppercase start) or index (digits)
                IF buf ~ '^[A-Z]' THEN
                    steps := steps || ROW('var', buf)::qjson_path_step;
                ELSIF buf ~ '^[0-9]+$' THEN
                    steps := steps || ROW('index', buf)::qjson_path_step;
                ELSE
                    RAISE EXCEPTION 'Invalid bracket content: %', buf;
                END IF;
            END IF;
        ELSE
            RAISE EXCEPTION 'Invalid path character at position %: %', pos, ch;
        END IF;
    END LOOP;
    RETURN steps;
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

-- ── SQL builder: path → JOIN chain ──────────────────────────

CREATE TYPE qjson_query_result AS (
    result_vid INTEGER,
    qjson      TEXT,
    bindings   JSONB
);

CREATE OR REPLACE FUNCTION qjson_select(
    p_root_id     INTEGER,
    p_select_path TEXT,
    p_where_expr  TEXT DEFAULT NULL,
    p_prefix      TEXT DEFAULT 'qjson_'
) RETURNS SETOF qjson_query_result AS $$
DECLARE
    select_steps qjson_path_step[];
    where_tokens TEXT[];
    sql_text     TEXT;
    join_clauses TEXT := '';
    where_clause TEXT := '';
    select_expr  TEXT;
    current_vid  TEXT;
    alias_num    INTEGER := 0;
    var_aliases  JSONB := '{}';
    var_selects  TEXT := '';
    step         qjson_path_step;
    a_alias      TEXT;
    ai_alias     TEXT;
    o_alias      TEXT;
    oi_alias     TEXT;
    t_value      TEXT;
    t_number     TEXT;
    t_string     TEXT;
    t_object     TEXT;
    t_object_item TEXT;
    t_array      TEXT;
    t_array_item TEXT;
    rec          RECORD;
BEGIN
    t_value       := quote_ident(p_prefix || 'value');
    t_number      := quote_ident(p_prefix || 'number');
    t_string      := quote_ident(p_prefix || 'string');
    t_object      := quote_ident(p_prefix || 'object');
    t_object_item := quote_ident(p_prefix || 'object_item');
    t_array       := quote_ident(p_prefix || 'array');
    t_array_item  := quote_ident(p_prefix || 'array_item');

    -- Parse SELECT path
    select_steps := qjson_parse_path(p_select_path);
    current_vid := 'root.id';

    -- Build JOIN chain for SELECT path
    FOREACH step IN ARRAY select_steps LOOP
        alias_num := alias_num + 1;

        IF step.step_type = 'key' THEN
            o_alias  := 'o_'  || alias_num;
            alias_num := alias_num + 1;
            oi_alias := 'oi_' || alias_num;
            join_clauses := join_clauses ||
                format(' JOIN %s %s ON %s.value_id = %s', t_object, o_alias, o_alias, current_vid) ||
                format(' JOIN %s %s ON %s.object_id = %s.id AND %s.key_id IN (SELECT value_id FROM %s WHERE value = %L)',
                       t_object_item, oi_alias, oi_alias, o_alias, oi_alias, t_string, step.step_val);
            current_vid := oi_alias || '.value_id';

        ELSIF step.step_type = 'index' THEN
            a_alias  := 'a_'  || alias_num;
            alias_num := alias_num + 1;
            ai_alias := 'ai_' || alias_num;
            join_clauses := join_clauses ||
                format(' JOIN %s %s ON %s.value_id = %s', t_array, a_alias, a_alias, current_vid) ||
                format(' JOIN %s %s ON %s.array_id = %s.id AND %s.idx = %s',
                       t_array_item, ai_alias, ai_alias, a_alias, ai_alias, step.step_val);
            current_vid := ai_alias || '.value_id';

        ELSIF step.step_type = 'var' THEN
            IF var_aliases ? step.step_val THEN
                current_vid := (var_aliases ->> step.step_val) || '.value_id';
            ELSE
                a_alias  := 'a_'  || alias_num;
                alias_num := alias_num + 1;
                ai_alias := 'ai_' || alias_num;
                join_clauses := join_clauses ||
                    format(' JOIN %s %s ON %s.value_id = %s', t_array, a_alias, a_alias, current_vid) ||
                    format(' JOIN %s %s ON %s.array_id = %s.id', t_array_item, ai_alias, ai_alias, a_alias);
                var_aliases := var_aliases || jsonb_build_object(step.step_val, ai_alias);
                current_vid := ai_alias || '.value_id';
                IF var_selects != '' THEN var_selects := var_selects || ', '; END IF;
                var_selects := var_selects || format('%s.idx AS %I', ai_alias, step.step_val);
            END IF;

        ELSIF step.step_type = 'iter' THEN
            a_alias  := 'a_'  || alias_num;
            alias_num := alias_num + 1;
            ai_alias := 'ai_' || alias_num;
            join_clauses := join_clauses ||
                format(' JOIN %s %s ON %s.value_id = %s', t_array, a_alias, a_alias, current_vid) ||
                format(' JOIN %s %s ON %s.array_id = %s.id', t_array_item, ai_alias, ai_alias, a_alias);
            current_vid := ai_alias || '.value_id';
        END IF;
    END LOOP;

    select_expr := current_vid || ' AS result_vid';
    if var_selects != '' THEN
        select_expr := select_expr || ', ' || var_selects;
    END IF;

    -- Build WHERE clause from expression
    IF p_where_expr IS NOT NULL AND p_where_expr != '' THEN
        where_clause := qjson_compile_where(
            p_where_expr, p_prefix, alias_num, var_aliases);
    END IF;

    -- Assemble full query
    sql_text := format('SELECT %s FROM %s root %s WHERE root.id = %s',
                       select_expr, t_value, join_clauses, p_root_id);
    IF where_clause != '' THEN
        sql_text := sql_text || ' AND (' || where_clause || ')';
    END IF;

    -- Execute and return
    FOR rec IN EXECUTE sql_text LOOP
        DECLARE
            bindings JSONB := '{}';
            vname TEXT;
        BEGIN
            FOR vname IN SELECT jsonb_object_keys(var_aliases) LOOP
                bindings := bindings || jsonb_build_object(
                    vname, rec);
            END LOOP;
            RETURN NEXT ROW(rec.result_vid, qjson_reconstruct(rec.result_vid, p_prefix), bindings)::qjson_query_result;
        END;
    END LOOP;
END;
$$ LANGUAGE plpgsql;

-- ── WHERE clause compiler ───────────────────────────────────
-- Translates a predicate string into a SQL WHERE fragment.
-- Called internally by qjson_select.

CREATE OR REPLACE FUNCTION qjson_compile_where(
    p_where_expr  TEXT,
    p_prefix      TEXT,
    p_alias_num   INTEGER,
    p_var_aliases JSONB
) RETURNS TEXT AS $$
DECLARE
    result    TEXT := '';
    pos       INTEGER := 1;
    expr_len  INTEGER;
    ch        TEXT;
    token     TEXT;
    path_expr TEXT;
    op        TEXT;
    val       TEXT;
    val_type  TEXT;
    path_steps qjson_path_step[];
    current_vid TEXT;
    step      qjson_path_step;
    a_alias   TEXT;
    ai_alias  TEXT;
    o_alias   TEXT;
    oi_alias  TEXT;
    n_alias   TEXT;
    sv_alias  TEXT;
    v_alias   TEXT;
    t_value   TEXT;
    t_number  TEXT;
    t_string  TEXT;
    t_object  TEXT;
    t_object_item TEXT;
    t_array   TEXT;
    t_array_item TEXT;
    join_sql  TEXT := '';
    q_lo      DOUBLE PRECISION;
    q_hi      DOUBLE PRECISION;
    q_str     TEXT;
    raw_num   TEXT;
BEGIN
    t_value       := quote_ident(p_prefix || 'value');
    t_number      := quote_ident(p_prefix || 'number');
    t_string      := quote_ident(p_prefix || 'string');
    t_object      := quote_ident(p_prefix || 'object');
    t_object_item := quote_ident(p_prefix || 'object_item');
    t_array       := quote_ident(p_prefix || 'array');
    t_array_item  := quote_ident(p_prefix || 'array_item');
    expr_len := length(p_where_expr);

    WHILE pos <= expr_len LOOP
        -- Skip whitespace
        WHILE pos <= expr_len AND substr(p_where_expr, pos, 1) ~ '\s' LOOP
            pos := pos + 1;
        END LOOP;
        IF pos > expr_len THEN EXIT; END IF;

        ch := substr(p_where_expr, pos, 1);

        -- Parentheses
        IF ch = '(' OR ch = ')' THEN
            result := result || ' ' || ch;
            pos := pos + 1;

        -- Logical operators
        ELSIF substr(p_where_expr, pos, 3) = 'AND' AND
              substr(p_where_expr, pos + 3, 1) ~ '[\s(.]' THEN
            result := result || ' AND';
            pos := pos + 3;
        ELSIF substr(p_where_expr, pos, 2) = 'OR' AND
              substr(p_where_expr, pos + 2, 1) ~ '[\s(.]' THEN
            result := result || ' OR';
            pos := pos + 2;
        ELSIF substr(p_where_expr, pos, 3) = 'NOT' AND
              substr(p_where_expr, pos + 3, 1) ~ '[\s(.]' THEN
            result := result || ' NOT';
            pos := pos + 3;

        -- Path expression (starts with . or [)
        ELSIF ch = '.' OR ch = '[' THEN
            -- Collect path
            path_expr := '';
            WHILE pos <= expr_len AND substr(p_where_expr, pos, 1) ~ '[.\[a-zA-Z0-9_\]]' LOOP
                path_expr := path_expr || substr(p_where_expr, pos, 1);
                pos := pos + 1;
            END LOOP;

            -- Skip whitespace
            WHILE pos <= expr_len AND substr(p_where_expr, pos, 1) ~ '\s' LOOP pos := pos + 1; END LOOP;

            -- Collect operator
            op := '';
            WHILE pos <= expr_len AND substr(p_where_expr, pos, 1) ~ '[=!<>]' LOOP
                op := op || substr(p_where_expr, pos, 1);
                pos := pos + 1;
            END LOOP;

            -- Skip whitespace
            WHILE pos <= expr_len AND substr(p_where_expr, pos, 1) ~ '\s' LOOP pos := pos + 1; END LOOP;

            -- Collect value
            ch := substr(p_where_expr, pos, 1);
            IF ch = '"' THEN
                -- String literal
                pos := pos + 1;
                val := '';
                WHILE pos <= expr_len AND substr(p_where_expr, pos, 1) != '"' LOOP
                    IF substr(p_where_expr, pos, 1) = '\' THEN pos := pos + 1; END IF;
                    val := val || substr(p_where_expr, pos, 1);
                    pos := pos + 1;
                END LOOP;
                pos := pos + 1;  -- skip closing "
                val_type := 'string';
            ELSIF ch ~ '[0-9\-]' THEN
                -- Number
                val := '';
                WHILE pos <= expr_len AND substr(p_where_expr, pos, 1) ~ '[0-9eE.+\-NMLnml]' LOOP
                    val := val || substr(p_where_expr, pos, 1);
                    pos := pos + 1;
                END LOOP;
                val_type := 'number';
            ELSIF substr(p_where_expr, pos, 4) = 'true' THEN
                val := 'true'; val_type := 'literal'; pos := pos + 4;
            ELSIF substr(p_where_expr, pos, 5) = 'false' THEN
                val := 'false'; val_type := 'literal'; pos := pos + 5;
            ELSIF substr(p_where_expr, pos, 4) = 'null' THEN
                val := 'null'; val_type := 'literal'; pos := pos + 4;
            ELSE
                RAISE EXCEPTION 'Cannot parse value at position %', pos;
            END IF;

            -- Resolve path to value_id expression
            path_steps := qjson_parse_path(path_expr);
            current_vid := 'root.id';
            FOREACH step IN ARRAY path_steps LOOP
                p_alias_num := p_alias_num + 1;
                IF step.step_type = 'key' THEN
                    o_alias  := 'wo_'  || p_alias_num;
                    p_alias_num := p_alias_num + 1;
                    oi_alias := 'woi_' || p_alias_num;
                    join_sql := join_sql ||
                        format(' JOIN %s %s ON %s.value_id = %s', t_object, o_alias, o_alias, current_vid) ||
                        format(' JOIN %s %s ON %s.object_id = %s.id AND %s.key_id IN (SELECT value_id FROM %s WHERE value = %L)',
                               t_object_item, oi_alias, oi_alias, o_alias, oi_alias, t_string, step.step_val);
                    current_vid := oi_alias || '.value_id';
                ELSIF step.step_type = 'index' THEN
                    a_alias  := 'wa_'  || p_alias_num;
                    p_alias_num := p_alias_num + 1;
                    ai_alias := 'wai_' || p_alias_num;
                    join_sql := join_sql ||
                        format(' JOIN %s %s ON %s.value_id = %s', t_array, a_alias, a_alias, current_vid) ||
                        format(' JOIN %s %s ON %s.array_id = %s.id AND %s.idx = %s',
                               t_array_item, ai_alias, ai_alias, a_alias, ai_alias, step.step_val);
                    current_vid := ai_alias || '.value_id';
                ELSIF step.step_type = 'var' THEN
                    IF p_var_aliases ? step.step_val THEN
                        current_vid := (p_var_aliases ->> step.step_val) || '.value_id';
                    ELSE
                        a_alias  := 'wa_'  || p_alias_num;
                        p_alias_num := p_alias_num + 1;
                        ai_alias := 'wai_' || p_alias_num;
                        join_sql := join_sql ||
                            format(' JOIN %s %s ON %s.value_id = %s', t_array, a_alias, a_alias, current_vid) ||
                            format(' JOIN %s %s ON %s.array_id = %s.id', t_array_item, ai_alias, ai_alias, a_alias);
                        current_vid := ai_alias || '.value_id';
                    END IF;
                ELSIF step.step_type = 'iter' THEN
                    a_alias  := 'wa_'  || p_alias_num;
                    p_alias_num := p_alias_num + 1;
                    ai_alias := 'wai_' || p_alias_num;
                    join_sql := join_sql ||
                        format(' JOIN %s %s ON %s.value_id = %s', t_array, a_alias, a_alias, current_vid) ||
                        format(' JOIN %s %s ON %s.array_id = %s.id', t_array_item, ai_alias, ai_alias, a_alias);
                    current_vid := ai_alias || '.value_id';
                END IF;
            END LOOP;

            -- Generate comparison SQL
            IF val_type = 'literal' THEN
                p_alias_num := p_alias_num + 1;
                v_alias := 'wv_' || p_alias_num;
                join_sql := join_sql ||
                    format(' JOIN %s %s ON %s.id = %s', t_value, v_alias, v_alias, current_vid);
                IF op = '==' THEN
                    result := result || format(' %s.type = %L', v_alias, val);
                ELSIF op = '!=' THEN
                    result := result || format(' %s.type != %L', v_alias, val);
                END IF;

            ELSIF val_type = 'string' THEN
                p_alias_num := p_alias_num + 1;
                sv_alias := 'wsv_' || p_alias_num;
                join_sql := join_sql ||
                    format(' JOIN %s %s ON %s.value_id = %s', t_string, sv_alias, sv_alias, current_vid);
                IF op = '==' THEN
                    result := result || format(' %s.value = %L', sv_alias, val);
                ELSIF op = '!=' THEN
                    result := result || format(' %s.value != %L', sv_alias, val);
                END IF;

            ELSIF val_type = 'number' THEN
                -- Strip suffix, project interval
                raw_num := val;
                IF right(raw_num, 1) ~ '[NMLnml]' THEN
                    raw_num := left(raw_num, length(raw_num) - 1);
                END IF;
                q_lo := raw_num::DOUBLE PRECISION;
                q_hi := q_lo;
                q_str := NULL;
                -- For exact interval detection we compare the numeric
                -- to its double-precision round-trip
                IF raw_num::NUMERIC != q_lo::NUMERIC THEN
                    -- Inexact: need interval
                    IF q_lo::NUMERIC > raw_num::NUMERIC THEN
                        q_hi := q_lo;
                        -- q_lo = nextdown(q_lo) — approximate via subtraction
                        q_lo := q_lo - (q_hi - q_lo);  -- will be refined by actual ULP
                    ELSE
                        q_lo := q_lo;
                        q_hi := q_lo + (q_lo - q_lo);
                    END IF;
                    q_str := raw_num;
                END IF;

                p_alias_num := p_alias_num + 1;
                n_alias := 'wn_' || p_alias_num;
                join_sql := join_sql ||
                    format(' JOIN %s %s ON %s.value_id = %s', t_number, n_alias, n_alias, current_vid);

                IF op = '==' THEN
                    result := result || format(
                        ' (%s.lo = %s AND %s.hi = %s AND ((%s.str IS NULL AND %L IS NULL) OR %s.str = %L))',
                        n_alias, q_lo, n_alias, q_hi, n_alias, q_str, n_alias, q_str);
                ELSIF op = '!=' THEN
                    result := result || format(
                        ' NOT (%s.lo = %s AND %s.hi = %s AND ((%s.str IS NULL AND %L IS NULL) OR %s.str = %L))',
                        n_alias, q_lo, n_alias, q_hi, n_alias, q_str, n_alias, q_str);
                ELSIF op = '>' THEN
                    result := result || format(
                        ' (%s.hi > %s AND qjson_cmp_gt(v_t.type, %s.lo, %s.str, %s.hi, %L, %s, %L, %s) = 1)',
                        n_alias, q_lo, n_alias, n_alias, n_alias, q_type_str, q_lo, q_str, q_hi);
                ELSIF op = '>=' THEN
                    result := result || format(
                        ' (%s.hi >= %s AND qjson_cmp_ge(v_t.type, %s.lo, %s.str, %s.hi, %L, %s, %L, %s) = 1)',
                        n_alias, q_lo, n_alias, n_alias, n_alias, q_type_str, q_lo, q_str, q_hi);
                ELSIF op = '<' THEN
                    result := result || format(
                        ' (%s.lo < %s AND qjson_cmp_lt(v_t.type, %s.lo, %s.str, %s.hi, %L, %s, %L, %s) = 1)',
                        n_alias, q_hi, n_alias, n_alias, n_alias, q_type_str, q_lo, q_str, q_hi);
                ELSIF op = '<=' THEN
                    result := result || format(
                        ' (%s.lo <= %s AND qjson_cmp_le(v_t.type, %s.lo, %s.str, %s.hi, %L, %s, %L, %s) = 1)',
                        n_alias, q_hi, n_alias, n_alias, n_alias, q_type_str, q_lo, q_str, q_hi);
                END IF;
            END IF;

        ELSE
            RAISE EXCEPTION 'Unexpected character at position %: %', pos, ch;
        END IF;
    END LOOP;

    -- Prepend any WHERE joins to the result
    -- These get injected into the main query by the caller
    -- We return them as a special prefix: JOINS|||WHERE
    IF join_sql != '' THEN
        result := join_sql || '|||' || result;
    END IF;

    RETURN result;
END;
$$ LANGUAGE plpgsql;

-- ── Override qjson_select to handle WHERE joins ─────────────
-- The compile_where function returns "joins|||predicate".
-- The select function splits them and injects both.

CREATE OR REPLACE FUNCTION qjson_select(
    p_root_id     INTEGER,
    p_select_path TEXT,
    p_where_expr  TEXT DEFAULT NULL,
    p_prefix      TEXT DEFAULT 'qjson_'
) RETURNS SETOF qjson_query_result AS $$
DECLARE
    select_steps qjson_path_step[];
    sql_text     TEXT;
    join_clauses TEXT := '';
    where_joins  TEXT := '';
    where_pred   TEXT := '';
    select_expr  TEXT;
    current_vid  TEXT;
    alias_num    INTEGER := 0;
    var_aliases  JSONB := '{}';
    var_selects  TEXT := '';
    step         qjson_path_step;
    a_alias      TEXT;
    ai_alias     TEXT;
    o_alias      TEXT;
    oi_alias     TEXT;
    t_value      TEXT;
    t_string     TEXT;
    t_object     TEXT;
    t_object_item TEXT;
    t_array      TEXT;
    t_array_item TEXT;
    where_result TEXT;
    sep_pos      INTEGER;
    rec          RECORD;
BEGIN
    t_value       := quote_ident(p_prefix || 'value');
    t_string      := quote_ident(p_prefix || 'string');
    t_object      := quote_ident(p_prefix || 'object');
    t_object_item := quote_ident(p_prefix || 'object_item');
    t_array       := quote_ident(p_prefix || 'array');
    t_array_item  := quote_ident(p_prefix || 'array_item');

    select_steps := qjson_parse_path(p_select_path);
    current_vid := 'root.id';

    FOREACH step IN ARRAY select_steps LOOP
        alias_num := alias_num + 1;
        IF step.step_type = 'key' THEN
            o_alias  := 'o_'  || alias_num;
            alias_num := alias_num + 1;
            oi_alias := 'oi_' || alias_num;
            join_clauses := join_clauses ||
                format(' JOIN %s %s ON %s.value_id = %s', t_object, o_alias, o_alias, current_vid) ||
                format(' JOIN %s %s ON %s.object_id = %s.id AND %s.key_id IN (SELECT value_id FROM %s WHERE value = %L)',
                       t_object_item, oi_alias, oi_alias, o_alias, oi_alias, t_string, step.step_val);
            current_vid := oi_alias || '.value_id';
        ELSIF step.step_type = 'index' THEN
            a_alias  := 'a_'  || alias_num;
            alias_num := alias_num + 1;
            ai_alias := 'ai_' || alias_num;
            join_clauses := join_clauses ||
                format(' JOIN %s %s ON %s.value_id = %s', t_array, a_alias, a_alias, current_vid) ||
                format(' JOIN %s %s ON %s.array_id = %s.id AND %s.idx = %s',
                       t_array_item, ai_alias, ai_alias, a_alias, ai_alias, step.step_val);
            current_vid := ai_alias || '.value_id';
        ELSIF step.step_type = 'var' THEN
            IF var_aliases ? step.step_val THEN
                current_vid := (var_aliases ->> step.step_val) || '.value_id';
            ELSE
                a_alias  := 'a_'  || alias_num;
                alias_num := alias_num + 1;
                ai_alias := 'ai_' || alias_num;
                join_clauses := join_clauses ||
                    format(' JOIN %s %s ON %s.value_id = %s', t_array, a_alias, a_alias, current_vid) ||
                    format(' JOIN %s %s ON %s.array_id = %s.id', t_array_item, ai_alias, ai_alias, a_alias);
                var_aliases := var_aliases || jsonb_build_object(step.step_val, ai_alias);
                current_vid := ai_alias || '.value_id';
                IF var_selects != '' THEN var_selects := var_selects || ', '; END IF;
                var_selects := var_selects || format('%s.idx AS %I', ai_alias, step.step_val);
            END IF;
        ELSIF step.step_type = 'iter' THEN
            a_alias  := 'a_'  || alias_num;
            alias_num := alias_num + 1;
            ai_alias := 'ai_' || alias_num;
            join_clauses := join_clauses ||
                format(' JOIN %s %s ON %s.value_id = %s', t_array, a_alias, a_alias, current_vid) ||
                format(' JOIN %s %s ON %s.array_id = %s.id', t_array_item, ai_alias, ai_alias, a_alias);
            current_vid := ai_alias || '.value_id';
        END IF;
    END LOOP;

    select_expr := current_vid || ' AS result_vid';
    IF var_selects != '' THEN
        select_expr := select_expr || ', ' || var_selects;
    END IF;

    -- Compile WHERE
    IF p_where_expr IS NOT NULL AND p_where_expr != '' THEN
        where_result := qjson_compile_where(p_where_expr, p_prefix, alias_num, var_aliases);
        sep_pos := position('|||' IN where_result);
        IF sep_pos > 0 THEN
            where_joins := left(where_result, sep_pos - 1);
            where_pred  := substr(where_result, sep_pos + 3);
        ELSE
            where_pred := where_result;
        END IF;
    END IF;

    sql_text := format('SELECT %s FROM %s root %s %s WHERE root.id = %s',
                       select_expr, t_value, join_clauses, where_joins, p_root_id);
    IF where_pred != '' THEN
        sql_text := sql_text || ' AND (' || where_pred || ')';
    END IF;

    RAISE NOTICE 'QJSON SQL: %', sql_text;

    FOR rec IN EXECUTE sql_text LOOP
        RETURN NEXT ROW(rec.result_vid, qjson_reconstruct(rec.result_vid, p_prefix), '{}'::JSONB)::qjson_query_result;
    END LOOP;
END;
$$ LANGUAGE plpgsql;

-- ── Solver ───────────────────────────────────────────────────
--
-- NOT implemented in PostgreSQL.
--
-- The constraint solver requires directed rounding (round-down
-- and round-up to IEEE 754 doubles) to project solved values
-- into the [lo, str, hi] interval.  PostgreSQL has no directed
-- rounding — CAST to DOUBLE PRECISION always rounds to nearest.
-- An interval with lo or hi off by one ULP in the wrong
-- direction breaks comparison correctness.
--
-- The solver lives in the application layer (wyatt), which has
-- libbf for exact directed rounding.  QJSON's database layer
-- is storage + queries + comparison + crypto.  Data does not
-- move (no new values are computed) at this layer.
--
-- See: native/qjson_sqlite_ext.c for the libbf-backed solver
--      (available in SQLite/SQLCipher builds with libbf linked)
--

-- ════════════════════════════════════════════════════════════
-- Crypto, arithmetic, solver, and closure are NOT in this file.
--
-- QJSON's database layer is storage + queries + comparison.
-- Data does not move at this layer.
--
-- Crypto (SHA-256, AES, HMAC, JWT, Shamir) and compute
-- (arithmetic, solver, closure) live in the application
-- layer (wyatt), which has libbf for directed rounding and
-- LibreSSL for cryptographic primitives.
--
-- This keeps SQLite and PostgreSQL identical: same schema,
-- same queries, same comparisons.  No platform-specific
-- behavior to test or debug.
--
-- See: native/qjson_crypto.c     (crypto functions)
--      native/qjson_sqlite_ext.c (arithmetic + solver)
-- ════════════════════════════════════════════════════════════
