#!/usr/bin/env python3
# ============================================================
# test_qjson.py — Tests for QJSON parser/serializer
# ============================================================

import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from decimal import Decimal
from qjson import parse, stringify, BigInt, BigFloat, Blob, Unbound, QMap, is_json, is_bound, js64_encode, js64_decode

passed = 0
failed = 0

def test(name, fn):
    global passed, failed
    try:
        fn()
        passed += 1
        print("  \u2713 " + name)
    except Exception as e:
        failed += 1
        print("  \u2717 " + name + ": " + str(e))


# ── Backward compat: valid JSON parses correctly ─────────────

def test_json_compat():
    assert parse('42') == 42
    assert parse('-3.14') == -3.14
    assert parse('"hello"') == "hello"
    assert parse('true') is True
    assert parse('false') is False
    assert parse('null') is None
    assert parse('{"a":1,"b":[2,3]}') == {"a": 1, "b": [2, 3]}
    assert parse('[]') == []
    assert parse('{}') == {}

def test_string_escapes():
    assert parse(r'"hello\nworld"') == "hello\nworld"
    assert parse(r'"tab\there"') == "tab\there"
    assert parse(r'"quote\""') == 'quote"'
    assert parse(r'"slash\\"') == "slash\\"
    assert parse(r'"unicode\u0041"') == "unicodeA"

# ── Comments ─────────────────────────────────────────────────

def test_line_comments():
    assert parse('// leading\n42') == 42
    assert parse('42 // trailing') == 42
    assert parse('{\n// comment\n"a": 1\n}') == {"a": 1}

def test_block_comments():
    assert parse('/* before */ 42') == 42
    assert parse('42 /* after */') == 42
    assert parse('{"a": /* inline */ 1}') == {"a": 1}

def test_mixed_comments():
    text = """
    {
      // line comment
      "x": 1,
      /* block
         comment */
      "y": 2
    }
    """
    assert parse(text) == {"x": 1, "y": 2}

def test_nested_block_comments():
    assert parse('/* outer /* inner */ still comment */ 42') == 42
    text = '{ /* a /* b */ c */ "x": 1 }'
    assert parse(text) == {"x": 1}

# ── Trailing commas ──────────────────────────────────────────

def test_trailing_comma_object():
    assert parse('{"a": 1, "b": 2,}') == {"a": 1, "b": 2}

def test_trailing_comma_array():
    assert parse('[1, 2, 3,]') == [1, 2, 3]

def test_trailing_comma_nested():
    assert parse('{"a": [1, 2,], "b": {"c": 3,},}') == {"a": [1, 2], "b": {"c": 3}}

# ── Unquoted keys ────────────────────────────────────────────

def test_unquoted_keys():
    assert parse('{name: "alice", age: 30}') == {"name": "alice", "age": 30}

def test_unquoted_keys_underscore():
    assert parse('{_id: 1, $ref: "x"}') == {"_id": 1, "$ref": "x"}

def test_mixed_keys():
    assert parse('{"quoted": 1, bare: 2}') == {"quoted": 1, "bare": 2}

# ── BigInt ───────────────────────────────────────────────────

def test_bigint_parse():
    v = parse('42N')
    assert isinstance(v, BigInt)
    assert v == 42

def test_bigint_large():
    v = parse('98765432101234567890N')
    assert isinstance(v, BigInt)
    assert v == 98765432101234567890

def test_bigint_negative():
    v = parse('-123N')
    assert isinstance(v, BigInt)
    assert v == -123

def test_bigint_in_object():
    v = parse('{"nonce": 42n, "name": "test"}')
    assert isinstance(v["nonce"], BigInt)
    assert v["nonce"] == 42
    assert v["name"] == "test"

# ── BigDecimal ───────────────────────────────────────────────

def test_bigdecimal_parse():
    v = parse('3.14M')
    assert isinstance(v, Decimal)
    assert v == Decimal("3.14")

def test_bigdecimal_integer():
    v = parse('100M')
    assert isinstance(v, Decimal)
    assert v == Decimal("100")

def test_bigdecimal_precision():
    v = parse('3.141592653589793238462643383279M')
    assert isinstance(v, Decimal)
    assert str(v) == '3.141592653589793238462643383279'

def test_bigdecimal_negative():
    v = parse('-0.001M')
    assert isinstance(v, Decimal)
    assert v == Decimal("-0.001")

# ── BigFloat ─────────────────────────────────────────────────

