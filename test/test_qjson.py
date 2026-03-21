#!/usr/bin/env python3
# ============================================================
# test_qjson.py — Tests for QJSON parser/serializer
# ============================================================

import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from decimal import Decimal
from qjson import parse, stringify, BigInt, BigFloat, Blob, js64_encode, js64_decode

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

print("\n%d tests: %d passed, %d failed" % (passed + failed, passed, failed))
if failed:
    sys.exit(1)
