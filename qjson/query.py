"""QJSON query translator — jq-like path expressions → SQL.

Usage:
    from qjson.query import select, update

    results = select(conn, root_id, '.items[K]',
                     where_expr='.items[K].t > 30')
    update(conn, root_id, '.items[K].y', 3,
           where_expr='.items[K].t > 30')
"""

import importlib.util as _ilu
import os as _os
import sys as _sys

# Ensure src/ is on path so qjson_query.py can import qjson_sql
_src = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'src')
if _src not in _sys.path:
    _sys.path.insert(0, _src)

from qjson_query import (
    qjson_select as select,
    qjson_update as update,
    parse_path,
    compile_where,
)

__all__ = ["select", "update", "parse_path"]
