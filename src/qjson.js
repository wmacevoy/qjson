// ============================================================
// qjson.js — QJSON: JSON + comments + BigInt + BigDecimal + BigFloat
//
// Superset of JSON using QuickJS bignum syntax:
//   123N          → BigInt      (native if available, else string)
//   123.456M      → BigDecimal  (QuickJS native, else string)
//   3.14L         → BigFloat    (QuickJS native, else string)
//
// Uppercase preferred, lowercase accepted.  Consistent and visible.
//   // line       → comment
//   /* block */   → comment
//
// Valid JSON is valid QJSON.  No collisions.
//
// Portable: ES5 style (var, function, no arrows).
// BigInt/BigDecimal used only at runtime if the host supports them.
//
// Usage:
//   var q = qjson.parse('{"n": 42n, "d": 3.14m}');
//   var s = qjson.stringify(q);
// ============================================================

// ── JS64 blob encoding ──────────────────────────────────────

var _js64alpha = "$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
var _js64rev = null;

function _js64_init_rev() {
  _js64rev = {};
  for (var i = 0; i < _js64alpha.length; i++) {
    _js64rev[_js64alpha.charAt(i)] = i;
  }
}

function js64_decode(str) {
  // str does NOT have leading '$' — prepend it for decoding
  if (_js64rev === null) _js64_init_rev();
  var js64 = "$" + str;
  var js64len = js64.length - 1;
  var blobLen = (js64len * 3) >> 2;
  var blob = [];
  var code = 0, bits = 0, byte = 0;
  for (var i = 0; i < js64len; i++) {
    var v = _js64rev[js64.charAt(i + 1)];
    if (v === undefined) throw new Error("Invalid JS64 character");
    code = code | (v << bits);
    bits += 6;
    if (bits >= 8) {
      blob[byte] = code & 0xFF;
      code = code >> 8;
      bits -= 8;
      byte++;
    }
  }
  return blob;
}

function js64_encode(blob) {
  // Returns string WITHOUT leading '$' (for 0j prefix)
  var js64len = ((blob.length * 4 + 2) / 3) | 0;
  var parts = [];
  var code = 0, bits = 6, byte = 0;
  for (var i = 0; i <= js64len; i++) {
    var ch = _js64alpha.charAt(code & 0x3F);
    if (i > 0) parts.push(ch); // skip the leading '$'
    code = code >> 6;
    bits -= 6;
    if (bits < 6 && byte < blob.length) {
      code = code | (blob[byte] << bits);
      bits += 8;
      byte++;
    }
  }
  return parts.join("");
}

// ── Unbound type ────────────────────────────────────────────

function Unbound(name) {
  if (!(this instanceof Unbound)) return new Unbound(name);
  this.$qjson = "unbound";
  this.name = (name === undefined || name === null) ? "" : String(name);
}

// ── Parser ──────────────────────────────────────────────────

