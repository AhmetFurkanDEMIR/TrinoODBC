// SQLGetInfo / SQLGetFunctions.
//
// SQLGetInfo answers every information type defined by sqlext.h for ODBC 3.8. The values describe
// what the driver and Trino actually do: the function bitmasks list exactly the escape sequences the
// translator in sql/escape.cpp understands, the cursor/transaction answers match the statement layer,
// and the SQL-grammar answers match Trino's grammar. Keep them in sync with the modules they describe.
//
// SQLGetFunctions reports exactly the entry points exported by the driver (docs/design/IMPLEMENTATION-NOTES.md).
#include "odbc/api_common.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

// ---- Names that older driver-manager headers may lack (values from the Microsoft sqlext.h) ------
#ifndef SQL_API_ALL_FUNCTIONS
#  define SQL_API_ALL_FUNCTIONS 0
#endif
#ifndef SQL_API_ODBC3_ALL_FUNCTIONS
#  define SQL_API_ODBC3_ALL_FUNCTIONS 999
#endif
#ifndef SQL_API_ODBC3_ALL_FUNCTIONS_SIZE
#  define SQL_API_ODBC3_ALL_FUNCTIONS_SIZE 250
#endif
#ifndef SQL_MAX_ASYNC_CONCURRENT_STATEMENTS
#  define SQL_MAX_ASYNC_CONCURRENT_STATEMENTS 10022
#endif
#ifndef SQL_DTC_TRANSITION_COST
#  define SQL_DTC_TRANSITION_COST 1750
#endif
#ifndef SQL_CONVERT_INTERVAL_YEAR_MONTH
#  define SQL_CONVERT_INTERVAL_YEAR_MONTH 123
#endif
#ifndef SQL_CONVERT_INTERVAL_DAY_TIME
#  define SQL_CONVERT_INTERVAL_DAY_TIME 124
#endif
#ifndef SQL_CVT_INTERVAL_YEAR_MONTH
#  define SQL_CVT_INTERVAL_YEAR_MONTH 0x00080000L
#endif
#ifndef SQL_CVT_INTERVAL_DAY_TIME
#  define SQL_CVT_INTERVAL_DAY_TIME 0x00100000L
#endif
#ifndef SQL_CONVERT_WCHAR
#  define SQL_CONVERT_WCHAR 122
#endif
#ifndef SQL_CONVERT_WLONGVARCHAR
#  define SQL_CONVERT_WLONGVARCHAR 125
#endif
#ifndef SQL_CONVERT_WVARCHAR
#  define SQL_CONVERT_WVARCHAR 126
#endif
#ifndef SQL_CVT_WCHAR
#  define SQL_CVT_WCHAR 0x00200000L
#endif
#ifndef SQL_CVT_WLONGVARCHAR
#  define SQL_CVT_WLONGVARCHAR 0x00400000L
#endif
#ifndef SQL_CVT_WVARCHAR
#  define SQL_CVT_WVARCHAR 0x00800000L
#endif
#ifndef SQL_DDL_INDEX
#  define SQL_DDL_INDEX 170
#endif
#ifndef SQL_CURSOR_SENSITIVITY
#  define SQL_CURSOR_SENSITIVITY 10001
#endif
#ifndef SQL_DESCRIBE_PARAMETER
#  define SQL_DESCRIBE_PARAMETER 10002
#endif
#ifndef SQL_CATALOG_NAME
#  define SQL_CATALOG_NAME 10003
#endif
#ifndef SQL_COLLATION_SEQ
#  define SQL_COLLATION_SEQ 10004
#endif
#ifndef SQL_MAX_IDENTIFIER_LEN
#  define SQL_MAX_IDENTIFIER_LEN 10005
#endif
#ifndef SQL_XOPEN_CLI_YEAR
#  define SQL_XOPEN_CLI_YEAR 10000
#endif
#ifndef SQL_UNSPECIFIED
#  define SQL_UNSPECIFIED 0
#endif
#ifndef SQL_IK_NONE
#  define SQL_IK_NONE 0x00000000L
#endif
#ifndef SQL_SS_ADDITIONS
#  define SQL_SS_ADDITIONS 0x00000001L
#endif
#ifndef SQL_MAX_ROW_SIZE_INCLUDES_LONG
#  define SQL_MAX_ROW_SIZE_INCLUDES_LONG 103
#endif
#ifndef SQL_DM_VER
#  define SQL_DM_VER 171
#endif
#ifndef SQL_DRIVER_HLIB
#  define SQL_DRIVER_HLIB 76
#endif
#ifndef SQL_DRIVER_HDESC
#  define SQL_DRIVER_HDESC 135
#endif
#ifndef SQL_API_SQLCOLATTRIBUTE
#  define SQL_API_SQLCOLATTRIBUTE 6
#endif
#ifndef SQL_API_SQLBULKOPERATIONS
#  define SQL_API_SQLBULKOPERATIONS 24
#endif
#ifndef SQL_API_SQLALLOCHANDLE
#  define SQL_API_SQLALLOCHANDLE 1001
#endif
#ifndef SQL_API_SQLCLOSECURSOR
#  define SQL_API_SQLCLOSECURSOR 1003
#endif
#ifndef SQL_API_SQLCOPYDESC
#  define SQL_API_SQLCOPYDESC 1004
#endif
#ifndef SQL_API_SQLENDTRAN
#  define SQL_API_SQLENDTRAN 1005
#endif
#ifndef SQL_API_SQLFREEHANDLE
#  define SQL_API_SQLFREEHANDLE 1006
#endif
#ifndef SQL_API_SQLGETCONNECTATTR
#  define SQL_API_SQLGETCONNECTATTR 1007
#endif
#ifndef SQL_API_SQLGETDESCFIELD
#  define SQL_API_SQLGETDESCFIELD 1008
#endif
#ifndef SQL_API_SQLGETDESCREC
#  define SQL_API_SQLGETDESCREC 1009
#endif
#ifndef SQL_API_SQLGETDIAGFIELD
#  define SQL_API_SQLGETDIAGFIELD 1010
#endif
#ifndef SQL_API_SQLGETDIAGREC
#  define SQL_API_SQLGETDIAGREC 1011
#endif
#ifndef SQL_API_SQLGETENVATTR
#  define SQL_API_SQLGETENVATTR 1012
#endif
#ifndef SQL_API_SQLGETSTMTATTR
#  define SQL_API_SQLGETSTMTATTR 1014
#endif
#ifndef SQL_API_SQLSETCONNECTATTR
#  define SQL_API_SQLSETCONNECTATTR 1016
#endif
#ifndef SQL_API_SQLSETDESCFIELD
#  define SQL_API_SQLSETDESCFIELD 1017
#endif
#ifndef SQL_API_SQLSETDESCREC
#  define SQL_API_SQLSETDESCREC 1018
#endif
#ifndef SQL_API_SQLSETENVATTR
#  define SQL_API_SQLSETENVATTR 1019
#endif
#ifndef SQL_API_SQLSETSTMTATTR
#  define SQL_API_SQLSETSTMTATTR 1020
#endif
#ifndef SQL_API_SQLFETCHSCROLL
#  define SQL_API_SQLFETCHSCROLL 1021
#endif

