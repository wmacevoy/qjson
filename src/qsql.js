// ============================================================
// qsql.js — QSQL: QJSON values → per-name typed SQLite tables
// with interval arithmetic for exact BigNum comparisons.
//
//   price(btc, 67432.50M)  →  table "qjson_price_2"
//     _key TEXT PRIMARY KEY
//     arg0      TEXT  = 'btc'        (atom → text, no interval)
//     arg0_lo   NULL
//     arg0_hi   NULL
//     arg1      TEXT  = '67432.50'   (value serialized as string)
//     arg1_lo   REAL  = 67432.5      (ieee_double_round_down)
//     arg1_hi   REAL  = 67432.5      (ieee_double_round_up)
//
// Each numeric arg → [round_down(x), x, round_up(x)]:
//   arg     = value as string (exact representation)
//   arg_lo  = largest IEEE double ≤ exact value
//   arg_hi  = smallest IEEE double ≥ exact value
//
// Exact doubles (most numbers): lo == hi → point interval.
// Non-exact (rare): lo + 1 ULP == hi → 1-ULP bracket.
// Atoms: lo = hi = NULL, arg = atom name.
//
// Equality:  NOT (a_hi < b_lo OR b_hi < a_lo)
//        AND ((a_lo == a_hi AND b_lo == b_hi) OR a_val == b_val)
//
// Point intervals resolve via fast REAL comparison (99.999%).
// String fallback only for the rare non-exact boundary case.
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
//
// Compares two non-negative plain decimal strings numerically.
// Handles plain and scientific notation (converted first).

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
//
// Given double v and exact decimal string raw, determine whether
// v > exact(raw), v < exact(raw), or v == exact(raw).
//
// Returns: 1 (v > exact), -1 (v < exact), 0 (equal).
// Language-agnostic: produces identical results to Python's
// decimal.Decimal comparison for all inputs.
//
// Uses toPrecision(_maxPrec) to get the double's exact decimal
// expansion.  ES2015+ engines support toPrecision(100), which
// handles all practical values.  Falls back to 21 on ES5-only
// engines (correct for values with ≤ 21 significant digits).

var _maxPrec = 21;
try { (1.0).toPrecision(100); _maxPrec = 100; } catch(e) {}