function qjson_parse(text) {
  var pos = 0;
  var len = text.length;

  function ch() { return pos < len ? text[pos] : ""; }

  function ws() {
    while (pos < len) {
      var c = text[pos];
      if (c === " " || c === "\t" || c === "\n" || c === "\r") { pos++; continue; }
      if (c === "/" && pos + 1 < len) {
        if (text[pos + 1] === "/") {
          pos += 2;
          while (pos < len && text[pos] !== "\n") pos++;
          continue;
        }
        if (text[pos + 1] === "*") {
          pos += 2;
          var depth = 1;
          while (pos + 1 < len && depth > 0) {
            if (text[pos] === "/" && text[pos + 1] === "*") { depth++; pos += 2; }
            else if (text[pos] === "*" && text[pos + 1] === "/") { depth--; pos += 2; }
            else pos++;
          }
          continue;
        }
      }
      break;
    }
  }

  function expect(c) {
    if (pos >= len || text[pos] !== c) throw new Error("Expected '" + c + "' at " + pos);
    pos++;
  }

  function ident() {
    var start = pos;
    var c = ch();
    if (!((c >= "a" && c <= "z") || (c >= "A" && c <= "Z") || c === "_" || c === "$"))
      throw new Error("Expected identifier at " + pos);
    pos++;
    while (pos < len) {
      c = text[pos];
      if ((c >= "a" && c <= "z") || (c >= "A" && c <= "Z") ||
          (c >= "0" && c <= "9") || c === "_" || c === "$") pos++;
      else break;
    }
    return text.substring(start, pos);
  }

  function key() {
    if (ch() === '"') return string();
    return ident();
  }

  function value() {
    ws();
    var c = ch();
    if (c === '"') return string();
    if (c === "{") return obj();
    if (c === "[") return arr();
    if (c === "t") return literal("true", true);
    if (c === "f") return literal("false", false);
    if (c === "n" && text.substr(pos, 4) === "null") return literal("null", null);
    if (c === "0" && pos + 1 < len && (text[pos + 1] === "j" || text[pos + 1] === "J")) return blob();
    if (c === "-" || (c >= "0" && c <= "9")) return number();
    if (c === "?") return unbound();
    throw new Error("Unexpected '" + c + "' at " + pos);
  }

  function literal(word, val) {
    if (text.substr(pos, word.length) !== word) throw new Error("Expected '" + word + "' at " + pos);
    pos += word.length;
    return val;
  }

  function string() {
    expect('"');
    var parts = [];
    while (pos < len) {
      var c = text[pos];
      if (c === '"') { pos++; return parts.join(""); }
      if (c === "\\") {
        pos++;
        var e = text[pos];
        if      (e === '"')  parts.push('"');
        else if (e === "\\") parts.push("\\");
        else if (e === "/")  parts.push("/");
        else if (e === "b")  parts.push("\b");
        else if (e === "f")  parts.push("\f");
        else if (e === "n")  parts.push("\n");
        else if (e === "r")  parts.push("\r");
        else if (e === "t")  parts.push("\t");
        else if (e === "u") {
          parts.push(String.fromCharCode(parseInt(text.substr(pos + 1, 4), 16)));
          pos += 4;
        }
        pos++;
      } else {
        parts.push(c);
        pos++;
      }
    }
    throw new Error("Unterminated string");
  }

  function number() {
    var start = pos;
    if (ch() === "-") pos++;
    while (pos < len && text[pos] >= "0" && text[pos] <= "9") pos++;
    var isFloat = false;
    if (pos < len && text[pos] === ".") {
      isFloat = true;
      pos++;
      while (pos < len && text[pos] >= "0" && text[pos] <= "9") pos++;
    }
    if (pos < len && (text[pos] === "e" || text[pos] === "E")) {
      isFloat = true;
      pos++;
      if (pos < len && (text[pos] === "+" || text[pos] === "-")) pos++;
      while (pos < len && text[pos] >= "0" && text[pos] <= "9") pos++;
    }
    var raw = text.substring(start, pos);
    // BigInt suffix (N preferred, n accepted)
    if (pos < len && (text[pos] === "N" || text[pos] === "n")) {
      pos++;
      if (typeof BigInt !== "undefined") return BigInt(raw);
      return raw + "N";
    }
    // BigDecimal suffix (M preferred, m accepted)
    if (pos < len && (text[pos] === "M" || text[pos] === "m")) {
      pos++;
      if (typeof BigDecimal !== "undefined") return BigDecimal(raw);
      return raw + "M";
    }
    // BigFloat suffix (L preferred, l accepted)
    if (pos < len && (text[pos] === "L" || text[pos] === "l")) {
      pos++;
      if (typeof BigFloat !== "undefined") return BigFloat(raw);
      return raw + "L";
    }
    // Regular number
    if (isFloat) return parseFloat(raw);
    var n = parseInt(raw, 10);
    return n;
  }

  function blob() {
    pos += 2; // skip 0j / 0J
    var start = pos;
    while (pos < len) {
      var c = text[pos];
      if ((c >= "0" && c <= "9") || (c >= "A" && c <= "Z") ||
          (c >= "a" && c <= "z") || c === "$" || c === "_") pos++;
      else break;
    }
    var raw = text.substring(start, pos);
    return { $qjson: "blob", data: js64_decode(raw) };
  }

  function unbound() {
    pos++; // skip '?'
    var name;
    if (pos < len && text[pos] === '"') {
      name = string();
    } else {
      var start = pos;
      while (pos < len) {
        var uc = text[pos];
        if ((uc >= "a" && uc <= "z") || (uc >= "A" && uc <= "Z") ||
            (uc >= "0" && uc <= "9") || uc === "_") pos++;
        else break;
      }
      name = pos > start ? text.substring(start, pos) : "";
    }
    return new Unbound(name);
  }

  function obj() {
    expect("{");
    var d = {};
    ws();
    if (ch() === "}") { pos++; return d; }
    while (true) {
      ws();
      var k = key();
      ws(); expect(":");
      d[k] = value();
      ws();
      if (ch() === "}") { pos++; return d; }
      expect(",");
      ws();
      if (ch() === "}") { pos++; return d; }  // trailing comma
    }
  }

  function arr() {
    expect("[");
    var a = [];
    ws();
    if (ch() === "]") { pos++; return a; }
    while (true) {
      a.push(value());
      ws();
      if (ch() === "]") { pos++; return a; }
      expect(",");
      ws();
      if (ch() === "]") { pos++; return a; }  // trailing comma
    }
  }

  var result = value();
  ws();
  if (pos < len) throw new Error("Trailing content at " + pos);
  return result;
}

