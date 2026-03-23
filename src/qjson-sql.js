// ============================================================
// qjson-sql.js — normalized relational storage for QJSON values.
//
// Stores any QJSON value (null, boolean, number, bigint, bigdec,
// bigfloat, string, blob, array, object) in a normalized schema
// with interval projection for exact numeric comparisons.
//
// Schema (prefix default "qjson_"):
//   {prefix}value        — root table, every value gets an id + type
//   {prefix}number       — [lo, str, hi] interval for all numeric types
//   {prefix}string       — string values
//   {prefix}blob         — binary data
//   {prefix}array        — array containers
//   {prefix}array_item   — array elements (idx, value_id)
//   {prefix}object       — object containers
//   {prefix}object_item  — object entries (key, value_id)
//
// Numeric interval [lo, str, hi]:
//   lo  = largest IEEE double <= exact value
//   hi  = smallest IEEE double >= exact value
//   str = exact string repr, NULL when lo == hi (exact double)
//
// Portable: ES5 style (var, function, no arrows).
// ============================================================

// ── IEEE 754 nextUp / nextDown ──────────────────────────────
//
// These are ULP (unit in the last place) operations on doubles.
// Used to compute the tightest interval [lo, hi] that brackets
// the exact decimal value when it's not exactly representable.

var _ivBuf = typeof ArrayBuffer !== "undefined" ? new ArrayBuffer(8) : null;
var _ivF64 = _ivBuf ? new Float64Array(_ivBuf) : null;
var _ivU32 = _ivBuf ? new Uint32Array(_ivBuf) : null;
var _loIdx = 0, _hiIdx = 1;

// Detect endianness
if (_ivF64 && _ivU32) {
  _ivF64[0] = 1.0; // 0x3FF0000000000000
  if (_ivU32[0] !== 0) { _loIdx = 1; _hiIdx = 0; } // big-endian
}

function _nextUp(x) {
  if (x !== x || x === Infinity) return x;
  if (x === 0) return 5e-324;
  if (x === -Infinity) return -1.7976931348623157e+308;
  if (!_ivF64) return x + Math.abs(x) * 1.11e-16; // fallback
  _ivF64[0] = x;
  var lo = _ivU32[_loIdx], hi = _ivU32[_hiIdx];
  if (x > 0) {
    lo = (lo + 1) >>> 0;
    if (lo === 0) hi = (hi + 1) >>> 0;
  } else {
    if (lo === 0) { hi = (hi - 1) >>> 0; lo = 0xFFFFFFFF; }
    else { lo = (lo - 1) >>> 0; }
  }
  _ivU32[_loIdx] = lo;
  _ivU32[_hiIdx] = hi;
  return _ivF64[0];
}

function _nextDown(x) {
  return -_nextUp(-x);
}

// ── Scientific notation → plain decimal ──────────────────────

function _sciToPlain(s) {
  var eIdx = s.indexOf("e");
  if (eIdx < 0) eIdx = s.indexOf("E");
  if (eIdx < 0) return s;
  var mantissa = s.substring(0, eIdx);
  var exp = parseInt(s.substring(eIdx + 1), 10);
  var dot = mantissa.indexOf(".");
  var digits = dot >= 0
    ? mantissa.substring(0, dot) + mantissa.substring(dot + 1)
    : mantissa;
  var intLen = (dot >= 0 ? dot : mantissa.length) + exp;
  if (intLen >= digits.length) {
    while (digits.length < intLen) digits += "0";
    return digits;
  }
  if (intLen <= 0) {
    var zeros = "";
    for (var i = 0; i < -intLen; i++) zeros += "0";
    return "0." + zeros + digits;
  }
  return digits.substring(0, intLen) + "." + digits.substring(intLen);
}

// ── Decimal string comparison ────────────────────────────────

function _decCmp(a, b) {
  a = _sciToPlain(a);
  b = _sciToPlain(b);
  var aDot = a.indexOf(".");
  var bDot = b.indexOf(".");
  var aInt = (aDot >= 0 ? a.substring(0, aDot) : a).replace(/^0+/, "") || "0";
  var bInt = (bDot >= 0 ? b.substring(0, bDot) : b).replace(/^0+/, "") || "0";
  if (aInt.length !== bInt.length) return aInt.length > bInt.length ? 1 : -1;
  if (aInt > bInt) return 1;
  if (aInt < bInt) return -1;
  var aFrac = aDot >= 0 ? a.substring(aDot + 1) : "";
  var bFrac = bDot >= 0 ? b.substring(bDot + 1) : "";
  var maxLen = aFrac.length > bFrac.length ? aFrac.length : bFrac.length;
  while (aFrac.length < maxLen) aFrac += "0";
  while (bFrac.length < maxLen) bFrac += "0";
  if (aFrac > bFrac) return 1;
  if (aFrac < bFrac) return -1;
  return 0;
}

