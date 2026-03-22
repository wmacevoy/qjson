"""QJSON SQL adapter — normalized relational storage for QJSON values.

Usage:
    from qjson.sql import adapter

    a = adapter(':memory:')
    a['setup']()
    vid = a['store']({'price': Decimal('67432.50'), 'items': [1, 2, 3]})
    val = a['load'](vid)
"""

import importlib.util as _ilu
import os as _os

_src_path = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'src', 'qjson_sql.py')
_spec = _ilu.spec_from_file_location("_qjson_sql_src", _src_path)
_mod = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_mod)

adapter = _mod.qjson_sql_adapter
round_down = _mod.round_down
round_up = _mod.round_up

__all__ = ["adapter", "round_down", "round_up"]
