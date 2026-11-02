# Trino -> ODBC type mapping rules (implemented in src/types/odbc_types.cpp; convert, catalog and the ODBC layer rely on them)

Notation: `U` = `TypeMappingOptions::unicode` (true -> SQL_WCHAR family, else SQL_CHAR family), `L` = defaultStringLength.

| Trino | conciseType | verbose / subcode | columnSize | octetLength | displaySize | decimalDigits | radix | defaultC | storage | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| boolean | SQL_BIT | SQL_BIT / 0 | 1 | 1 | 1 | 0 | 0 | SQL_C_BIT | Bool | searchable BASIC, unsigned=true (n/a) |
| tinyint | SQL_TINYINT | | 3 | 1 | 4 | 0 | 10 | SQL_C_STINYINT | Int64 | signed |
| smallint | SQL_SMALLINT | | 5 | 2 | 6 | 0 | 10 | SQL_C_SSHORT | Int64 | |
| integer | SQL_INTEGER | | 10 | 4 | 11 | 0 | 10 | SQL_C_SLONG | Int64 | |
| bigint | SQL_BIGINT | | 19 | 8 | 20 | 0 | 10 | SQL_C_SBIGINT | Int64 | |
| real | SQL_REAL | | 7 | 4 | 14 | 0 | 2 | SQL_C_FLOAT | Double | |
| double | SQL_DOUBLE | | 15 | 8 | 24 | 0 | 2 | SQL_C_DOUBLE | Double | |
| decimal(p,s) | SQL_DECIMAL (SQL_DOUBLE if decimalAsDouble) | | p | p+2 | p+2 | s | 10 | SQL_C_CHAR | String | precision=p scale=s, createParams "precision,scale", min/max scale 0/38 |
| varchar(n) | U?SQL_WVARCHAR:SQL_VARCHAR | | n | U?2n:n (clamped to SQLLEN max) | n | 0 | 0 | U?SQL_C_WCHAR:SQL_C_CHAR | String | caseSensitive, searchable SQL_SEARCHABLE, literal `'` `'`, createParams "length" |
| varchar (unbounded) | as varchar(L); SQL_WLONGVARCHAR/SQL_LONGVARCHAR when unboundedAsLong | | L | | L | | | | String | typeName "varchar" |
| char(n) | U?SQL_WCHAR:SQL_CHAR | | n | U?2n:n | n | | | | String | |
| varbinary | unboundedAsLong?SQL_LONGVARBINARY:SQL_VARBINARY | | L | L | 2L | | | SQL_C_BINARY | Binary | literal `X'` `'` |
| json | U?SQL_WLONGVARCHAR:SQL_LONGVARCHAR | | L | U?2L:L | L | | | | String | caseSensitive |
| date | SQL_TYPE_DATE | SQL_DATETIME / SQL_CODE_DATE | 10 | 6 | 10 | 0 | 0 | SQL_C_TYPE_DATE | String | literal `DATE '` `'` |
| time(p) | SQL_TYPE_TIME | SQL_DATETIME / SQL_CODE_TIME | 8 (+p+1 if p>0) | 6 | same as columnSize | min(p,9) | | SQL_C_TYPE_TIME | String | precision p (Trino default 3), literal `TIME '` `'`, createParams "precision" |
| time(p) with time zone | timeTz==String (default): U?SQL_WVARCHAR:SQL_VARCHAR size 8+(p+1)+6 ; else (Session/Utc/WallClock) SQL_TYPE_TIME as time(p) | | | | | | | | String | |
| timestamp(p) | SQL_TYPE_TIMESTAMP | SQL_DATETIME / SQL_CODE_TIMESTAMP | 19 (+p+1 if p>0) | 16 | same | min(p,9) | | SQL_C_TYPE_TIMESTAMP | String | literal `TIMESTAMP '` `'` |
| timestamp(p) with time zone | timestampTz==String: (W)VARCHAR size 19+(p+1)+1+40 ; else (Session (default)/Utc/WallClock) SQL_TYPE_TIMESTAMP as timestamp(p) | | | | | | | | String | |
| interval year to month | interval==String: (W)VARCHAR(20) ; else SQL_INTERVAL_YEAR_TO_MONTH, verbose SQL_INTERVAL, subcode SQL_CODE_YEAR_TO_MONTH, size 20, octet sizeof(SQL_INTERVAL_STRUCT), defaultC SQL_C_INTERVAL_YEAR_TO_MONTH | | | | | | | | String | intervalType=true |
| interval day to second | interval==String: (W)VARCHAR(30) ; else SQL_INTERVAL_DAY_TO_SECOND / SQL_CODE_DAY_TO_SECOND, size 30, decimalDigits 3 | | | | | | | | String | |
| array/map/row | U?SQL_WLONGVARCHAR:SQL_LONGVARCHAR | | L | U?2L:L | L | | | | String (raw JSON) | caseSensitive |
| uuid | uuidAsGuid ? SQL_GUID (size 36, octet 16, display 36, SQL_C_GUID) : (W)VARCHAR(36) | | | | | | | | String | |
| ipaddress | (W)VARCHAR(45) | | | | | | | | String | |
| anything else | (W)VARCHAR(L) | | | | | | | | String | typeName = raw name |

