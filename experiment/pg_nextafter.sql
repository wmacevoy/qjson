-- Exact nextup/nextdown/round_down/round_up for IEEE 754 doubles in pure PL/pgSQL.
-- No C extension needed. Works on hosted PostgreSQL.
-- Matches libbf BF_RNDD/BF_RNDU exactly.

-- ── float8 → bits ────────────────────────────────────────────

CREATE OR REPLACE FUNCTION qjson_float8_to_bits(x float8)
RETURNS bigint AS $$
    SELECT ('x' || encode(float8send(x), 'hex'))::bit(64)::bigint;
$$ LANGUAGE sql IMMUTABLE STRICT;

-- ── bits → float8 (exact reconstruction) ─────────────────────
-- Every step is exact in IEEE 754:
--   frac::float8           exact (frac < 2^52)
--   frac / 2^52            exact (division by power of 2)
--   1.0 + frac             exact (frac < 1, fits in 53 bits)
--   * 2^(exp-1023)         exact (multiplication by power of 2)

CREATE OR REPLACE FUNCTION qjson_bits_to_float8(bits bigint)
RETURNS float8 AS $$
DECLARE
    sign int := (bits >> 63) & 1;
    exp int := ((bits >> 52) & 2047);
    frac bigint := bits & ((1::bigint << 52) - 1);
BEGIN
    IF exp = 2047 THEN
        IF frac = 0 THEN
            RETURN (CASE WHEN sign = 1 THEN '-Infinity' ELSE 'Infinity' END)::float8;
        END IF;
        RETURN 'NaN'::float8;
    END IF;

    IF exp = 0 AND frac = 0 THEN RETURN 0.0; END IF;

    IF exp = 0 THEN
        -- Denormalized: frac * 2^-1074
        RETURN (CASE WHEN sign = 1 THEN -1.0 ELSE 1.0 END)
             * frac::float8 * power(2.0::float8, -1074);
    END IF;

    -- Normalized: (-1)^sign * 2^(exp-1023) * (1 + frac/2^52)
    RETURN (CASE WHEN sign = 1 THEN -1.0 ELSE 1.0 END)
         * power(2.0::float8, exp - 1023)
         * (1.0 + frac::float8 / 4503599627370496.0);
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

-- ── nextup / nextdown ────────────────────────────────────────
-- Positive: increment bits. Negative: decrement bits.
-- Works for normalized, denormalized, zero, infinity.

CREATE OR REPLACE FUNCTION qjson_nextup(x float8)
RETURNS float8 AS $$
DECLARE
    bits bigint;
BEGIN
    IF x != x THEN RETURN x; END IF;
    IF x = 'Infinity'::float8 THEN RETURN x; END IF;
    IF x = 0.0 THEN RETURN 5e-324; END IF;
    bits := qjson_float8_to_bits(x);
    IF x > 0.0 THEN RETURN qjson_bits_to_float8(bits + 1);
    ELSE RETURN qjson_bits_to_float8(bits - 1);
    END IF;
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION qjson_nextdown(x float8)
RETURNS float8 AS $$
DECLARE
    bits bigint;
BEGIN
    IF x != x THEN RETURN x; END IF;
    IF x = '-Infinity'::float8 THEN RETURN x; END IF;
    IF x = 0.0 THEN RETURN -5e-324; END IF;
    bits := qjson_float8_to_bits(x);
    IF x > 0.0 THEN RETURN qjson_bits_to_float8(bits - 1);
    ELSE RETURN qjson_bits_to_float8(bits + 1);
    END IF;
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

-- ── Exact decimal of a float8 ────────────────────────────────
-- Compute the EXACT decimal representation of an IEEE 754 double
-- using NUMERIC integer arithmetic (no floating-point rounding).

CREATE OR REPLACE FUNCTION qjson_float8_exact(x float8)
RETURNS numeric AS $$
DECLARE
    bits bigint := qjson_float8_to_bits(x);
    sign int := (bits >> 63) & 1;
    exp int := ((bits >> 52) & 2047);
    frac bigint := bits & ((1::bigint << 52) - 1);
    significand numeric;
    e int;
    result numeric;
BEGIN
    IF exp = 0 AND frac = 0 THEN RETURN 0; END IF;

    IF exp = 0 THEN
        -- Denormalized: frac * 2^-1074
        significand := frac::numeric;
        e := -1074;
    ELSE
        -- Normalized: (2^52 + frac) * 2^(exp - 1075)
        significand := 4503599627370496::numeric + frac::numeric;
        e := exp - 1075;
    END IF;

    IF e >= 0 THEN
        result := significand * power(2::numeric, e);
    ELSE
        -- significand / 2^|e| = significand * 5^|e| / 10^|e|
        -- Multiply by 5^|e| (exact integer arithmetic), then shift
        -- decimal point by dividing by 10^|e|.
        -- NUMERIC division truncates, so use string manipulation instead.
        DECLARE
            scaled text := (significand * power(5::numeric, -e))::text;
            point_pos int := length(scaled) + e;  -- e is negative
        BEGIN
            IF point_pos <= 0 THEN
                result := ('0.' || repeat('0', -point_pos) || scaled)::numeric;
            ELSE
                result := (left(scaled, point_pos) || '.' || substr(scaled, point_pos + 1))::numeric;
            END IF;
        END;
    END IF;

    IF sign = 1 THEN result := -result; END IF;
    RETURN result;
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

-- ── round_down / round_up ────────────────────────────────────
-- Largest double ≤ exact value / smallest double ≥ exact value.
-- Uses qjson_float8_exact for correct comparison direction.

CREATE OR REPLACE FUNCTION qjson_round_down(raw TEXT)
RETURNS float8 AS $$
DECLARE
    v float8;
    exact numeric;
    v_exact numeric;
BEGIN
    -- Handle overflow
    BEGIN
        v := raw::float8;
    EXCEPTION WHEN OTHERS THEN
        -- Overflow: raw > DBL_MAX → round_down = DBL_MAX
        IF raw::numeric > 0 THEN RETURN 1.7976931348623157e308;
        ELSE RETURN '-Infinity'::float8;
        END IF;
    END;
    IF v != v THEN RETURN v; END IF;

    exact := raw::numeric;
    v_exact := qjson_float8_exact(v);

    IF v_exact <= exact THEN RETURN v; END IF;
    RETURN qjson_nextdown(v);
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION qjson_round_up(raw TEXT)
RETURNS float8 AS $$
DECLARE
    v float8;
    exact numeric;
    v_exact numeric;
BEGIN
    BEGIN
        v := raw::float8;
    EXCEPTION WHEN OTHERS THEN
        IF raw::numeric < 0 THEN RETURN -1.7976931348623157e308;
        ELSE RETURN 'Infinity'::float8;
        END IF;
    END;
    IF v != v THEN RETURN v; END IF;

    exact := raw::numeric;
    v_exact := qjson_float8_exact(v);

    IF v_exact >= exact THEN RETURN v; END IF;
    RETURN qjson_nextup(v);
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;
