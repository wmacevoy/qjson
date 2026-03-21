// ============================================================
// test-qjson.js — Tests for QJSON parser/serializer
//
// Run:  node src/test-qjson.js
//       bun run src/test-qjson.js
// ============================================================

import { qjson_parse, qjson_stringify, js64_encode, js64_decode } from '../src/qjson.js';

var passed = 0, failed = 0;
var hasBigInt = typeof BigInt !== "undefined";

function test(name, fn) {
  try { fn(); passed++; console.log("  \u2713 " + name); }
  catch(e) { failed++; console.log("  \u2717 " + name + ": " + e.message); }
}
function assert(cond, msg) { if (!cond) throw new Error(msg || "assertion failed"); }
function eq(a, b, msg) { assert(a === b, msg || "expected " + b + ", got " + a); }

// ── JSON backward compat ────────────────────────────────────

console.log("qjson.js");

test("JSON compat: primitives", function() {
  eq(qjson_parse("42"), 42);
  eq(qjson_parse("-3.14"), -3.14);
  eq(qjson_parse('"hello"'), "hello");
  eq(qjson_parse("true"), true);
  eq(qjson_parse("false"), false);
  eq(qjson_parse("null"), null);
});

test("JSON compat: objects and arrays", function() {
  var obj = qjson_parse('{"a":1,"b":[2,3]}');
  eq(obj.a, 1);
  eq(obj.b.length, 2);
  eq(obj.b[0], 2);
  var arr = qjson_parse("[]");
  eq(arr.length, 0);
});

test("string escapes", function() {
  eq(qjson_parse('"hello\\nworld"'), "hello\nworld");
  eq(qjson_parse('"tab\\there"'), "tab\there");
  eq(qjson_parse('"quote\\""'), 'quote"');
  eq(qjson_parse('"unicode\\u0041"'), "unicodeA");
});

// ── Comments ────────────────────────────────────────────────

test("line comments", function() {
  eq(qjson_parse("// leading\n42"), 42);
  eq(qjson_parse("42 // trailing"), 42);
  var obj = qjson_parse('{\n// comment\n"a": 1\n}');
  eq(obj.a, 1);
});

test("block comments", function() {
  eq(qjson_parse("/* before */ 42"), 42);
  eq(qjson_parse("42 /* after */"), 42);
  var obj = qjson_parse('{"a": /* inline */ 1}');
  eq(obj.a, 1);
});

test("mixed comments", function() {
  var obj = qjson_parse('{\n  // line\n  "x": 1,\n  /* block */\n  "y": 2\n}');
  eq(obj.x, 1);
  eq(obj.y, 2);
});

test("nested block comments", function() {
  eq(qjson_parse("/* outer /* inner */ still */ 42"), 42);
});

test("trailing commas", function() {
  var obj = qjson_parse('{"a": 1, "b": 2,}');
  eq(obj.a, 1); eq(obj.b, 2);
  var arr = qjson_parse("[1, 2, 3,]");
  eq(arr.length, 3); eq(arr[2], 3);
});

test("unquoted keys", function() {
  var obj = qjson_parse('{name: "alice", age: 30}');
  eq(obj.name, "alice"); eq(obj.age, 30);
});

test("unquoted keys _ and $", function() {
  var obj = qjson_parse('{_id: 1, $ref: "x"}');
  eq(obj._id, 1); eq(obj.$ref, "x");
});

test("mixed quoted/bare keys", function() {
  var obj = qjson_parse('{"quoted": 1, bare: 2}');
  eq(obj.quoted, 1); eq(obj.bare, 2);
});

// ── BigInt ──────────────────────────────────────────────────

test("BigInt parse", function() {
  var v = qjson_parse("42N");
  if (hasBigInt) {
    eq(typeof v, "bigint");
    assert(v === BigInt(42), "expected 42n");
  } else {
    eq(v, "42N");  // fallback: string
  }
});

test("BigInt negative", function() {
  var v = qjson_parse("-123N");
  if (hasBigInt) {
    assert(v === BigInt(-123), "expected -123n");
  }
});

test("BigInt in object", function() {
  var obj = qjson_parse('{"nonce": 42n, "name": "test"}');
  eq(obj.name, "test");
  if (hasBigInt) {
    eq(typeof obj.nonce, "bigint");
  }
});

// ── BigDecimal ──────────────────────────────────────────────

test("BigDecimal parse", function() {
  var v = qjson_parse("3.14M");
  // On QuickJS: bigdecimal. On Node: string fallback.
  if (typeof BigDecimal !== "undefined") {
    eq(typeof v, "bigdecimal");
  } else {
    eq(v, "3.14M");
  }
});

test("BigDecimal integer form", function() {
  var v = qjson_parse("100M");
  if (typeof BigDecimal !== "undefined") {
    eq(typeof v, "bigdecimal");
  } else {
    eq(v, "100M");
  }
});

// ── BigFloat ────────────────────────────────────────────────

test("BigFloat parse", function() {
  var v = qjson_parse("3.14L");
  // On QuickJS: bigfloat. On Node/Bun: string fallback.
  if (typeof BigFloat !== "undefined") {
    eq(typeof v, "bigfloat");
  } else {
    eq(v, "3.14L");
  }
});

test("BigFloat precision preserved", function() {
  var v = qjson_parse("3.141592653589793238462643383279L");
  if (typeof BigFloat !== "undefined") {
    eq(typeof v, "bigfloat");
  } else {
    eq(v, "3.141592653589793238462643383279L");
  }
});