def test_bigfloat_parse():
    v = parse('3.14L')
    assert isinstance(v, BigFloat)
    assert float(v) == 3.14

def test_bigfloat_precision():
    v = parse('3.141592653589793238462643383279L')
    assert isinstance(v, BigFloat)
    assert str(v) == '3.141592653589793238462643383279'

def test_bigfloat_negative():
    v = parse('-1.5L')
    assert isinstance(v, BigFloat)
    assert float(v) == -1.5

def test_bigfloat_integer_form():
    v = parse('100L')
    assert isinstance(v, BigFloat)
    assert float(v) == 100.0

def test_bigint_lowercase_accepted():
    v = parse('99n')
    assert isinstance(v, BigInt)
    assert stringify(v) == '99N'

def test_bigdecimal_lowercase_accepted():
    v = parse('1.5m')
    assert isinstance(v, Decimal)
    assert stringify(v) == '1.5M'

def test_bigfloat_lowercase_accepted():
    v = parse('2.718l')
    assert isinstance(v, BigFloat)
    assert stringify(v) == '2.718L'

# ── Serializer ───────────────────────────────────────────────

def test_stringify_basic():
    assert stringify(42) == '42'
    assert stringify("hi") == '"hi"'
    assert stringify(True) == 'true'
    assert stringify(None) == 'null'
    assert stringify([1, 2]) == '[1,2]'
    assert stringify({"a": 1}) == '{"a":1}'

def test_stringify_bigint():
    assert stringify(BigInt(42)) == '42N'
    assert stringify(BigInt(-99)) == '-99N'
    assert stringify(BigInt(98765432101234567890)) == '98765432101234567890N'

def test_stringify_bigdecimal():
    assert stringify(Decimal("3.14")) == '3.14M'
    assert stringify(Decimal("-0.001")) == '-0.001M'

def test_stringify_bigfloat():
    assert stringify(BigFloat("3.14")) == '3.14L'
    assert stringify(BigFloat("3.141592653589793238462643383279")) == '3.141592653589793238462643383279L'

def test_stringify_nested():
    obj = {"amount": Decimal("99.99"), "count": BigInt(7), "tags": ["a", "b"]}
    s = stringify(obj)
    assert '"amount":99.99M' in s
    assert '"count":7N' in s

# ── Round-trip ───────────────────────────────────────────────

def test_roundtrip_bigint():
    v = BigInt(12345678901234567890)
    assert parse(stringify(v)) == v

def test_roundtrip_bigdecimal():
    v = Decimal("3.141592653589793238462643383279")
    assert parse(stringify(v)) == v

def test_roundtrip_bigfloat():
    v = BigFloat("3.141592653589793238462643383279")
    assert parse(stringify(v)) == v

def test_roundtrip_complex():
    obj = {
        "n": BigInt(42),
        "d": Decimal("1.5"),
        "s": "hello",
        "a": [1, BigInt(2), Decimal("3.0")],
        "nested": {"ok": True}
    }
    assert parse(stringify(obj)) == obj

def test_roundtrip_regular_json():
    obj = {"a": 1, "b": [2, 3.5, "x", None, True, False]}
    assert parse(stringify(obj)) == obj


# ── Run ──────────────────────────────────────────────────────

print("qjson.py")
test("JSON backward compat", test_json_compat)
test("string escapes", test_string_escapes)
test("line comments", test_line_comments)
test("block comments", test_block_comments)
test("mixed comments", test_mixed_comments)
test("nested block comments", test_nested_block_comments)
test("trailing comma object", test_trailing_comma_object)
test("trailing comma array", test_trailing_comma_array)
test("trailing comma nested", test_trailing_comma_nested)
test("unquoted keys", test_unquoted_keys)
test("unquoted keys _ and $", test_unquoted_keys_underscore)
test("mixed quoted/bare keys", test_mixed_keys)
test("BigInt parse", test_bigint_parse)
test("BigInt large", test_bigint_large)
test("BigInt negative", test_bigint_negative)
test("BigInt in object", test_bigint_in_object)
test("BigDecimal parse", test_bigdecimal_parse)
test("BigDecimal integer form", test_bigdecimal_integer)
test("BigDecimal precision", test_bigdecimal_precision)
test("BigDecimal negative", test_bigdecimal_negative)
test("BigFloat parse", test_bigfloat_parse)
test("BigFloat precision", test_bigfloat_precision)
test("BigFloat negative", test_bigfloat_negative)
test("BigFloat integer form", test_bigfloat_integer_form)
test("BigInt lowercase n accepted", test_bigint_lowercase_accepted)
test("BigDecimal lowercase m accepted", test_bigdecimal_lowercase_accepted)
test("BigFloat lowercase l accepted", test_bigfloat_lowercase_accepted)
test("stringify basic", test_stringify_basic)
test("stringify BigInt", test_stringify_bigint)
test("stringify BigDecimal", test_stringify_bigdecimal)
test("stringify BigFloat", test_stringify_bigfloat)
test("stringify nested", test_stringify_nested)
test("round-trip BigInt", test_roundtrip_bigint)
test("round-trip BigDecimal", test_roundtrip_bigdecimal)
test("round-trip BigFloat", test_roundtrip_bigfloat)
test("round-trip complex", test_roundtrip_complex)
test("round-trip regular JSON", test_roundtrip_regular_json)