General: `nullable = SQL_NULLABLE`; `unsignedAttr = !isNumeric` (numerics are signed); `caseSensitive` true for
char/varchar/json/complex/unknown; `searchable = SQL_SEARCHABLE` for character-ish, `SQL_PRED_BASIC` otherwise;
`typeName = type.toString()`, `localTypeName = typeName`; `fixedPrecScale=false`, `autoIncrement=false`.

SQLGetTypeInfo rows (odbcTypeInfoTable) ordered by DATA_TYPE ascending, then "closest match" first:
SQL_GUID(uuid, if enabled), SQL_WLONGVARCHAR(json / long varchar), SQL_WVARCHAR(varchar), SQL_WCHAR(char), SQL_BIT(boolean),
SQL_TINYINT, SQL_BIGINT, SQL_LONGVARBINARY(if unboundedAsLong) / SQL_VARBINARY(varbinary), [SQL_LONGVARCHAR, SQL_CHAR,
SQL_VARCHAR when !U instead of W types], SQL_DECIMAL(decimal), SQL_INTEGER, SQL_SMALLINT, SQL_REAL, SQL_DOUBLE, SQL_TYPE_DATE,
SQL_TYPE_TIME, SQL_TYPE_TIMESTAMP, SQL_INTERVAL_YEAR_TO_MONTH, SQL_INTERVAL_DAY_TO_SECOND (last two only when interval==Interval).
TYPE_NAME uses base names ("varchar", "decimal", "timestamp"), LITERAL_PREFIX/SUFFIX as in the table (empty for numerics),
CREATE_PARAMS as noted, NULLABLE=SQL_NULLABLE, MINIMUM_SCALE/MAXIMUM_SCALE 0/38 for decimal, 0/9 for time/timestamp, else NULL,
INTERVAL_PRECISION NULL except intervals (2 / 9). Legacy requests SQL_DATE(9)/SQL_TIME(10)/SQL_TIMESTAMP(11) map to the
SQL_TYPE_* rows, SQL_ALL_TYPES returns everything.

Result set column layout of SQLGetTypeInfo (19 columns, in this order and with these Trino types):
TYPE_NAME varchar, DATA_TYPE smallint, COLUMN_SIZE integer, LITERAL_PREFIX varchar, LITERAL_SUFFIX varchar, CREATE_PARAMS varchar,
NULLABLE smallint, CASE_SENSITIVE smallint, SEARCHABLE smallint, UNSIGNED_ATTRIBUTE smallint, FIXED_PREC_SCALE smallint,
AUTO_UNIQUE_VALUE smallint, LOCAL_TYPE_NAME varchar, MINIMUM_SCALE smallint, MAXIMUM_SCALE smallint, SQL_DATA_TYPE smallint,
SQL_DATETIME_SUB smallint, NUM_PREC_RADIX integer, INTERVAL_PRECISION smallint.

trinoTypeForSqlType: char family -> "varchar" (or varchar(n) when 0 < n < 2147483647); SQL_BIT->boolean; SQL_TINYINT->tinyint;
SQL_SMALLINT->smallint; SQL_INTEGER->integer; SQL_BIGINT->bigint; SQL_REAL->real; SQL_FLOAT/SQL_DOUBLE->double;
SQL_DECIMAL/SQL_NUMERIC->decimal(p,s) (p=columnSize or 38, s=decimalDigits); binary family->varbinary; SQL_TYPE_DATE/SQL_DATE->date;
SQL_TYPE_TIME/SQL_TIME->time(decimalDigits); SQL_TYPE_TIMESTAMP/SQL_TIMESTAMP->timestamp(decimalDigits, default 3);
SQL_GUID->uuid; SQL_INTERVAL_YEAR_TO_MONTH (and YEAR, MONTH)->interval year to month; other SQL_INTERVAL_*->interval day to second; default varchar.

Text rendering of values (cellToText, used for SQL_C_CHAR/WCHAR and catalog output): booleans follow the ODBC convention,
SQL_BIT to character yields "1" or "0". Doubles: shortest round-trip (doubleToString);
decimal: as stored; timestamp with time zone in Session mode (default): "YYYY-MM-DD HH:MM:SS.fff" converted to
ConvertOptions::sessionTimeZone (default Europe/Istanbul; unknown/empty session zone -> UTC + 01000), no zone suffix; Utc
mode: same, converted to UTC; unknown value zone in Session/Utc -> wall clock + 01000; WallClock: same digits without zone;
String: original text. time with time zone: Session shifts by the session zone's current offset, Utc to UTC (default String).