// ── Rounding direction detection ─────────────────────────────

var _maxPrec = 21;
try { (1.0).toPrecision(100); _maxPrec = 100; } catch(e) {}

function _roundingDir(v, raw) {
  if (v === Infinity) return 1;
  if (v === -Infinity) return -1;
  if (v === 0) {
    var rawClean = raw.replace(/^[-+]/, "").replace(/[0.]/g, "");
    return rawClean === "" ? 0 : (raw.charAt(0) === "-" ? 1 : -1);
  }
  var neg = v < 0;
  var absV = neg ? -v : v;
  var absRaw = raw.charAt(0) === "-" ? raw.substring(1) : raw;
  var dStr = absV.toPrecision(_maxPrec);
  var cmp = _decCmp(dStr, absRaw);
  return neg ? -cmp : cmp;
}

// ── Directed rounding: decimal string → IEEE double ─────────
//
// roundDown(raw): largest IEEE double <= exact(raw)
// roundUp(raw):   smallest IEEE double >= exact(raw)
//
// Equivalent to the C qjson_project() which uses BF_RNDD/BF_RNDU
// (libbf) or FE_DOWNWARD/FE_UPWARD (fesetround).  All three
// implementations produce identical results for all inputs.

function roundDown(raw) {
  var v = Number(raw);
  var dir = _roundingDir(v, raw);
  if (dir <= 0) return v;       // v <= exact → v is round-down
  return _nextDown(v);          // v > exact → step down one ULP
}

function roundUp(raw) {
  var v = Number(raw);
  var dir = _roundingDir(v, raw);
  if (dir >= 0) return v;       // v >= exact → v is round-up
  return _nextUp(v);            // v < exact → step up one ULP
}

// ── Numeric interval projection ─────────────────────────────
//
// project(raw) → [lo, str, hi]:
//   lo  = roundDown(raw)
//   hi  = roundUp(raw)
//   str = raw when lo != hi, NULL when lo == hi (exact double)

function _projectNumeric(raw) {
  var lo = roundDown(raw);
  var hi = roundUp(raw);
  var str = (lo === hi) ? null : raw;
  return [lo, str, hi];
}

// ── Value type classification ────────────────────────────────
//
// Classifies a native JS value (as produced by qjson_parse) into
// its QJSON type string and optional raw decimal string.
// Returns { type: string, raw: string|null }

function _classifyValue(value) {
  if (value === null || value === undefined) return { type: "null", raw: null };
  if (value === true) return { type: "true", raw: null };
  if (value === false) return { type: "false", raw: null };
  if (typeof value === "bigint") return { type: "bigint", raw: String(value) };
  if (typeof value === "bigdecimal") return { type: "bigdec", raw: value.toString() };
  if (typeof value === "bigfloat") return { type: "bigfloat", raw: value.toString() };
  if (typeof value === "number") return { type: "number", raw: null };
  if (typeof value === "string") {
    var last = value.charAt(value.length - 1);
    if (last === "N" || last === "n") {
      var body = value.substring(0, value.length - 1);
      if (/^-?\d+$/.test(body)) return { type: "bigint", raw: body };
    }
    if (last === "M" || last === "m") {
      var body = value.substring(0, value.length - 1);
      if (/^-?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(body)) return { type: "bigdec", raw: body };
    }
    if (last === "L" || last === "l") {
      var body = value.substring(0, value.length - 1);
      if (/^-?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(body)) return { type: "bigfloat", raw: body };
    }
    return { type: "string", raw: null };
  }
  if (typeof value === "object" && value !== null) {
    if (value.$qjson === "unbound") return { type: "unbound", raw: (value.name != null) ? value.name : "" };
    if (value.$qjson === "blob") return { type: "blob", raw: null };
    if (Array.isArray(value)) return { type: "array", raw: null };
    return { type: "object", raw: null };
  }
  return { type: "string", raw: null };
}

