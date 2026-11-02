// Result-set entry points: column binding, result metadata (SQLNumResultCols, SQLDescribeCol,
// SQLColAttribute), block fetching with column-wise / row-wise binding and SQLGetData chunking.
// See docs/design/ODBC-API-DETAILS.md "api_results" and ARCHITECTURE.md §8.
#include "odbc/api_common.h"
#include "odbc/stmt_results.h"

#include "convert/convert.h"
#include "types/odbc_types.h"
#include "util/errors.h"
#include "util/strings.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace trino_odbc {

bool isOdbc2Application(const Connection* c) {
  return c != nullptr && c->env != nullptr && c->env->odbcVersion == SQL_OV_ODBC2;
}

SQLSMALLINT sqlTypeForApplication(const Connection* c, SQLSMALLINT sqlType) {
  if (!isOdbc2Application(c)) return sqlType;
  switch (sqlType) {
    case SQL_TYPE_DATE: return SQL_DATE;
    case SQL_TYPE_TIME: return SQL_TIME;
    case SQL_TYPE_TIMESTAMP: return SQL_TIMESTAMP;
    default: return sqlType;
  }
}

size_t cTypeSize(SQLSMALLINT cType) {
  switch (normalizeCType(cType)) {
    case SQL_C_BIT:
    case SQL_C_STINYINT:
    case SQL_C_UTINYINT:
      return sizeof(SQLCHAR);
    case SQL_C_SSHORT:
    case SQL_C_USHORT:
      return sizeof(SQLSMALLINT);
    case SQL_C_SLONG:
    case SQL_C_ULONG:
      return sizeof(SQLINTEGER);
    case SQL_C_SBIGINT:
    case SQL_C_UBIGINT:
      return sizeof(SQLBIGINT);
    case SQL_C_FLOAT:
      return sizeof(SQLREAL);
    case SQL_C_DOUBLE:
      return sizeof(SQLDOUBLE);
    case SQL_C_NUMERIC:
      return sizeof(SQL_NUMERIC_STRUCT);
    case SQL_C_TYPE_DATE:
      return sizeof(SQL_DATE_STRUCT);
    case SQL_C_TYPE_TIME:
      return sizeof(SQL_TIME_STRUCT);
    case SQL_C_TYPE_TIMESTAMP:
      return sizeof(SQL_TIMESTAMP_STRUCT);
    case SQL_C_GUID:
      return sizeof(SQLGUID);
    case SQL_C_INTERVAL_YEAR:
    case SQL_C_INTERVAL_MONTH:
    case SQL_C_INTERVAL_DAY:
    case SQL_C_INTERVAL_HOUR:
    case SQL_C_INTERVAL_MINUTE:
    case SQL_C_INTERVAL_SECOND:
    case SQL_C_INTERVAL_YEAR_TO_MONTH:
    case SQL_C_INTERVAL_DAY_TO_HOUR:
    case SQL_C_INTERVAL_DAY_TO_MINUTE:
    case SQL_C_INTERVAL_DAY_TO_SECOND:
    case SQL_C_INTERVAL_HOUR_TO_MINUTE:
    case SQL_C_INTERVAL_HOUR_TO_SECOND:
    case SQL_C_INTERVAL_MINUTE_TO_SECOND:
      return sizeof(SQL_INTERVAL_STRUCT);
    default:
      return 0;  // SQL_C_CHAR / SQL_C_WCHAR / SQL_C_BINARY / SQL_C_DEFAULT: element size = buffer length
  }
}

