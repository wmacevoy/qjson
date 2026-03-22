// ============================================================
// qjson-wasm.js — QJSON adapter for SQLCipher WASM.
//
// Uses the sqlcipher-libressl v0.2.0 unified async API
// (SQLCipher.open) to store and query QJSON values in an
// encrypted browser database with OPFS or IndexedDB persistence.
//
// Usage:
//   var qdb = await QJSONDatabase.open({key: "secret"});
//   var id = await qdb.store({price: "67432.50M", items: [1, 2, 3]});
//   var val = await qdb.load(id);       // → {price: "67432.50M", ...}
//   var qjson = await qdb.toQJSON(id);  // → '{"items":[1,2,3],"price":"67432.50M"}'
//   await qdb.save();
//   await qdb.close();
//
// Requires (same directory or adjust paths):
//   sqlcipher.js, sqlcipher.wasm, sqlcipher-worker.js, sqlcipher-api.js
// ============================================================

var QJSONDatabase = (function() {
  "use strict";

  var PREFIX = "qjson_";

  // ── Schema DDL ──────────────────────────────────────────────

  var SCHEMA = [
    'CREATE TABLE IF NOT EXISTS "' + PREFIX + 'value" (id INTEGER PRIMARY KEY, type TEXT NOT NULL)',
    'CREATE TABLE IF NOT EXISTS "' + PREFIX + 'number" (id INTEGER PRIMARY KEY, value_id INTEGER REFERENCES "' + PREFIX + 'value"(id), lo REAL, str TEXT, hi REAL)',
    'CREATE TABLE IF NOT EXISTS "' + PREFIX + 'string" (id INTEGER PRIMARY KEY, value_id INTEGER REFERENCES "' + PREFIX + 'value"(id), value TEXT)',
    'CREATE TABLE IF NOT EXISTS "' + PREFIX + 'blob" (id INTEGER PRIMARY KEY, value_id INTEGER REFERENCES "' + PREFIX + 'value"(id), value BLOB)',
    'CREATE TABLE IF NOT EXISTS "' + PREFIX + 'array" (id INTEGER PRIMARY KEY, value_id INTEGER REFERENCES "' + PREFIX + 'value"(id))',
    'CREATE TABLE IF NOT EXISTS "' + PREFIX + 'array_item" (id INTEGER PRIMARY KEY, array_id INTEGER REFERENCES "' + PREFIX + 'array"(id), idx INTEGER, value_id INTEGER REFERENCES "' + PREFIX + 'value"(id))',
    'CREATE TABLE IF NOT EXISTS "' + PREFIX + 'object" (id INTEGER PRIMARY KEY, value_id INTEGER REFERENCES "' + PREFIX + 'value"(id))',
    'CREATE TABLE IF NOT EXISTS "' + PREFIX + 'object_item" (id INTEGER PRIMARY KEY, object_id INTEGER REFERENCES "' + PREFIX + 'object"(id), key TEXT, value_id INTEGER REFERENCES "' + PREFIX + 'value"(id))',
  ];

  var INDEXES = [
    'CREATE INDEX IF NOT EXISTS "ix_' + PREFIX + 'number_vid" ON "' + PREFIX + 'number"(value_id)',
    'CREATE INDEX IF NOT EXISTS "ix_' + PREFIX + 'number_lo" ON "' + PREFIX + 'number"(lo)',
    'CREATE INDEX IF NOT EXISTS "ix_' + PREFIX + 'number_hi" ON "' + PREFIX + 'number"(hi)',
    'CREATE INDEX IF NOT EXISTS "ix_' + PREFIX + 'string_vid" ON "' + PREFIX + 'string"(value_id)',
    'CREATE INDEX IF NOT EXISTS "ix_' + PREFIX + 'blob_vid" ON "' + PREFIX + 'blob"(value_id)',
    'CREATE INDEX IF NOT EXISTS "ix_' + PREFIX + 'array_vid" ON "' + PREFIX + 'array"(value_id)',
    'CREATE INDEX IF NOT EXISTS "ix_' + PREFIX + 'array_item_aid" ON "' + PREFIX + 'array_item"(array_id)',
    'CREATE INDEX IF NOT EXISTS "ix_' + PREFIX + 'object_vid" ON "' + PREFIX + 'object"(value_id)',
    'CREATE INDEX IF NOT EXISTS "ix_' + PREFIX + 'object_item_oid" ON "' + PREFIX + 'object_item"(object_id)',
  ];

  // ── Value classification ────────────────────────────────────

  function classify(value) {
    if (value === null || value === undefined) return {type: "null", raw: null};
    if (value === true)  return {type: "true", raw: null};
    if (value === false) return {type: "false", raw: null};
    if (typeof value === "bigint") return {type: "bigint", raw: String(value)};
    if (typeof value === "number") return {type: "number", raw: null};
    if (typeof value === "string") {
      var last = value.charAt(value.length - 1);
      if ((last === "N" || last === "n") && /^-?\d+$/.test(value.slice(0, -1)))
        return {type: "bigint", raw: value.slice(0, -1)};
      if ((last === "M" || last === "m") && /^-?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(value.slice(0, -1)))
        return {type: "bigdec", raw: value.slice(0, -1)};
      if ((last === "L" || last === "l") && /^-?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(value.slice(0, -1)))
        return {type: "bigfloat", raw: value.slice(0, -1)};
      return {type: "string", raw: null};
    }
    if (Array.isArray(value)) return {type: "array", raw: null};
    if (typeof value === "object") return {type: "object", raw: null};
    return {type: "string", raw: null};
  }

  // ── QJSON Database handle ───────────────────────────────────

  function QDB(handle) {
    this._db = handle;
  }

  QDB.prototype._lastId = async function() {
    var rows = await this._db.select("SELECT last_insert_rowid() AS id");
    return rows[0].id;
  };

  QDB.prototype.setup = async function() {
    for (var i = 0; i < SCHEMA.length; i++)
      await this._db.exec(SCHEMA[i]);
    for (var i = 0; i < INDEXES.length; i++)
      await this._db.exec(INDEXES[i]);
  };

  QDB.prototype.store = async function(value) {
    var c = classify(value);
    await this._db.exec(
      'INSERT INTO "' + PREFIX + 'value" (type) VALUES (?)', [c.type]);
    var vid = await this._lastId();

    if (c.type === "null" || c.type === "true" || c.type === "false") {
      return vid;
    }

    if (c.type === "number") {
      await this._db.exec(
        'INSERT INTO "' + PREFIX + 'number" (value_id, lo, str, hi) VALUES (?, ?, ?, ?)',
        [vid, value, null, value]);
      return vid;
    }

    if (c.type === "bigint" || c.type === "bigdec" || c.type === "bigfloat") {
      var v = Number(c.raw);
      // Simple projection: for WASM we use Number() which is round-to-nearest
      // Full roundDown/roundUp would need the ULP logic from qjson-sql.js
      await this._db.exec(
        'INSERT INTO "' + PREFIX + 'number" (value_id, lo, str, hi) VALUES (?, ?, ?, ?)',
        [vid, v, (v === v && isFinite(v) && String(v) === c.raw) ? null : c.raw, v]);
      return vid;
    }

    if (c.type === "string") {
      await this._db.exec(
        'INSERT INTO "' + PREFIX + 'string" (value_id, value) VALUES (?, ?)',
        [vid, value]);
      return vid;
    }

    if (c.type === "array") {
      await this._db.exec(
        'INSERT INTO "' + PREFIX + 'array" (value_id) VALUES (?)', [vid]);
      var arrayId = await this._lastId();
      for (var i = 0; i < value.length; i++) {
        var itemVid = await this.store(value[i]);
        await this._db.exec(
          'INSERT INTO "' + PREFIX + 'array_item" (array_id, idx, value_id) VALUES (?, ?, ?)',
          [arrayId, i, itemVid]);
      }
      return vid;
    }

    if (c.type === "object") {
      await this._db.exec(
        'INSERT INTO "' + PREFIX + 'object" (value_id) VALUES (?)', [vid]);
      var objectId = await this._lastId();
      var keys = Object.keys(value);
      for (var i = 0; i < keys.length; i++) {
        var itemVid = await this.store(value[keys[i]]);
        await this._db.exec(
          'INSERT INTO "' + PREFIX + 'object_item" (object_id, key, value_id) VALUES (?, ?, ?)',
          [objectId, keys[i], itemVid]);
      }
      return vid;
    }

    return vid;
  };

  QDB.prototype.load = async function(vid) {
    var rows = await this._db.select(
      'SELECT type FROM "' + PREFIX + 'value" WHERE id = ?', [vid]);
    if (!rows.length) return undefined;
    var type = rows[0].type;

    if (type === "null") return null;
    if (type === "true") return true;
    if (type === "false") return false;

    if (type === "number" || type === "bigint" || type === "bigdec" || type === "bigfloat") {
      var nr = await this._db.select(
        'SELECT lo, str, hi FROM "' + PREFIX + 'number" WHERE value_id = ?', [vid]);
      if (!nr.length) return null;
      if (type === "number") return nr[0].lo;
      var raw = nr[0].str !== null ? nr[0].str : String(nr[0].lo);
      if (type === "bigint") return typeof BigInt !== "undefined" ? BigInt(raw) : raw + "N";
      if (type === "bigdec") return raw + "M";
      if (type === "bigfloat") return raw + "L";
    }

    if (type === "string") {
      var sr = await this._db.select(
        'SELECT value FROM "' + PREFIX + 'string" WHERE value_id = ?', [vid]);
      return sr.length ? sr[0].value : "";
    }

    if (type === "array") {
      var ar = await this._db.select(
        'SELECT id FROM "' + PREFIX + 'array" WHERE value_id = ?', [vid]);
      if (!ar.length) return [];
      var items = await this._db.select(
        'SELECT value_id FROM "' + PREFIX + 'array_item" WHERE array_id = ? ORDER BY idx', [ar[0].id]);
      var result = [];
      for (var i = 0; i < items.length; i++) {
        result.push(await this.load(items[i].value_id));
      }
      return result;
    }

    if (type === "object") {
      var ob = await this._db.select(
        'SELECT id FROM "' + PREFIX + 'object" WHERE value_id = ?', [vid]);
      if (!ob.length) return {};
      var items = await this._db.select(
        'SELECT key, value_id FROM "' + PREFIX + 'object_item" WHERE object_id = ?', [ob[0].id]);
      var result = {};
      for (var i = 0; i < items.length; i++) {
        result[items[i].key] = await this.load(items[i].value_id);
      }
      return result;
    }

    return null;
  };

  // ── QJSON text reconstruction ───────────────────────────────

  QDB.prototype.toQJSON = async function(vid) {
    var rows = await this._db.select(
      'SELECT type FROM "' + PREFIX + 'value" WHERE id = ?', [vid]);
    if (!rows.length) return "null";
    var type = rows[0].type;

    if (type === "null") return "null";
    if (type === "true") return "true";
    if (type === "false") return "false";

    if (type === "number" || type === "bigint" || type === "bigdec" || type === "bigfloat") {
      var nr = await this._db.select(
        'SELECT lo, str, hi FROM "' + PREFIX + 'number" WHERE value_id = ?', [vid]);
      if (!nr.length) return "null";
      if (type === "number") return String(nr[0].lo);
      var raw = nr[0].str !== null ? nr[0].str : String(nr[0].lo);
      if (type === "bigint") return raw + "N";
      if (type === "bigdec") return raw + "M";
      if (type === "bigfloat") return raw + "L";
    }

    if (type === "string") {
      var sr = await this._db.select(
        'SELECT value FROM "' + PREFIX + 'string" WHERE value_id = ?', [vid]);
      var s = sr.length ? sr[0].value : "";
      return '"' + s.replace(/\\/g, '\\\\').replace(/"/g, '\\"')
                     .replace(/\n/g, '\\n').replace(/\r/g, '\\r')
                     .replace(/\t/g, '\\t') + '"';
    }

    if (type === "array") {
      var ar = await this._db.select(
        'SELECT id FROM "' + PREFIX + 'array" WHERE value_id = ?', [vid]);
      if (!ar.length) return "[]";
      var items = await this._db.select(
        'SELECT value_id FROM "' + PREFIX + 'array_item" WHERE array_id = ? ORDER BY idx', [ar[0].id]);
      var parts = [];
      for (var i = 0; i < items.length; i++) {
        parts.push(await this.toQJSON(items[i].value_id));
      }
      return "[" + parts.join(",") + "]";
    }

    if (type === "object") {
      var ob = await this._db.select(
        'SELECT id FROM "' + PREFIX + 'object" WHERE value_id = ?', [vid]);
      if (!ob.length) return "{}";
      var items = await this._db.select(
        'SELECT key, value_id FROM "' + PREFIX + 'object_item" WHERE object_id = ? ORDER BY key', [ob[0].id]);
      var parts = [];
      for (var i = 0; i < items.length; i++) {
        var k = '"' + items[i].key.replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';
        parts.push(k + ":" + await this.toQJSON(items[i].value_id));
      }
      return "{" + parts.join(",") + "}";
    }

    return "null";
  };

  // Convenience
  QDB.prototype.save = function() { return this._db.save(); };
  QDB.prototype.export = function() { return this._db.export(); };
  QDB.prototype.close = function() { return this._db.close(); };
  QDB.prototype.shred = function() { return this._db.shred(); };

  // ── Factory ─────────────────────────────────────────────────

  async function open(opts) {
    if (!opts) opts = {};
    var handle = await SQLCipher.open({
      filename: opts.filename || "/qjson.db",
      key: opts.key,
      workerUrl: opts.workerUrl || "sqlcipher-worker.js"
    });
    var qdb = new QDB(handle);
    qdb.mode = handle.mode;
    await qdb.setup();
    return qdb;
  }

  return {open: open};
})();
