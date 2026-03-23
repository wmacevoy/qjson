# ============================================================
# qjson.py — QJSON: JSON + comments + BigInt + BigDecimal + BigFloat
#
# Superset of JSON using QuickJS bignum syntax:
#   123N          → BigInt      (Python: BigInt subclass of int)
#   123.456M      → BigDecimal  (Python: decimal.Decimal)
#   3.14L         → BigFloat    (Python: BigFloat — preserves full precision)
#
# Uppercase preferred, lowercase accepted.  Consistent and visible.
#   // line       → comment
#   /* block */   → comment
#
# Valid JSON is valid QJSON.  No collisions.
#
# Usage:
#   from qjson import parse, stringify, BigInt, BigFloat
#   obj = parse('{"n": 42n, "d": 3.14m, "f": 3.14l}')
#   text = stringify(obj)
# ============================================================

from decimal import Decimal


# ── JS64 blob encoding ──────────────────────────────────────

_JS64_ALPHA = "$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz"
_JS64_REV = None


def _js64_init_rev():
    global _JS64_REV
    _JS64_REV = {}
    for i, c in enumerate(_JS64_ALPHA):
        _JS64_REV[c] = i


def js64_decode(s):
    """Decode JS64 string (without leading '$') to bytes."""
    if _JS64_REV is None:
        _js64_init_rev()
    js64 = "$" + s  # restore the implicit leading '$'
    js64_len = len(js64) - 1
    blob_len = (js64_len * 3) >> 2
    blob = bytearray(blob_len)
    code = 0
    bits = 0
    byte_idx = 0
    for i in range(js64_len):
        v = _JS64_REV.get(js64[i + 1])
        if v is None:
            raise ValueError("Invalid JS64 character: %r" % js64[i + 1])
        code = code | (v << bits)
        bits += 6
        if bits >= 8:
            if byte_idx < blob_len:
                blob[byte_idx] = code & 0xFF
            code = code >> 8
            bits -= 8
            byte_idx += 1
    return bytes(blob)