namespace trino_odbc {

namespace {

// ---- Output helpers -----------------------------------------------------------------------------
/// Writes one SQLGetInfo answer with the width the spec prescribes for that info type. The spec
/// says BufferLength is ignored for numeric answers, so they are written whenever `value` is set;
/// *strLen is always filled so callers probing for the size (value == nullptr) work.
class InfoWriter {
 public:
  InfoWriter(SQLPOINTER value, SQLSMALLINT bufLen, SQLSMALLINT* strLen, bool wide, AnsiEncoding enc, Diag& diag)
      : value_(value), bufLen_(bufLen), strLen_(strLen), wide_(wide), enc_(enc), diag_(diag) {}

  SQLRETURN str(std::string_view utf8) {
    if (wide_) {
      // SQLGetInfoW: BufferLength and *StringLength are byte counts.
      TextOut out(static_cast<SQLWCHAR*>(value_), static_cast<SQLLEN>(bufLen_), strLen_, /*lengthInBytes=*/true);
      return out.writeWithDiag(utf8, diag_);
    }
    TextOut out(static_cast<SQLCHAR*>(value_), static_cast<SQLLEN>(bufLen_), strLen_, enc_);
    return out.writeWithDiag(utf8, diag_);
  }
  SQLRETURN yn(bool yes) { return str(yes ? "Y" : "N"); }
  SQLRETURN u16(SQLUSMALLINT v) { return fixed(&v, sizeof(v)); }
  SQLRETURN u32(SQLUINTEGER v) { return fixed(&v, sizeof(v)); }
  SQLRETURN i32(SQLINTEGER v) { return fixed(&v, sizeof(v)); }

 private:
  SQLRETURN fixed(const void* src, size_t size) {
    if (value_ != nullptr) std::memcpy(value_, src, size);
    if (strLen_ != nullptr) *strLen_ = static_cast<SQLSMALLINT>(size);
    return SQL_SUCCESS;
  }