# ── Blob / JS64 tests ──────────────────────────────────────

def test_js64_roundtrip():
    hello = b"\x48\x65\x6c\x6c\x6f"  # "Hello"
    enc = js64_encode(hello)
    dec = js64_decode(enc)
    assert dec == hello, "round-trip Hello"

def test_js64_empty():
    enc = js64_encode(b"")
    assert enc == ""
    dec = js64_decode("")
    assert dec == b""

def test_js64_single_byte():
    enc = js64_encode(b"\xff")
    dec = js64_decode(enc)
    assert dec == b"\xff"

def test_blob_parse():
    hello = b"\x48\x65\x6c\x6c\x6f"
    enc = js64_encode(hello)
    obj = parse("0j" + enc)
    assert isinstance(obj, Blob)
    assert obj.data == hello

def test_blob_parse_uppercase():
    enc = js64_encode(b"\x48\x65")
    obj = parse("0J" + enc)
    assert isinstance(obj, Blob)
    assert obj.data == b"\x48\x65"

def test_blob_in_object():
    enc = js64_encode(b"\x01\x02\x03")
    obj = parse("{key: 0j" + enc + "}")
    assert isinstance(obj["key"], Blob)
    assert obj["key"].data == b"\x01\x02\x03"

def test_blob_stringify_roundtrip():
    hello = b"\x48\x65\x6c\x6c\x6f"
    text = stringify(Blob(hello))
    assert text.startswith("0j"), "starts with 0j"
    rt = parse(text)
    assert isinstance(rt, Blob)
    assert rt.data == hello

def test_blob_empty():
    obj = parse("0j")
    assert isinstance(obj, Blob)
    assert obj.data == b""

test("JS64 round-trip", test_js64_roundtrip)
test("JS64 empty", test_js64_empty)
test("JS64 single byte", test_js64_single_byte)
test("blob parse", test_blob_parse)
test("blob parse uppercase", test_blob_parse_uppercase)
test("blob in object", test_blob_in_object)
test("blob stringify round-trip", test_blob_stringify_roundtrip)
test("blob empty", test_blob_empty)

# ── Unbound ──────────────────────────────────────────────────

def test_unbound_parse_bare():
    v = parse('?X')
    assert isinstance(v, Unbound)
    assert v.name == 'X'

def test_unbound_parse_anonymous():
    v = parse('?')
    assert isinstance(v, Unbound)
    assert v.name == ''

def test_unbound_parse_underscore():
    v = parse('?_')
    assert isinstance(v, Unbound)
    assert v.name == '_'

def test_unbound_anon_vs_underscore():
    """? and ?_ are distinct."""
    anon = parse('?')
    under = parse('?_')
    assert anon.name == ''
    assert under.name == '_'
    assert anon != under

def test_unbound_parse_multichar():
    v = parse('?myVar_1')
    assert isinstance(v, Unbound)
    assert v.name == 'myVar_1'

def test_unbound_parse_quoted():
    v = parse('?"Bob\'s Last Memo"')
    assert isinstance(v, Unbound)
    assert v.name == "Bob's Last Memo"

def test_unbound_in_array():
    v = parse('["reading", ?From, "temp", ?Val]')
    assert len(v) == 4
    assert isinstance(v[1], Unbound) and v[1].name == 'From'
    assert isinstance(v[3], Unbound) and v[3].name == 'Val'

def test_unbound_in_object():
    v = parse('{key: ?X, value: 42}')
    assert isinstance(v['key'], Unbound) and v['key'].name == 'X'
    assert v['value'] == 42