// ── Serializer ──────────────────────────────────────────────

function qjson_stringify(obj) {
  return _fmt(obj);
}

function _fmt(obj) {
  if (obj === null || obj === undefined) return "null";
  if (obj === true)  return "true";
  if (obj === false) return "false";
  // BigInt
  if (typeof obj === "bigint") return String(obj) + "N";
  // BigDecimal (QuickJS)
  if (typeof obj === "bigdecimal") return obj.toString() + "M";
  // BigFloat (QuickJS)
  if (typeof obj === "bigfloat") return obj.toString() + "L";
  // Number
  if (typeof obj === "number") {
    if (obj !== obj || obj === Infinity || obj === -Infinity) return "null";
    return String(obj);
  }
  // Unbound
  if (typeof obj === "object" && obj !== null && obj.$qjson === "unbound") {
    var uname = obj.name;
    if (uname === "" || uname === undefined || uname === null) return "?";
    if (/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(uname)) return "?" + uname;
    return "?" + _esc(uname);
  }
  // Blob
  if (typeof obj === "object" && obj !== null && obj.$qjson === "blob") {
    return "0j" + js64_encode(obj.data);
  }
  // String
  if (typeof obj === "string") return _esc(obj);
  // Array
  if (Array.isArray(obj)) {
    if (obj.length === 0) return "[]";
    var items = [];
    for (var i = 0; i < obj.length; i++) items.push(_fmt(obj[i]));
    return "[" + items.join(",") + "]";
  }
  // Object
  if (typeof obj === "object") {
    var keys = Object.keys(obj);
    if (keys.length === 0) return "{}";
    var pairs = [];
    for (var i = 0; i < keys.length; i++) {
      pairs.push(_esc(keys[i]) + ":" + _fmt(obj[keys[i]]));
    }
    return "{" + pairs.join(",") + "}";
  }
  return String(obj);
}

function _esc(s) {
  var r = '"';
  for (var i = 0; i < s.length; i++) {
    var c = s[i];
    if      (c === '"')  r += '\\"';
    else if (c === "\\") r += "\\\\";
    else if (c === "\n") r += "\\n";
    else if (c === "\r") r += "\\r";
    else if (c === "\t") r += "\\t";
    else if (c === "\b") r += "\\b";
    else if (c === "\f") r += "\\f";
    else if (c.charCodeAt(0) < 0x20) {
      var h = c.charCodeAt(0).toString(16);
      r += "\\u" + ("0000" + h).slice(-4);
    }
    else r += c;
  }
  return r + '"';
}

// ── Export (dual ESM/CJS) ───────────────────────────────────

if (typeof exports !== "undefined") {
  exports.qjson_parse = qjson_parse;
  exports.qjson_stringify = qjson_stringify;
  exports.js64_encode = js64_encode;
  exports.js64_decode = js64_decode;
  exports.Unbound = Unbound;
}
export { qjson_parse, qjson_stringify, js64_encode, js64_decode, Unbound };