namespace {

constexpr const char* kLog = "results";

/// Empty column list shared by non-result statements.
const std::vector<ResultColumn>& noColumns() {
  static const std::vector<ResultColumn> none;
  return none;
}

/// Columns the statement currently describes: the open result, else the DESCRIBE OUTPUT cache
/// (fetched lazily for prepared statements), else none for executed DML.
const std::vector<ResultColumn>& resolveColumns(Statement& s) {
  requireNotBusy(s);
  if (s.result) return s.result->columns();
  switch (s.state) {
    case StmtState::Allocated:
      throw DriverError(sqlstate::kFunctionSequence, "No statement has been prepared or executed");
    case StmtState::ExecutedNoCursor:
      return noColumns();
    default:
      break;
  }
  impl::ensureDescribedOutput(s);
  return s.describedOutputIsResultSet ? s.describedColumns : noColumns();
}

/// Element address for column-wise (stride = element size) or row-wise (stride = bind type) binding.
void* elementAddress(void* base, SQLINTEGER bindType, SQLLEN offset, size_t index, size_t elementSize) {
  if (!base) return nullptr;
  const size_t stride = bindType == SQL_BIND_BY_COLUMN ? elementSize : static_cast<size_t>(bindType);
  return static_cast<char*>(base) + offset + index * stride;
}

/// Effective C type of a bound column for size arithmetic (SQL_C_DEFAULT follows the column).
SQLSMALLINT effectiveCType(SQLSMALLINT cType, const ResultColumn& col) {
  SQLSMALLINT c = normalizeCType(cType);
  if (c == SQL_C_DEFAULT) {
    c = normalizeCType(col.odbc.defaultCType != 0 ? col.odbc.defaultCType : defaultCTypeFor(col.odbc.conciseType));
  }
  return c;
}

bool isCharOrBinaryType(SQLSMALLINT sqlType) {
  switch (sqlType) {
    case SQL_CHAR: case SQL_VARCHAR: case SQL_LONGVARCHAR:
    case SQL_WCHAR: case SQL_WVARCHAR: case SQL_WLONGVARCHAR:
    case SQL_BINARY: case SQL_VARBINARY: case SQL_LONGVARBINARY:
      return true;
    default:
      return false;
  }
}

/// SQL_ATTR_MAX_LENGTH in bytes of `cType` data (whole SQLWCHAR units for SQL_C_WCHAR); 0 = no cap.
/// The attribute limits character and binary columns only (ODBC); other columns are never cut.
SQLLEN maxLengthCap(const ResultColumn& col, SQLSMALLINT cType, SQLULEN maxLength) {
  if (maxLength == 0 || !isChunkedCType(cType) || !isCharOrBinaryType(col.odbc.conciseType)) return 0;
  SQLLEN cap = static_cast<SQLLEN>(std::min<SQLULEN>(maxLength, static_cast<SQLULEN>(INT64_MAX / 4)));
  if (cType == SQL_C_WCHAR) cap -= cap % static_cast<SQLLEN>(sizeof(SQLWCHAR));
  return cap;
}

/// Converts one cell honouring SQL_ATTR_MAX_LENGTH. Data beyond the cap does not exist for the
/// application (ODBC): a value cut only by the cap is delivered with SQL_SUCCESS (no 01004), the
/// length reported is the capped one and a following SQLGetData returns SQL_NO_DATA. Truncation by
/// a small buffer inside the capped data behaves as usual (01004, chunked SQLGetData).
/// `fullLength` is the per-column slot remembering the uncapped length at the first SQLGetData
/// call of a row (nullptr for bound columns, which have no chunk state).
SQLRETURN convertCapped(const ResultColumn& col, const Column& data, size_t row, const ConvertTarget& target,
                        SQLSMALLINT cType, SQLULEN maxLength, const ConvertOptions& opts, size_t* chunkOffset,
                        SQLLEN* fullLength, Diag& diag, SQLLEN rowNumber, SQLINTEGER columnNumber) {
  const SQLLEN cap = maxLengthCap(col, cType, maxLength);
  if (cap <= 0 || (chunkOffset && *chunkOffset == SIZE_MAX)) {
    return convertToC(col, data, row, target, opts, chunkOffset, diag, rowNumber, columnNumber);
  }
  const SQLLEN term = cType == SQL_C_CHAR ? 1 : cType == SQL_C_WCHAR ? static_cast<SQLLEN>(sizeof(SQLWCHAR)) : 0;

  // Capped bytes already handed out by earlier SQLGetData calls on this row: the uncapped length
  // at the first call minus what is left now (a length-only probe consumes nothing).
  SQLLEN already = 0;
  if (chunkOffset) {
    Diag scratch;
    SQLLEN rest = 0;
    ConvertTarget probe = target;
    probe.buffer = nullptr;
    probe.strLenOrInd = &rest;
    const SQLRETURN probeRc = convertToC(col, data, row, probe, opts, chunkOffset, scratch, rowNumber, columnNumber);
    if (probeRc == SQL_ERROR || probeRc == SQL_NO_DATA || rest < 0) {
      // NULL, conversion errors and end of data are reported by the ordinary path.
      return convertToC(col, data, row, target, opts, chunkOffset, diag, rowNumber, columnNumber);
    }
    if (*chunkOffset == 0 && fullLength) *fullLength = rest;
    const SQLLEN full = fullLength ? *fullLength : rest;
    already = full > rest ? full - rest : 0;
  }
  const SQLLEN remaining = cap - already;
  if (remaining <= 0) {
    if (chunkOffset) *chunkOffset = SIZE_MAX;
    return SQL_NO_DATA;
  }

  const SQLLEN bufLen = target.bufferLength < 0 ? 0 : target.bufferLength;
  const bool capLimits = target.buffer != nullptr && remaining + term <= bufLen;
  ConvertTarget t = target;
  if (capLimits) t.bufferLength = remaining + term;
  constexpr SQLLEN kUnset = std::numeric_limits<SQLLEN>::min();
  SQLLEN ind = kUnset;
  t.strLenOrInd = &ind;
  Diag local;
  local.serverName = diag.serverName;
  local.connectionName = diag.connectionName;
  SQLRETURN rc = convertToC(col, data, row, t, opts, chunkOffset, local, rowNumber, columnNumber);

  bool dropTruncation = false;
  if (rc != SQL_ERROR && rc != SQL_NO_DATA && ind != kUnset && ind >= 0) {
    bool truncated = false;
    for (const DiagRecord& r : local.records()) {
      if (r.sqlstate == sqlstate::kStringTruncated) truncated = true;
    }
    if (truncated && capLimits) {
      // Only the cap cut the value: the rest of it is not available to the application.
      dropTruncation = true;
      if (chunkOffset) *chunkOffset = SIZE_MAX;
    }
    if (ind > remaining) ind = remaining;
  }
  if (ind != kUnset && target.strLenOrInd) *target.strLenOrInd = ind;
  if (!dropTruncation) {
    for (const DiagRecord& r : local.records()) diag.add(r);
    return rc;
  }
  rc = SQL_SUCCESS;
  for (const DiagRecord& r : local.records()) {
    if (r.sqlstate == sqlstate::kStringTruncated) continue;
    rc = mergeReturnCode(rc, diag.add(r));
  }
  return rc;
}

/// SQLFetch / SQLFetchScroll / SQLExtendedFetch / SQLGetData / SQLSetPos state check: HY010 before
/// any execution (S1) or for a prepared, unexecuted statement (S2/S3); 24000 when the statement was
/// executed but has no result set (S4). `needFetched` additionally requires a fetched rowset (S6/S7).
void requireResultCursor(const Statement& s, bool needFetched, const char* fn) {
  switch (s.state) {
    case StmtState::Allocated:
    case StmtState::Prepared:
      throw DriverError(sqlstate::kFunctionSequence,
                        std::string("Function sequence error: ") + fn + " requires an executed statement");
    case StmtState::CursorOpen:
      if (needFetched) {
        throw DriverError(sqlstate::kInvalidCursorState, std::string(fn) + " requires a fetched row");
      }
      return;
    case StmtState::Fetched:
      return;
    default:
      throw DriverError(sqlstate::kInvalidCursorState, "Invalid cursor state: no cursor is open");
  }
}

/// Descriptor record describing column `col` (1-based) — the IRD when populated, otherwise a record
/// built on the fly from the column metadata.
DescRecord columnRecord(Statement& s, const std::vector<ResultColumn>& cols, SQLUSMALLINT col) {
  if (const DescRecord* r = s.ird->recordIf(static_cast<SQLSMALLINT>(col))) {
    if (!r->typeName.empty() || r->name == cols[col - 1].name) return *r;
  }
  Descriptor tmp;
  tmp.populateFromColumns(cols);
  return tmp.records[col - 1];
}

/// Fetches the next rowset and converts every bound column. `statusArray` / `rowsFetchedPtr` come
/// from the IRD header (SQLFetch / SQLFetchScroll) or from the SQLExtendedFetch arguments.
SQLRETURN fetchRowsetInto(Statement& s, SQLUSMALLINT* statusArray, SQLULEN* rowsFetchedPtr, SQLULEN rowsetSizeAttr) {
  requireNotBusy(s);
  requireResultCursor(s, false, "SQLFetch");
  if (!s.result) throw DriverError(sqlstate::kFunctionSequence, "No result set is open");
  ResultSet& rs = *s.result;
  const size_t columnCount = rs.columnCount();
  const std::vector<ResultColumn>& cols = rs.columns();
  const Descriptor& ard = *s.ard;
  const size_t rowsetSize = rowsetSizeAttr == 0 ? 1 : static_cast<size_t>(rowsetSizeAttr);

  // A binding beyond the result's columns is a descriptor error, detected before rows are consumed.
  for (size_t j = 0; j < ard.records.size(); ++j) {
    const DescRecord& rec = ard.records[j];
    if (rec.bound && (rec.dataPtr || rec.indicatorPtr || rec.octetLengthPtr) && j >= columnCount) {
      throw DriverError(sqlstate::kInvalidDescriptorIndex,
                        formatString("Column %zu is bound but the result set has only %zu column(s)", j + 1, columnCount));
    }
  }

  const size_t rows = rs.fetchRowset(rowsetSize);
  if (rowsFetchedPtr) *rowsFetchedPtr = static_cast<SQLULEN>(rows);
  if (statusArray) {
    for (size_t i = rows; i < rowsetSize; ++i) statusArray[i] = SQL_ROW_NOROW;
  }
  s.rowsetPos = 1;
  s.getData.reset(columnCount);
  s.state = StmtState::Fetched;
  s.diag.cursorRowCount = -1;
  if (rows == 0) {
    // Cursor is positioned after the last row: SQLGetData now reports 24000 (rowsetRow is invalid).
    s.rowNumber = 0;
    TRINO_LOG_TRACE(kLog, "fetch: end of data after %lld row(s)", static_cast<long long>(rs.rowsFetched()));
    return SQL_NO_DATA;
  }
  s.rowNumber = static_cast<SQLULEN>(rs.rowsetStartRowNumber());

  const SQLINTEGER bindType = ard.header.bindType;
  const SQLLEN offset = ard.header.bindOffsetPtr ? *ard.header.bindOffsetPtr : 0;
  const ConvertOptions opts = s.convertOptions();
  const bool convert = s.retrieveData != SQL_RD_OFF;

  size_t errorRows = 0;
  bool anyInfo = false;
  for (size_t i = 0; i < rows; ++i) {
    SQLRETURN rowRc = SQL_SUCCESS;
    if (convert) {
      const RowRef row = rs.rowsetRow(i);
      const size_t boundCount = std::min(ard.records.size(), columnCount);
      for (size_t j = 0; j < boundCount; ++j) {
        const DescRecord& rec = ard.records[j];
        if (!rec.bound) continue;
        SQLLEN* indPtr = rec.indicatorPtr ? rec.indicatorPtr : rec.octetLengthPtr;
        if (!rec.dataPtr && !indPtr) continue;
        const ResultColumn& col = cols[j];
        const SQLSMALLINT cType = effectiveCType(rec.conciseType, col);
        const size_t fixed = cTypeSize(cType);
        const size_t elementSize = fixed ? fixed : static_cast<size_t>(rec.octetLength < 0 ? 0 : rec.octetLength);

        ConvertTarget target;
        target.cType = rec.conciseType;
        target.buffer = elementAddress(rec.dataPtr, bindType, offset, i, elementSize);
        target.bufferLength = rec.octetLength;
        target.strLenOrInd = static_cast<SQLLEN*>(elementAddress(indPtr, bindType, offset, i, sizeof(SQLLEN)));
        if (rec.precisionSet) target.numericPrecision = rec.precision;
        if (rec.scaleSet) {
          target.numericScale = rec.scale;
          target.numericScaleSet = true;
        }
        const SQLRETURN rc = convertCapped(col, row.page->columns[j], row.row, target, cType, s.maxLength, opts,
                                           nullptr, nullptr, s.diag, static_cast<SQLLEN>(i + 1),
                                           static_cast<SQLINTEGER>(j + 1));
        rowRc = mergeReturnCode(rowRc, rc);
      }
    }
    if (rowRc == SQL_ERROR) {
      ++errorRows;
      s.diag.addWarning(sqlstate::kErrorInRow, "Error in row", 0, static_cast<SQLLEN>(i + 1));
      if (statusArray) statusArray[i] = SQL_ROW_ERROR;
    } else if (rowRc == SQL_SUCCESS_WITH_INFO) {
      anyInfo = true;
      if (statusArray) statusArray[i] = SQL_ROW_SUCCESS_WITH_INFO;
    } else {
      if (statusArray) statusArray[i] = SQL_ROW_SUCCESS;
    }
  }
  TRINO_LOG_TRACE(kLog, "fetch: %zu row(s) starting at %llu, %zu with errors", rows,
                  static_cast<unsigned long long>(s.rowNumber), errorRows);
  if (errorRows == rows) return SQL_ERROR;
  return (errorRows > 0 || anyInfo) ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

/// True when `field` expects a character value (written through TextOut with the byte convention).
bool isCharacterField(SQLUSMALLINT field) {
  switch (field) {
    case SQL_DESC_BASE_COLUMN_NAME:
    case SQL_DESC_BASE_TABLE_NAME:
    case SQL_DESC_CATALOG_NAME:   // == SQL_COLUMN_QUALIFIER_NAME
    case SQL_DESC_LABEL:          // == SQL_COLUMN_LABEL
    case SQL_DESC_LITERAL_PREFIX:
    case SQL_DESC_LITERAL_SUFFIX:
    case SQL_DESC_LOCAL_TYPE_NAME:
    case SQL_DESC_NAME:
    case SQL_COLUMN_NAME:
    case SQL_DESC_SCHEMA_NAME:    // == SQL_COLUMN_OWNER_NAME
    case SQL_DESC_TABLE_NAME:     // == SQL_COLUMN_TABLE_NAME
    case SQL_DESC_TYPE_NAME:      // == SQL_COLUMN_TYPE_NAME
      return true;
    default:
      return false;
  }
}

}  // namespace

// ---- impl ---------------------------------------------------------------------------------------
namespace impl {

SQLRETURN fetchRowset(Statement& s) {
  return fetchRowsetInto(s, s.ird->header.arrayStatusPtr, s.ird->header.rowsProcessedPtr, s.ard->header.arraySize);
}

SQLRETURN describeCol(Statement& s, SQLUSMALLINT col, TextOut& name, SQLSMALLINT* dataType, SQLULEN* columnSize,
                      SQLSMALLINT* decimalDigits, SQLSMALLINT* nullable) {
  const std::vector<ResultColumn>& cols = resolveColumns(s);
  if (col == 0 || static_cast<size_t>(col) > cols.size()) {
    throw DriverError(sqlstate::kInvalidDescriptorIndex,
                      formatString("Column number %u is out of range (1..%zu)", static_cast<unsigned>(col), cols.size()));
  }
  const ResultColumn& c = cols[col - 1];
  if (dataType) *dataType = sqlTypeForApplication(s.conn, c.odbc.conciseType);
  if (columnSize) *columnSize = c.odbc.columnSize;
  if (decimalDigits) *decimalDigits = c.odbc.decimalDigits;
  if (nullable) *nullable = c.odbc.nullable;
  return name.writeWithDiag(c.name, s.diag);
}

SQLRETURN colAttribute(Statement& s, SQLUSMALLINT col, SQLUSMALLINT field, SQLPOINTER charAttr, SQLSMALLINT bufLen,
                       SQLSMALLINT* strLen, SQLLEN* numAttr, bool wide) {
  const std::vector<ResultColumn>& cols = resolveColumns(s);
  if (field == SQL_DESC_COUNT || field == SQL_COLUMN_COUNT) {  // column 0 is allowed here
    if (numAttr) *numAttr = static_cast<SQLLEN>(cols.size());
    return SQL_SUCCESS;
  }
  if (col == 0 || static_cast<size_t>(col) > cols.size()) {
    throw DriverError(sqlstate::kInvalidDescriptorIndex,
                      formatString("Column number %u is out of range (1..%zu)", static_cast<unsigned>(col), cols.size()));
  }
  const ResultColumn& c = cols[col - 1];
  const DescRecord rec = columnRecord(s, cols, col);

  if (isCharacterField(field)) {
    std::string text;
    switch (field) {
      case SQL_DESC_BASE_COLUMN_NAME: text = rec.baseColumnName; break;
      case SQL_DESC_BASE_TABLE_NAME: text = rec.baseTableName; break;
      case SQL_DESC_CATALOG_NAME: text = rec.catalogName; break;
      case SQL_DESC_LABEL: text = rec.label.empty() ? rec.name : rec.label; break;
      case SQL_DESC_LITERAL_PREFIX: text = rec.literalPrefix; break;
      case SQL_DESC_LITERAL_SUFFIX: text = rec.literalSuffix; break;
      case SQL_DESC_LOCAL_TYPE_NAME: text = rec.localTypeName; break;
      case SQL_DESC_NAME:
      case SQL_COLUMN_NAME: text = rec.name; break;
      case SQL_DESC_SCHEMA_NAME: text = rec.schemaName; break;
      case SQL_DESC_TABLE_NAME: text = rec.tableName; break;
      case SQL_DESC_TYPE_NAME: text = rec.typeName; break;
      default: break;
    }
    // Both SQLColAttribute and SQLColAttributeW report lengths in bytes for character attributes.
    if (wide) {
      TextOut out(static_cast<SQLWCHAR*>(charAttr), bufLen, strLen, true);
      return out.writeWithDiag(text, s.diag);
    }
    TextOut out(static_cast<SQLCHAR*>(charAttr), bufLen, strLen, ansiEncodingOf(s));
    return out.writeWithDiag(text, s.diag);
  }

  SQLLEN value = 0;
  switch (field) {
    case SQL_DESC_AUTO_UNIQUE_VALUE: value = rec.autoUniqueValue; break;  // == SQL_COLUMN_AUTO_INCREMENT
    case SQL_DESC_CASE_SENSITIVE: value = rec.caseSensitive; break;       // == SQL_COLUMN_CASE_SENSITIVE
    case SQL_DESC_CONCISE_TYPE:                                          // == SQL_COLUMN_TYPE
      value = sqlTypeForApplication(s.conn, rec.conciseType);
      break;
    case SQL_DESC_TYPE: value = rec.type; break;
    case SQL_DESC_DATETIME_INTERVAL_CODE: value = rec.datetimeIntervalCode; break;
    case SQL_DESC_DATETIME_INTERVAL_PRECISION: value = rec.datetimeIntervalPrecision; break;
    case SQL_DESC_DISPLAY_SIZE: value = rec.displaySize; break;           // == SQL_COLUMN_DISPLAY_SIZE
    case SQL_DESC_FIXED_PREC_SCALE: value = rec.fixedPrecScale; break;    // == SQL_COLUMN_MONEY
    case SQL_DESC_LENGTH: value = rec.length; break;
    case SQL_COLUMN_LENGTH:
      // ODBC 2 "length": bytes transferred for character/binary data, the column size otherwise.
      value = isCharOrBinaryType(rec.conciseType) ? rec.octetLength : rec.length;
      break;
    case SQL_DESC_NULLABLE:
    case SQL_COLUMN_NULLABLE: value = rec.nullable; break;
    case SQL_DESC_NUM_PREC_RADIX: value = rec.numPrecRadix; break;
    case SQL_DESC_OCTET_LENGTH: value = rec.octetLength; break;
    case SQL_DESC_PRECISION: value = rec.precision; break;
    case SQL_COLUMN_PRECISION:
      // ODBC 2 "precision" is the column size for every type except exact/approximate numerics.
      value = c.odbc.numPrecRadix != 0 ? rec.precision : rec.length;
      break;
    case SQL_DESC_SCALE:
    case SQL_COLUMN_SCALE: value = rec.scale; break;
    case SQL_DESC_SEARCHABLE: value = rec.searchable; break;              // == SQL_COLUMN_SEARCHABLE
    case SQL_DESC_UNNAMED: value = rec.unnamed; break;
    case SQL_DESC_UNSIGNED: value = rec.unsignedAttr; break;              // == SQL_COLUMN_UNSIGNED
    case SQL_DESC_UPDATABLE: value = rec.updatable; break;                // == SQL_COLUMN_UPDATABLE
    default:
      throw DriverError(sqlstate::kInvalidDescriptorFieldId,
                        "Unknown or unsupported column attribute " + std::to_string(static_cast<int>(field)));
  }
  if (numAttr) *numAttr = value;
  return SQL_SUCCESS;
}

}  // namespace impl

}  // namespace trino_odbc

