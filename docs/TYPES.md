# Data types

How Trino types are reported to ODBC applications (`SQLDescribeCol`, `SQLColAttribute`,
`SQLColumns`, `SQLGetTypeInfo`) and how values are converted to C buffers (`SQLBindCol`,
`SQLGetData`) and rendered as parameters (`SQLBindParameter`). `SQLColumns` and `SQLDescribeCol`
always agree because both go through the same mapping.

## Options that influence the mapping

| Option | Symbol below | Effect |
|---|---|---|
| `UseUnicodeSqlCharacterTypes` (default 1) | **U** | character types are `SQL_WCHAR`/`SQL_WVARCHAR`/`SQL_WLONGVARCHAR` (U=1) or `SQL_CHAR`/`SQL_VARCHAR`/`SQL_LONGVARCHAR` (U=0) |
| `DefaultStringColumnLength` (default 65535) | **L** | column size of unbounded `varchar`, `json`, `varbinary`, array/map/row and unknown types |
| `UnboundedVarcharAsLongVarchar` (default 0) | | unbounded `varchar` / `varbinary` become the LONG variants |
| `TimestampWithTimeZoneMode` (default `session`) | | see "Time zones" |
| `TimeWithTimeZoneMode` (default `string`) | | see "Time zones" |
| `IntervalMode` (default `string`) | | intervals as text or `SQL_INTERVAL_*` |
| `UuidAsGuid` (default 0) | | `uuid` as `SQL_GUID` |
| `DecimalAsDouble` (default 0) | | `decimal` as `SQL_DOUBLE` |

## Column metadata

`(W)VARCHAR` means `SQL_WVARCHAR` when U=1, `SQL_VARCHAR` when U=0 (likewise for CHAR and
LONGVARCHAR). The octet length (`SQL_DESC_OCTET_LENGTH`, `BUFFER_LENGTH` / `CHAR_OCTET_LENGTH` of
`SQLColumns`) of a character column is the column size times the character width:
`sizeof(SQLWCHAR)` × size for the wide types (2× on Windows and unixODBC, 4× on iODBC, where
`SQLWCHAR` is 4 bytes) and 1× size for the narrow types. The display size is the column size.