function _roundingDir(v, raw) {
  // Overflow: Infinity > any finite exact value
  if (v === Infinity) return 1;
  if (v === -Infinity) return -1;
  // Underflow to zero: direction from sign of raw
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

// ── Helpers ──────────────────────────────────────────────────

function _qsql_safeName(name) {
  return name.replace(/[^a-zA-Z0-9_]/g, "_");
}

function _qsql_tableName(prefix, name, arity) {
  return prefix + _qsql_safeName(name) + "_" + arity;
}

// Primary value for the arg column (backward compat utility)
function _qsql_argVal(arg) {
  if (!arg) return null;
  if (arg.t === "a") return arg.n;
  if (arg.t === "n") {
    var v = arg.v;
    if (typeof v === "number") return v;
    return String(v);
  }
  return JSON.stringify(arg);
}

// Interval: [str, lo, hi] for a serialized arg.
//
//   atom:       [name,  null,          null         ]
//   exact num:  [null,  v,             v            ]  str NULL (lo IS the value)
//   inexact BN: [raw,   round_down(v), round_up(v) ]  1-ULP bracket
//
// str is NULL when lo == hi — the double IS the exact value.
// str is populated only in the overlap zone (~0.001%).
//
// round_down = largest double ≤ exact value
// round_up   = smallest double ≥ exact value
//
// Language-agnostic: JS and Python produce identical intervals.
function _qsql_argInterval(arg) {
  if (!arg) return [null, null, null];
  if (arg.t === "a") return [arg.n, null, null];
  if (arg.t === "n") {
    var v = arg.v;
    if (typeof v !== "number") v = Number(String(v).replace(/[NMLnml]$/, ""));
    if (!arg.r) return [null, v, v]; // plain number — exact, str NULL
    // BigNum with repr — determine tightest interval
    var raw = arg.r.replace(/[NMLnml]$/, "");
    var dir = _roundingDir(v, raw);
    if (dir === 0) return [null, v, v];            // exact double, str NULL
    if (dir === 1) return [raw, _nextDown(v), v];  // v > exact
    return [raw, v, _nextUp(v)];                   // v < exact (dir === -1)
  }
  return [JSON.stringify(arg), null, null];
}

// ── Adapter Factory ──────────────────────────────────────────

function qsqlAdapter(db, options) {
  var _parse = (options && options.parse) || JSON.parse;
  var _prefix = (options && options.prefix) || "qjson_";
  var _known = {};
  var _cache = {};
  var _metaTable = _prefix + "meta";

  function _stmt(sql) {
    if (!_cache[sql]) _cache[sql] = db.prepare(sql);
    return _cache[sql];
  }

  function _ensureTable(functor, arity) {
    var fa = functor + "/" + arity;
    if (_known[fa]) return;

    var tbl = _qsql_tableName(_prefix, functor, arity);
    var ddl = 'CREATE TABLE IF NOT EXISTS "' + tbl + '" (_key TEXT PRIMARY KEY';
    for (var i = 0; i < arity; i++) {
      ddl += ", arg" + i + " TEXT";
      ddl += ", arg" + i + "_lo REAL";
      ddl += ", arg" + i + "_hi REAL";
    }
    ddl += ")";
    db.exec(ddl);

    // Index on primary value (atom/string equality)
    // and on _lo (numeric range queries, interval pushdown)
    for (var i = 0; i < arity; i++) {
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + tbl + '_' + i +
              '" ON "' + tbl + '"(arg' + i + ')');
      db.exec('CREATE INDEX IF NOT EXISTS "ix_' + tbl + '_' + i + 'lo' +
              '" ON "' + tbl + '"(arg' + i + '_lo)');
    }

    _stmt('INSERT OR IGNORE INTO "' + _metaTable + '" VALUES (?, ?)').run(functor, arity);
    _known[fa] = true;
  }

  return {
    setup: function() {
      db.exec(
        'CREATE TABLE IF NOT EXISTS "' + _metaTable + '" ' +
        "(functor TEXT, arity INTEGER, PRIMARY KEY(functor, arity))"
      );
      var metas = _stmt('SELECT functor, arity FROM "' + _metaTable + '"').all();
      for (var i = 0; i < metas.length; i++) {
        _known[metas[i].functor + "/" + metas[i].arity] = true;
      }
    },

    insert: function(key, functor, arity) {
      if (functor == null) return;
      _ensureTable(functor, arity);

      var obj = _parse(key);
      var values = [key];
      if (obj.t === "c" && obj.a) {
        for (var i = 0; i < arity; i++) {
          var iv = i < obj.a.length ? _qsql_argInterval(obj.a[i]) : [null, null, null];
          values.push(iv[0], iv[1], iv[2]);
        }
      }

      var tbl = _qsql_tableName(_prefix, functor, arity);
      var ph = "?";
      for (var i = 1; i < values.length; i++) ph += ", ?";
      var sql = 'INSERT OR IGNORE INTO "' + tbl + '" VALUES (' + ph + ')';
      var s = _stmt(sql);
      s.run.apply(s, values);
    },

    remove: function(key) {
      var obj;
      try { obj = _parse(key); } catch(e) { return; }
      var functor, arity;
      if (obj.t === "c") { functor = obj.f; arity = (obj.a || []).length; }
      else if (obj.t === "a") { functor = obj.n; arity = 0; }
      else return;

      var fa = functor + "/" + arity;
      if (!_known[fa]) return;
      var tbl = _qsql_tableName(_prefix, functor, arity);
      _stmt('DELETE FROM "' + tbl + '" WHERE _key = ?').run(key);
    },

    all: function(predicates) {
      var results = [];
      var metas;
      if (predicates) {
        metas = [];
        var keys = Object.keys(predicates);
        for (var i = 0; i < keys.length; i++) {
          var parts = keys[i].split("/");
          metas.push({ functor: parts[0], arity: parseInt(parts[1], 10) });
        }
      } else {
        metas = _stmt('SELECT functor, arity FROM "' + _metaTable + '"').all();
      }
      for (var i = 0; i < metas.length; i++) {
        var m = metas[i];
        if (!_known[m.functor + "/" + m.arity]) continue;
        var tbl = _qsql_tableName(_prefix, m.functor, m.arity);
        try {
          var rows = _stmt('SELECT _key FROM "' + tbl + '"').all();
          for (var j = 0; j < rows.length; j++) {
            results.push(rows[j]._key);
          }
        } catch(e) {}
      }
      return results;
    },

    commit: function() {},

    close: function() {
      _cache = {};
      if (db.close) db.close();
    }
  };
}

// ── Export (dual ESM/CJS) ───────────────────────────────────

if (typeof exports !== "undefined") {
  exports.qsqlAdapter = qsqlAdapter;
  exports._qsql_tableName = _qsql_tableName;
  exports._qsql_argVal = _qsql_argVal;
  exports._qsql_argInterval = _qsql_argInterval;
  exports._qsql_safeName = _qsql_safeName;
  exports._nextUp = _nextUp;
  exports._nextDown = _nextDown;
  exports._sciToPlain = _sciToPlain;
  exports._decCmp = _decCmp;
  exports._roundingDir = _roundingDir;
}
export { qsqlAdapter, _qsql_tableName, _qsql_argVal, _qsql_argInterval, _qsql_safeName, _nextUp, _nextDown, _sciToPlain, _decCmp, _roundingDir };