def test_unbound_stringify_bare():
    assert stringify(Unbound('X')) == '?X'

def test_unbound_stringify_anonymous():
    assert stringify(Unbound('')) == '?'

def test_unbound_stringify_underscore():
    assert stringify(Unbound('_')) == '?_'

def test_unbound_stringify_quoted():
    s = stringify(Unbound("Bob's Last Memo"))
    assert s.startswith('?')
    assert s[1] == '"'

def test_unbound_roundtrip_bare():
    v = Unbound('myVar')
    rt = parse(stringify(v))
    assert isinstance(rt, Unbound)
    assert rt.name == 'myVar'

def test_unbound_roundtrip_quoted():
    v = Unbound("Bob's Last Memo")
    rt = parse(stringify(v))
    assert isinstance(rt, Unbound)
    assert rt.name == "Bob's Last Memo"

def test_unbound_equality():
    assert Unbound('X') == Unbound('X')
    assert Unbound('X') != Unbound('Y')

test("Unbound parse bare", test_unbound_parse_bare)
test("Unbound parse anonymous", test_unbound_parse_anonymous)
test("Unbound parse underscore", test_unbound_parse_underscore)
test("Unbound anon vs underscore", test_unbound_anon_vs_underscore)
test("Unbound parse multi-char", test_unbound_parse_multichar)
test("Unbound parse quoted", test_unbound_parse_quoted)
test("Unbound in array", test_unbound_in_array)
test("Unbound in object", test_unbound_in_object)
test("Unbound stringify bare", test_unbound_stringify_bare)
test("Unbound stringify anonymous", test_unbound_stringify_anonymous)
test("Unbound stringify underscore", test_unbound_stringify_underscore)
test("Unbound stringify quoted", test_unbound_stringify_quoted)
test("Unbound round-trip bare", test_unbound_roundtrip_bare)
test("Unbound round-trip quoted", test_unbound_roundtrip_quoted)
test("Unbound equality", test_unbound_equality)

# ── Complex keys + set shorthand tests ──────────────────────

def test_bare_ident_value():
    assert parse('alice') == 'alice'
    assert parse('truthy') == 'truthy'
    assert parse('nullable') == 'nullable'

def test_bare_ident_in_array():
    assert parse('[alice, bob]') == ['alice', 'bob']

def test_set_shorthand_strings():
    v = parse('{alice, bob, carol}')
    assert v == {'alice': True, 'bob': True, 'carol': True}

def test_set_shorthand_numbers():
    v = parse('{1, 2, 3}')
    assert isinstance(v, QMap)
    assert all(val is True for _, val in v.entries)
    assert [k for k, _ in v.entries] == [1, 2, 3]

def test_complex_key_number():
    v = parse('{42: "answer"}')
    assert isinstance(v, QMap)
    assert v[42] == 'answer'

def test_complex_key_array():
    v = parse('{[1,2]: "pair"}')
    assert isinstance(v, QMap)
    assert v.entries[0] == ([1, 2], 'pair')

def test_set_shorthand_tuples():
    v = parse('{[alice, bob], [bob, carol]}')
    assert isinstance(v, QMap)
    assert len(v.entries) == 2
    assert v.entries[0] == (['alice', 'bob'], True)
    assert v.entries[1] == (['bob', 'carol'], True)

def test_complex_key_stringify():
    m = QMap([(42, 'answer')])
    assert stringify(m) == '{42:"answer"}'

def test_set_shorthand_stringify():
    m = QMap([([1, 2], True), ([3, 4], True)])
    assert stringify(m) == '{[1,2],[3,4]}'

def test_set_shorthand_roundtrip():
    text = '{[1,2],[3,4]}'
    v = parse(text)
    assert stringify(v) == text

def test_qmap_equality():
    a = QMap([(42, 'x')])
    b = QMap([(42, 'x')])
    assert a == b

def test_qmap_dict_equality():
    m = QMap([('a', 1), ('b', 2)])
    assert m == {'a': 1, 'b': 2}

def test_mixed_keys():
    v = parse('{name: 1, 42: 2}')
    assert isinstance(v, QMap)
    assert v['name'] == 1
    assert v[42] == 2

def test_boolean_key():
    v = parse('{true: 1}')
    assert isinstance(v, QMap)
    assert v[True] == 1

def test_is_json_qmap():
    assert not is_json(QMap([(42, 'x')]))
    assert is_json(QMap([('a', 1), ('b', 2)]))