| Trino type | SQL data type (`DATA_TYPE`) | Column size | Decimal digits | Radix | Default C type | Notes |
|---|---|---|---|---|---|---|
| `boolean` | `SQL_BIT` | 1 | 0 | 0 | `SQL_C_BIT` | renders as `1` / `0` in character buffers |
| `tinyint` | `SQL_TINYINT` | 3 | 0 | 10 | `SQL_C_STINYINT` | signed |
| `smallint` | `SQL_SMALLINT` | 5 | 0 | 10 | `SQL_C_SSHORT` | |
| `integer` | `SQL_INTEGER` | 10 | 0 | 10 | `SQL_C_SLONG` | |
| `bigint` | `SQL_BIGINT` | 19 | 0 | 10 | `SQL_C_SBIGINT` | |
| `real` | `SQL_REAL` | 7 | 0 | 2 | `SQL_C_FLOAT` | `NaN`, `Infinity`, `-Infinity` supported |
| `double` | `SQL_DOUBLE` | 15 | 0 | 2 | `SQL_C_DOUBLE` | |
| `decimal(p,s)` | `SQL_DECIMAL` (`SQL_DOUBLE` with `DecimalAsDouble=1`: size 15, radix 2) | p | s | 10 | `SQL_C_CHAR` (`SQL_C_DOUBLE` with `DecimalAsDouble=1`) | exact; octet/display length p+2; `CREATE_PARAMS` = `precision,scale`, scale 0..38 |
| `varchar(n)` | `(W)VARCHAR` | n | 0 | 0 | `SQL_C_(W)CHAR` | case sensitive, searchable |
| `varchar` (unbounded) | `(W)VARCHAR` — `(W)LONGVARCHAR` with `UnboundedVarcharAsLongVarchar=1` | L | | | | type name `varchar` |
| `char(n)` | `(W)CHAR` | n (1 for bare `char`) | | | | |
| `varbinary` | `SQL_VARBINARY` — `SQL_LONGVARBINARY` with `UnboundedVarcharAsLongVarchar=1` | L | | | `SQL_C_BINARY` | display size 2L; literal `X'...'` |
| `json` | `(W)LONGVARCHAR` | L | | | | JSON text; literal prefix `JSON '` |
| `date` | `SQL_TYPE_DATE` (`SQL_DATETIME` / `SQL_CODE_DATE`) | 10 | 0 | | `SQL_C_TYPE_DATE` | literal `DATE '...'` |
| `time(p)` | `SQL_TYPE_TIME` (`SQL_DATETIME` / `SQL_CODE_TIME`) | 8, or 9+p when p>0 | min(p,9) | | `SQL_C_TYPE_TIME` | Trino default p=3; `SQL_TIME_STRUCT` has no fraction (`01S07` when dropped) |
| `time(p) with time zone` | `string` mode: `(W)VARCHAR`, size 8 + (p+1 when p>0) + 6; `session`/`utc`/`wallclock`: as `time(p)` | | | | | |
| `timestamp(p)` | `SQL_TYPE_TIMESTAMP` (`SQL_DATETIME` / `SQL_CODE_TIMESTAMP`) | 19, or 20+p when p>0 | min(p,9) | | `SQL_C_TYPE_TIMESTAMP` | literal `TIMESTAMP '...'` |
| `timestamp(p) with time zone` | `string` mode: `(W)VARCHAR`, size 19 + (p+1 when p>0) + 41; `session`/`utc`/`wallclock`: as `timestamp(p)` | | | | | |
| `interval year to month` | `string` mode: `(W)VARCHAR(20)`; `interval` mode: `SQL_INTERVAL_YEAR_TO_MONTH` (`SQL_INTERVAL` / `SQL_CODE_YEAR_TO_MONTH`), size 20 | | | | `SQL_C_INTERVAL_YEAR_TO_MONTH` | text form `1-2` |
| `interval day to second` | `string` mode: `(W)VARCHAR(30)`; `interval` mode: `SQL_INTERVAL_DAY_TO_SECOND` (`SQL_CODE_DAY_TO_SECOND`), size 30, decimal digits 3 | | | | `SQL_C_INTERVAL_DAY_TO_SECOND` | text form `3 04:05:06.789` |
| `array(...)`, `map(...)`, `row(...)` | `(W)LONGVARCHAR` | L | | | | the raw JSON text Trino sends (`[1,2]`, `{"a":1}`); the type name is the full Trino signature |
| `uuid` | `UuidAsGuid=0`: `(W)VARCHAR(36)`; `UuidAsGuid=1`: `SQL_GUID` (size 36, octet length 16) | | | | `SQL_C_GUID` when GUID | |
| `ipaddress` | `(W)VARCHAR(45)` | 45 | | | | literal prefix `IPADDRESS '` |
| any other type | `(W)VARCHAR(L)` | L | | | | type name = Trino name, value = textual form |

Common attributes: `NULLABLE = SQL_NULLABLE` (Trino does not expose nullability);
`UNSIGNED = false` for numerics, `true` otherwise; `CASE_SENSITIVE = true` and
`SEARCHABLE = SQL_SEARCHABLE` for char, varchar, json, array/map/row and unknown types;
`CASE_SENSITIVE = false` and `SEARCHABLE = SQL_PRED_BASIC` for everything else, including the types
that are only exposed as text (uuid, ipaddress, intervals and zoned time/timestamp in `string`
mode); `FIXED_PREC_SCALE = false`; `AUTO_INCREMENT = false`; `TYPE_NAME` / `LOCAL_TYPE_NAME` = the
Trino type as written (`varchar(10)`, `decimal(18,2)`).

## SQLGetTypeInfo

The table is built from the same mapping (so it follows the options above), sorted by
`DATA_TYPE` (numeric value, ascending) with the closest match first within one `DATA_TYPE`, as the
specification requires. `TYPE_NAME` uses the base name (`varchar`, `decimal`, `timestamp`). With
defaults:

