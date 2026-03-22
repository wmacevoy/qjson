"""QJSON: JSON superset with exact numerics and interval-projected SQL storage.

Usage:
    from qjson import parse, stringify, BigInt, BigFloat, Blob
    from qjson.sql import adapter
    from qjson.query import select, update
"""

import importlib.util as _ilu
import os as _os

__version__ = "0.2.0"

# Load src/qjson.py directly to avoid circular import with this package
_src_path = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'src', 'qjson.py')
_spec = _ilu.spec_from_file_location("_qjson_src", _src_path)
_mod = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_mod)

# Re-export
parse = _mod.parse
stringify = _mod.stringify
BigInt = _mod.BigInt
BigFloat = _mod.BigFloat
Blob = _mod.Blob
js64_encode = _mod.js64_encode
js64_decode = _mod.js64_decode
qjson_parse = _mod.qjson_parse if hasattr(_mod, 'qjson_parse') else _mod.parse
qjson_stringify = _mod.qjson_stringify if hasattr(_mod, 'qjson_stringify') else _mod.stringify

__all__ = [
    "parse", "stringify", "BigInt", "BigFloat", "Blob",
    "js64_encode", "js64_decode",
]