def js64_encode(data):
    """Encode bytes to JS64 string (without leading '$', for 0j prefix)."""
    if isinstance(data, (bytes, bytearray)):
        data = list(data)
    js64_len = ((len(data) * 4 + 2) // 3)
    parts = []
    code = 0
    bits = 6  # start with 6 zero bits (the implicit '$')
    byte_idx = 0
    for i in range(js64_len + 1):
        ch = _JS64_ALPHA[code & 0x3F]
        if i > 0:  # skip the leading '$'
            parts.append(ch)
        code = code >> 6
        bits -= 6
        if bits < 6 and byte_idx < len(data):
            code = code | (data[byte_idx] << bits)
            bits += 8
            byte_idx += 1
    return "".join(parts)


class Blob:
    """Binary data that round-trips through QJSON with 0j prefix (JS64)."""
    __slots__ = ("data",)

    def __init__(self, data):
        if isinstance(data, (list, tuple)):
            data = bytes(data)
        self.data = bytes(data)

    def __eq__(self, other):
        if isinstance(other, Blob):
            return self.data == other.data
        return NotImplemented

    def __hash__(self):
        return hash(("Blob", self.data))

    def __repr__(self):
        return "Blob(%r)" % self.data


class BigInt(int):
    """Integer that round-trips through QJSON with 'n' suffix."""
    pass


class BigFloat:
    """High-precision base-2 float.  Round-trips with 'l' suffix.

    Stores the full-precision string so no bits are lost.
    float(bf) gives the nearest 64-bit IEEE value.
    """
    __slots__ = ("_raw",)

    def __init__(self, value):
        self._raw = str(value)

    def __float__(self):
        return float(self._raw)

    def __repr__(self):
        return "BigFloat('%s')" % self._raw

    def __str__(self):
        return self._raw

    def __eq__(self, other):
        if isinstance(other, BigFloat):
            return self._raw == other._raw
        return NotImplemented

    def __hash__(self):
        return hash(("BigFloat", self._raw))


class Unbound:
    """Unbound variable.  Round-trips with '?' prefix.

    ?X, ?_, ?"Bob's Last Memo"
    Projects to [-Inf, "?name", +Inf] — matches everything.
    """
    __slots__ = ("name",)

    def __init__(self, name=""):
        self.name = str(name)

    def __repr__(self):
        return "Unbound(%r)" % self.name

    def __str__(self):
        return "?" + self.name

    def __eq__(self, other):
        if isinstance(other, Unbound):
            return self.name == other.name
        return NotImplemented

    def __hash__(self):
        return hash(("Unbound", self.name))


# ── Type predicates ──────────────────────────────────────────

def is_json(value):
    """True if value contains only standard JSON types (no BigInt/BigDecimal/BigFloat/Blob/Unbound).
    Recursive for arrays and objects."""
    if value is None or isinstance(value, bool):
        return True
    if isinstance(value, (Unbound, Blob, BigFloat, BigInt, Decimal)):
        return False
    if isinstance(value, (int, float, str)):
        return True
    if isinstance(value, (list, tuple)):
        return all(is_json(item) for item in value)
    if isinstance(value, dict):
        return all(is_json(v) for v in value.values())
    return False


def is_bound(value):
    """True if value contains no Unbound variables. Recursive."""
    if isinstance(value, Unbound):
        return False
    if isinstance(value, (list, tuple)):
        return all(is_bound(item) for item in value)
    if isinstance(value, dict):
        return all(is_bound(v) for v in value.values())
    return True


# ── Parser ───────────────────────────────────────────────────

def parse(text):
    """Parse QJSON text to Python objects."""
    p = _Parser(text)
    val = p.value()
    p.ws()
    if p.pos < p.end:
        raise ValueError("Trailing content at %d" % p.pos)
    return val


class _Parser:
    __slots__ = ("text", "pos", "end")

    def __init__(self, text):
        self.text = text
        self.pos = 0
        self.end = len(text)

    def ch(self):
        return self.text[self.pos] if self.pos < self.end else ""

    def ws(self):
        while self.pos < self.end:
            c = self.text[self.pos]
            if c in " \t\n\r":
                self.pos += 1
            elif c == "/" and self.pos + 1 < self.end:
                c2 = self.text[self.pos + 1]
                if c2 == "/":
                    self.pos += 2
                    while self.pos < self.end and self.text[self.pos] != "\n":
                        self.pos += 1
                elif c2 == "*":
                    self.pos += 2
                    depth = 1
                    while self.pos + 1 < self.end and depth > 0:
                        if self.text[self.pos] == "/" and self.text[self.pos + 1] == "*":
                            depth += 1
                            self.pos += 2
                        elif self.text[self.pos] == "*" and self.text[self.pos + 1] == "/":
                            depth -= 1
                            self.pos += 2
                        else:
                            self.pos += 1
                    if depth > 0:
                        raise ValueError("Unterminated block comment")
                else:
                    break
            else:
                break

    def expect(self, c):
        if self.pos >= self.end or self.text[self.pos] != c:
            raise ValueError("Expected '%s' at %d" % (c, self.pos))
        self.pos += 1

    def ident(self):
        """Parse an unquoted key (JS identifier)."""
        start = self.pos
        c = self.ch()
        if not (c.isalpha() or c == "_" or c == "$"):
            raise ValueError("Expected identifier at %d" % self.pos)
        self.pos += 1
        while self.pos < self.end:
            c = self.text[self.pos]
            if c.isalnum() or c == "_" or c == "$":
                self.pos += 1
            else:
                break
        return self.text[start:self.pos]

    def key(self):
        """Parse a key: quoted string or bare identifier."""
        if self.ch() == '"':
            return self.string()
        return self.ident()

    def value(self):
        self.ws()
        c = self.ch()
        if c == '"':  return self.string()
        if c == "{":  return self.obj()
        if c == "[":  return self.arr()
        if c == "t":  return self.literal("true", True)
        if c == "f":  return self.literal("false", False)
        if c == "n" and self.text[self.pos:self.pos + 4] == "null":
            return self.literal("null", None)
        if c == "0" and self.pos + 1 < self.end and self.text[self.pos + 1] in "jJ":
            return self.blob()
        if c == "-" or c.isdigit():
            return self.number()
        if c == "?":
            return self.unbound()
        raise ValueError("Unexpected '%s' at %d" % (c, self.pos))

    def literal(self, word, val):
        if self.text[self.pos:self.pos + len(word)] != word:
            raise ValueError("Expected '%s' at %d" % (word, self.pos))
        self.pos += len(word)
        return val

    def string(self):
        self.expect('"')
        parts = []
        while self.pos < self.end:
            c = self.text[self.pos]
            if c == '"':
                self.pos += 1
                return "".join(parts)
            if c == "\\":
                self.pos += 1
                e = self.text[self.pos]
                if   e == '"':  parts.append('"')
                elif e == "\\": parts.append("\\")
                elif e == "/":  parts.append("/")
                elif e == "b":  parts.append("\b")
                elif e == "f":  parts.append("\f")
                elif e == "n":  parts.append("\n")
                elif e == "r":  parts.append("\r")
                elif e == "t":  parts.append("\t")
                elif e == "u":
                    h = self.text[self.pos + 1:self.pos + 5]
                    parts.append(chr(int(h, 16)))
                    self.pos += 4
                self.pos += 1
            else:
                parts.append(c)
                self.pos += 1
        raise ValueError("Unterminated string")

    def number(self):
        start = self.pos
        if self.ch() == "-":
            self.pos += 1
        while self.pos < self.end and self.text[self.pos].isdigit():
            self.pos += 1
        is_float = False
        if self.pos < self.end and self.text[self.pos] == ".":
            is_float = True
            self.pos += 1
            while self.pos < self.end and self.text[self.pos].isdigit():
                self.pos += 1
        if self.pos < self.end and self.text[self.pos] in "eE":
            is_float = True
            self.pos += 1
            if self.pos < self.end and self.text[self.pos] in "+-":
                self.pos += 1
            while self.pos < self.end and self.text[self.pos].isdigit():
                self.pos += 1
        raw = self.text[start:self.pos]
        # BigInt suffix (N preferred, n accepted)
        if self.pos < self.end and self.text[self.pos] in "nN":
            self.pos += 1
            return BigInt(raw)
        # BigDecimal suffix (M preferred, m accepted)
        if self.pos < self.end and self.text[self.pos] in "mM":
            self.pos += 1
            return Decimal(raw)
        # BigFloat suffix (L preferred, l accepted)
        if self.pos < self.end and self.text[self.pos] in "lL":
            self.pos += 1
            return BigFloat(raw)
        # Regular number
        if is_float:
            return float(raw)
        return int(raw)

    def blob(self):
        self.pos += 2  # skip 0j / 0J
        start = self.pos
        js64_chars = set(_JS64_ALPHA)
        while self.pos < self.end and self.text[self.pos] in js64_chars:
            self.pos += 1
        raw = self.text[start:self.pos]
        return Blob(js64_decode(raw))

    def unbound(self):
        self.pos += 1  # skip '?'
        if self.pos < self.end and self.text[self.pos] == '"':
            name = self.string()
        else:
            start = self.pos
            while self.pos < self.end:
                c = self.text[self.pos]
                if c.isalnum() or c == '_':
                    self.pos += 1
                else:
                    break
            name = self.text[start:self.pos] if self.pos > start else ""
        return Unbound(name)

    def obj(self):
        self.expect("{")
        d = {}
        self.ws()
        if self.ch() == "}":
            self.pos += 1
            return d
        while True:
            self.ws()
            k = self.key()
            self.ws()
            self.expect(":")
            d[k] = self.value()
            self.ws()
            if self.ch() == "}":
                self.pos += 1
                return d
            self.expect(",")
            self.ws()
            if self.ch() == "}":  # trailing comma
                self.pos += 1
                return d

    def arr(self):
        self.expect("[")
        a = []
        self.ws()
        if self.ch() == "]":
            self.pos += 1
            return a
        while True:
            a.append(self.value())
            self.ws()
            if self.ch() == "]":
                self.pos += 1
                return a
            self.expect(",")
            self.ws()
            if self.ch() == "]":  # trailing comma
                self.pos += 1
                return a


# ── Serializer ───────────────────────────────────────────────

def stringify(obj, indent=None):
    """Serialize to QJSON.  BigInt → 'n', Decimal → 'm', BigFloat → 'l'."""
    return _fmt(obj, indent, 0)


def _fmt(obj, ind, depth):
    if obj is None:
        return "null"
    if obj is True:
        return "true"
    if obj is False:
        return "false"
    if isinstance(obj, Unbound):
        import re
        name = obj.name
        if name == "":
            return "?"
        if re.match(r'^[a-zA-Z_][a-zA-Z0-9_]*$', name):
            return "?" + name
        return "?" + _esc(name)
    if isinstance(obj, Blob):
        return "0j" + js64_encode(obj.data)
    if isinstance(obj, BigFloat):
        return obj._raw + "L"
    if isinstance(obj, BigInt):
        return int.__repr__(obj) + "N"
    if isinstance(obj, Decimal):
        return str(obj) + "M"
    if isinstance(obj, float):
        if obj != obj or obj == float("inf") or obj == float("-inf"):
            return "null"
        return repr(obj)
    if isinstance(obj, int):
        return str(obj)
    if isinstance(obj, str):
        return _esc(obj)
    if isinstance(obj, (list, tuple)):
        if not obj:
            return "[]"
        if ind is None:
            return "[" + ",".join(_fmt(v, None, 0) for v in obj) + "]"
        nl = "\n" + " " * (ind * (depth + 1))
        end = "\n" + " " * (ind * depth)
        return "[" + ",".join(nl + _fmt(v, ind, depth + 1) for v in obj) + end + "]"
    if isinstance(obj, dict):
        if not obj:
            return "{}"
        if ind is None:
            return "{" + ",".join(
                _esc(k) + ":" + _fmt(v, None, 0) for k, v in obj.items()
            ) + "}"
        nl = "\n" + " " * (ind * (depth + 1))
        end = "\n" + " " * (ind * depth)
        return "{" + ",".join(
            nl + _esc(k) + ": " + _fmt(v, ind, depth + 1) for k, v in obj.items()
        ) + end + "}"
    return str(obj)


_ESC = {'"': '\\"', "\\": "\\\\", "\n": "\\n", "\r": "\\r",
        "\t": "\\t", "\b": "\\b", "\f": "\\f"}

def _esc(s):
    r = ['"']
    for c in s:
        if c in _ESC:
            r.append(_ESC[c])
        elif ord(c) < 0x20:
            r.append("\\u%04x" % ord(c))
        else:
            r.append(c)
    r.append('"')
    return "".join(r)