`SQL_WLONGVARCHAR` (json), `SQL_WVARCHAR` (varchar), `SQL_WCHAR` (char), `SQL_BIT` (boolean),
`SQL_TINYINT`, `SQL_BIGINT`, `SQL_VARBINARY` (varbinary), `SQL_DECIMAL` (decimal), `SQL_INTEGER`,
`SQL_SMALLINT`, `SQL_REAL`, `SQL_DOUBLE`, `SQL_TYPE_DATE`, `SQL_TYPE_TIME`, `SQL_TYPE_TIMESTAMP`.

Variations:

* `UuidAsGuid=1`: a `SQL_GUID` (uuid) row comes first.
* `IntervalMode=interval`: the `SQL_INTERVAL_YEAR_TO_MONTH` and `SQL_INTERVAL_DAY_TO_SECOND` rows
  are appended.
* `UnboundedVarcharAsLongVarchar=1`: the `(W)LONGVARCHAR` row is the unbounded `varchar` (no
  `CREATE_PARAMS`) instead of `json`, and the varbinary row becomes `SQL_LONGVARBINARY`.
* `DecimalAsDouble=1`: the decimal row is reported as `SQL_DOUBLE`, listed after the `double` row.
* `UseUnicodeSqlCharacterTypes=0`: the narrow types `SQL_LONGVARCHAR`, `SQL_CHAR`, `SQL_VARCHAR`
  appear instead of the W types (and sort accordingly).

A `DataType` argument selects the matching rows: the ODBC 2 ids `SQL_DATE`, `SQL_TIME`,
`SQL_TIMESTAMP` return the `SQL_TYPE_*` rows, `SQL_NUMERIC` returns the decimal row and
`SQL_FLOAT` the double row.

`LITERAL_PREFIX`/`SUFFIX`: `'` / `'` for char and varchar, `JSON '` / `'` for json, `X'` / `'`
for varbinary, `DATE '`, `TIME '`, `TIMESTAMP '` for temporal types, `UUID '` for uuid,
`INTERVAL '` / `' YEAR TO MONTH` and `INTERVAL '` / `' DAY TO SECOND` for the interval rows, empty
for numerics and boolean. The time and timestamp rows describe `time(9)` / `timestamp(9)` (column
size 18 / 29) because the ODBC structures carry at most nanoseconds. `MINIMUM_SCALE` /
`MAXIMUM_SCALE` are 0/38 for decimal, 0/9 for time and timestamp. `INTERVAL_PRECISION` is 2 and 9
for the two interval rows.

## Values and conversions

Internally every value is kept in one of five storages: **Bool**, **Int64** (all integer types),
**Double** (real/double), **Binary** (varbinary), **String** (everything else, holding exactly the
text Trino sent). Conversions to C types are implemented for all of them:

| Target C type | From |
|---|---|
| `SQL_C_CHAR`, `SQL_C_WCHAR` | every type (text form; `01004` when the buffer is too small, retrievable in chunks with `SQLGetData`; see "Chunks and NULLs" below) |
| `SQL_C_BIT`, `SQL_C_(S/U)TINYINT`, `SQL_C_(S/U)SHORT`, `SQL_C_(S/U)LONG`, `SQL_C_(S/U)BIGINT` | Bool, Int64, Double, decimal and numeric strings. `22003` when out of range; a non-zero fraction gives `01S07` (rounded toward zero); non-numeric text gives `22018` |
| `SQL_C_FLOAT`, `SQL_C_DOUBLE` | Bool, Int64, Double, decimal/numeric strings (`22018` otherwise) |
| `SQL_C_NUMERIC` | decimal, integers, doubles, numeric strings; exact 128-bit arithmetic, precision/scale from the ARD (`SQL_DESC_PRECISION`/`SQL_DESC_SCALE`) or the column; `22003` on overflow, `01S07` on lost digits |
| `SQL_C_BINARY` | Binary as-is; other types as their text bytes |
| `SQL_C_TYPE_DATE` / `SQL_C_DATE` | date, timestamp (time part dropped, `01S07` when non-zero), text |
| `SQL_C_TYPE_TIME` / `SQL_C_TIME` | time, timestamp, text (fraction dropped with `01S07`) |
| `SQL_C_TYPE_TIMESTAMP` / `SQL_C_TIMESTAMP` | timestamp, date, time, text; fraction in nanoseconds, `22008` when the value does not fit |
| `SQL_C_INTERVAL_*` | interval columns and interval text; `22015` on overflow |
| `SQL_C_GUID` | uuid text (`22018` when malformed) |
| `SQL_C_DEFAULT` | the "Default C type" column of the table above |