test("lowercase n/m/l all accepted", function() {
  // BigInt lowercase
  var bi = qjson_parse("99n");
  if (hasBigInt) {
    assert(bi === BigInt(99), "99n should parse as BigInt");
  } else {
    eq(bi, "99N");
  }
  // BigDecimal lowercase
  var bd = qjson_parse("1.5m");
  if (typeof BigDecimal !== "undefined") {
    eq(typeof bd, "bigdecimal");
  } else {
    eq(bd, "1.5M");
  }
  // BigFloat lowercase
  var bf = qjson_parse("2.718l");
  if (typeof BigFloat !== "undefined") {
    eq(typeof bf, "bigfloat");
  } else {
    eq(bf, "2.718L");
  }
});

// ── Serializer ──────────────────────────────────────────────

test("stringify basic", function() {
  eq(qjson_stringify(42), "42");
  eq(qjson_stringify("hi"), '"hi"');
  eq(qjson_stringify(true), "true");
  eq(qjson_stringify(null), "null");
  eq(qjson_stringify([1, 2]), "[1,2]");
  eq(qjson_stringify({a: 1}), '{"a":1}');
});

test("stringify BigInt", function() {
  if (!hasBigInt) return;  // skip on runtimes without BigInt
  eq(qjson_stringify(BigInt(42)), "42N");
  eq(qjson_stringify(BigInt(-99)), "-99N");
});

test("stringify nested", function() {
  if (!hasBigInt) return;
  var s = qjson_stringify({count: BigInt(7), tags: ["a"]});
  assert(s.indexOf("7N") > -1, "should contain 7N");
  assert(s.indexOf('"a"') > -1, "should contain tag");
});

// ── Round-trip ──────────────────────────────────────────────

test("round-trip BigInt", function() {
  if (!hasBigInt) return;
  var v = BigInt(12345678901234567890);
  var rt = qjson_parse(qjson_stringify(v));
  assert(rt === v, "BigInt round-trip failed");
});

test("round-trip regular JSON", function() {
  var obj = {a: 1, b: [2, 3.5, "x", null, true, false]};
  var rt = qjson_parse(qjson_stringify(obj));
  eq(rt.a, 1);
  eq(rt.b[1], 3.5);
  eq(rt.b[2], "x");
  eq(rt.b[3], null);
});

test("round-trip complex", function() {
  if (!hasBigInt) return;
  var obj = {n: BigInt(42), s: "hello", a: [1, BigInt(2)], nested: {ok: true}};
  var rt = qjson_parse(qjson_stringify(obj));
  assert(rt.n === BigInt(42), "n should be 42n");
  eq(rt.s, "hello");
  assert(rt.a[1] === BigInt(2), "a[1] should be 2n");
  eq(rt.nested.ok, true);
});

// ── Blob / JS64 tests ──────────────────────────────────────

test("JS64 encode/decode round-trip", function() {
  var hello = [0x48, 0x65, 0x6c, 0x6c, 0x6f]; // "Hello"
  var enc = js64_encode(hello);
  var dec = js64_decode(enc);
  eq(dec.length, 5);
  eq(dec[0], 0x48);
  eq(dec[4], 0x6f);
});

test("JS64 empty round-trip", function() {
  var enc = js64_encode([]);
  eq(enc, "");
  var dec = js64_decode("");
  eq(dec.length, 0);
});

test("JS64 single byte 0xFF", function() {
  var enc = js64_encode([0xFF]);
  var dec = js64_decode(enc);
  eq(dec.length, 1);
  eq(dec[0], 0xFF);
});

test("blob parse 0j", function() {
  var hello = [0x48, 0x65, 0x6c, 0x6c, 0x6f];
  var enc = js64_encode(hello);
  var obj = qjson_parse("0j" + enc);
  eq(obj.$qjson, "blob");
  eq(obj.data.length, 5);
  eq(obj.data[0], 0x48);
  eq(obj.data[4], 0x6f);
});

test("blob parse 0J (uppercase)", function() {
  var enc = js64_encode([0x48, 0x65]);
  var obj = qjson_parse("0J" + enc);
  eq(obj.$qjson, "blob");
  eq(obj.data.length, 2);
});

test("blob in object", function() {
  var enc = js64_encode([1, 2, 3]);
  var obj = qjson_parse("{key: 0j" + enc + "}");
  eq(obj.key.$qjson, "blob");
  eq(obj.key.data.length, 3);
  eq(obj.key.data[0], 1);
});

test("blob stringify round-trip", function() {
  var hello = [0x48, 0x65, 0x6c, 0x6c, 0x6f];
  var obj = { $qjson: "blob", data: hello };
  var text = qjson_stringify(obj);
  assert(text.indexOf("0j") === 0, "starts with 0j");
  var rt = qjson_parse(text);
  eq(rt.$qjson, "blob");
  eq(rt.data.length, 5);
  eq(rt.data[0], 0x48);
  eq(rt.data[4], 0x6f);
});

test("empty blob parse", function() {
  var obj = qjson_parse("0j");
  eq(obj.$qjson, "blob");
  eq(obj.data.length, 0);
});

console.log("\n" + (passed + failed) + " tests: " + passed + " passed, " + failed + " failed");
if (failed) process.exit(1);
