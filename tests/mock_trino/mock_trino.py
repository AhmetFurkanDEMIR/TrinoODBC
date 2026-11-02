#!/usr/bin/env python3
"""Protocol-accurate mock of the Trino coordinator REST API for driver tests.

Python 3.9, standard library only.  Implements the pieces of ARCHITECTURE.md section 7 that a
client can observe: ``POST /v1/statement``, paging through ``nextUri`` (columns are deliberately
withheld until the second response, exactly like a queued query on a real cluster), ``stats``,
error objects, 503 storms, cancellation via ``DELETE``, Basic/Bearer authentication, the
``X-Trino-Set-*`` family of response headers, ``system.jdbc.*`` catalog answers, ``DESCRIBE
INPUT/OUTPUT`` for header-prepared statements, ``EXECUTE IMMEDIATE`` echo and the spooled
protocol with inline + spooled ``json`` segments.

Usage::

    python3 mock_trino.py --port 18090 [--auth basic:user:pass | --auth bearer:TOKEN]
                          [--tls cert.pem key.pem] [--page-rows 1000] [--delay-ms 0]
                          [--spooling] [--verbose]

``GET /mock/stats`` returns counters as JSON and ``POST /mock/reset`` clears them, so tests can
assert on what the driver did on the wire (acks, cancels, request counts).
"""

import argparse
import base64
import datetime
import json
import re
import ssl
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable, Dict, List, Optional, Tuple

MOCK_VERSION = "483-mock"
SPOOL_SECRET_HEADER = "x-trino-mock-secret"
SPOOL_SECRET = "s3cr3t"
SEGMENTS_PER_RESPONSE = 2
SLOW_TABLE_QUEUED_RESPONSES = 10
SLOW_TABLE_RESPONSE_DELAY_SEC = 0.5
FLAKY_503_COUNT = 3

# ---------------------------------------------------------------------------------------------
# Trino type signatures
# ---------------------------------------------------------------------------------------------


def _split_top_level(text: str, sep: str = ",") -> List[str]:
    """Splits on ``sep`` outside parentheses and quotes."""
    parts: List[str] = []
    depth = 0
    quote: Optional[str] = None
    cur: List[str] = []
    i = 0
    while i < len(text):
        c = text[i]
        if quote:
            cur.append(c)
            if c == quote:
                if i + 1 < len(text) and text[i + 1] == quote:
                    cur.append(quote)
                    i += 1
                else:
                    quote = None
        elif c in ("'", '"'):
            quote = c
            cur.append(c)
        elif c == "(":
            depth += 1
            cur.append(c)
        elif c == ")":
            depth -= 1
            cur.append(c)
        elif c == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    if cur or parts:
        parts.append("".join(cur))
    return parts


def type_signature(type_name: str) -> Dict[str, Any]:
    """Builds the ``typeSignature`` object Trino sends next to ``type`` (best effort, the driver
    parses the ``type`` string and ignores this member)."""
    name = type_name.strip()
    m = re.match(r"^([a-z ]+?)\s*\((.*)\)\s*(with time zone)?$", name)
    if not m:
        return {"rawType": name, "arguments": []}
    raw = m.group(1).strip()
    if m.group(3):
        raw = raw + " " + m.group(3)
    args: List[Dict[str, Any]] = []
    for arg in _split_top_level(m.group(2)):
        arg = arg.strip()
        if re.match(r"^\d+$", arg):
            args.append({"kind": "LONG", "value": int(arg)})
        elif raw == "row":
            fm = re.match(r"^(\"[^\"]+\"|\w+)\s+(.+)$", arg)
            if fm:
                args.append({"kind": "NAMED_TYPE", "value": {"fieldName": {"name": fm.group(1).strip('"')},
                                                              "typeSignature": type_signature(fm.group(2))}})
            else:
                args.append({"kind": "TYPE", "value": type_signature(arg)})
        else:
            args.append({"kind": "TYPE", "value": type_signature(arg)})
    return {"rawType": raw, "arguments": args}


def column_json(name: str, type_name: str) -> Dict[str, Any]:
    return {"name": name, "type": type_name, "typeSignature": type_signature(type_name)}


def type_fixed_size(type_name: str) -> int:
    """``Type Size`` column of DESCRIBE OUTPUT: fixed width types report their byte width, 0 otherwise."""
    base = type_name.split("(")[0].strip().lower()
    sizes = {"boolean": 1, "tinyint": 1, "smallint": 2, "integer": 4, "bigint": 8, "real": 4, "double": 8,
             "date": 4, "time": 8, "timestamp": 8, "interval year to month": 4, "interval day to second": 8}
    if type_name.lower().startswith("timestamp") and "with time zone" in type_name.lower():
        return 8
    return sizes.get(base, 0)


# ---------------------------------------------------------------------------------------------
# Sample data
# ---------------------------------------------------------------------------------------------

TYPES_DEMO_COLUMNS: List[Tuple[str, str]] = [
    ("c_boolean", "boolean"),
    ("c_tinyint", "tinyint"),
    ("c_smallint", "smallint"),
    ("c_integer", "integer"),
    ("c_bigint", "bigint"),
    ("c_real", "real"),
    ("c_double", "double"),
    ("c_decimal_10_2", "decimal(10,2)"),
    ("c_decimal_38_10", "decimal(38,10)"),
    ("c_varchar", "varchar"),
    ("c_varchar20", "varchar(20)"),
    ("c_char5", "char(5)"),
    ("c_varbinary", "varbinary"),
    ("c_json", "json"),
    ("c_date", "date"),
    ("c_time3", "time(3)"),
    ("c_time6_tz", "time(6) with time zone"),
    ("c_timestamp3", "timestamp(3)"),
    ("c_timestamp6_tz", "timestamp(6) with time zone"),
    ("c_interval_ym", "interval year to month"),
    ("c_interval_ds", "interval day to second"),
    ("c_array", "array(integer)"),
    ("c_map", "map(varchar,integer)"),
    ("c_row", "row(a integer, b varchar)"),
    ("c_uuid", "uuid"),
    ("c_ipaddress", "ipaddress"),
]

UNICODE_TEXT = "Türkçe ğüşiöç 日本語 😀"

# Row 1: ordinary values. Row 2: every column NULL. Row 3: extremes, NaN/-Infinity, unicode text.
# The zoned timestamp of row 1 is deliberately outside the 2024-03-10 DST gap of America/New_York
# (02:00-03:00 does not exist there) so its UTC value is unambiguous: 16:30:00.123456.
TYPES_DEMO_ROWS: List[List[Any]] = [
    [True, 127, 32767, 2147483647, 9223372036854775807, 1.5, 3.141592653589793, "12345.67",
     "1234567890123456789012345678.1234567890", "hello world", "twenty", "abc  ",
     base64.b64encode(bytes([1, 2, 3, 255])).decode("ascii"), '{"a":1,"b":[1,2]}', "2024-03-10",
     "12:34:56.789", "12:34:56.123456+05:30", "2024-03-10 02:30:00.123",
     "2024-03-10 12:30:00.123456 America/New_York", "1-2", "3 04:05:06.789", [1, 2, 3], {"a": 1, "b": 2},
     [1, "x"], "12345678-1234-5678-1234-567812345678", "10.0.0.1"],
    [None] * len(TYPES_DEMO_COLUMNS),
    [False, -128, -32768, -2147483648, -9223372036854775808, "NaN", "-Infinity", "-0.01", "-1.0000000000",
     UNICODE_TEXT, "Türkçe ğüşiöç", "xyz  ", base64.b64encode(b"\x00").decode("ascii"), '"str"', "1900-01-01",
     "00:00:00.000", "23:59:59.999999-08:00", "1970-01-01 00:00:00.000", "2024-03-10 02:30:00.123456 +02:00",
     "-0-11", "-1 00:00:00.000", [], {}, [None, "y"], "00000000-0000-0000-0000-000000000000", "2001:db8::1"],
]

BIG_TABLE_COLUMNS: List[Tuple[str, str]] = [
    ("id", "bigint"),
    ("name", "varchar"),
    ("value", "double"),
    ("ts", "timestamp(3)"),
    ("amount", "decimal(18,4)"),
]
BIG_TABLE_DEFAULT_ROWS = 100000
_BIG_TABLE_EPOCH = datetime.datetime(2024, 1, 1, 0, 0, 0)