Other SQLSTATEs: `07006` (conversion not allowed, e.g. binary to date), `22018` (text that is not
a valid date/time/number for the requested target). Row-level failures during `SQLFetch` are reported as `01S01` per row with the row status
array set to `SQL_ROW_ERROR`.

### Chunks and NULLs with `SQLGetData`

* A column is consumed once. After the last chunk (or a value that fit completely) another
  `SQLGetData` on the same column returns `SQL_NO_DATA`.
* A NULL cell returns `SQL_SUCCESS` with `SQL_NULL_DATA` in the indicator once; the next call on
  that column returns `SQL_NO_DATA`, like any consumed column. Without an indicator pointer a NULL
  is `22002`.
* `SQL_C_CHAR` chunks are cut at byte boundaries: a buffer smaller than one UTF-8 code point (or a
  cut in the middle of one) splits the sequence across two calls. Concatenate the chunks before
  decoding.
* `SQL_C_WCHAR` chunks never split a character. A buffer that cannot hold one character of the
  value (for example room for a single UTF-16 unit when the next character needs a surrogate pair)
  returns `HY090`.
* `SQL_C_BINARY` chunks are raw bytes.

Text rendering (`SQL_C_CHAR`/`SQL_C_WCHAR`): booleans `1`/`0`; doubles in the shortest form that
round-trips (`1.5`, `1e+300`, `NaN`, `Infinity`); decimals as stored (`12.340` keeps its scale);
dates `YYYY-MM-DD`; times `HH:MM:SS[.fff]`; timestamps `YYYY-MM-DD HH:MM:SS[.fff]`; binary as
hexadecimal.

`SQL_C_CHAR` buffers use `AnsiEncoding` (UTF-8 by default on Unix, the system code page on
Windows); `SQL_C_WCHAR` is UTF-16 (Windows, unixODBC) or UTF-32 (iODBC) as dictated by the driver
manager. Lengths returned for `SQL_C_WCHAR` are in bytes, i.e. multiples of `sizeof(SQLWCHAR)`.

## Time zones

`timestamp with time zone` values arrive as text with either a fixed offset
(`2024-03-10 02:30:00.123 +02:00`) or an IANA zone (`... America/New_York`).

Examples for `2024-03-10 02:30:00.123 +02:00` (00:30 UTC) with the default session time zone
`Europe/Istanbul` (UTC+03:00):

| Mode | Column type | Value |
|---|---|---|
| `session` (default; aliases `local`, `sessiontz`) | `SQL_TYPE_TIMESTAMP` | converted to the session time zone (`TimeZone`, default `Europe/Istanbul`): `2024-03-10 03:30:00.123`; text form has no zone suffix |
| `utc` | `SQL_TYPE_TIMESTAMP` | converted to UTC: `2024-03-10 00:30:00.123`; text form has no zone suffix |
| `wallclock` | `SQL_TYPE_TIMESTAMP` | the wall-clock digits as sent, zone dropped: `2024-03-10 02:30:00.123` |
| `string` | `(W)VARCHAR` | the original text including the zone |

A value with an IANA zone is converted the same way: `2024-03-10 12:30:00.123 America/New_York`
(EDT, 16:30 UTC) is delivered as `2024-03-10 19:30:00.123` in `session` mode. Wall-clock values that
fall into a DST gap of their zone (`2024-03-10 02:30 America/New_York`) use the offset in effect
before the gap, as Java does. When the session zone observes DST (`TimeZone=Europe/Berlin`) the
offset of the instant itself is used (UTC+2 in summer, UTC+1 in winter).