  SQLPOINTER value_;
  SQLSMALLINT bufLen_;
  SQLSMALLINT* strLen_;
  bool wide_;
  AnsiEncoding enc_;
  Diag& diag_;
};

// ---- Capability sets shared by several info types ----------------------------------------------
// The escape translator (sql/escape.cpp) supports exactly these scalar functions;
// docs/design/IMPLEMENTATION-NOTES.md is the source of truth for both modules.
constexpr SQLUINTEGER kStringFunctions =
    SQL_FN_STR_ASCII | SQL_FN_STR_BIT_LENGTH | SQL_FN_STR_CHAR | SQL_FN_STR_CHAR_LENGTH |
    SQL_FN_STR_CHARACTER_LENGTH | SQL_FN_STR_CONCAT | SQL_FN_STR_INSERT | SQL_FN_STR_LCASE | SQL_FN_STR_LEFT |
    SQL_FN_STR_LENGTH | SQL_FN_STR_LOCATE | SQL_FN_STR_LOCATE_2 | SQL_FN_STR_LTRIM | SQL_FN_STR_OCTET_LENGTH |
    SQL_FN_STR_POSITION | SQL_FN_STR_REPEAT | SQL_FN_STR_REPLACE | SQL_FN_STR_RIGHT | SQL_FN_STR_RTRIM |
    SQL_FN_STR_SOUNDEX | SQL_FN_STR_SPACE | SQL_FN_STR_SUBSTRING | SQL_FN_STR_UCASE;

constexpr SQLUINTEGER kNumericFunctions =
    SQL_FN_NUM_ABS | SQL_FN_NUM_ACOS | SQL_FN_NUM_ASIN | SQL_FN_NUM_ATAN | SQL_FN_NUM_ATAN2 | SQL_FN_NUM_CEILING |
    SQL_FN_NUM_COS | SQL_FN_NUM_COT | SQL_FN_NUM_DEGREES | SQL_FN_NUM_EXP | SQL_FN_NUM_FLOOR | SQL_FN_NUM_LOG |
    SQL_FN_NUM_LOG10 | SQL_FN_NUM_MOD | SQL_FN_NUM_PI | SQL_FN_NUM_POWER | SQL_FN_NUM_RADIANS | SQL_FN_NUM_RAND |
    SQL_FN_NUM_ROUND | SQL_FN_NUM_SIGN | SQL_FN_NUM_SIN | SQL_FN_NUM_SQRT | SQL_FN_NUM_TAN | SQL_FN_NUM_TRUNCATE;

constexpr SQLUINTEGER kTimedateFunctions =
    SQL_FN_TD_CURRENT_DATE | SQL_FN_TD_CURRENT_TIME | SQL_FN_TD_CURRENT_TIMESTAMP | SQL_FN_TD_CURDATE |
    SQL_FN_TD_CURTIME | SQL_FN_TD_DAYNAME | SQL_FN_TD_DAYOFMONTH | SQL_FN_TD_DAYOFWEEK | SQL_FN_TD_DAYOFYEAR |
    SQL_FN_TD_EXTRACT | SQL_FN_TD_HOUR | SQL_FN_TD_MINUTE | SQL_FN_TD_MONTH | SQL_FN_TD_MONTHNAME | SQL_FN_TD_NOW |
    SQL_FN_TD_QUARTER | SQL_FN_TD_SECOND | SQL_FN_TD_TIMESTAMPADD | SQL_FN_TD_TIMESTAMPDIFF | SQL_FN_TD_WEEK |
    SQL_FN_TD_YEAR;

constexpr SQLUINTEGER kSystemFunctions = SQL_FN_SYS_DBNAME | SQL_FN_SYS_USERNAME | SQL_FN_SYS_IFNULL;

constexpr SQLUINTEGER kConvertFunctions = SQL_FN_CVT_CAST | SQL_FN_CVT_CONVERT;

constexpr SQLUINTEGER kTimestampIntervals =
    SQL_FN_TSI_FRAC_SECOND | SQL_FN_TSI_SECOND | SQL_FN_TSI_MINUTE | SQL_FN_TSI_HOUR | SQL_FN_TSI_DAY |
    SQL_FN_TSI_WEEK | SQL_FN_TSI_MONTH | SQL_FN_TSI_QUARTER | SQL_FN_TSI_YEAR;

// Targets of the CONVERT escape (the translator renders CONVERT as CAST, so the answer is Trino's CAST matrix).
constexpr SQLUINTEGER kCvtChar = SQL_CVT_CHAR | SQL_CVT_VARCHAR | SQL_CVT_LONGVARCHAR | SQL_CVT_WCHAR |
                                 SQL_CVT_WVARCHAR | SQL_CVT_WLONGVARCHAR;
constexpr SQLUINTEGER kCvtNumeric = SQL_CVT_NUMERIC | SQL_CVT_DECIMAL | SQL_CVT_INTEGER | SQL_CVT_SMALLINT |
                                    SQL_CVT_FLOAT | SQL_CVT_REAL | SQL_CVT_DOUBLE | SQL_CVT_BIT | SQL_CVT_TINYINT |
                                    SQL_CVT_BIGINT;
constexpr SQLUINTEGER kCvtDateTimestamp = SQL_CVT_DATE | SQL_CVT_TIMESTAMP;
constexpr SQLUINTEGER kCvtTime = SQL_CVT_TIME;
constexpr SQLUINTEGER kCvtBinary = SQL_CVT_BINARY | SQL_CVT_VARBINARY | SQL_CVT_LONGVARBINARY;
constexpr SQLUINTEGER kCvtGuid = SQL_CVT_GUID;
// Trino has no varchar -> interval cast (interval values only come from literals and arithmetic), so
// the character types do not claim the interval targets.
constexpr SQLUINTEGER kCvtFromChar = kCvtChar | kCvtNumeric | kCvtDateTimestamp | kCvtTime | kCvtBinary | kCvtGuid;
constexpr SQLUINTEGER kCvtFromNumeric = kCvtNumeric | kCvtChar;
constexpr SQLUINTEGER kCvtFromDateTimestamp = kCvtDateTimestamp | kCvtChar;
constexpr SQLUINTEGER kCvtFromTime = kCvtTime | kCvtChar;
// Trino rejects CAST(varbinary AS varchar) (from_utf8() is required), so binary converts only to binary.
constexpr SQLUINTEGER kCvtFromBinary = kCvtBinary;
constexpr SQLUINTEGER kCvtFromGuid = kCvtGuid | kCvtChar;

constexpr SQLUINTEGER kCatalogUsage = SQL_CU_DML_STATEMENTS | SQL_CU_TABLE_DEFINITION | SQL_CU_INDEX_DEFINITION |
                                      SQL_CU_PRIVILEGE_DEFINITION;
constexpr SQLUINTEGER kSchemaUsage = SQL_SU_DML_STATEMENTS | SQL_SU_TABLE_DEFINITION | SQL_SU_INDEX_DEFINITION |
                                     SQL_SU_PRIVILEGE_DEFINITION;

constexpr SQLUINTEGER kTxnIsolationOptions =
    SQL_TXN_READ_UNCOMMITTED | SQL_TXN_READ_COMMITTED | SQL_TXN_REPEATABLE_READ | SQL_TXN_SERIALIZABLE;

// Trino reserved words (docs: "Reserved keywords"); ODBC asks for the ones beyond SQL-92.
constexpr const char* kKeywords =
    "ALTER,AND,AS,BETWEEN,BY,CASE,CAST,CONSTRAINT,CREATE,CROSS,CUBE,CURRENT_CATALOG,CURRENT_DATE,CURRENT_PATH,"
    "CURRENT_ROLE,CURRENT_SCHEMA,CURRENT_TIME,CURRENT_TIMESTAMP,CURRENT_USER,DEALLOCATE,DELETE,DESCRIBE,DISTINCT,"
    "DROP,ELSE,END,ESCAPE,EXCEPT,EXECUTE,EXISTS,EXTRACT,FALSE,FOR,FROM,FULL,GROUP,GROUPING,HAVING,IN,INNER,INSERT,"
    "INTERSECT,INTO,IS,JOIN,JSON_ARRAY,JSON_EXISTS,JSON_OBJECT,JSON_QUERY,JSON_TABLE,JSON_VALUE,LEFT,LIKE,LISTAGG,"
    "LOCALTIME,LOCALTIMESTAMP,NATURAL,NORMALIZE,NOT,NULL,ON,OR,ORDER,OUTER,PREPARE,RECURSIVE,RIGHT,ROLLUP,SELECT,"
    "SKIP,TABLE,THEN,TRIM,TRUE,UESCAPE,UNION,UNNEST,USING,VALUES,WHEN,WHERE,WITH";

/// Parses up to three leading dot-separated numeric components of a Trino version string
/// ("483", "476.1", "453-e.2"): anything after the numeric prefix is ignored.
void parseVersionNumbers(std::string_view v, int out[3]) {
  out[0] = out[1] = out[2] = 0;
  size_t pos = 0;
  for (int part = 0; part < 3 && pos < v.size(); ++part) {
    if (v[pos] < '0' || v[pos] > '9') break;
    long value = 0;
    while (pos < v.size() && v[pos] >= '0' && v[pos] <= '9') {
      // Cap instead of overflowing: the field is printed with a fixed width anyway.
      if (value < 100000) value = value * 10 + (v[pos] - '0');
      ++pos;
    }
    out[part] = static_cast<int>(value);
    if (pos < v.size() && v[pos] == '.') {
      ++pos;
    } else {
      break;
    }
  }
}

// ---- SQLGetFunctions ------------------------------------------------------------------------------
// Exactly the entry points the driver exports (docs/design/IMPLEMENTATION-NOTES.md, "Exported entry points"). Unicode
// variants share the id of their ANSI counterpart. SQLDataSources/SQLDrivers/SQLCompleteAsync are
// driver-manager functions and are not listed.
constexpr SQLUSMALLINT kSupportedFunctions[] = {
    // ODBC 1.x/2.x ids (< 100, also present in the SQL_API_ALL_FUNCTIONS array)
    SQL_API_SQLALLOCCONNECT, SQL_API_SQLALLOCENV, SQL_API_SQLALLOCSTMT, SQL_API_SQLBINDCOL, SQL_API_SQLCANCEL,
    SQL_API_SQLCOLATTRIBUTE /* == SQL_API_SQLCOLATTRIBUTES */, SQL_API_SQLCONNECT, SQL_API_SQLDESCRIBECOL,
    SQL_API_SQLDISCONNECT, SQL_API_SQLERROR, SQL_API_SQLEXECDIRECT, SQL_API_SQLEXECUTE, SQL_API_SQLFETCH,
    SQL_API_SQLFREECONNECT, SQL_API_SQLFREEENV, SQL_API_SQLFREESTMT, SQL_API_SQLGETCURSORNAME,
    SQL_API_SQLNUMRESULTCOLS, SQL_API_SQLPREPARE, SQL_API_SQLROWCOUNT, SQL_API_SQLSETCURSORNAME,
    SQL_API_SQLSETPARAM, SQL_API_SQLTRANSACT, SQL_API_SQLBULKOPERATIONS, SQL_API_SQLCOLUMNS,
    SQL_API_SQLDRIVERCONNECT, SQL_API_SQLGETCONNECTOPTION, SQL_API_SQLGETDATA, SQL_API_SQLGETFUNCTIONS,
    SQL_API_SQLGETINFO, SQL_API_SQLGETSTMTOPTION, SQL_API_SQLGETTYPEINFO, SQL_API_SQLPARAMDATA,
    SQL_API_SQLPUTDATA, SQL_API_SQLSETCONNECTOPTION, SQL_API_SQLSETSTMTOPTION, SQL_API_SQLSPECIALCOLUMNS,
    SQL_API_SQLSTATISTICS, SQL_API_SQLTABLES, SQL_API_SQLBROWSECONNECT, SQL_API_SQLCOLUMNPRIVILEGES,
    SQL_API_SQLDESCRIBEPARAM, SQL_API_SQLEXTENDEDFETCH, SQL_API_SQLFOREIGNKEYS, SQL_API_SQLMORERESULTS,
    SQL_API_SQLNATIVESQL, SQL_API_SQLNUMPARAMS, SQL_API_SQLPARAMOPTIONS, SQL_API_SQLPRIMARYKEYS,
    SQL_API_SQLPROCEDURECOLUMNS, SQL_API_SQLPROCEDURES, SQL_API_SQLSETPOS, SQL_API_SQLSETSCROLLOPTIONS,
    SQL_API_SQLTABLEPRIVILEGES, SQL_API_SQLBINDPARAMETER,
    // ODBC 3.x ids
    SQL_API_SQLALLOCHANDLE, SQL_API_SQLCLOSECURSOR, SQL_API_SQLCOPYDESC, SQL_API_SQLENDTRAN, SQL_API_SQLFREEHANDLE,
    SQL_API_SQLGETCONNECTATTR, SQL_API_SQLGETDESCFIELD, SQL_API_SQLGETDESCREC, SQL_API_SQLGETDIAGFIELD,
    SQL_API_SQLGETDIAGREC, SQL_API_SQLGETENVATTR, SQL_API_SQLGETSTMTATTR, SQL_API_SQLSETCONNECTATTR,
    SQL_API_SQLSETDESCFIELD, SQL_API_SQLSETDESCREC, SQL_API_SQLSETENVATTR, SQL_API_SQLSETSTMTATTR,
    SQL_API_SQLFETCHSCROLL,
    // ODBC 3.8
    SQL_API_SQLCANCELHANDLE,
};

constexpr SQLUSMALLINT kOdbc3BitmapBits = SQL_API_ODBC3_ALL_FUNCTIONS_SIZE * 16;

bool functionSupported(SQLUSMALLINT id) {
  for (SQLUSMALLINT f : kSupportedFunctions) {
    if (f == id) return true;
  }
  return false;
}

}  // namespace

// ---- Version text --------------------------------------------------------------------------------
/// SQL_DBMS_VER: "##.##.####" built from the numeric Trino version, followed by the product-specific
/// text ODBC allows ("483.00.0000 Trino 483"). "00.00.0000" when the version is unknown (for example
/// ValidateConnection=0 skipped `SELECT version()`).
std::string dbmsVersionString(const Connection& c) {
  const std::string& raw = c.serverVersion;
  int parts[3];
  parseVersionNumbers(raw, parts);
  if (raw.empty() || (parts[0] == 0 && parts[1] == 0 && parts[2] == 0)) return "00.00.0000";
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%02d.%02d.%04d", parts[0], parts[1], parts[2]);
  std::string out(buf);
  out += " Trino ";
  out += raw;
  return out;
}

// ---- impl::getInfo ---------------------------------------------------------------------------------
namespace impl {

SQLRETURN getInfo(Connection& c, SQLUSMALLINT infoType, SQLPOINTER value, SQLSMALLINT bufLen, SQLSMALLINT* strLen,
                  bool wide) {
  // HY090 per spec; BufferLength is otherwise ignored for numeric answers (handled by InfoWriter).
  if (bufLen < 0) {
    throw DriverError(sqlstate::kInvalidStringOrBufferLength, "Invalid string or buffer length");
  }
  InfoWriter w(value, bufLen, strLen, wide, ansiEncodingOf(c), c.diag);
  const ConnectionConfig& cfg = c.config;

  switch (infoType) {
    // ---- Driver information ---------------------------------------------------------------------
    case SQL_ACTIVE_ENVIRONMENTS: return w.u16(0);
    case SQL_ASYNC_DBC_FUNCTIONS: return w.u32(SQL_ASYNC_DBC_NOT_CAPABLE);
    case SQL_ASYNC_MODE: return w.u32(SQL_AM_NONE);
    case SQL_ASYNC_NOTIFICATION: return w.u32(SQL_ASYNC_NOTIFICATION_NOT_CAPABLE);
    case SQL_BATCH_ROW_COUNT: return w.u32(0);
    case SQL_BATCH_SUPPORT: return w.u32(0);
    case SQL_DATA_SOURCE_NAME: return w.str(c.dataSourceName.empty() ? cfg.dsn : c.dataSourceName);
    case SQL_DRIVER_AWARE_POOLING_SUPPORTED: return w.u32(SQL_DRIVER_AWARE_POOLING_NOT_CAPABLE);
    // The Driver Manager answers the handle questions itself; reaching the driver means a broken caller.
    case SQL_DRIVER_HDBC:
    case SQL_DRIVER_HENV:
    case SQL_DRIVER_HSTMT:
    case SQL_DRIVER_HDESC:
    case SQL_DRIVER_HLIB:
    case SQL_DM_VER:
      throw DriverError(sqlstate::kInformationTypeOutOfRange,
                        "Information type " + std::to_string(infoType) + " is answered by the Driver Manager");
    case SQL_DRIVER_NAME: return w.str(kDriverFileName);
    case SQL_DRIVER_ODBC_VER: return w.str(kDriverOdbcVersion);
    case SQL_DRIVER_VER: return w.str(kDriverVersionOdbc);
    case SQL_DYNAMIC_CURSOR_ATTRIBUTES1: return w.u32(0);
    case SQL_DYNAMIC_CURSOR_ATTRIBUTES2: return w.u32(0);
    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1:
      return w.u32(SQL_CA1_NEXT | SQL_CA1_LOCK_NO_CHANGE | SQL_CA1_POS_POSITION);
    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2: return w.u32(SQL_CA2_READ_ONLY_CONCURRENCY | SQL_CA2_MAX_ROWS_SELECT);
    case SQL_FILE_USAGE: return w.u16(SQL_FILE_NOT_SUPPORTED);
    case SQL_GETDATA_EXTENSIONS: return w.u32(SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BLOCK | SQL_GD_BOUND);
    case SQL_INFO_SCHEMA_VIEWS: return w.u32(SQL_ISV_COLUMNS | SQL_ISV_SCHEMATA | SQL_ISV_TABLES | SQL_ISV_VIEWS);
    case SQL_KEYSET_CURSOR_ATTRIBUTES1: return w.u32(0);
    case SQL_KEYSET_CURSOR_ATTRIBUTES2: return w.u32(0);
    case SQL_MAX_ASYNC_CONCURRENT_STATEMENTS: return w.u32(0);
    case SQL_MAX_CONCURRENT_ACTIVITIES: return w.u16(0);
    case SQL_MAX_DRIVER_CONNECTIONS: return w.u16(0);
    // SQL_ODBC_VER is normally the DM's; when the driver is loaded directly answer with our own level.
    case SQL_ODBC_VER: return w.str("03.80.0000");
    case SQL_ODBC_INTERFACE_CONFORMANCE: return w.u32(SQL_OIC_CORE);
    case SQL_ODBC_API_CONFORMANCE: return w.u16(SQL_OAC_LEVEL1);
    case SQL_ODBC_SAG_CLI_CONFORMANCE: return w.u16(SQL_OSCC_COMPLIANT);
    case SQL_ODBC_SQL_CONFORMANCE: return w.u16(SQL_OSC_CORE);
    case SQL_PARAM_ARRAY_ROW_COUNTS: return w.u32(SQL_PARC_NO_BATCH);
    case SQL_PARAM_ARRAY_SELECTS: return w.u32(SQL_PAS_NO_SELECT);
    case SQL_ROW_UPDATES: return w.yn(false);
    // MetadataCaseSensitive=1 makes every catalog-function argument a literal compared with '=', so
    // an application that escaped '_' / '%' with the advertised character would never match; an
    // empty escape tells it not to escape at all.
    case SQL_SEARCH_PATTERN_ESCAPE: return w.str(cfg.metadataCaseSensitive ? "" : "\\");
    case SQL_SERVER_NAME: return w.str(cfg.host);
    case SQL_STATIC_CURSOR_ATTRIBUTES1: return w.u32(0);
    case SQL_STATIC_CURSOR_ATTRIBUTES2: return w.u32(0);

    // ---- DBMS product information ----------------------------------------------------------------
    case SQL_DATABASE_NAME: return w.str(c.currentCatalog());
    case SQL_DBMS_NAME: return w.str("Trino");
    case SQL_DBMS_VER: return w.str(dbmsVersionString(c));

    // ---- Data source information -----------------------------------------------------------------
    case SQL_ACCESSIBLE_PROCEDURES: return w.yn(false);
    case SQL_ACCESSIBLE_TABLES: return w.yn(true);
    case SQL_BOOKMARK_PERSISTENCE: return w.u32(0);
    case SQL_CATALOG_TERM: return w.str("catalog");
    case SQL_COLLATION_SEQ: return w.str("");
    case SQL_CONCAT_NULL_BEHAVIOR: return w.u16(SQL_CB_NULL);
    case SQL_CURSOR_COMMIT_BEHAVIOR: return w.u16(SQL_CB_PRESERVE);
    case SQL_CURSOR_ROLLBACK_BEHAVIOR: return w.u16(SQL_CB_PRESERVE);
    case SQL_CURSOR_SENSITIVITY: return w.u32(SQL_UNSPECIFIED);
    case SQL_DATA_SOURCE_READ_ONLY: return w.yn(cfg.readOnly || c.accessMode == SQL_MODE_READ_ONLY);
    case SQL_DEFAULT_TXN_ISOLATION: return w.u32(SQL_TXN_READ_UNCOMMITTED);
    case SQL_DESCRIBE_PARAMETER: return w.yn(true);
    case SQL_MULT_RESULT_SETS: return w.yn(false);
    case SQL_MULTIPLE_ACTIVE_TXN: return w.yn(false);
    case SQL_NEED_LONG_DATA_LEN: return w.yn(false);
    // Trino's default is NULLS LAST for both ASC and DESC.
    case SQL_NULL_COLLATION: return w.u16(SQL_NC_END);
    case SQL_PROCEDURE_TERM: return w.str("procedure");
    case SQL_SCHEMA_TERM: return w.str("schema");
    case SQL_SCROLL_OPTIONS: return w.u32(SQL_SO_FORWARD_ONLY);
    case SQL_TABLE_TERM: return w.str("table");
    case SQL_TXN_CAPABLE: return w.u16(cfg.enableTransactions ? SQL_TC_DML : SQL_TC_NONE);
    case SQL_TXN_ISOLATION_OPTION: return w.u32(kTxnIsolationOptions);  // Trino supports all four levels
    case SQL_USER_NAME: return w.str(cfg.user);

    // ---- Supported SQL ----------------------------------------------------------------------------
    case SQL_AGGREGATE_FUNCTIONS:
      return w.u32(SQL_AF_ALL | SQL_AF_AVG | SQL_AF_COUNT | SQL_AF_DISTINCT | SQL_AF_MAX | SQL_AF_MIN | SQL_AF_SUM);
    case SQL_ALTER_DOMAIN: return w.u32(0);
    case SQL_ALTER_TABLE: return w.u32(SQL_AT_ADD_COLUMN | SQL_AT_DROP_COLUMN);
    case SQL_CATALOG_LOCATION: return w.u16(SQL_CL_START);
    case SQL_CATALOG_NAME: return w.yn(true);
    case SQL_CATALOG_NAME_SEPARATOR: return w.str(".");
    case SQL_CATALOG_USAGE: return w.u32(kCatalogUsage);
    case SQL_COLUMN_ALIAS: return w.yn(true);
    case SQL_CORRELATION_NAME: return w.u16(SQL_CN_ANY);
    case SQL_CREATE_ASSERTION: return w.u32(0);
    case SQL_CREATE_CHARACTER_SET: return w.u32(0);
    case SQL_CREATE_COLLATION: return w.u32(0);
    case SQL_CREATE_DOMAIN: return w.u32(0);
    case SQL_CREATE_SCHEMA: return w.u32(SQL_CS_CREATE_SCHEMA);
    case SQL_CREATE_TABLE: return w.u32(SQL_CT_CREATE_TABLE);
    case SQL_CREATE_TRANSLATION: return w.u32(0);
    case SQL_CREATE_VIEW: return w.u32(SQL_CV_CREATE_VIEW);
    case SQL_DDL_INDEX: return w.u32(0);
    case SQL_DROP_ASSERTION: return w.u32(0);
    case SQL_DROP_CHARACTER_SET: return w.u32(0);
    case SQL_DROP_COLLATION: return w.u32(0);
    case SQL_DROP_DOMAIN: return w.u32(0);
    case SQL_DROP_SCHEMA: return w.u32(SQL_DS_DROP_SCHEMA);
    case SQL_DROP_TABLE: return w.u32(SQL_DT_DROP_TABLE);
    case SQL_DROP_TRANSLATION: return w.u32(0);
    case SQL_DROP_VIEW: return w.u32(SQL_DV_DROP_VIEW);
    case SQL_EXPRESSIONS_IN_ORDERBY: return w.yn(true);
    case SQL_GROUP_BY: return w.u16(SQL_GB_GROUP_BY_CONTAINS_SELECT);
    case SQL_IDENTIFIER_CASE: return w.u16(SQL_IC_LOWER);
    case SQL_IDENTIFIER_QUOTE_CHAR: return w.str("\"");
    case SQL_INDEX_KEYWORDS: return w.u32(SQL_IK_NONE);
    case SQL_INSERT_STATEMENT: return w.u32(SQL_IS_INSERT_LITERALS | SQL_IS_INSERT_SEARCHED);
    case SQL_INTEGRITY: return w.yn(false);  // == SQL_ODBC_SQL_OPT_IEF
    case SQL_KEYWORDS: return w.str(kKeywords);
    case SQL_LIKE_ESCAPE_CLAUSE: return w.yn(true);
    case SQL_NON_NULLABLE_COLUMNS: return w.u16(SQL_NNC_NON_NULL);
    case SQL_OJ_CAPABILITIES:  // == SQL_OUTER_JOIN_CAPABILITIES
      return w.u32(SQL_OJ_LEFT | SQL_OJ_RIGHT | SQL_OJ_FULL | SQL_OJ_NESTED | SQL_OJ_NOT_ORDERED | SQL_OJ_INNER |
                   SQL_OJ_ALL_COMPARISON_OPS);
    case SQL_ORDER_BY_COLUMNS_IN_SELECT: return w.yn(false);
    case SQL_OUTER_JOINS: return w.yn(true);
    case SQL_PROCEDURES: return w.yn(false);
    case SQL_QUOTED_IDENTIFIER_CASE: return w.u16(SQL_IC_SENSITIVE);
    case SQL_SCHEMA_USAGE: return w.u32(kSchemaUsage);
    case SQL_SPECIAL_CHARACTERS: return w.str("");
    case SQL_SQL_CONFORMANCE: return w.u32(SQL_SC_SQL92_ENTRY);
    case SQL_SUBQUERIES:
      return w.u32(SQL_SQ_CORRELATED_SUBQUERIES | SQL_SQ_COMPARISON | SQL_SQ_EXISTS | SQL_SQ_IN | SQL_SQ_QUANTIFIED);
    case SQL_UNION: return w.u32(SQL_U_UNION | SQL_U_UNION_ALL);

    // ---- SQL limits (0 = no limit or unknown) ----------------------------------------------------
    case SQL_MAX_BINARY_LITERAL_LEN: return w.u32(0);
    case SQL_MAX_CATALOG_NAME_LEN: return w.u16(128);
    case SQL_MAX_CHAR_LITERAL_LEN: return w.u32(0);
    case SQL_MAX_COLUMN_NAME_LEN: return w.u16(128);
    case SQL_MAX_COLUMNS_IN_GROUP_BY: return w.u16(0);
    case SQL_MAX_COLUMNS_IN_INDEX: return w.u16(0);
    case SQL_MAX_COLUMNS_IN_ORDER_BY: return w.u16(0);
    case SQL_MAX_COLUMNS_IN_SELECT: return w.u16(0);
    case SQL_MAX_COLUMNS_IN_TABLE: return w.u16(0);
    case SQL_MAX_CURSOR_NAME_LEN: return w.u16(128);
    case SQL_MAX_IDENTIFIER_LEN: return w.u16(128);
    case SQL_MAX_INDEX_SIZE: return w.u32(0);
    case SQL_MAX_PROCEDURE_NAME_LEN: return w.u16(128);
    case SQL_MAX_ROW_SIZE: return w.u32(0);
    case SQL_MAX_ROW_SIZE_INCLUDES_LONG: return w.yn(true);
    case SQL_MAX_SCHEMA_NAME_LEN: return w.u16(128);
    case SQL_MAX_STATEMENT_LEN: return w.u32(0);
    case SQL_MAX_TABLE_NAME_LEN: return w.u16(128);
    case SQL_MAX_TABLES_IN_SELECT: return w.u16(0);
    case SQL_MAX_USER_NAME_LEN: return w.u16(128);

    // ---- Scalar function support ----------------------------------------------------------------
    case SQL_CONVERT_FUNCTIONS: return w.u32(kConvertFunctions);
    case SQL_NUMERIC_FUNCTIONS: return w.u32(kNumericFunctions);
    case SQL_STRING_FUNCTIONS: return w.u32(kStringFunctions);
    case SQL_SYSTEM_FUNCTIONS: return w.u32(kSystemFunctions);
    case SQL_TIMEDATE_ADD_INTERVALS: return w.u32(kTimestampIntervals);
    case SQL_TIMEDATE_DIFF_INTERVALS: return w.u32(kTimestampIntervals);
    case SQL_TIMEDATE_FUNCTIONS: return w.u32(kTimedateFunctions);

    // ---- Conversion (CONVERT escape) -------------------------------------------------------------
    case SQL_CONVERT_BIGINT:
    case SQL_CONVERT_BIT:
    case SQL_CONVERT_DECIMAL:
    case SQL_CONVERT_DOUBLE:
    case SQL_CONVERT_FLOAT:
    case SQL_CONVERT_INTEGER:
    case SQL_CONVERT_NUMERIC:
    case SQL_CONVERT_REAL:
    case SQL_CONVERT_SMALLINT:
    case SQL_CONVERT_TINYINT:
      return w.u32(kCvtFromNumeric);
    case SQL_CONVERT_CHAR:
    case SQL_CONVERT_VARCHAR:
    case SQL_CONVERT_LONGVARCHAR:
    case SQL_CONVERT_WCHAR:
    case SQL_CONVERT_WVARCHAR:
    case SQL_CONVERT_WLONGVARCHAR:
      return w.u32(kCvtFromChar);
    case SQL_CONVERT_DATE:
    case SQL_CONVERT_TIMESTAMP:
      return w.u32(kCvtFromDateTimestamp);
    case SQL_CONVERT_TIME: return w.u32(kCvtFromTime);
    case SQL_CONVERT_BINARY:
    case SQL_CONVERT_VARBINARY:
    case SQL_CONVERT_LONGVARBINARY:
      return w.u32(kCvtFromBinary);
    case SQL_CONVERT_GUID: return w.u32(kCvtFromGuid);
    case SQL_CONVERT_INTERVAL_YEAR_MONTH: return w.u32(SQL_CVT_INTERVAL_YEAR_MONTH | kCvtChar);
    case SQL_CONVERT_INTERVAL_DAY_TIME: return w.u32(SQL_CVT_INTERVAL_DAY_TIME | kCvtChar);

    // ---- SQL-92 feature sets ---------------------------------------------------------------------
    case SQL_DATETIME_LITERALS:
      return w.u32(SQL_DL_SQL92_DATE | SQL_DL_SQL92_TIME | SQL_DL_SQL92_TIMESTAMP | SQL_DL_SQL92_INTERVAL_YEAR |
                   SQL_DL_SQL92_INTERVAL_MONTH | SQL_DL_SQL92_INTERVAL_DAY | SQL_DL_SQL92_INTERVAL_HOUR |
                   SQL_DL_SQL92_INTERVAL_MINUTE | SQL_DL_SQL92_INTERVAL_SECOND | SQL_DL_SQL92_INTERVAL_YEAR_TO_MONTH |
                   SQL_DL_SQL92_INTERVAL_DAY_TO_HOUR | SQL_DL_SQL92_INTERVAL_DAY_TO_MINUTE |
                   SQL_DL_SQL92_INTERVAL_DAY_TO_SECOND | SQL_DL_SQL92_INTERVAL_HOUR_TO_MINUTE |
                   SQL_DL_SQL92_INTERVAL_HOUR_TO_SECOND | SQL_DL_SQL92_INTERVAL_MINUTE_TO_SECOND);
    case SQL_SQL92_DATETIME_FUNCTIONS:
      return w.u32(SQL_SDF_CURRENT_DATE | SQL_SDF_CURRENT_TIME | SQL_SDF_CURRENT_TIMESTAMP);
    case SQL_SQL92_FOREIGN_KEY_DELETE_RULE: return w.u32(0);
    case SQL_SQL92_FOREIGN_KEY_UPDATE_RULE: return w.u32(0);
    case SQL_SQL92_GRANT: return w.u32(0);
    case SQL_SQL92_NUMERIC_VALUE_FUNCTIONS:
      // Native syntax only: Trino has no char_length/character_length/octet_length functions (the
      // {fn ...} escapes for them are translated and reported through SQL_STRING_FUNCTIONS).
      return w.u32(SQL_SNVF_EXTRACT | SQL_SNVF_POSITION);
    case SQL_SQL92_PREDICATES:
      return w.u32(SQL_SP_BETWEEN | SQL_SP_COMPARISON | SQL_SP_EXISTS | SQL_SP_IN | SQL_SP_ISNOTNULL | SQL_SP_ISNULL |
                   SQL_SP_LIKE | SQL_SP_QUANTIFIED_COMPARISON);
    case SQL_SQL92_RELATIONAL_JOIN_OPERATORS:
      // No NATURAL JOIN ("Natural join not supported") and no UNION JOIN in Trino's grammar.
      return w.u32(SQL_SRJO_CROSS_JOIN | SQL_SRJO_EXCEPT_JOIN | SQL_SRJO_FULL_OUTER_JOIN | SQL_SRJO_INNER_JOIN |
                   SQL_SRJO_INTERSECT_JOIN | SQL_SRJO_LEFT_OUTER_JOIN | SQL_SRJO_RIGHT_OUTER_JOIN);
    case SQL_SQL92_REVOKE: return w.u32(0);
    case SQL_SQL92_ROW_VALUE_CONSTRUCTOR:
      return w.u32(SQL_SRVC_VALUE_EXPRESSION | SQL_SRVC_NULL | SQL_SRVC_ROW_SUBQUERY);
    case SQL_SQL92_STRING_FUNCTIONS:
      return w.u32(SQL_SSF_LOWER | SQL_SSF_UPPER | SQL_SSF_SUBSTRING | SQL_SSF_TRIM_BOTH | SQL_SSF_TRIM_LEADING |
                   SQL_SSF_TRIM_TRAILING);
    case SQL_SQL92_VALUE_EXPRESSIONS: return w.u32(SQL_SVE_CASE | SQL_SVE_CAST | SQL_SVE_COALESCE | SQL_SVE_NULLIF);
    case SQL_STANDARD_CLI_CONFORMANCE: return w.u32(SQL_SCC_ISO92_CLI);
    case SQL_XOPEN_CLI_YEAR: return w.str("1995");

    // ---- Distributed transactions ---------------------------------------------------------------
    case SQL_DTC_TRANSITION_COST: return w.u32(0);

    // ---- ODBC 2.x information types kept for old applications ----------------------------------
    case SQL_FETCH_DIRECTION: return w.u32(SQL_FD_FETCH_NEXT);
    case SQL_LOCK_TYPES: return w.u32(SQL_LCK_NO_CHANGE);
    case SQL_POS_OPERATIONS: return w.i32(SQL_POS_POSITION);
    case SQL_POSITIONED_STATEMENTS: return w.i32(0);
    case SQL_SCROLL_CONCURRENCY: return w.u32(SQL_SCCO_READ_ONLY);
    case SQL_STATIC_SENSITIVITY: return w.i32(0);

    default:
      break;
  }
  throw DriverError(sqlstate::kInformationTypeOutOfRange,
                    "Information type out of range: " + std::to_string(infoType));
}

}  // namespace impl

}  // namespace trino_odbc