// ── Dialect config ───────────────────────────────────────────
//
// SQLite:     REAL (8-byte), BLOB, INTEGER PRIMARY KEY
// PostgreSQL: DOUBLE PRECISION (8-byte), BYTEA, SERIAL PRIMARY KEY
//
// REAL in PostgreSQL is only 4 bytes — would destroy interval precision.

var _dialects = {
  sqlite: { floatType: "REAL", blobType: "BLOB", serialPk: "INTEGER PRIMARY KEY" },
  postgres: { floatType: "DOUBLE PRECISION", blobType: "BYTEA", serialPk: "SERIAL PRIMARY KEY" }
};

// ── Adapter Factory ──────────────────────────────────────────

function qjsonSqlAdapter(db, options) {
  var _prefix = (options && options.prefix) || "qjson_";
  var _dialect = (options && options.dialect) || "sqlite";
  var _key = (options && options.key) || null;
  var _d = _dialects[_dialect];
  var _cache = {};

  // SQLCipher encryption key (must be set before any other operation)
  if (_key && _dialect === "sqlite") {
    db.pragma("key = '" + _key.replace(/'/g, "''") + "'");
  }

  // Table names
  var _tValue = _prefix + "value";
  var _tNumber = _prefix + "number";
  var _tString = _prefix + "string";
  var _tBlob = _prefix + "blob";
  var _tArray = _prefix + "array";
  var _tArrayItem = _prefix + "array_item";
  var _tObject = _prefix + "object";
  var _tObjectItem = _prefix + "object_item";

  function _stmt(sql) {
    if (!_cache[sql]) _cache[sql] = db.prepare(sql);
    return _cache[sql];
  }

  function _insertValue(type) {
    var info = _stmt('INSERT INTO "' + _tValue + '" (type) VALUES (?)').run(type);
    return Number(info.lastInsertRowid);
  }

  function _store(value) {
    var c = _classifyValue(value);
    var vid = _insertValue(c.type);

    if (c.type === "null" || c.type === "true" || c.type === "false") {
      return vid;
    }

    if (c.type === "number") {
      _stmt('INSERT INTO "' + _tNumber + '" (value_id, lo, str, hi) VALUES (?, ?, ?, ?)').run(vid, value, null, value);
      return vid;
    }

    if (c.type === "bigint" || c.type === "bigdec" || c.type === "bigfloat") {
      var iv = _projectNumeric(c.raw);
      _stmt('INSERT INTO "' + _tNumber + '" (value_id, lo, str, hi) VALUES (?, ?, ?, ?)').run(vid, iv[0], iv[1], iv[2]);
      return vid;
    }

    if (c.type === "string") {
      _stmt('INSERT INTO "' + _tString + '" (value_id, value) VALUES (?, ?)').run(vid, value);
      return vid;
    }

    if (c.type === "unbound") {
      _stmt('INSERT INTO "' + _tNumber + '" (value_id, lo, str, hi) VALUES (?, ?, ?, ?)').run(vid, -Infinity, "?" + c.raw, Infinity);
      return vid;
    }

    if (c.type === "blob") {
      var buf = typeof Buffer !== "undefined"
        ? Buffer.from(value.data)
        : new Uint8Array(value.data);
      _stmt('INSERT INTO "' + _tBlob + '" (value_id, value) VALUES (?, ?)').run(vid, buf);
      return vid;
    }

    if (c.type === "array") {
      var aInfo = _stmt('INSERT INTO "' + _tArray + '" (value_id) VALUES (?)').run(vid);
      var arrayId = Number(aInfo.lastInsertRowid);
      for (var i = 0; i < value.length; i++) {
        var itemVid = _store(value[i]);
        _stmt('INSERT INTO "' + _tArrayItem + '" (array_id, idx, value_id) VALUES (?, ?, ?)').run(arrayId, i, itemVid);
      }
      return vid;
    }

    if (c.type === "object") {
      var oInfo = _stmt('INSERT INTO "' + _tObject + '" (value_id) VALUES (?)').run(vid);
      var objectId = Number(oInfo.lastInsertRowid);
      var keys = Object.keys(value);
      for (var i = 0; i < keys.length; i++) {
        var k = keys[i];
        var itemVid = _store(value[k]);
        _stmt('INSERT INTO "' + _tObjectItem + '" (object_id, key, value_id) VALUES (?, ?, ?)').run(objectId, k, itemVid);
      }
      return vid;
    }

    return vid;
  }

  function _load(vid) {
    var row = _stmt('SELECT type FROM "' + _tValue + '" WHERE id = ?').get(vid);
    if (!row) return undefined;
    var type = row.type;

    if (type === "null") return null;
    if (type === "true") return true;
    if (type === "false") return false;

    if (type === "number" || type === "bigint" || type === "bigdec" || type === "bigfloat") {
      var nr = _stmt('SELECT lo, str, hi FROM "' + _tNumber + '" WHERE value_id = ?').get(vid);
      if (!nr) return null;
      if (type === "number") return nr.lo;
      if (type === "bigint") {
        var raw = nr.str !== null ? nr.str : String(nr.lo);
        if (typeof BigInt !== "undefined") return BigInt(raw);
        return raw + "N";
      }
      if (type === "bigdec") {
        var raw = nr.str !== null ? nr.str : String(nr.lo);
        if (typeof BigDecimal !== "undefined") return BigDecimal(raw);
        return raw + "M";
      }
      if (type === "bigfloat") {
        var raw = nr.str !== null ? nr.str : String(nr.lo);
        if (typeof BigFloat !== "undefined") return BigFloat(raw);
        return raw + "L";
      }
    }

    if (type === "unbound") {
      var nr = _stmt('SELECT str FROM "' + _tNumber + '" WHERE value_id = ?').get(vid);
      var name = (nr && nr.str) ? nr.str : "?";
      if (name.charAt(0) === "?") name = name.substring(1);
      return { $qjson: "unbound", name: name };
    }

    if (type === "string") {
      var sr = _stmt('SELECT value FROM "' + _tString + '" WHERE value_id = ?').get(vid);
      return sr ? sr.value : "";
    }

    if (type === "blob") {
      var br = _stmt('SELECT value FROM "' + _tBlob + '" WHERE value_id = ?').get(vid);
      if (!br) return { $qjson: "blob", data: [] };
      var buf = br.value;
      var data = [];
      for (var i = 0; i < buf.length; i++) data.push(buf[i]);
      return { $qjson: "blob", data: data };
    }

    if (type === "array") {
      var ar = _stmt('SELECT id FROM "' + _tArray + '" WHERE value_id = ?').get(vid);
      if (!ar) return [];
      var items = _stmt('SELECT value_id FROM "' + _tArrayItem + '" WHERE array_id = ? ORDER BY idx').all(ar.id);
      var result = [];
      for (var i = 0; i < items.length; i++) {
        result.push(_load(items[i].value_id));
      }
      return result;
    }

    if (type === "object") {
      var or_ = _stmt('SELECT id FROM "' + _tObject + '" WHERE value_id = ?').get(vid);
      if (!or_) return {};
      var items = _stmt('SELECT key, value_id FROM "' + _tObjectItem + '" WHERE object_id = ?').all(or_.id);
      var result = {};
      for (var i = 0; i < items.length; i++) {
        result[items[i].key] = _load(items[i].value_id);
      }
      return result;
    }

    return null;
  }

  function _remove(vid) {
    var row = _stmt('SELECT type FROM "' + _tValue + '" WHERE id = ?').get(vid);
    if (!row) return;
    var type = row.type;

    if (type === "number" || type === "bigint" || type === "bigdec" || type === "bigfloat") {
      _stmt('DELETE FROM "' + _tNumber + '" WHERE value_id = ?').run(vid);
    } else if (type === "string") {
      _stmt('DELETE FROM "' + _tString + '" WHERE value_id = ?').run(vid);
    } else if (type === "blob") {
      _stmt('DELETE FROM "' + _tBlob + '" WHERE value_id = ?').run(vid);
    } else if (type === "array") {
      var ar = _stmt('SELECT id FROM "' + _tArray + '" WHERE value_id = ?').get(vid);
      if (ar) {
        var items = _stmt('SELECT value_id FROM "' + _tArrayItem + '" WHERE array_id = ?').all(ar.id);
        for (var i = 0; i < items.length; i++) {
          _remove(items[i].value_id);
        }
        _stmt('DELETE FROM "' + _tArrayItem + '" WHERE array_id = ?').run(ar.id);
        _stmt('DELETE FROM "' + _tArray + '" WHERE id = ?').run(ar.id);
      }
    } else if (type === "object") {
      var or_ = _stmt('SELECT id FROM "' + _tObject + '" WHERE value_id = ?').get(vid);
      if (or_) {
        var items = _stmt('SELECT value_id FROM "' + _tObjectItem + '" WHERE object_id = ?').all(or_.id);
        for (var i = 0; i < items.length; i++) {
          _remove(items[i].value_id);
        }
        _stmt('DELETE FROM "' + _tObjectItem + '" WHERE object_id = ?').run(or_.id);
        _stmt('DELETE FROM "' + _tObject + '" WHERE id = ?').run(or_.id);
      }
    }

    _stmt('DELETE FROM "' + _tValue + '" WHERE id = ?').run(vid);
  }

  return {
    setup: function() {
      var FT = _d.floatType;
      var BT = _d.blobType;
      var SPK = _d.serialPk;
      db.exec('CREATE TABLE IF NOT EXISTS "' + _tValue + '" (id ' + SPK + ', type TEXT NOT NULL)');
      db.exec('CREATE TABLE IF NOT EXISTS "' + _tNumber + '" (id ' + SPK + ', value_id INTEGER REFERENCES "' + _tValue + '"(id), lo ' + FT + ', str TEXT, hi ' + FT + ')');
      db.exec('CREATE TABLE IF NOT EXISTS "' + _tString + '" (id ' + SPK + ', value_id INTEGER REFERENCES "' + _tValue + '"(id), value TEXT)');
      db.exec('CREATE TABLE IF NOT EXISTS "' + _tBlob + '" (id ' + SPK + ', value_id INTEGER REFERENCES "' + _tValue + '"(id), value ' + BT + ')');
      db.exec('CREATE TABLE IF NOT EXISTS "' + _tArray + '" (id ' + SPK + ', value_id INTEGER REFERENCES "' + _tValue + '"(id))');
      db.exec('CREATE TABLE IF NOT EXISTS "' + _tArrayItem + '" (id ' + SPK + ', array_id INTEGER REFERENCES "' + _tArray + '"(id), idx INTEGER, value_id INTEGER REFERENCES "' + _tValue + '"(id))');
      db.exec('CREATE TABLE IF NOT EXISTS "' + _tObject + '" (id ' + SPK + ', value_id INTEGER REFERENCES "' + _tValue + '"(id))');
      db.exec('CREATE TABLE IF NOT EXISTS "' + _tObjectItem + '" (id ' + SPK + ', object_id INTEGER REFERENCES "' + _tObject + '"(id), key TEXT, value_id INTEGER REFERENCES "' + _tValue + '"(id))');

      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + _tNumber + '_vid" ON "' + _tNumber + '"(value_id)');
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + _tNumber + '_lo" ON "' + _tNumber + '"(lo)');
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + _tNumber + '_hi" ON "' + _tNumber + '"(hi)');
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + _tString + '_vid" ON "' + _tString + '"(value_id)');
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + _tBlob + '_vid" ON "' + _tBlob + '"(value_id)');
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + _tArray + '_vid" ON "' + _tArray + '"(value_id)');
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + _tArrayItem + '_aid" ON "' + _tArrayItem + '"(array_id)');
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + _tObject + '_vid" ON "' + _tObject + '"(value_id)');
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + _tObjectItem + '_oid" ON "' + _tObjectItem + '"(object_id)');
    },

    store: function(value) {
      return _store(value);
    },

    load: function(valueId) {
      return _load(valueId);
    },

    remove: function(valueId) {
      _remove(valueId);
    },

    close: function() {
      _cache = {};
      if (db.close) db.close();
    }
  };
}

// ── Export (dual ESM/CJS) ───────────────────────────────────

if (typeof exports !== "undefined") {
  exports.qjsonSqlAdapter = qjsonSqlAdapter;
  exports.roundDown = roundDown;
  exports.roundUp = roundUp;
  exports._nextUp = _nextUp;
  exports._nextDown = _nextDown;
  exports._sciToPlain = _sciToPlain;
  exports._decCmp = _decCmp;
  exports._roundingDir = _roundingDir;
  exports._projectNumeric = _projectNumeric;
  exports._classifyValue = _classifyValue;
}
export { qjsonSqlAdapter, roundDown, roundUp, _nextUp, _nextDown, _sciToPlain, _decCmp, _roundingDir, _projectNumeric, _classifyValue };