def test_is_bound_qmap():
    assert is_bound(QMap([(42, 'x')]))
    assert not is_bound(QMap([(Unbound('X'), 1)]))

test("bare ident as value", test_bare_ident_value)
test("bare ident in array", test_bare_ident_in_array)
test("set shorthand strings", test_set_shorthand_strings)
test("set shorthand numbers", test_set_shorthand_numbers)
test("complex key number", test_complex_key_number)
test("complex key array", test_complex_key_array)
test("set shorthand tuples", test_set_shorthand_tuples)
test("complex key stringify", test_complex_key_stringify)
test("set shorthand stringify", test_set_shorthand_stringify)
test("set shorthand round-trip", test_set_shorthand_roundtrip)
test("QMap equality", test_qmap_equality)
test("QMap dict equality", test_qmap_dict_equality)
test("mixed string/number keys", test_mixed_keys)
test("boolean key", test_boolean_key)
test("is_json QMap", test_is_json_qmap)
test("is_bound QMap", test_is_bound_qmap)

def test_null_key():
    v = parse('{null: 1}')
    assert isinstance(v, QMap)
    assert v[None] == 1

def test_object_key():
    v = parse('{{a: 1}: "nested"}')
    assert isinstance(v, QMap)
    assert v.entries[0][0] == {'a': 1}
    assert v.entries[0][1] == 'nested'

def test_trailing_comma_set():
    v = parse('{a, b,}')
    assert v == {'a': True, 'b': True}

def test_single_element_set():
    v = parse('{alice}')
    assert v == {'alice': True}

def test_nested_set():
    v = parse('{items: {a, b, c}}')
    assert v['items'] == {'a': True, 'b': True, 'c': True}

def test_set_in_array():
    v = parse('[{x, y}, {a, b}]')
    assert v[0] == {'x': True, 'y': True}
    assert v[1] == {'a': True, 'b': True}

def test_keyword_in_set():
    """true/false/null as set keys are booleans/null, not strings."""
    v = parse('{true, false, null}')
    assert isinstance(v, QMap)
    assert len(v.entries) == 3
    keys = [k for k, _ in v.entries]
    assert True in keys
    assert False in keys
    assert None in keys

def test_set_stringify_keyword_keys():
    m = QMap([(True, True), (False, True)])
    s = stringify(m)
    assert s == '{true,false}'

def test_complex_key_round_trip_sql():
    """QMap round-trips through SQL adapter."""
    import sqlite3
    from qjson_sql import qjson_sql_adapter
    conn = sqlite3.connect(':memory:')
    a = qjson_sql_adapter(conn)
    a['setup']()
    m = QMap([(42, 'answer'), ([1, 2], 'pair')])
    vid = a['store'](m)
    a['commit']()
    v = a['load'](vid)
    assert isinstance(v, QMap)
    assert len(v.entries) == 2

def test_set_round_trip_sql():
    """String-key sets round-trip through SQL as dicts."""
    import sqlite3
    from qjson_sql import qjson_sql_adapter
    conn = sqlite3.connect(':memory:')
    a = qjson_sql_adapter(conn)
    a['setup']()
    d = {'alice': True, 'bob': True}
    vid = a['store'](d)
    a['commit']()
    v = a['load'](vid)
    assert v == d

def test_bare_ident_not_in_quoted_string():
    """Bare idents don't interfere with quoted strings."""
    v = parse('{"alice": "bob"}')
    assert v == {'alice': 'bob'}

def test_complex_key_stringify_round_trip():
    """Complex key map syntax round-trips."""
    m = QMap([(42, 'answer'), ('name', 'alice')])
    s = stringify(m)
    v = parse(s)
    assert isinstance(v, QMap)
    assert len(v.entries) == 2

test("null key", test_null_key)
test("object as key", test_object_key)
test("trailing comma set", test_trailing_comma_set)
test("single element set", test_single_element_set)
test("nested set", test_nested_set)
test("set in array", test_set_in_array)
test("keyword keys in set", test_keyword_in_set)
test("set stringify keyword keys", test_set_stringify_keyword_keys)
test("complex key SQL round-trip", test_complex_key_round_trip_sql)
test("set SQL round-trip", test_set_round_trip_sql)
test("bare ident with quoted string", test_bare_ident_not_in_quoted_string)
test("complex key stringify round-trip", test_complex_key_stringify_round_trip)

print("\n%d tests: %d passed, %d failed" % (passed + failed, passed, failed))
if failed:
    sys.exit(1)