`Europe/Istanbul` as the default is a deliberate project choice (see
[CONNECTION-OPTIONS.md](CONNECTION-OPTIONS.md#default-time-zone-europeistanbul)); set
`TimeZone=system` for the operating-system zone, another IANA name, or
`TimestampWithTimeZoneMode=utc` for UTC values.

Zone names are resolved from the operating system's tz database (`/usr/share/zoneinfo`, `TZDIR`;
on Windows ICU when available, otherwise a built-in Windows-name to IANA table). `UTC`, `Z`, `GMT`,
`Etc/UTC`, fixed offsets and `Europe/Istanbul` (compiled-in +03:00 fallback) always resolve. When
the zone of a value cannot be resolved the value is returned as wall clock; when the session zone
cannot be resolved (`session` mode) the value is returned in UTC. Either way one warning record
(`01000`) is added per statement.

`time with time zone` defaults to `string` mode because a time of day with an offset has no
unambiguous instant; `utc` and `wallclock` behave as for timestamps, and `session` shifts the time
by the session zone's offset in effect at the current instant (always +03:00 for Istanbul).

The session time zone sent to Trino (`X-Trino-Time-Zone`, option `TimeZone`) also determines how
the server evaluates `current_timestamp`, `localtimestamp` and casts between timestamp types, and
is the zone of "today" when the driver converts a `time` to a timestamp.

## Parameters

`SQLBindParameter` binds input parameters (`SQL_PARAM_INPUT` only) that are rendered as Trino
literals typed by the bound **SQL type** (`ParameterType`), using the C buffer as the source:

| SQL type bound | Literal sent |
|---|---|
| character types | `'abc'` (quotes doubled) |
| `SQL_BIT` | `true` / `false` |
| integers | `42` |
| `SQL_DOUBLE`, `SQL_FLOAT`, `SQL_REAL` | `DOUBLE '1.5'` / `REAL '1.5'` |
| `SQL_DECIMAL`, `SQL_NUMERIC` | `DECIMAL '12.34'` |
| binary types | `X'0102'` |
| `SQL_TYPE_DATE` | `DATE '2024-01-02'` |
| `SQL_TYPE_TIME` | `TIME '03:04:05.678'` |
| `SQL_TYPE_TIMESTAMP` | `TIMESTAMP '2024-01-02 03:04:05.678'` |
| `SQL_INTERVAL_*` | `INTERVAL '1-2' YEAR TO MONTH`, `INTERVAL '3 04:05:06.789' DAY TO SECOND` |
| `SQL_GUID` | `UUID '...'` |
| `SQL_NULL_DATA` indicator | `CAST(NULL AS <trino type>)` |

The Trino type used for `CAST(NULL AS ...)` and for typing: character → `varchar` (or
`varchar(n)` when a column size was given), `SQL_BIT` → `boolean`, `SQL_TINYINT` → `tinyint`,
`SQL_SMALLINT` → `smallint`, `SQL_INTEGER` → `integer`, `SQL_BIGINT` → `bigint`, `SQL_REAL` →
`real`, `SQL_FLOAT`/`SQL_DOUBLE` → `double`, `SQL_DECIMAL`/`SQL_NUMERIC` → `decimal(p,s)` (p =
column size capped at 38, 38 when 0; s = decimal digits capped at p), binary → `varbinary`, date →
`date`, time → `time(decimal digits)` (0..12), timestamp → `timestamp(decimal digits)` (3 when the
decimal digits are 0, at most 12), `SQL_GUID` → `uuid`, year-month intervals → `interval year to
month`, other intervals → `interval day to second`, anything else → `varchar`.

When the statement was prepared, `SQLDescribeParam` returns the types Trino reports through
`DESCRIBE INPUT`; parameters of unknown type are described as `SQL_WVARCHAR(L)`.

Data-at-execution (`SQL_DATA_AT_EXEC`, `SQL_LEN_DATA_AT_EXEC(n)`) with `SQLParamData` /
`SQLPutData` is supported for every type; character data may be sent in pieces.