// ---- exported entry points ---------------------------------------------------------------------
using namespace trino_odbc;

// SQLColAttribute's NumericAttribute argument is SQLPOINTER on 32-bit Windows and SQLLEN* elsewhere;
// the definition must match the driver-manager header or C++ sees a conflicting declaration.
#if defined(_WIN32) && !defined(_WIN64)
using ColAttrNumPtr = SQLPOINTER;
#else
using ColAttrNumPtr = SQLLEN*;
#endif

extern "C" {

TRINO_ODBC_API SQLRETURN SQL_API SQLBindCol(SQLHSTMT hstmt, SQLUSMALLINT icol, SQLSMALLINT fCType, SQLPOINTER rgbValue,
                                            SQLLEN cbValueMax, SQLLEN* pcbValue) {
  return guardStmt(hstmt, "SQLBindCol", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (icol == 0) throw DriverError(sqlstate::kInvalidDescriptorIndex, "Column 0 cannot be bound: bookmarks are not supported");
    if (icol > 0x7fff) {
      // Descriptor records are addressed with SQLSMALLINT: larger numbers can never be a column.
      throw DriverError(sqlstate::kInvalidDescriptorIndex,
                        "Column number " + std::to_string(static_cast<unsigned>(icol)) + " is out of range");
    }
    if (!isValidCType(fCType)) {
      throw DriverError(sqlstate::kInvalidAppBufferType,
                        "Invalid application buffer type " + std::to_string(static_cast<int>(fCType)));
    }
    if (cbValueMax < 0) {
      throw DriverError(sqlstate::kInvalidStringOrBufferLength,
                        "BufferLength must not be negative (" + std::to_string(static_cast<long long>(cbValueMax)) + ")");
    }
    Descriptor& ard = *s.ard;
    if (!rgbValue && !pcbValue) {
      // Unbinding: clear the record; when it was the last one shrink SQL_DESC_COUNT to the highest bound record.
      if (icol <= static_cast<SQLUSMALLINT>(ard.count())) {
        DescRecord& r = ard.record(static_cast<SQLSMALLINT>(icol));
        r.dataPtr = nullptr;
        r.octetLengthPtr = nullptr;
        r.indicatorPtr = nullptr;
        r.bound = false;
        SQLSMALLINT last = ard.count();
        while (last > 0 && !ard.records[static_cast<size_t>(last - 1)].bound) --last;
        ard.setCount(last);
      }
      return SQL_SUCCESS;
    }
    const SQLSMALLINT normalized = normalizeCType(fCType);
    DescRecord& r = ard.record(static_cast<SQLSMALLINT>(icol));
    r.conciseType = fCType;
    r.type = normalized == SQL_C_DEFAULT ? SQL_C_DEFAULT : verboseTypeOf(normalized);
    r.datetimeIntervalCode = normalized == SQL_C_DEFAULT ? 0 : datetimeSubcodeOf(normalized);
    r.octetLength = cbValueMax;
    r.length = cbValueMax;
    r.dataPtr = rgbValue;
    r.octetLengthPtr = pcbValue;
    r.indicatorPtr = pcbValue;
    r.bound = true;
    TRINO_LOG_TRACE(kLog, "bind column %u: C=%s buf=%lld", static_cast<unsigned>(icol), cTypeName(fCType),
                    static_cast<long long>(cbValueMax));
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT hstmt, SQLSMALLINT* pccol) {
  return guardStmt(hstmt, "SQLNumResultCols", [&](Statement& s) -> SQLRETURN {
    const std::vector<ResultColumn>& cols = resolveColumns(s);
    if (pccol) *pccol = static_cast<SQLSMALLINT>(std::min<size_t>(cols.size(), 0x7fff));
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLDescribeCol(SQLHSTMT hstmt, SQLUSMALLINT icol, SQLCHAR* szColName,
                                                SQLSMALLINT cbColNameMax, SQLSMALLINT* pcbColName, SQLSMALLINT* pfSqlType,
                                                SQLULEN* pcbColDef, SQLSMALLINT* pibScale, SQLSMALLINT* pfNullable) {
  return guardStmt(hstmt, "SQLDescribeCol", [&](Statement& s) -> SQLRETURN {
    // Qualified: <wingdi.h> turns the bare name into a macro that collides with GDI's TextOutA.
    trino_odbc::TextOut name(szColName, cbColNameMax, pcbColName, ansiEncodingOf(s));
    return impl::describeCol(s, icol, name, pfSqlType, pcbColDef, pibScale, pfNullable);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLColAttribute(SQLHSTMT hstmt, SQLUSMALLINT icol, SQLUSMALLINT fFieldId,
                                                 SQLPOINTER pCharAttr, SQLSMALLINT cbCharAttrMax, SQLSMALLINT* pcbCharAttr,
                                                 ColAttrNumPtr pNumAttr) {
  return guardStmt(hstmt, "SQLColAttribute", [&](Statement& s) -> SQLRETURN {
    return impl::colAttribute(s, icol, fFieldId, pCharAttr, cbCharAttrMax, pcbCharAttr,
                              static_cast<SQLLEN*>(pNumAttr), false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLColAttributes(SQLHSTMT hstmt, SQLUSMALLINT icol, SQLUSMALLINT fDescType,
                                                  SQLPOINTER rgbDesc, SQLSMALLINT cbDescMax, SQLSMALLINT* pcbDesc,
                                                  SQLLEN* pfDesc) {
  return guardStmt(hstmt, "SQLColAttributes", [&](Statement& s) -> SQLRETURN {
    return impl::colAttribute(s, icol, fDescType, rgbDesc, cbDescMax, pcbDesc, pfDesc, false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLFetch(SQLHSTMT hstmt) {
  return guardStmt(hstmt, "SQLFetch", [&](Statement& s) -> SQLRETURN { return impl::fetchRowset(s); });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLFetchScroll(SQLHSTMT hstmt, SQLSMALLINT fFetchOrientation, SQLLEN fFetchOffset) {
  return guardStmt(hstmt, "SQLFetchScroll", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (fFetchOrientation != SQL_FETCH_NEXT) {
      throw DriverError(sqlstate::kFetchTypeOutOfRange,
                        formatString("Fetch orientation %d (offset %lld) is not supported by a forward-only cursor",
                                     static_cast<int>(fFetchOrientation), static_cast<long long>(fFetchOffset)));
    }
    return impl::fetchRowset(s);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLExtendedFetch(SQLHSTMT hstmt, SQLUSMALLINT fFetchType, SQLLEN irow, SQLULEN* pcrow,
                                                  SQLUSMALLINT* rgfRowStatus) {
  return guardStmt(hstmt, "SQLExtendedFetch", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (fFetchType != SQL_FETCH_NEXT) {
      throw DriverError(sqlstate::kFetchTypeOutOfRange,
                        formatString("Fetch type %u (row %lld) is not supported by a forward-only cursor",
                                     static_cast<unsigned>(fFetchType), static_cast<long long>(irow)));
    }
    // ODBC 2 block fetch: the rowset size is SQL_ROWSET_SIZE (its own attribute, not the ARD array
    // size SQLFetch uses) and the status / row-count outputs are the call's own arguments rather
    // than the IRD header pointers.
    return fetchRowsetInto(s, rgfRowStatus, pcrow, s.odbc2RowsetSize);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetData(SQLHSTMT hstmt, SQLUSMALLINT icol, SQLSMALLINT fCType, SQLPOINTER rgbValue,
                                            SQLLEN cbValueMax, SQLLEN* pcbValue) {
  return guardStmt(hstmt, "SQLGetData", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    requireResultCursor(s, true, "SQLGetData");
    if (!s.result) throw DriverError(sqlstate::kInvalidCursorState, "SQLGetData requires a fetched row");
    ResultSet& rs = *s.result;
    const size_t columnCount = rs.columnCount();
    if (icol == 0) throw DriverError(sqlstate::kInvalidDescriptorIndex, "Column 0 is the bookmark column, which is not supported");
    if (static_cast<size_t>(icol) > columnCount) {
      throw DriverError(sqlstate::kInvalidDescriptorIndex,
                        formatString("Column number %u is out of range (1..%zu)", static_cast<unsigned>(icol), columnCount));
    }
    const RowRef row = rs.rowsetRow(s.rowsetPos - 1);
    if (!row.valid()) throw DriverError(sqlstate::kInvalidCursorState, "The cursor is not positioned on a row");
    const ResultColumn& col = rs.columns()[icol - 1];

    ConvertTarget target;
    target.cType = fCType;
    if (fCType == SQL_ARD_TYPE) {
      // Use the ARD record's type (and SQL_C_NUMERIC precision/scale) for this column.
      // Only the record's type/precision/scale are used: the column need not be bound (no data
      // pointer), the application may have set just SQL_DESC_TYPE etc. through SQLSetDescField.
      const DescRecord* rec = icol <= 0x7fff ? s.ard->recordIf(static_cast<SQLSMALLINT>(icol)) : nullptr;
      if (!rec) {
        throw DriverError(sqlstate::kInvalidDescriptorIndex,
                          formatString("SQL_ARD_TYPE requested but the ARD has no record for column %u",
                                       static_cast<unsigned>(icol)));
      }
      if (!isValidCType(rec->conciseType)) {
        throw DriverError(sqlstate::kInvalidAppBufferType,
                          formatString("SQL_ARD_TYPE: the ARD record of column %u has no valid C type (%d)",
                                       static_cast<unsigned>(icol), static_cast<int>(rec->conciseType)));
      }
      target.cType = rec->conciseType;
      if (rec->precisionSet) target.numericPrecision = rec->precision;
      if (rec->scaleSet) {
        target.numericScale = rec->scale;
        target.numericScaleSet = true;
      }
    } else if (!isValidCType(fCType)) {
      throw DriverError(sqlstate::kInvalidAppBufferType,
                        "Invalid application buffer type " + std::to_string(static_cast<int>(fCType)));
    }
    const SQLSMALLINT cType = effectiveCType(target.cType, col);
    if (cbValueMax < 0 && isChunkedCType(cType)) {
      throw DriverError(sqlstate::kInvalidStringOrBufferLength,
                        "BufferLength must not be negative (" + std::to_string(static_cast<long long>(cbValueMax)) + ")");
    }
    target.buffer = rgbValue;
    target.bufferLength = cbValueMax;
    target.strLenOrInd = pcbValue;

    if (s.getData.offsets.size() != columnCount) s.getData.reset(columnCount);
    if (s.maxLengthDelivered.size() != columnCount) s.maxLengthDelivered.assign(columnCount, 0);
    size_t* chunkOffset = &s.getData.offsets[icol - 1];
    const SQLRETURN rc = convertCapped(col, row.page->columns[icol - 1], row.row, target, cType, s.maxLength,
                                       s.convertOptions(), chunkOffset, &s.maxLengthDelivered[icol - 1], s.diag,
                                       static_cast<SQLLEN>(s.rowsetPos), static_cast<SQLINTEGER>(icol));
    TRINO_LOG_TRACE(kLog, "SQLGetData col=%u C=%s buf=%lld -> %d", static_cast<unsigned>(icol), cTypeName(target.cType),
                    static_cast<long long>(cbValueMax), static_cast<int>(rc));
    return rc;
  });
}

}  // extern "C"