def big_table_row(i: int, cols: int) -> List[Any]:
    """Row ``i`` (0-based) of big_table: deterministic and cheap to compute on demand."""
    ts = _BIG_TABLE_EPOCH + datetime.timedelta(seconds=i)
    row: List[Any] = [i + 1, "name_%d" % (i + 1), i * 0.5, ts.strftime("%Y-%m-%d %H:%M:%S.000"),
                      "%d.%04d" % ((i * 125) // 100, (i * 125) % 100 * 100)]
    if cols < len(row):
        return row[:cols]
    for extra in range(len(row), cols):
        row.append("v%d_%d" % (extra + 1, i + 1))
    return row


def big_table_columns(cols: int) -> List[Tuple[str, str]]:
    columns = list(BIG_TABLE_COLUMNS[:cols])
    for extra in range(len(BIG_TABLE_COLUMNS), cols):
        columns.append(("col%d" % (extra + 1), "varchar"))
    return columns


CUSTOMER_COLUMNS: List[Tuple[str, str]] = [
    ("custkey", "bigint"), ("name", "varchar(25)"), ("address", "varchar(40)"), ("nationkey", "bigint"),
    ("phone", "varchar(15)"), ("acctbal", "double"), ("mktsegment", "varchar(10)"), ("comment", "varchar(117)"),
]
CUSTOMER_ROWS = 1500
_SEGMENTS = ["BUILDING", "AUTOMOBILE", "MACHINERY", "HOUSEHOLD", "FURNITURE"]


def customer_row(i: int) -> List[Any]:
    key = i + 1
    return [key, "Customer#%09d" % key, "addr %d" % key, key % 25, "%02d-%03d-%03d-%04d" % (key % 25 + 10, key % 1000, (key * 7) % 1000, key % 10000),
            round(key * 1.5 - 500, 2), _SEGMENTS[key % 5], "comment %d" % key]


ORDERS_COLUMNS = [("orderkey", "bigint"), ("custkey", "bigint"), ("orderstatus", "varchar(1)"), ("totalprice", "double"),
                  ("orderdate", "date"), ("orderpriority", "varchar(15)"), ("clerk", "varchar(15)"),
                  ("shippriority", "integer"), ("comment", "varchar(79)")]
LINEITEM_COLUMNS = [("orderkey", "bigint"), ("partkey", "bigint"), ("suppkey", "bigint"), ("linenumber", "integer"),
                    ("quantity", "double"), ("extendedprice", "double"), ("discount", "double"), ("tax", "double"),
                    ("returnflag", "varchar(1)"), ("linestatus", "varchar(1)"), ("shipdate", "date"),
                    ("commitdate", "date"), ("receiptdate", "date"), ("shipinstruct", "varchar(25)"),
                    ("shipmode", "varchar(10)"), ("comment", "varchar(44)")]
NATION_COLUMNS = [("nationkey", "bigint"), ("name", "varchar(25)"), ("regionkey", "bigint"), ("comment", "varchar(152)")]
REGION_COLUMNS = [("regionkey", "bigint"), ("name", "varchar(25)"), ("comment", "varchar(152)")]
DEMO_VIEW_COLUMNS = [("id", "bigint"), ("label", "varchar")]

# (catalog, schema, table, type, columns)
CATALOG_TABLES: List[Tuple[str, str, str, str, List[Tuple[str, str]]]] = [
    ("tpch", "tiny", "customer", "TABLE", CUSTOMER_COLUMNS),
    ("tpch", "tiny", "orders", "TABLE", ORDERS_COLUMNS),
    ("tpch", "tiny", "lineitem", "TABLE", LINEITEM_COLUMNS),
    ("tpch", "tiny", "nation", "TABLE", NATION_COLUMNS),
    ("tpch", "tiny", "region", "TABLE", REGION_COLUMNS),
    ("mock", "demo", "types_demo", "TABLE", TYPES_DEMO_COLUMNS),
    ("mock", "demo", "big_table", "TABLE", BIG_TABLE_COLUMNS),
    ("mock", "demo", "demo_view", "VIEW", DEMO_VIEW_COLUMNS),
]
CATALOGS = ["mock", "system", "tpch"]
SCHEMAS: List[Tuple[str, str]] = [  # (table_catalog, table_schem)
    ("mock", "demo"), ("mock", "information_schema"),
    ("system", "information_schema"), ("system", "jdbc"), ("system", "runtime"),
    ("tpch", "information_schema"), ("tpch", "sf1"), ("tpch", "tiny"),
]
TABLE_TYPES = ["TABLE", "VIEW"]

# JDBC type codes for system.jdbc.columns.data_type (the driver recomputes them, but a real
# server fills them so the mock does too).
_JDBC_TYPES = {"boolean": 16, "tinyint": -6, "smallint": 5, "integer": 4, "bigint": -5, "real": 7, "double": 8,
               "decimal": 3, "varchar": 12, "char": 1, "varbinary": -3, "json": 2000, "date": 91, "time": 92,
               "timestamp": 93, "array": 2003, "map": 2000, "row": 2000, "uuid": 2000, "ipaddress": 2000,
               "interval year to month": 2000, "interval day to second": 2000}


def jdbc_column_row(cat: str, schem: str, table: str, name: str, type_name: str, ordinal: int) -> Dict[str, Any]:
    base = type_name.split("(")[0].strip().lower()
    if type_name.lower().startswith("time") and "with time zone" in type_name.lower():
        base = "time" if base.startswith("time(") or base == "time" else base
    code = _JDBC_TYPES.get(base, 2000)
    if "with time zone" in type_name.lower():
        code = 2014 if base.startswith("timestamp") else 2013
    m = re.match(r"^(?:varchar|char)\((\d+)\)$", type_name.lower())
    size: Optional[int] = int(m.group(1)) if m else None
    dm = re.match(r"^decimal\((\d+),\s*(\d+)\)$", type_name.lower())
    digits: Optional[int] = None
    if dm:
        size, digits = int(dm.group(1)), int(dm.group(2))
    return {
        "table_cat": cat, "table_schem": schem, "table_name": table, "column_name": name, "data_type": code,
        "type_name": type_name, "column_size": size, "buffer_length": None, "decimal_digits": digits,
        "num_prec_radix": 10 if dm else None, "nullable": 1, "remarks": None, "column_def": None,
        "sql_data_type": code, "sql_datetime_sub": None, "char_octet_length": size if m else None,
        "ordinal_position": ordinal, "is_nullable": "YES", "scope_catalog": None, "scope_schema": None,
        "scope_table": None, "source_data_type": None, "is_autoincrement": "", "is_generatedcolumn": "",
    }


JDBC_TABLE_COLUMNS: Dict[str, List[Tuple[str, str]]] = {
    "catalogs": [("table_cat", "varchar")],
    "schemas": [("table_schem", "varchar"), ("table_catalog", "varchar")],
    "tables": [("table_cat", "varchar"), ("table_schem", "varchar"), ("table_name", "varchar"), ("table_type", "varchar"),
               ("remarks", "varchar"), ("type_cat", "varchar"), ("type_schem", "varchar"), ("type_name", "varchar"),
               ("self_referencing_col_name", "varchar"), ("ref_generation", "varchar")],
    "columns": [("table_cat", "varchar"), ("table_schem", "varchar"), ("table_name", "varchar"), ("column_name", "varchar"),
                ("data_type", "integer"), ("type_name", "varchar"), ("column_size", "integer"), ("buffer_length", "integer"),
                ("decimal_digits", "integer"), ("num_prec_radix", "integer"), ("nullable", "integer"), ("remarks", "varchar"),
                ("column_def", "varchar"), ("sql_data_type", "integer"), ("sql_datetime_sub", "integer"),
                ("char_octet_length", "integer"), ("ordinal_position", "integer"), ("is_nullable", "varchar"),
                ("scope_catalog", "varchar"), ("scope_schema", "varchar"), ("scope_table", "varchar"),
                ("source_data_type", "smallint"), ("is_autoincrement", "varchar"), ("is_generatedcolumn", "varchar")],
    "table_types": [("table_type", "varchar")],
    "types": [("type_name", "varchar"), ("data_type", "integer"), ("precision", "integer"), ("literal_prefix", "varchar"),
              ("literal_suffix", "varchar"), ("create_params", "varchar"), ("nullable", "smallint"),
              ("case_sensitive", "boolean"), ("searchable", "smallint"), ("unsigned_attribute", "boolean"),
              ("fixed_prec_scale", "boolean"), ("auto_increment", "boolean"), ("local_type_name", "varchar"),
              ("minimum_scale", "smallint"), ("maximum_scale", "smallint"), ("sql_data_type", "integer"),
              ("sql_datetime_sub", "integer"), ("num_prec_radix", "integer")],
    "procedures": [("procedure_cat", "varchar"), ("procedure_schem", "varchar"), ("procedure_name", "varchar"),
                   ("remarks", "varchar"), ("procedure_type", "smallint"), ("specific_name", "varchar")],
    "procedure_columns": [("procedure_cat", "varchar"), ("procedure_schem", "varchar"), ("procedure_name", "varchar"),
                          ("column_name", "varchar"), ("column_type", "smallint"), ("data_type", "integer"),
                          ("type_name", "varchar"), ("precision", "integer"), ("length", "integer"), ("scale", "smallint"),
                          ("radix", "smallint"), ("nullable", "smallint"), ("remarks", "varchar"), ("column_def", "varchar"),
                          ("sql_data_type", "integer"), ("sql_datetime_sub", "integer"), ("char_octet_length", "integer"),
                          ("ordinal_position", "integer"), ("is_nullable", "varchar"), ("specific_name", "varchar")],
}


def jdbc_rows(table: str) -> List[Dict[str, Any]]:
    if table == "catalogs":
        return [{"table_cat": c} for c in CATALOGS]
    if table == "schemas":
        return [{"table_schem": s, "table_catalog": c} for c, s in SCHEMAS]
    if table == "tables":
        return [{"table_cat": c, "table_schem": s, "table_name": t, "table_type": ty, "remarks": None, "type_cat": None,
                 "type_schem": None, "type_name": None, "self_referencing_col_name": None, "ref_generation": None}
                for c, s, t, ty, _ in CATALOG_TABLES]
    if table == "columns":
        rows = []
        for c, s, t, _, cols in CATALOG_TABLES:
            for i, (name, ty) in enumerate(cols):
                rows.append(jdbc_column_row(c, s, t, name, ty, i + 1))
        return rows
    if table == "table_types":
        return [{"table_type": t} for t in TABLE_TYPES]
    if table == "types":
        rows = []
        for name, code, prec in [("boolean", 16, 1), ("bigint", -5, 19), ("integer", 4, 10), ("double", 8, 15),
                                 ("varchar", 12, 2147483647), ("date", 91, 10), ("timestamp", 93, 23)]:
            rows.append({"type_name": name, "data_type": code, "precision": prec, "literal_prefix": None,
                         "literal_suffix": None, "create_params": None, "nullable": 1, "case_sensitive": name == "varchar",
                         "searchable": 3, "unsigned_attribute": False, "fixed_prec_scale": False, "auto_increment": False,
                         "local_type_name": name, "minimum_scale": None, "maximum_scale": None, "sql_data_type": code,
                         "sql_datetime_sub": None, "num_prec_radix": 10})
        return rows
    return []  # procedures / procedure_columns


# ---------------------------------------------------------------------------------------------
# SQL text helpers
# ---------------------------------------------------------------------------------------------


def strip_comments(sql: str) -> str:
    out: List[str] = []
    i = 0
    quote: Optional[str] = None
    while i < len(sql):
        c = sql[i]
        if quote:
            out.append(c)
            if c == quote:
                quote = None
            i += 1
        elif c in ("'", '"'):
            quote = c
            out.append(c)
            i += 1
        elif sql.startswith("/*", i):
            end = sql.find("*/", i + 2)
            i = len(sql) if end < 0 else end + 2
            out.append(" ")
        elif sql.startswith("--", i):
            end = sql.find("\n", i)
            i = len(sql) if end < 0 else end
        else:
            out.append(c)
            i += 1
    return "".join(out)


def normalize(sql: str) -> str:
    text = strip_comments(sql)
    # Whitespace outside quotes collapses to single spaces; quoted text is preserved verbatim.
    out: List[str] = []
    quote: Optional[str] = None
    pending_space = False
    for c in text:
        if quote:
            out.append(c)
            if c == quote:
                quote = None
        elif c in ("'", '"'):
            if pending_space and out:
                out.append(" ")
            pending_space = False
            quote = c
            out.append(c)
        elif c.isspace():
            pending_space = True
        else:
            if pending_space and out:
                out.append(" ")
            pending_space = False
            out.append(c)
    result = "".join(out).strip()
    while result.endswith(";"):
        result = result[:-1].rstrip()
    return result


def unquote_ident(name: str) -> str:
    name = name.strip()
    if len(name) >= 2 and name[0] == '"' and name[-1] == '"':
        return name[1:-1].replace('""', '"')
    return name.lower()


def sql_string_literal(text: str, pos: int) -> Tuple[str, int]:
    """Parses ``'...'`` at ``pos`` (with '' escapes). Returns (value, index after closing quote)."""
    if pos >= len(text) or text[pos] != "'":
        raise ValueError("expected string literal")
    i = pos + 1
    out: List[str] = []
    while i < len(text):
        if text[i] == "'":
            if i + 1 < len(text) and text[i + 1] == "'":
                out.append("'")
                i += 2
                continue
            return "".join(out), i + 1
        out.append(text[i])
        i += 1
    raise ValueError("unterminated string literal")


def find_keyword(text: str, keyword: str) -> int:
    """Index of ``keyword`` (case-insensitive, whole word) outside quotes and parentheses, or -1."""
    depth = 0
    quote: Optional[str] = None
    n = len(keyword)
    i = 0
    while i < len(text):
        c = text[i]
        if quote:
            if c == quote:
                quote = None
        elif c in ("'", '"'):
            quote = c
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif depth == 0 and text[i:i + n].lower() == keyword.lower():
            before = i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")
            after = i + n >= len(text) or not (text[i + n].isalnum() or text[i + n] == "_")
            if before and after:
                return i
        i += 1
    return -1


def count_placeholders(text: str) -> int:
    count = 0
    quote: Optional[str] = None
    for c in text:
        if quote:
            if c == quote:
                quote = None
        elif c in ("'", '"'):
            quote = c
        elif c == "?":
            count += 1
    return count


def like_to_regex(pattern: str) -> str:
    out: List[str] = []
    i = 0
    while i < len(pattern):
        c = pattern[i]
        if c == "\\" and i + 1 < len(pattern):
            out.append(re.escape(pattern[i + 1]))
            i += 2
            continue
        if c == "%":
            out.append(".*")
        elif c == "_":
            out.append(".")
        else:
            out.append(re.escape(c))
        i += 1
    return "^" + "".join(out) + "$"


class MockError(Exception):
    """A Trino ``error`` object to deliver in the response stream."""

    def __init__(self, name: str, message: str, code: int = 1, error_type: str = "USER_ERROR",
                 sql_state: Optional[str] = None, line: int = 1, column: int = 1) -> None:
        super().__init__(message)
        self.name = name
        self.message = message
        self.code = code
        self.error_type = error_type
        self.sql_state = sql_state
        self.line = line
        self.column = column

    def to_json(self) -> Dict[str, Any]:
        obj: Dict[str, Any] = {
            "message": self.message, "errorCode": self.code, "errorName": self.name, "errorType": self.error_type,
            "errorLocation": {"lineNumber": self.line, "columnNumber": self.column},
            "failureInfo": {"type": "io.trino.spi.TrinoException", "message": self.message, "suppressed": [],
                            "stack": []},
        }
        if self.sql_state:
            obj["sqlState"] = self.sql_state
        return obj


def syntax_error(sql: str) -> MockError:
    return MockError("SYNTAX_ERROR", "line 1:1: mismatched input '%s'. Expecting: statement" % sql[:40].replace("'", ""), 1)


# ---------------------------------------------------------------------------------------------
# Query plans
# ---------------------------------------------------------------------------------------------


class Plan:
    """What a statement produces: columns + rows (materialised or lazily generated), or an update
    result, plus the failure modes of the special mock tables."""

    def __init__(self) -> None:
        self.columns: List[Tuple[str, str]] = []
        self.rows: List[List[Any]] = []
        self.row_count = 0
        self.row_fn: Optional[Callable[[int], List[Any]]] = None
        self.update_type: Optional[str] = None
        self.update_count: Optional[int] = None
        self.error: Optional[MockError] = None      # delivered instead of columns (2nd response)
        self.error_after_first_page = False          # error_table: delivered after one data page
        self.queued_responses = 0                    # slow_table: extra QUEUED responses
        self.flaky_503 = 0                           # flaky_table: leading 503 answers
        self.http_status: Optional[Tuple[int, str]] = None  # auth_table: raw HTTP failure on POST
        self.warnings: List[Dict[str, Any]] = []
        self.spoolable = False
        self.response_headers: Dict[str, str] = {}
        self.source_catalog = ""
        self.source_schema = ""
        self.source_table = ""
        self.describe_output_empty = False           # DESCRIBE OUTPUT of a non-query statement

    @property
    def has_columns(self) -> bool:
        return bool(self.columns)

    def total_rows(self) -> int:
        return self.row_count if self.row_fn else len(self.rows)

    def row(self, i: int) -> List[Any]:
        return self.row_fn(i) if self.row_fn else self.rows[i]

    def slice(self, start: int, end: int) -> List[List[Any]]:
        end = min(end, self.total_rows())
        return [self.row(i) for i in range(start, end)]

    @staticmethod
    def single(columns: List[Tuple[str, str]], rows: List[List[Any]]) -> "Plan":
        p = Plan()
        p.columns = columns
        p.rows = rows
        return p

    @staticmethod
    def update(update_type: str, count: Optional[int]) -> "Plan":
        p = Plan()
        p.update_type = update_type
        p.update_count = count
        return p


class RequestContext:
    """The request headers a statement depends on."""

    def __init__(self, headers: Any, auth_user: Optional[str]) -> None:
        self.user = headers.get("X-Trino-User") or auth_user or ""
        self.catalog = headers.get("X-Trino-Catalog") or "mock"
        self.schema = headers.get("X-Trino-Schema") or "demo"
        self.prepared: Dict[str, str] = {}
        for value in _header_values(headers, "X-Trino-Prepared-Statement"):
            for item in value.split(","):
                if "=" in item:
                    name, enc = item.split("=", 1)
                    self.prepared[name.strip()] = urllib.parse.unquote(enc.strip())
        self.transaction_id = headers.get("X-Trino-Transaction-Id")
        self.encoding = headers.get("X-Trino-Query-Data-Encoding") or ""


def _header_values(headers: Any, name: str) -> List[str]:
    return [v for k, v in headers.items() if k.lower() == name.lower()]


class Planner:
    def __init__(self, page_rows: int) -> None:
        self.page_rows = page_rows

    # ---- entry -------------------------------------------------------------------------------
    def plan(self, raw_sql: str, ctx: RequestContext) -> Plan:
        options = self._comment_options(raw_sql)
        sql = normalize(raw_sql)
        low = sql.lower()
        if not sql:
            raise syntax_error(sql)

        # Transactions follow Trino: only START TRANSACTION (sent with X-Trino-Transaction-Id: NONE)
        # opens one and returns X-Trino-Started-Transaction-Id; statements carrying that id run
        # inside it; COMMIT / ROLLBACK end it. NONE on any other statement means autocommit.
        txn = (ctx.transaction_id or "").strip()
        ends_txn = low.startswith("start transaction") or low in ("commit", "commit work", "rollback", "rollback work")
        if txn and txn.upper() != "NONE" and not ends_txn:
            if not STATE.in_transaction(txn):
                raise MockError("UNKNOWN_TRANSACTION", "Unknown transaction ID: %s" % txn, 45)
            STATE.bump("transactional_statements")

        m = re.match(r"^select version\(\)$", low)
        if m:
            return Plan.single([("_col0", "varchar")], [[MOCK_VERSION]])
        if low == "select current_catalog":
            return Plan.single([("_col0", "varchar")], [[ctx.catalog]])
        if low == "select current_schema":
            return Plan.single([("_col0", "varchar")], [[ctx.schema]])
        if low == "select current_user":
            return Plan.single([("_col0", "varchar")], [[ctx.user]])
        m = re.match(r"^select (-?\d+)$", low)
        if m:
            v = int(m.group(1))
            return Plan.single([("_col0", "integer" if -2147483648 <= v <= 2147483647 else "bigint")], [[v]])
        if low.startswith("select '"):
            try:
                value, end = sql_string_literal(sql, 7)
            except ValueError:
                raise syntax_error(sql)
            if end == len(sql):
                return Plan.single([("_col0", "varchar(%d)" % max(len(value), 1))], [[value]])

        if low.startswith("select ") or low.startswith("with "):
            return self._plan_select(sql, ctx, options)
        if low == "show catalogs":
            return Plan.single([("Catalog", "varchar")], [[c] for c in CATALOGS])
        m = re.match(r"^show schemas(?: from (\S+))?$", low)
        if m:
            cat = unquote_ident(m.group(1)) if m.group(1) else ctx.catalog
            return Plan.single([("Schema", "varchar")], [[s] for c, s in SCHEMAS if c == cat])
        m = re.match(r"^show tables(?: from (\S+))?$", low)
        if m:
            ref = m.group(1)
            if ref and "." in ref:
                cat, schem = [unquote_ident(x) for x in ref.split(".", 1)]
            else:
                cat, schem = ctx.catalog, (unquote_ident(ref) if ref else ctx.schema)
            return Plan.single([("Table", "varchar")], [[t] for c, s, t, _, _ in CATALOG_TABLES if c == cat and s == schem])
        if low.startswith("use "):
            return self._plan_use(sql[4:], ctx)
        m = re.match(r"^set session (\S+?)\s*=\s*(.+)$", sql, re.IGNORECASE)
        if m:
            value = m.group(2).strip()
            if value.startswith("'"):
                try:
                    value, _ = sql_string_literal(value, 0)
                except ValueError:
                    raise syntax_error(sql)
            p = Plan.update("SET SESSION", None)
            p.response_headers["X-Trino-Set-Session"] = "%s=%s" % (m.group(1), urllib.parse.quote(value, safe=""))
            return p
        m = re.match(r"^reset session (\S+)$", sql, re.IGNORECASE)
        if m:
            p = Plan.update("RESET SESSION", None)
            p.response_headers["X-Trino-Clear-Session"] = m.group(1)
            return p
        if low.startswith("start transaction"):
            if not txn:
                raise MockError("INCOMPATIBLE_CLIENT", "Client does not support transactions", 60)
            if txn.upper() != "NONE":
                raise MockError("NOT_SUPPORTED", "Nested transactions not supported", 13)
            p = Plan.update("START TRANSACTION", None)
            p.response_headers["X-Trino-Started-Transaction-Id"] = STATE.begin_transaction()
            return p
        if low in ("commit", "commit work", "rollback", "rollback work"):
            verb = low.split()[0]
            if not txn or txn.upper() == "NONE":
                raise MockError("NOT_IN_TRANSACTION", "No transaction in progress", 44)
            if not STATE.end_transaction(txn, verb == "commit"):
                raise MockError("UNKNOWN_TRANSACTION", "Unknown transaction ID: %s" % txn, 45)
            p = Plan.update(verb.upper(), None)
            p.response_headers["X-Trino-Clear-Transaction-Id"] = "true"
            return p
        m = re.match(r"^prepare (\S+) from (.+)$", sql, re.IGNORECASE | re.DOTALL)
        if m:
            p = Plan.update("PREPARE", None)
            p.response_headers["X-Trino-Added-Prepare"] = "%s=%s" % (m.group(1), urllib.parse.quote(m.group(2), safe=""))
            return p
        m = re.match(r"^deallocate prepare (\S+)$", sql, re.IGNORECASE)
        if m:
            p = Plan.update("DEALLOCATE PREPARE", None)
            p.response_headers["X-Trino-Deallocated-Prepare"] = m.group(1)
            return p
        m = re.match(r"^describe output (\S+)$", sql, re.IGNORECASE)
        if m:
            return self._plan_describe_output(m.group(1), ctx)
        m = re.match(r"^describe input (\S+)$", sql, re.IGNORECASE)
        if m:
            return self._plan_describe_input(m.group(1), ctx)
        if low.startswith("execute immediate "):
            return self._plan_execute_immediate(sql, ctx)
        m = re.match(r"^execute (\S+)(?: using (.+))?$", sql, re.IGNORECASE | re.DOTALL)
        if m:
            name = m.group(1)
            if name not in ctx.prepared:
                raise MockError("NOT_FOUND", "Prepared statement not found: %s" % name, 5)
            return self._plan_execute(ctx.prepared[name], m.group(2), ctx)
        m = re.match(r"^(insert|update|delete|merge)\b", low)
        if m:
            return Plan.update(m.group(1).upper(), 3)
        m = re.match(r"^(create|drop|alter|comment|grant|revoke|deny|analyze|refresh|truncate|call)\b(?:\s+(or replace))?\s*(\w+)?", low)
        if m:
            words = [m.group(1).upper()]
            if m.group(3) and m.group(1) in ("create", "drop", "alter", "refresh", "truncate", "comment"):
                words.append(m.group(3).upper())
            return Plan.update(" ".join(words), None)
        if low.startswith("explain"):
            return Plan.single([("Query Plan", "varchar")], [["Mock plan"]])
        raise syntax_error(sql)

    # ---- helpers -----------------------------------------------------------------------------
    @staticmethod
    def _comment_options(raw_sql: str) -> Dict[str, int]:
        opts: Dict[str, int] = {}
        for m in re.finditer(r"/\*(.*?)\*/", raw_sql, re.DOTALL):
            for key, value in re.findall(r"(rows|cols)\s*=\s*(\d+)", m.group(1)):
                opts[key] = int(value)
        return opts

    def _plan_use(self, target: str, ctx: RequestContext) -> Plan:
        parts = [unquote_ident(x) for x in _split_top_level(target.strip(), ".")]
        p = Plan.update("USE", None)
        if len(parts) >= 2:
            p.response_headers["X-Trino-Set-Catalog"] = parts[0]
            p.response_headers["X-Trino-Set-Schema"] = parts[1]
        elif parts[0] in CATALOGS:
            # A bare identifier naming a catalog switches the catalog (the driver's
            # SQL_ATTR_CURRENT_CATALOG path); anything else is a schema of the current catalog.
            p.response_headers["X-Trino-Set-Catalog"] = parts[0]
        else:
            if not any(c == ctx.catalog and s == parts[0] for c, s in SCHEMAS):
                raise MockError("SCHEMA_NOT_FOUND", "Schema does not exist: %s.%s" % (ctx.catalog, parts[0]), 44)
            p.response_headers["X-Trino-Set-Schema"] = parts[0]
        return p

    def _resolve_table(self, ref: str, ctx: RequestContext) -> Tuple[str, str, str]:
        parts = [unquote_ident(x) for x in _split_top_level(ref, ".")]
        if len(parts) == 3:
            return parts[0], parts[1], parts[2]
        if len(parts) == 2:
            return ctx.catalog, parts[0], parts[1]
        return ctx.catalog, ctx.schema, parts[0]

    def _plan_select(self, sql: str, ctx: RequestContext, options: Dict[str, int]) -> Plan:
        if count_placeholders(sql):
            raise MockError("SYNTAX_ERROR", "line 1:1: Incorrect number of parameters: expected %d but found 0"
                            % count_placeholders(sql), 1)
        from_pos = find_keyword(sql, "from")
        if from_pos < 0:
            raise syntax_error(sql)
        select_list = sql[6:from_pos].strip()
        rest = sql[from_pos + 4:].strip()
        where_pos = find_keyword(rest, "where")
        order_pos = find_keyword(rest, "order")
        limit_pos = find_keyword(rest, "limit")
        table_end = min(x for x in (where_pos, order_pos, limit_pos, len(rest)) if x >= 0)
        table_ref = rest[:table_end].strip()
        if not table_ref or " " in table_ref and not table_ref.startswith('"'):
            # "FROM t alias" is tolerated by dropping the alias.
            table_ref = table_ref.split(" ")[0]
        where = None
        if where_pos >= 0:
            where_end = min(x for x in (order_pos, limit_pos, len(rest)) if x >= 0 and x > where_pos)
            where = rest[where_pos + 5:where_end].strip()
        order_by = None
        if order_pos >= 0:
            order_end = limit_pos if limit_pos > order_pos else len(rest)
            order_by = rest[order_pos:order_end]
            order_by = re.sub(r"^order\s+by\s+", "", order_by, flags=re.IGNORECASE).strip()
        limit = None
        if limit_pos >= 0:
            lm = re.match(r"^limit\s+(\d+)", rest[limit_pos:], re.IGNORECASE)
            if not lm:
                raise syntax_error(sql)
            limit = int(lm.group(1))

        cat, schem, table = self._resolve_table(table_ref, ctx)
        if cat == "system" and schem == "jdbc":
            return self._plan_jdbc(table, select_list, where, order_by, limit)
        plan = self._table_plan(table, options, ctx)
        if plan is None:
            raise MockError("TABLE_NOT_FOUND", "line 1:%d: Table '%s.%s.%s' does not exist"
                            % (from_pos + 6, cat, schem, table), 46, column=from_pos + 6)
        plan.source_catalog, plan.source_schema, plan.source_table = cat, schem, table
        if where is not None:
            plan = self._apply_where(plan, where, sql)
        plan = self._project(plan, select_list, sql)
        if limit is not None and limit < plan.total_rows():
            plan = self._limit(plan, limit)
        return plan

    def _table_plan(self, table: str, options: Dict[str, int], ctx: RequestContext) -> Optional[Plan]:
        p = Plan()
        if table == "types_demo":
            p.columns = TYPES_DEMO_COLUMNS
            p.rows = TYPES_DEMO_ROWS
            p.spoolable = True
        elif table == "big_table":
            rows = options.get("rows", BIG_TABLE_DEFAULT_ROWS)
            cols = max(1, options.get("cols", len(BIG_TABLE_COLUMNS)))
            p.columns = big_table_columns(cols)
            p.row_count = rows
            p.row_fn = lambda i, cols=cols: big_table_row(i, cols)
            p.spoolable = True
        elif table == "customer":
            p.columns = CUSTOMER_COLUMNS
            p.row_count = CUSTOMER_ROWS
            p.row_fn = customer_row
            p.spoolable = True
        elif table == "demo_view":
            p.columns = DEMO_VIEW_COLUMNS
            p.rows = [[1, "one"], [2, "two"]]
        elif table in ("orders", "lineitem", "nation", "region"):
            p.columns = {"orders": ORDERS_COLUMNS, "lineitem": LINEITEM_COLUMNS, "nation": NATION_COLUMNS,
                         "region": REGION_COLUMNS}[table]
            p.rows = []
        elif table == "error_table":
            p.columns = [("id", "bigint"), ("name", "varchar")]
            p.row_count = self.page_rows * 2
            p.row_fn = lambda i: [i + 1, "row_%d" % (i + 1)]
            p.error = MockError("DIVISION_BY_ZERO", "Division by zero", 65545)
            p.error_after_first_page = True
        elif table == "slow_table":
            p.columns = [("id", "bigint")]
            p.rows = [[1]]
            p.queued_responses = SLOW_TABLE_QUEUED_RESPONSES
        elif table == "flaky_table":
            p.columns = [("id", "bigint"), ("name", "varchar")]
            p.rows = [[1, "flaky"], [2, "but fine"]]
            p.flaky_503 = FLAKY_503_COUNT
        elif table == "auth_table":
            p.http_status = (403, "Access Denied")
        elif table == "warn_table":
            p.columns = [("id", "bigint"), ("name", "varchar")]
            p.rows = [[1, "warned"]]
            p.warnings = [{"warningCode": {"code": 1, "name": "MOCK_WARNING"}, "message": "mock warning"}]
        else:
            return None
        return p

    @staticmethod
    def _apply_where(plan: Plan, where: str, sql: str) -> Plan:
        m = re.match(r"^(\"?\w+\"?)\s*=\s*(-?\d+|'(?:[^']|'')*')$", where.strip())
        if not m:
            raise syntax_error(sql)
        col = unquote_ident(m.group(1))
        names = [c[0] for c in plan.columns]
        if col not in names:
            raise MockError("COLUMN_NOT_FOUND", "line 1:1: Column '%s' cannot be resolved" % col, 47)
        idx = names.index(col)
        raw = m.group(2)
        value: Any = sql_string_literal(raw, 0)[0] if raw.startswith("'") else int(raw)
        out = Plan()
        out.columns = plan.columns
        out.spoolable = plan.spoolable
        out.warnings = plan.warnings
        if plan.row_fn and col == "id" and isinstance(value, int):
            # big_table ids are 1-based row numbers: answer without scanning.
            out.rows = [plan.row(value - 1)] if 1 <= value <= plan.total_rows() else []
        else:
            out.rows = [r for r in plan.slice(0, plan.total_rows()) if r[idx] == value]
        return out

    @staticmethod
    def _project(plan: Plan, select_list: str, sql: str) -> Plan:
        select_list = select_list.strip()
        if select_list == "*":
            return plan
        if re.match(r"^count\(\*\)$", select_list, re.IGNORECASE):
            out = Plan.single([("_col0", "bigint")], [[plan.total_rows()]])
            out.warnings = plan.warnings
            return out
        names = [c[0] for c in plan.columns]
        indexes: List[int] = []
        columns: List[Tuple[str, str]] = []
        for item in _split_top_level(select_list):
            item = item.strip()
            am = re.match(r"^(.+?)\s+as\s+(\"[^\"]+\"|\w+)$", item, re.IGNORECASE)
            alias = None
            if am:
                item, alias = am.group(1).strip(), unquote_ident(am.group(2))
            if "." in item and not item.startswith('"'):
                item = item.split(".")[-1]
            elif item.count('"') >= 4:
                item = '"' + item.rsplit('"', 2)[-2] + '"'
            name = unquote_ident(item)
            if name not in names:
                raise MockError("COLUMN_NOT_FOUND", "line 1:8: Column '%s' cannot be resolved" % name, 47, column=8)
            indexes.append(names.index(name))
            columns.append((alias or name, plan.columns[names.index(name)][1]))
        out = Plan()
        out.columns = columns
        out.spoolable = plan.spoolable
        out.warnings = plan.warnings
        out.source_catalog, out.source_schema, out.source_table = plan.source_catalog, plan.source_schema, plan.source_table
        out.error, out.error_after_first_page = plan.error, plan.error_after_first_page
        out.queued_responses, out.flaky_503 = plan.queued_responses, plan.flaky_503
        if plan.row_fn:
            fn = plan.row_fn
            out.row_count = plan.row_count
            out.row_fn = lambda i, fn=fn, idx=indexes: [fn(i)[j] for j in idx]
        else:
            out.rows = [[r[j] for j in indexes] for r in plan.rows]
        return out

    @staticmethod
    def _limit(plan: Plan, limit: int) -> Plan:
        out = Plan()
        out.__dict__.update(plan.__dict__)
        if plan.row_fn:
            out.row_count = min(limit, plan.row_count)
        else:
            out.rows = plan.rows[:limit]
        return out

    # ---- system.jdbc ---------------------------------------------------------------------------
    def _plan_jdbc(self, table: str, select_list: str, where: Optional[str], order_by: Optional[str],
                   limit: Optional[int]) -> Plan:
        if table not in JDBC_TABLE_COLUMNS:
            raise MockError("TABLE_NOT_FOUND", "Table 'system.jdbc.%s' does not exist" % table, 46)
        all_columns = JDBC_TABLE_COLUMNS[table]
        rows = jdbc_rows(table)
        if where:
            rows = [r for r in rows if self._eval_where(where, r)]
        if order_by:
            keys = [unquote_ident(k.strip().split(" ")[0]) for k in _split_top_level(order_by)]
            for key in reversed(keys):
                rows.sort(key=lambda r, k=key: (r.get(k) is None,
                                                 (0, r.get(k), "") if isinstance(r.get(k), (int, float)) and not isinstance(r.get(k), bool)
                                                 else (1, 0, str(r.get(k)) if r.get(k) is not None else "")))
        if select_list.strip() == "*":
            selected = all_columns
        else:
            selected = []
            known = dict(all_columns)
            for item in _split_top_level(select_list):
                name = unquote_ident(item.strip())
                if name not in known:
                    raise MockError("COLUMN_NOT_FOUND", "Column '%s' cannot be resolved" % name, 47)
                selected.append((name, known[name]))
        data = [[r.get(name) for name, _ in selected] for r in rows]
        if limit is not None:
            data = data[:limit]
        return Plan.single(selected, data)

    @staticmethod
    def _eval_where(where: str, row: Dict[str, Any]) -> bool:
        for pred in re.split(r"\s+and\s+", where, flags=re.IGNORECASE):
            pred = pred.strip()
            m = re.match(r"^(\"?\w+\"?)\s+like\s+('(?:[^']|'')*')(?:\s+escape\s+'(.*?)')?$", pred, re.IGNORECASE)
            if m:
                value = row.get(unquote_ident(m.group(1)))
                pattern = sql_string_literal(m.group(2), 0)[0]
                if value is None or not re.match(like_to_regex(pattern), str(value), re.DOTALL):
                    return False
                continue
            m = re.match(r"^(\"?\w+\"?)\s*=\s*('(?:[^']|'')*'|-?\d+)$", pred, re.IGNORECASE)
            if m:
                value = row.get(unquote_ident(m.group(1)))
                lit = m.group(2)
                wanted: Any = sql_string_literal(lit, 0)[0] if lit.startswith("'") else int(lit)
                if value is None or value != wanted:
                    return False
                continue
            m = re.match(r"^(\"?\w+\"?)\s+in\s*\((.*)\)$", pred, re.IGNORECASE)
            if m:
                value = row.get(unquote_ident(m.group(1)))
                items = [sql_string_literal(x.strip(), 0)[0] for x in _split_top_level(m.group(2))]
                if value is None or value not in items:
                    return False
                continue
            m = re.match(r"^(\"?\w+\"?)\s+is\s+null$", pred, re.IGNORECASE)
            if m:
                if row.get(unquote_ident(m.group(1))) is not None:
                    return False
                continue
            raise MockError("SYNTAX_ERROR", "line 1:1: unsupported predicate in mock: %s" % pred, 1)
        return True

    # ---- prepared statements -------------------------------------------------------------------
    def _plan_describe_output(self, name: str, ctx: RequestContext) -> Plan:
        if name not in ctx.prepared:
            raise MockError("NOT_FOUND", "Prepared statement not found: %s" % name, 5)
        inner = ctx.prepared[name]
        columns = [("Column Name", "varchar"), ("Catalog", "varchar"), ("Schema", "varchar"), ("Table", "varchar"),
                   ("Type", "varchar"), ("Type Size", "integer"), ("Aliased", "boolean")]
        try:
            target = self._plan_execute(inner, None, ctx, describing=True)
        except MockError as e:
            if e.name in ("SYNTAX_ERROR", "TABLE_NOT_FOUND", "COLUMN_NOT_FOUND"):
                raise
            raise
        rows: List[List[Any]] = []
        if target.has_columns:
            for cname, ctype in target.columns:
                rows.append([cname, target.source_catalog, target.source_schema, target.source_table, ctype,
                             type_fixed_size(ctype), False])
        return Plan.single(columns, rows)

    @staticmethod
    def _plan_describe_input(name: str, ctx: RequestContext) -> Plan:
        if name not in ctx.prepared:
            raise MockError("NOT_FOUND", "Prepared statement not found: %s" % name, 5)
        inner = ctx.prepared[name]
        rows: List[List[Any]] = []
        position = 1  # Trino numbers DESCRIBE INPUT positions from 1
        quote: Optional[str] = None
        for i, c in enumerate(inner):
            if quote:
                if c == quote:
                    quote = None
                continue
            if c in ("'", '"'):
                quote = c
                continue
            if c == "?":
                before = inner[:i]
                m = re.search(r"(\"?\w+\"?)\s*(?:=|<>|<|>|<=|>=)\s*$", before)
                col = unquote_ident(m.group(1)) if m else ""
                if col == "id" or col.endswith("key"):
                    typ = "bigint"
                elif col == "name":
                    typ = "varchar"
                else:
                    typ = "unknown"
                rows.append([position, typ])
                position += 1
        return Plan.single([("Position", "integer"), ("Type", "varchar")], rows)

    def _plan_execute_immediate(self, sql: str, ctx: RequestContext) -> Plan:
        try:
            inner, end = sql_string_literal(sql, len("execute immediate "))
        except ValueError:
            raise syntax_error(sql)
        rest = sql[end:].strip()
        using: Optional[str] = None
        if rest:
            m = re.match(r"^using\s+(.+)$", rest, re.IGNORECASE | re.DOTALL)
            if not m:
                raise syntax_error(sql)
            using = m.group(1).strip()
        return self._plan_execute(inner, using, ctx)

    def _plan_execute(self, inner: str, using: Optional[str], ctx: RequestContext, describing: bool = False) -> Plan:
        placeholders = count_placeholders(inner)
        literals = _split_top_level(using) if using else []
        if using is not None and placeholders != len(literals):
            raise MockError("SYNTAX_ERROR", "line 1:1: Incorrect number of parameters: expected %d but found %d"
                            % (placeholders, len(literals)), 1)
        norm = normalize(inner)
        low = norm.lower()
        # `SELECT * FROM big_table WHERE id = ?` answers with the addressed row.
        m = re.match(r"^select (.+) from ((?:[\w\"]+\.)*[\w\"]*big_table)(?: (\w+))? where (\"?id\"?) = \?$", norm, re.IGNORECASE)
        if m and placeholders == 1:
            if describing:
                return self._plan_select(norm.replace("?", "0"), ctx, self._comment_options(inner))
            lit = literals[0].strip()
            im = re.match(r"^(?:bigint\s*)?'?(-?\d+)'?$", lit, re.IGNORECASE)
            if not im:
                raise MockError("TYPE_MISMATCH", "Cannot use %s for id" % lit, 11)
            return self._plan_select(norm.replace("?", im.group(1)), ctx, self._comment_options(inner))
        if placeholders == 0:
            return self.plan(inner, ctx)
        if re.match(r"^(insert|update|delete|merge)\b", low):
            return Plan.update(low.split()[0].upper(), 1)
        if not (low.startswith("select") or low.startswith("with")):
            raise syntax_error(norm)
        echo = Plan.single([("literals", "varchar"), ("count", "bigint")],
                           [] if describing else [[using or "", len(literals)]])
        return echo


# ---------------------------------------------------------------------------------------------
# Query runtime
# ---------------------------------------------------------------------------------------------


class Query:
    def __init__(self, qid: str, plan: Plan, spooled: bool, page_rows: int) -> None:
        self.id = qid
        self.token = "t%s" % qid.rsplit("_", 1)[0][-6:]
        self.plan = plan
        self.spooled = spooled
        self.page_rows = page_rows
        self.response_no = 0        # 1 = POST answer
        self.next_row = 0
        self.queued_left = plan.queued_responses
        self.flaky_left = plan.flaky_503
        self.cancelled = False
        self.finished = False
        self.data_pages_sent = 0
        self.segments: Dict[int, bytes] = {}
        self.segment_seq = 0
        self.started = time.time()
        self.lock = threading.Lock()


class MockState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.queries: Dict[str, Query] = {}
        self.seq = 0
        self.stats = {"queries": 0, "cancelled": 0, "acks": 0, "requests": 0, "segments_served": 0,
                      "errors": 0, "unauthorized": 0, "transactions_started": 0, "commits": 0,
                      "rollbacks": 0, "transactional_statements": 0}
        # Open transactions (START TRANSACTION ids not yet committed / rolled back). Not cleared by
        # /mock/reset: a connection of a running test may still use its id.
        self.transactions: Dict[str, bool] = {}
        self.txn_seq = 0

    def begin_transaction(self) -> str:
        with self.lock:
            self.txn_seq += 1
            tid = "mock-txn-%d-%d" % (int(time.time() * 1000), self.txn_seq)
            self.transactions[tid] = True
            self.stats["transactions_started"] += 1
            return tid

    def in_transaction(self, tid: str) -> bool:
        with self.lock:
            return tid in self.transactions

    def end_transaction(self, tid: str, commit: bool) -> bool:
        with self.lock:
            if self.transactions.pop(tid, None) is None:
                return False
            self.stats["commits" if commit else "rollbacks"] += 1
            return True

    def new_query(self, plan: Plan, spooled: bool, page_rows: int) -> Query:
        with self.lock:
            self.seq += 1
            qid = "%s_%05d_mock1" % (datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d_%H%M%S"), self.seq)
            q = Query(qid, plan, spooled, page_rows)
            self.queries[qid] = q
            self.stats["queries"] += 1
            return q

    def get(self, qid: str) -> Optional[Query]:
        with self.lock:
            return self.queries.get(qid)

    def bump(self, key: str, n: int = 1) -> None:
        with self.lock:
            self.stats[key] += n

    def reset(self) -> None:
        with self.lock:
            for k in self.stats:
                self.stats[k] = 0
            self.queries.clear()

    def snapshot(self) -> Dict[str, Any]:
        with self.lock:
            snap: Dict[str, Any] = dict(self.stats)
            snap["active"] = sum(1 for q in self.queries.values() if not q.finished)
            return snap


class Config:
    def __init__(self, args: argparse.Namespace) -> None:
        self.page_rows = max(1, args.page_rows)
        self.delay = max(0, args.delay_ms) / 1000.0
        self.spooling = args.spooling
        self.verbose = args.verbose
        self.auth_kind: Optional[str] = None
        self.auth_user = ""
        self.auth_password = ""
        self.auth_token = ""
        if args.auth:
            parts = args.auth.split(":", 2)
            if parts[0] == "basic" and len(parts) == 3:
                self.auth_kind, self.auth_user, self.auth_password = "basic", parts[1], parts[2]
            elif parts[0] == "bearer" and len(parts) >= 2:
                self.auth_kind, self.auth_token = "bearer", args.auth.split(":", 1)[1]
            else:
                raise SystemExit("--auth must be basic:user:pass or bearer:TOKEN")


STATE = MockState()
CONFIG: Optional[Config] = None
PLANNER: Optional[Planner] = None


def json_bytes(obj: Any) -> bytes:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockTrino/483"

    # ---- plumbing ------------------------------------------------------------------------------
    def log_message(self, fmt: str, *args: Any) -> None:  # noqa: N802 (BaseHTTPRequestHandler API)
        if CONFIG and CONFIG.verbose:
            sys.stderr.write("%s - - %s\n" % (self.address_string(), fmt % args))
            sys.stderr.flush()

    def _base_url(self) -> str:
        scheme = "https" if isinstance(self.connection, ssl.SSLSocket) else "http"
        host = self.headers.get("Host") or ("127.0.0.1:%d" % self.server.server_address[1])
        return "%s://%s" % (scheme, host)

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length > 0 else b""

    def _send(self, status: int, body: bytes, content_type: str = "application/json",
              extra: Optional[Dict[str, str]] = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, status: int, text: str, extra: Optional[Dict[str, str]] = None) -> None:
        self._send(status, text.encode("utf-8"), "text/plain; charset=utf-8", extra)

    def _authenticate(self) -> Tuple[bool, Optional[str]]:
        """Returns (ok, user resolved from credentials)."""
        assert CONFIG is not None
        if not CONFIG.auth_kind:
            return True, None
        header = self.headers.get("Authorization") or ""
        if CONFIG.auth_kind == "basic":
            if header.startswith("Basic "):
                try:
                    decoded = base64.b64decode(header[6:].strip()).decode("utf-8")
                except Exception:
                    decoded = ""
                user, _, password = decoded.partition(":")
                if user == CONFIG.auth_user and password == CONFIG.auth_password:
                    return True, user
            STATE.bump("unauthorized")
            self._send_text(401, "Unauthorized", {"WWW-Authenticate": 'Basic realm="Trino"'})
            return False, None
        if header.startswith("Bearer ") and header[7:].strip() == CONFIG.auth_token:
            return True, None
        STATE.bump("unauthorized")
        self._send_text(401, "Unauthorized", {"WWW-Authenticate": 'Bearer realm="Trino"'})
        return False, None

    # ---- verbs -----------------------------------------------------------------------------------
    def do_GET(self) -> None:  # noqa: N802
        if not self.path.startswith("/mock/"):
            STATE.bump("requests")  # control endpoints are not part of the protocol traffic
        path = urllib.parse.urlsplit(self.path).path
        if path == "/mock/stats":
            self._send(200, json_bytes(STATE.snapshot()))
            return
        if path.startswith("/v1/spooled/segments/"):
            self._serve_segment(path)
            return
        if path.startswith("/v1/statement/"):
            ok, _ = self._authenticate()
            if not ok:
                return
            self._statement_next(path)
            return
        if path == "/v1/info":
            self._send(200, json_bytes({"nodeVersion": {"version": MOCK_VERSION}, "environment": "mock",
                                        "coordinator": True, "starting": False, "uptime": "1.00m"}))
            return
        self._send_text(404, "Not Found")

    def do_POST(self) -> None:  # noqa: N802
        STATE.bump("requests")
        path = urllib.parse.urlsplit(self.path).path
        body = self._read_body()
        if path == "/mock/reset":
            STATE.reset()
            self._send(200, json_bytes({"ok": True}))
            return
        if path != "/v1/statement":
            self._send_text(404, "Not Found")
            return
        ok, auth_user = self._authenticate()
        if not ok:
            return
        assert CONFIG is not None and PLANNER is not None
        ctx = RequestContext(self.headers, auth_user)
        if not ctx.user:
            self._send_text(400, "User must be set")
            return
        sql = body.decode("utf-8", errors="replace")
        try:
            plan = PLANNER.plan(sql, ctx)
        except MockError as e:
            plan = Plan()
            plan.error = e
        if plan.http_status:
            STATE.bump("errors")
            self._send_text(plan.http_status[0], plan.http_status[1])
            return
        spooled = CONFIG.spooling and plan.spoolable and "json" in ctx.encoding.lower().split(",") \
            and plan.has_columns and plan.total_rows() > 0
        q = STATE.new_query(plan, spooled, CONFIG.page_rows)
        self._respond(q, plan.response_headers)

    def do_DELETE(self) -> None:  # noqa: N802
        STATE.bump("requests")
        path = urllib.parse.urlsplit(self.path).path
        if path.startswith("/v1/spooled/ack/"):
            parts = path.split("/")
            q = STATE.get(parts[4]) if len(parts) >= 6 else None
            if q is None:
                self._send_text(404, "Unknown segment")
                return
            STATE.bump("acks")
            with q.lock:
                q.segments.pop(int(parts[5]), None)
            self._send(200, b"")
            return
        if path.startswith("/v1/statement/"):
            ok, _ = self._authenticate()
            if not ok:
                return
            parts = path.split("/")
            q = STATE.get(parts[4]) if len(parts) >= 5 else None
            if q is None:
                self._send_text(404, "Unknown query")
                return
            with q.lock:
                if not q.finished and not q.cancelled:
                    q.cancelled = True
                    STATE.bump("cancelled")
            self._send(204, b"")
            return
        self._send_text(404, "Not Found")

    # ---- statement protocol --------------------------------------------------------------------
    def _statement_next(self, path: str) -> None:
        # /v1/statement/{queued|executing}/<id>/<token>/<n>
        parts = path.split("/")
        if len(parts) != 7 or parts[3] not in ("queued", "executing"):
            self._send_text(404, "Not Found")
            return
        q = STATE.get(parts[4])
        if q is None:
            self._send_text(404, "Query not found")
            return
        if q.token != parts[5]:
            self._send_text(410, "Gone")
            return
        self._respond(q, None)

    def _respond(self, q: Query, first_headers: Optional[Dict[str, str]]) -> None:
        assert CONFIG is not None
        if CONFIG.delay:
            time.sleep(CONFIG.delay)
        with q.lock:
            q.response_no += 1
            plan = q.plan
            base = self._base_url()
            extra: Dict[str, str] = dict(first_headers or {})
            if q.response_no > 1 and q.flaky_left > 0:
                q.flaky_left -= 1
                self._send_text(503, "Service Unavailable", {"Retry-After": "0"})
                return

            resp: Dict[str, Any] = {"id": q.id, "infoUri": "%s/ui/query.html?%s" % (base, q.id)}
            if q.response_no == 1:
                resp["nextUri"] = "%s/v1/statement/queued/%s/%s/1" % (base, q.id, q.token)
                resp["stats"] = self._stats(q, "QUEUED", queued=True)
                self._send(200, json_bytes(resp), extra=extra)
                return

            if q.cancelled:
                q.finished = True
                resp["error"] = MockError("USER_CANCELED", "Query was canceled by user", 3).to_json()
                resp["stats"] = self._stats(q, "FAILED")
                self._send(200, json_bytes(resp))
                return

            if q.queued_left > 0:
                q.queued_left -= 1
                time.sleep(SLOW_TABLE_RESPONSE_DELAY_SEC)
                if q.cancelled:
                    q.finished = True
                    resp["error"] = MockError("USER_CANCELED", "Query was canceled by user", 3).to_json()
                    resp["stats"] = self._stats(q, "FAILED")
                    self._send(200, json_bytes(resp))
                    return
                resp["nextUri"] = "%s/v1/statement/queued/%s/%s/%d" % (base, q.id, q.token, q.response_no)
                resp["stats"] = self._stats(q, "QUEUED", queued=True)
                self._send(200, json_bytes(resp))
                return

            if plan.error is not None and not (plan.error_after_first_page and q.data_pages_sent == 0):
                q.finished = True
                STATE.bump("errors")
                resp["error"] = plan.error.to_json()
                resp["stats"] = self._stats(q, "FAILED")
                self._send(200, json_bytes(resp))
                return

            if plan.warnings:
                resp["warnings"] = plan.warnings

            if not plan.has_columns:
                q.finished = True
                if plan.update_type:
                    resp["updateType"] = plan.update_type
                    if plan.update_count is not None:
                        resp["updateCount"] = plan.update_count
                resp["stats"] = self._stats(q, "FINISHED")
                self._send(200, json_bytes(resp))
                return

            resp["columns"] = [column_json(n, t) for n, t in plan.columns]
            total = plan.total_rows()
            more: bool
            if q.spooled:
                more = self._add_spooled_data(q, resp, base)
            else:
                end = min(q.next_row + q.page_rows, total)
                if end > q.next_row:
                    resp["data"] = plan.slice(q.next_row, end)
                    q.next_row = end
                    q.data_pages_sent += 1
                more = q.next_row < total
            if plan.error is not None and plan.error_after_first_page:
                more = True
            if more:
                resp["nextUri"] = "%s/v1/statement/executing/%s/%s/%d" % (base, q.id, q.token, q.response_no)
                resp["stats"] = self._stats(q, "RUNNING")
            else:
                q.finished = True
                resp["stats"] = self._stats(q, "FINISHED")
            self._send(200, json_bytes(resp), extra=extra)

    def _add_spooled_data(self, q: Query, resp: Dict[str, Any], base: str) -> bool:
        """Appends up to SEGMENTS_PER_RESPONSE segments (first one of the query inline, the others
        spooled) and returns whether rows remain."""
        plan = q.plan
        total = plan.total_rows()
        segments: List[Dict[str, Any]] = []
        for _ in range(SEGMENTS_PER_RESPONSE):
            if q.next_row >= total:
                break
            end = min(q.next_row + q.page_rows, total)
            payload = json_bytes(plan.slice(q.next_row, end))
            meta = {"segmentSize": len(payload), "rowOffset": q.next_row, "rowsCount": end - q.next_row,
                    "uncompressedSize": len(payload)}
            if q.segment_seq == 0:
                segments.append({"type": "inline", "data": base64.b64encode(payload).decode("ascii"), "metadata": meta})
            else:
                n = q.segment_seq
                q.segments[n] = payload
                segments.append({"type": "spooled",
                                 "uri": "%s/v1/spooled/segments/%s/%d" % (base, q.id, n),
                                 "ackUri": "%s/v1/spooled/ack/%s/%d" % (base, q.id, n),
                                 "headers": {SPOOL_SECRET_HEADER: [SPOOL_SECRET]},
                                 "metadata": meta})
            q.segment_seq += 1
            q.next_row = end
            q.data_pages_sent += 1
        resp["data"] = {"encoding": "json", "metadata": {}, "segments": segments}
        return q.next_row < total

    def _serve_segment(self, path: str) -> None:
        parts = path.split("/")
        q = STATE.get(parts[4]) if len(parts) >= 6 else None
        if q is None or not parts[5].isdigit():
            self._send_text(404, "Unknown segment")
            return
        if self.headers.get(SPOOL_SECRET_HEADER) != SPOOL_SECRET:
            self._send_text(403, "Forbidden: missing segment credentials")
            return
        with q.lock:
            payload = q.segments.get(int(parts[5]))
        if payload is None:
            self._send_text(404, "Segment not found (already acknowledged?)")
            return
        STATE.bump("segments_served")
        self._send(200, payload, "application/octet-stream")

    @staticmethod
    def _stats(q: Query, state: str, queued: bool = False) -> Dict[str, Any]:
        total = q.plan.total_rows() if q.plan.has_columns else 0
        done = q.next_row
        elapsed = int((time.time() - q.started) * 1000)
        return {
            "state": state, "queued": queued, "scheduled": not queued, "nodes": 1, "totalSplits": 1,
            "queuedSplits": 1 if queued else 0, "runningSplits": 0 if state != "RUNNING" else 1,
            "completedSplits": 1 if state == "FINISHED" else 0, "cpuTimeMillis": 0, "wallTimeMillis": elapsed,
            "queuedTimeMillis": 0, "elapsedTimeMillis": elapsed, "processedRows": done,
            "processedBytes": done * 16, "physicalInputBytes": 0, "peakMemoryBytes": 0, "spilledBytes": 0,
            "progressPercentage": (100.0 * done / total) if total else (100.0 if state == "FINISHED" else 0.0),
        }


def main(argv: Optional[List[str]] = None) -> int:
    global CONFIG, PLANNER
    parser = argparse.ArgumentParser(description="Mock Trino coordinator")
    parser.add_argument("--port", type=int, default=18090)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--auth", default=None, help="basic:user:pass or bearer:TOKEN")
    parser.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"), default=None)
    parser.add_argument("--page-rows", type=int, default=1000)
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--spooling", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    CONFIG = Config(args)
    PLANNER = Planner(CONFIG.page_rows)

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    server.daemon_threads = True
    if args.tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(certfile=args.tls[0], keyfile=args.tls[1])
        server.socket = context.wrap_socket(server.socket, server_side=True)
    sys.stdout.write("mock trino listening on %s:%d (%s%s%s)\n" % (
        args.bind, server.server_address[1], "https" if args.tls else "http",
        ", auth=%s" % CONFIG.auth_kind if CONFIG.auth_kind else "", ", spooling" if CONFIG.spooling else ""))
    sys.stdout.flush()
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