// ---- Exported entry points ----------------------------------------------------------------------------
using namespace trino_odbc;

extern "C" {

TRINO_ODBC_API SQLRETURN SQL_API SQLGetInfo(SQLHDBC hdbc, SQLUSMALLINT infoType, SQLPOINTER infoValue,
                                            SQLSMALLINT bufferLength, SQLSMALLINT* stringLength) {
  return guardDbc(hdbc, "SQLGetInfo", [&](Connection& c) -> SQLRETURN {
    return impl::getInfo(c, infoType, infoValue, bufferLength, stringLength, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetFunctions(SQLHDBC hdbc, SQLUSMALLINT functionId, SQLUSMALLINT* supported) {
  return guardDbc(hdbc, "SQLGetFunctions", [&](Connection&) -> SQLRETURN {
    if (supported == nullptr) throw DriverError(sqlstate::kInvalidNullPointer, "Supported pointer is null");

    if (functionId == SQL_API_ODBC3_ALL_FUNCTIONS) {
      // 4000-bit bitmap read through SQL_FUNC_EXISTS: word = id >> 4, bit = id & 0xF.
      std::memset(supported, 0, sizeof(SQLUSMALLINT) * SQL_API_ODBC3_ALL_FUNCTIONS_SIZE);
      for (SQLUSMALLINT id : kSupportedFunctions) {
        if (id < kOdbc3BitmapBits) {
          supported[id >> 4] = static_cast<SQLUSMALLINT>(supported[id >> 4] | (1u << (id & 0x000F)));
        }
      }
      return SQL_SUCCESS;
    }
    if (functionId == SQL_API_ALL_FUNCTIONS) {
      // ODBC 2.x array: one SQL_TRUE/SQL_FALSE per function id below 100.
      for (SQLUSMALLINT id = 0; id < 100; ++id) supported[id] = functionSupported(id) ? SQL_TRUE : SQL_FALSE;
      return SQL_SUCCESS;
    }
    if (functionId >= kOdbc3BitmapBits) {
      throw DriverError(sqlstate::kFunctionTypeOutOfRange,
                        "Function type out of range: " + std::to_string(functionId));
    }
    *supported = functionSupported(functionId) ? SQL_TRUE : SQL_FALSE;
    return SQL_SUCCESS;
  });
}

}  // extern "C"
