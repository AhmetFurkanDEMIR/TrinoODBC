// Descriptor entry points: SQLCopyDesc, SQLGetDescField, SQLSetDescField, SQLGetDescRec,
// SQLSetDescRec (ANSI entry points + shared impl:: used by api_unicode.cpp).
// See ODBC-API-DETAILS.md "api_desc.cpp" and ARCHITECTURE.md §8 (descriptors are real).
#include "odbc/api_common.h"

#include "types/odbc_types.h"
#include "util/errors.h"
#include "util/strings.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "odbc";

// Scalar field values travel inside the SQLPOINTER argument itself (ODBC convention).
SQLSMALLINT smallOf(SQLPOINTER v) { return static_cast<SQLSMALLINT>(reinterpret_cast<intptr_t>(v)); }
SQLINTEGER intOf(SQLPOINTER v) { return static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(v)); }
SQLLEN lenOf(SQLPOINTER v) { return static_cast<SQLLEN>(reinterpret_cast<intptr_t>(v)); }
SQLULEN ulenOf(SQLPOINTER v) { return static_cast<SQLULEN>(reinterpret_cast<uintptr_t>(v)); }

template <typename T>
SQLRETURN putFixed(SQLPOINTER value, SQLINTEGER* strLen, T v) {
  if (value) std::memcpy(value, &v, sizeof(T));
  if (strLen) *strLen = static_cast<SQLINTEGER>(sizeof(T));
  return SQL_SUCCESS;
}

/// String descriptor fields use the byte convention (SQLGetDescFieldW documents bytes).
SQLRETURN putText(Descriptor& d, SQLPOINTER value, SQLINTEGER bufLen, SQLINTEGER* strLen, bool wide, std::string_view utf8) {
  if (bufLen < 0) throw DriverError(sqlstate::kInvalidStringOrBufferLength, "BufferLength must not be negative");
  if (wide) {
    TextOut out(static_cast<SQLWCHAR*>(value), bufLen, strLen, /*lengthInBytes=*/true);
    return out.writeWithDiag(utf8, d.diag);
  }
  const AnsiEncoding enc = d.conn ? ansiEncodingOf(*d.conn) : defaultAnsiEncoding();
  TextOut out(static_cast<SQLCHAR*>(value), bufLen, strLen, enc);
  return out.writeWithDiag(utf8, d.diag);
}

[[noreturn]] void invalidField(SQLSMALLINT field) {
  throw DriverError(sqlstate::kInvalidDescriptorFieldId, "Invalid descriptor field identifier " + std::to_string(field));
}

[[noreturn]] void readOnlyField(SQLSMALLINT field) {
  throw DriverError(sqlstate::kInvalidDescriptorFieldId,
                    "Descriptor field " + std::to_string(field) + " is read-only");
}

[[noreturn]] void cannotModifyIrd() {
  throw DriverError(sqlstate::kCannotModifyIrd,
                    "Cannot modify an implementation row descriptor (only SQL_DESC_ARRAY_STATUS_PTR and "
                    "SQL_DESC_ROWS_PROCESSED_PTR may be set on an IRD)");
}

[[noreturn]] void invalidIndex(SQLSMALLINT rec) {
  throw DriverError(sqlstate::kInvalidDescriptorIndex,
                    "Invalid descriptor index " + std::to_string(rec) + " (bookmarks are not supported; records start at 1)");
}

/// Statement-level state checks that apply to a descriptor through its implicit owner.
void requireOwnerNotBusy(const Descriptor& d) {
  if (d.implicitOwner) requireNotBusy(*d.implicitOwner);
}

/// The IRD of a statement is only meaningful once the statement is prepared or executed (HY007).
/// A prepared statement describes its output lazily; that is triggered here so SQLGetDescField on
/// the IRD works before SQLExecute just like SQLDescribeCol does.
void ensureIrdReady(Descriptor& d) {
  if (d.kind != DescKind::Ird || !d.implicitOwner) return;
  Statement& s = *d.implicitOwner;
  HandleLock lock(s.mutex);  // connection (held by the guard) before statement
  if (s.state == StmtState::Allocated) {
    throw DriverError("HY007", "Associated statement is not prepared");
  }
  if (s.state == StmtState::Prepared && !s.result) {
    if (!s.describedOutputValid) impl::ensureDescribedOutput(s);
    if (s.describedOutputValid && s.describedOutputIsResultSet && d.records.empty()) s.rebuildIrd(s.describedColumns);
  }
}

bool isVerboseType(SQLSMALLINT t) { return t == SQL_DATETIME || t == SQL_INTERVAL; }

/// Type identifiers acceptable for a record of this descriptor kind (HY021 otherwise).
void validateTypeFor(const Descriptor& d, SQLSMALLINT concise) {
  if (d.isApplication()) {
    if (!isValidCType(concise) && concise != SQL_C_DEFAULT) {
      throw DriverError(sqlstate::kInconsistentDescriptor,
                        "Inconsistent descriptor information: " + std::to_string(concise) + " is not a valid C data type");
    }
  } else if (!isValidSqlType(concise)) {
    throw DriverError(sqlstate::kInconsistentDescriptor,
                      "Inconsistent descriptor information: " + std::to_string(concise) + " is not a valid SQL data type");
  }
}

/// Consistency check an application triggers by setting SQL_DESC_DATA_PTR on an IPD record: the SQL
/// type must be valid and precision / scale must be usable for it (HY021 otherwise).
void checkIpdRecordConsistency(const Descriptor& d, const DescRecord& r) {
  validateTypeFor(d, r.conciseType);
  auto inconsistent = [](const std::string& what) {
    throw DriverError(sqlstate::kInconsistentDescriptor, "Inconsistent descriptor information: " + what);
  };
  if (r.precision < 0) inconsistent("SQL_DESC_PRECISION must not be negative");
  if (r.scale < 0) inconsistent("SQL_DESC_SCALE must not be negative");
  if (r.conciseType == SQL_DECIMAL || r.conciseType == SQL_NUMERIC) {
    // Trino decimals hold at most 38 digits; a precision of 0 means "not set" (driver default).
    if (r.precision > 38) inconsistent("SQL_DESC_PRECISION " + std::to_string(r.precision) + " exceeds 38");
    if (r.precision > 0 && r.scale > r.precision) {
      inconsistent("SQL_DESC_SCALE " + std::to_string(r.scale) + " exceeds SQL_DESC_PRECISION " +
                   std::to_string(r.precision));
    }
  } else if (r.conciseType == SQL_TYPE_TIME || r.conciseType == SQL_TYPE_TIMESTAMP) {
    // Seconds precision (fractional digits); Trino supports up to picoseconds.
    if (r.precision > 12) inconsistent("SQL_DESC_PRECISION " + std::to_string(r.precision) + " exceeds 12");
  }
}

/// Keeps SQL_DESC_TYPE / SQL_DESC_CONCISE_TYPE / SQL_DESC_DATETIME_INTERVAL_CODE consistent when the
/// concise type changes.
void setConciseType(Descriptor& d, DescRecord& r, SQLSMALLINT concise) {
  if (d.isApplication()) concise = normalizeCType(concise);
  validateTypeFor(d, concise);
  r.conciseType = concise;
  r.type = verboseTypeOf(concise);
  r.datetimeIntervalCode = datetimeSubcodeOf(concise);
}

/// SQL_DESC_TYPE: a verbose datetime/interval type needs the interval code before the concise type
/// is known; any other value is its own concise type.
void setVerboseType(Descriptor& d, DescRecord& r, SQLSMALLINT type) {
  if (isVerboseType(type)) {
    r.type = type;
    r.conciseType = r.datetimeIntervalCode ? conciseTypeOf(type, r.datetimeIntervalCode) : type;
    if (r.conciseType != type) validateTypeFor(d, r.conciseType);
    return;
  }
  setConciseType(d, r, type);
}

void setIntervalCode(Descriptor& d, DescRecord& r, SQLSMALLINT code) {
  r.datetimeIntervalCode = code;
  if (isVerboseType(r.type)) {
    const SQLSMALLINT concise = conciseTypeOf(r.type, code);
    if (concise == 0 || concise == r.type) {
      throw DriverError(sqlstate::kInconsistentDescriptor,
                        "Inconsistent descriptor information: interval/datetime code " + std::to_string(code) +
                            " does not match SQL_DESC_TYPE " + std::to_string(r.type));
    }
    validateTypeFor(d, concise);
    r.conciseType = concise;
  }
}

/// Copies every field except SQL_DESC_ALLOC_TYPE (ODBC: never copied) from `src` into `dst`.
void copyDescriptor(const Descriptor& src, Descriptor& dst) {
  dst.header.arraySize = src.header.arraySize;
  dst.header.arrayStatusPtr = src.header.arrayStatusPtr;
  dst.header.bindOffsetPtr = src.header.bindOffsetPtr;
  dst.header.bindType = src.header.bindType;
  dst.header.rowsProcessedPtr = src.header.rowsProcessedPtr;
  dst.records = src.records;
}

}  // namespace

namespace impl {

SQLRETURN getDescField(Descriptor& d, SQLSMALLINT rec, SQLSMALLINT field, SQLPOINTER value, SQLINTEGER bufLen,
                       SQLINTEGER* strLen, bool wide) {
  // Header fields (RecNumber ignored).
  switch (field) {
    case SQL_DESC_ALLOC_TYPE: return putFixed<SQLSMALLINT>(value, strLen, d.header.allocType);
    case SQL_DESC_ARRAY_SIZE: return putFixed<SQLULEN>(value, strLen, d.header.arraySize);
    case SQL_DESC_ARRAY_STATUS_PTR: return putFixed<SQLUSMALLINT*>(value, strLen, d.header.arrayStatusPtr);
    case SQL_DESC_BIND_OFFSET_PTR: return putFixed<SQLLEN*>(value, strLen, d.header.bindOffsetPtr);
    case SQL_DESC_BIND_TYPE: return putFixed<SQLINTEGER>(value, strLen, d.header.bindType);
    case SQL_DESC_COUNT:
      if (d.kind == DescKind::Ird && d.implicitOwner && d.implicitOwner->state != StmtState::Allocated) ensureIrdReady(d);
      return putFixed<SQLSMALLINT>(value, strLen, d.count());
    case SQL_DESC_ROWS_PROCESSED_PTR: return putFixed<SQLULEN*>(value, strLen, d.header.rowsProcessedPtr);
    default:
      break;
  }

  // Record fields.
  if (rec < 1) invalidIndex(rec);
  ensureIrdReady(d);
  const DescRecord* r = d.recordIf(rec);
  if (!r) return SQL_NO_DATA;
  switch (field) {
    case SQL_DESC_AUTO_UNIQUE_VALUE: return putFixed<SQLINTEGER>(value, strLen, r->autoUniqueValue);
    case SQL_DESC_BASE_COLUMN_NAME: return putText(d, value, bufLen, strLen, wide, r->baseColumnName);
    case SQL_DESC_BASE_TABLE_NAME: return putText(d, value, bufLen, strLen, wide, r->baseTableName);
    case SQL_DESC_CASE_SENSITIVE: return putFixed<SQLINTEGER>(value, strLen, r->caseSensitive);
    case SQL_DESC_CATALOG_NAME: return putText(d, value, bufLen, strLen, wide, r->catalogName);
    case SQL_DESC_CONCISE_TYPE: return putFixed<SQLSMALLINT>(value, strLen, r->conciseType);
    case SQL_DESC_DATA_PTR: return putFixed<SQLPOINTER>(value, strLen, r->dataPtr);
    case SQL_DESC_DATETIME_INTERVAL_CODE: return putFixed<SQLSMALLINT>(value, strLen, r->datetimeIntervalCode);
    case SQL_DESC_DATETIME_INTERVAL_PRECISION: return putFixed<SQLINTEGER>(value, strLen, r->datetimeIntervalPrecision);
    case SQL_DESC_DISPLAY_SIZE: return putFixed<SQLLEN>(value, strLen, r->displaySize);
    case SQL_DESC_FIXED_PREC_SCALE: return putFixed<SQLSMALLINT>(value, strLen, r->fixedPrecScale);
    case SQL_DESC_INDICATOR_PTR: return putFixed<SQLLEN*>(value, strLen, r->indicatorPtr);
    case SQL_DESC_LABEL: return putText(d, value, bufLen, strLen, wide, r->label);
    case SQL_DESC_LENGTH: return putFixed<SQLULEN>(value, strLen, static_cast<SQLULEN>(r->length));
    case SQL_DESC_LITERAL_PREFIX: return putText(d, value, bufLen, strLen, wide, r->literalPrefix);
    case SQL_DESC_LITERAL_SUFFIX: return putText(d, value, bufLen, strLen, wide, r->literalSuffix);
    case SQL_DESC_LOCAL_TYPE_NAME: return putText(d, value, bufLen, strLen, wide, r->localTypeName);
    case SQL_DESC_NAME: return putText(d, value, bufLen, strLen, wide, r->name);
    case SQL_DESC_NULLABLE: return putFixed<SQLSMALLINT>(value, strLen, r->nullable);
    case SQL_DESC_NUM_PREC_RADIX: return putFixed<SQLINTEGER>(value, strLen, r->numPrecRadix);
    case SQL_DESC_OCTET_LENGTH: return putFixed<SQLLEN>(value, strLen, r->octetLength);
    case SQL_DESC_OCTET_LENGTH_PTR: return putFixed<SQLLEN*>(value, strLen, r->octetLengthPtr);
    case SQL_DESC_PARAMETER_TYPE: return putFixed<SQLSMALLINT>(value, strLen, r->parameterType);
    case SQL_DESC_PRECISION: return putFixed<SQLSMALLINT>(value, strLen, r->precision);
    case SQL_DESC_ROWVER: return putFixed<SQLSMALLINT>(value, strLen, static_cast<SQLSMALLINT>(SQL_FALSE));
    case SQL_DESC_SCALE: return putFixed<SQLSMALLINT>(value, strLen, r->scale);
    case SQL_DESC_SCHEMA_NAME: return putText(d, value, bufLen, strLen, wide, r->schemaName);
    case SQL_DESC_SEARCHABLE: return putFixed<SQLSMALLINT>(value, strLen, r->searchable);
    case SQL_DESC_TABLE_NAME: return putText(d, value, bufLen, strLen, wide, r->tableName);
    case SQL_DESC_TYPE: return putFixed<SQLSMALLINT>(value, strLen, r->type);
    case SQL_DESC_TYPE_NAME: return putText(d, value, bufLen, strLen, wide, r->typeName);
    case SQL_DESC_UNNAMED: return putFixed<SQLSMALLINT>(value, strLen, r->unnamed);
    case SQL_DESC_UNSIGNED: return putFixed<SQLSMALLINT>(value, strLen, r->unsignedAttr);
    case SQL_DESC_UPDATABLE: return putFixed<SQLSMALLINT>(value, strLen, r->updatable);
    default:
      invalidField(field);
  }
}

SQLRETURN setDescField(Descriptor& d, SQLSMALLINT rec, SQLSMALLINT field, SQLPOINTER value, SQLINTEGER bufLen, bool wide) {
  requireOwnerNotBusy(d);
  const bool ird = d.kind == DescKind::Ird;

  // Header fields.
  switch (field) {
    case SQL_DESC_ALLOC_TYPE:
      readOnlyField(field);
    case SQL_DESC_ARRAY_SIZE: {
      if (ird) cannotModifyIrd();
      const SQLULEN n = ulenOf(value);
      if (n == 0) throw DriverError(sqlstate::kInvalidAttributeValue, "SQL_DESC_ARRAY_SIZE must be at least 1");
      d.header.arraySize = n;
      return SQL_SUCCESS;
    }
    case SQL_DESC_ARRAY_STATUS_PTR:
      d.header.arrayStatusPtr = static_cast<SQLUSMALLINT*>(value);
      return SQL_SUCCESS;
    case SQL_DESC_BIND_OFFSET_PTR:
      if (ird) cannotModifyIrd();
      d.header.bindOffsetPtr = static_cast<SQLLEN*>(value);
      return SQL_SUCCESS;
    case SQL_DESC_BIND_TYPE:
      if (ird) cannotModifyIrd();
      d.header.bindType = intOf(value);
      return SQL_SUCCESS;
    case SQL_DESC_COUNT: {
      if (ird) cannotModifyIrd();
      const SQLSMALLINT n = smallOf(value);
      if (n < 0) throw DriverError(sqlstate::kInvalidAttributeValue, "SQL_DESC_COUNT must not be negative");
      d.setCount(n);
      return SQL_SUCCESS;
    }
    case SQL_DESC_ROWS_PROCESSED_PTR:
      d.header.rowsProcessedPtr = static_cast<SQLULEN*>(value);
      return SQL_SUCCESS;
    default:
      break;
  }

  // Record fields.
  if (ird) cannotModifyIrd();
  if (rec < 1) invalidIndex(rec);
  DescRecord& r = d.record(rec);  // grows SQL_DESC_COUNT to `rec` as the spec requires
  switch (field) {
    case SQL_DESC_CONCISE_TYPE:
      setConciseType(d, r, smallOf(value));
      return SQL_SUCCESS;
    case SQL_DESC_TYPE:
      setVerboseType(d, r, smallOf(value));
      return SQL_SUCCESS;
    case SQL_DESC_DATETIME_INTERVAL_CODE:
      setIntervalCode(d, r, smallOf(value));
      return SQL_SUCCESS;
    case SQL_DESC_DATETIME_INTERVAL_PRECISION:
      r.datetimeIntervalPrecision = intOf(value);
      return SQL_SUCCESS;
    case SQL_DESC_DATA_PTR:
      if (d.kind == DescKind::Ipd) {
        // ODBC: setting SQL_DESC_DATA_PTR of an IPD only forces the consistency check of the record;
        // the pointer itself is not stored.
        checkIpdRecordConsistency(d, r);
        return SQL_SUCCESS;
      }
      if (!d.isApplication()) readOnlyField(field);
      // Setting the data pointer triggers the consistency check of the record (ODBC).
      if (value != nullptr) validateTypeFor(d, r.conciseType);
      r.dataPtr = value;
      r.bound = r.dataPtr != nullptr || r.indicatorPtr != nullptr || r.octetLengthPtr != nullptr;
      return SQL_SUCCESS;
    case SQL_DESC_INDICATOR_PTR:
      if (!d.isApplication()) readOnlyField(field);
      r.indicatorPtr = static_cast<SQLLEN*>(value);
      r.bound = r.dataPtr != nullptr || r.indicatorPtr != nullptr || r.octetLengthPtr != nullptr;
      return SQL_SUCCESS;
    case SQL_DESC_OCTET_LENGTH_PTR:
      if (!d.isApplication()) readOnlyField(field);
      r.octetLengthPtr = static_cast<SQLLEN*>(value);
      r.bound = r.dataPtr != nullptr || r.indicatorPtr != nullptr || r.octetLengthPtr != nullptr;
      return SQL_SUCCESS;
    case SQL_DESC_LENGTH:
      r.length = static_cast<SQLLEN>(ulenOf(value));
      if (!d.isApplication()) r.boundColumnSize = ulenOf(value);
      return SQL_SUCCESS;
    case SQL_DESC_OCTET_LENGTH:
      r.octetLength = lenOf(value);
      return SQL_SUCCESS;
    case SQL_DESC_NAME: {
      if (d.kind != DescKind::Ipd) readOnlyField(field);
      const AnsiEncoding enc = d.conn ? ansiEncodingOf(*d.conn) : defaultAnsiEncoding();
      std::optional<std::string> name = textInBytes(value, bufLen, wide, enc);
      r.name = name.value_or(std::string());
      r.unnamed = r.name.empty() ? SQL_UNNAMED : SQL_NAMED;
      return SQL_SUCCESS;
    }
    case SQL_DESC_NUM_PREC_RADIX:
      r.numPrecRadix = intOf(value);
      return SQL_SUCCESS;
    case SQL_DESC_PARAMETER_TYPE: {
      if (d.kind != DescKind::Ipd) readOnlyField(field);
      const SQLSMALLINT t = smallOf(value);
      if (t == SQL_PARAM_INPUT) {
        r.parameterType = t;
        return SQL_SUCCESS;
      }
      if (t == SQL_PARAM_OUTPUT || t == SQL_PARAM_INPUT_OUTPUT) {
        throw DriverError(sqlstate::kOptionalFeatureNotImplemented,
                          "Optional feature not implemented: output parameters are not supported");
      }
      throw DriverError(sqlstate::kInvalidParameterType, "Invalid parameter type " + std::to_string(t));
    }
    case SQL_DESC_PRECISION:
      r.precision = smallOf(value);
      if (d.isApplication()) r.precisionSet = true;
      else r.boundColumnSize = static_cast<SQLULEN>(r.precision < 0 ? 0 : r.precision);
      return SQL_SUCCESS;
    case SQL_DESC_SCALE:
      r.scale = smallOf(value);
      if (d.isApplication()) r.scaleSet = true;
      else r.boundDecimalDigits = r.scale;
      return SQL_SUCCESS;
    case SQL_DESC_UNNAMED: {
      if (d.kind != DescKind::Ipd) readOnlyField(field);
      const SQLSMALLINT v = smallOf(value);
      if (v != SQL_UNNAMED) {
        throw DriverError(sqlstate::kInvalidAttribute, "SQL_DESC_UNNAMED can only be set to SQL_UNNAMED");
      }
      r.unnamed = SQL_UNNAMED;
      r.name.clear();
      return SQL_SUCCESS;
    }
    case SQL_DESC_AUTO_UNIQUE_VALUE:
    case SQL_DESC_BASE_COLUMN_NAME:
    case SQL_DESC_BASE_TABLE_NAME:
    case SQL_DESC_CASE_SENSITIVE:
    case SQL_DESC_CATALOG_NAME:
    case SQL_DESC_DISPLAY_SIZE:
    case SQL_DESC_FIXED_PREC_SCALE:
    case SQL_DESC_LABEL:
    case SQL_DESC_LITERAL_PREFIX:
    case SQL_DESC_LITERAL_SUFFIX:
    case SQL_DESC_LOCAL_TYPE_NAME:
    case SQL_DESC_NULLABLE:
    case SQL_DESC_ROWVER:
    case SQL_DESC_SCHEMA_NAME:
    case SQL_DESC_SEARCHABLE:
    case SQL_DESC_TABLE_NAME:
    case SQL_DESC_TYPE_NAME:
    case SQL_DESC_UNSIGNED:
    case SQL_DESC_UPDATABLE:
      readOnlyField(field);
    default:
      invalidField(field);
  }
}

SQLRETURN getDescRec(Descriptor& d, SQLSMALLINT rec, TextOut& name, SQLSMALLINT* type, SQLSMALLINT* subType, SQLLEN* length,
                     SQLSMALLINT* precision, SQLSMALLINT* scale, SQLSMALLINT* nullable) {
  if (rec < 1) invalidIndex(rec);
  ensureIrdReady(d);
  const DescRecord* r = d.recordIf(rec);
  if (!r) return SQL_NO_DATA;
  if (type) *type = r->type;
  if (subType) *subType = r->datetimeIntervalCode;
  if (length) *length = r->octetLength;
  if (precision) *precision = r->precision;
  if (scale) *scale = r->scale;
  if (nullable) *nullable = r->nullable;
  return name.writeWithDiag(r->name, d.diag);
}

SQLRETURN setDescRec(Descriptor& d, SQLSMALLINT rec, SQLSMALLINT type, SQLSMALLINT subType, SQLLEN length,
                     SQLSMALLINT precision, SQLSMALLINT scale, SQLPOINTER data, SQLLEN* stringLength, SQLLEN* indicator) {
  requireOwnerNotBusy(d);
  if (d.kind == DescKind::Ird) cannotModifyIrd();
  if (rec < 1) invalidIndex(rec);
  DescRecord& r = d.record(rec);
  // Type fields first so the record is consistent before pointers are attached.
  if (isVerboseType(type)) {
    r.type = type;
    setIntervalCode(d, r, subType);
  } else {
    setConciseType(d, r, type);
    if (subType != 0 && subType != r.datetimeIntervalCode) {
      throw DriverError(sqlstate::kInconsistentDescriptor,
                        "Inconsistent descriptor information: SubType " + std::to_string(subType) +
                            " does not match Type " + std::to_string(type));
    }
  }
  r.octetLength = length;  // SQLSetDescRec's Length argument sets SQL_DESC_OCTET_LENGTH (ODBC)
  r.precision = precision;
  r.scale = scale;
  if (d.isApplication()) {
    r.precisionSet = true;
    r.scaleSet = true;
    r.dataPtr = data;
    r.octetLengthPtr = stringLength;
    r.indicatorPtr = indicator;
    r.bound = data != nullptr || stringLength != nullptr || indicator != nullptr;
  } else {
    // IPD: the pointer arguments are ignored (there is nothing to bind on an implementation
    // descriptor); precision/scale describe the parameter.
    r.boundColumnSize = static_cast<SQLULEN>(precision < 0 ? 0 : precision);
    r.boundDecimalDigits = scale;
  }
  return SQL_SUCCESS;
}

}  // namespace impl
}  // namespace trino_odbc

using namespace trino_odbc;

extern "C" {

TRINO_ODBC_API SQLRETURN SQL_API SQLCopyDesc(SQLHDESC SourceDescHandle, SQLHDESC TargetDescHandle) {
  // Diagnostics go to the target: it is the handle being modified.
  return guardDesc(TargetDescHandle, "SQLCopyDesc", [&](Descriptor& target) -> SQLRETURN {
    Descriptor* src = asDescriptor(SourceDescHandle);
    if (!src) return SQL_INVALID_HANDLE;
    if (src == &target) return SQL_SUCCESS;
    if (target.kind == DescKind::Ird) cannotModifyIrd();
    requireOwnerNotBusy(target);

    // The guard holds the target's connection mutex; a source on another connection needs its own.
    // A bounded try-lock avoids a lock-order deadlock with a concurrent copy in the other direction.
    std::unique_lock<std::recursive_mutex> srcLock;
    if (src->conn && src->conn != target.conn) {
      srcLock = std::unique_lock<std::recursive_mutex>(src->conn->mutex, std::defer_lock);
      for (int attempt = 0; !srcLock.try_lock(); ++attempt) {
        if (attempt >= 2000) {
          throw DriverError(sqlstate::kGeneralError, "SQLCopyDesc: the source descriptor's connection is busy");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    if (src->kind == DescKind::Ird && src->implicitOwner) {
      if (src->implicitOwner->state == StmtState::Allocated) {
        throw DriverError("HY007", "Associated statement is not prepared: the source IRD has no result description");
      }
      ensureIrdReady(*src);
    }
    copyDescriptor(*src, target);
    TRINO_LOG_TRACE(kLog, "SQLCopyDesc copied %d record(s) from %p to %p", src->count(), static_cast<void*>(src),
                    static_cast<void*>(&target));
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetDescField(SQLHDESC DescriptorHandle, SQLSMALLINT RecNumber, SQLSMALLINT FieldIdentifier,
                                                 SQLPOINTER Value, SQLINTEGER BufferLength, SQLINTEGER* StringLength) {
  return guardDesc(DescriptorHandle, "SQLGetDescField", [&](Descriptor& d) -> SQLRETURN {
    return impl::getDescField(d, RecNumber, FieldIdentifier, Value, BufferLength, StringLength, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetDescField(SQLHDESC DescriptorHandle, SQLSMALLINT RecNumber, SQLSMALLINT FieldIdentifier,
                                                 SQLPOINTER Value, SQLINTEGER BufferLength) {
  return guardDesc(DescriptorHandle, "SQLSetDescField", [&](Descriptor& d) -> SQLRETURN {
    return impl::setDescField(d, RecNumber, FieldIdentifier, Value, BufferLength, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetDescRec(SQLHDESC DescriptorHandle, SQLSMALLINT RecNumber, SQLCHAR* Name,
                                               SQLSMALLINT BufferLength, SQLSMALLINT* StringLength, SQLSMALLINT* Type,
                                               SQLSMALLINT* SubType, SQLLEN* Length, SQLSMALLINT* Precision,
                                               SQLSMALLINT* Scale, SQLSMALLINT* Nullable) {
  return guardDesc(DescriptorHandle, "SQLGetDescRec", [&](Descriptor& d) -> SQLRETURN {
    if (BufferLength < 0) throw DriverError(sqlstate::kInvalidStringOrBufferLength, "BufferLength must not be negative");
    const AnsiEncoding enc = d.conn ? ansiEncodingOf(*d.conn) : defaultAnsiEncoding();
    TextOut name(Name, BufferLength, StringLength, enc);
    return impl::getDescRec(d, RecNumber, name, Type, SubType, Length, Precision, Scale, Nullable);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetDescRec(SQLHDESC DescriptorHandle, SQLSMALLINT RecNumber, SQLSMALLINT Type,
                                               SQLSMALLINT SubType, SQLLEN Length, SQLSMALLINT Precision, SQLSMALLINT Scale,
                                               SQLPOINTER Data, SQLLEN* StringLength, SQLLEN* Indicator) {
  return guardDesc(DescriptorHandle, "SQLSetDescRec", [&](Descriptor& d) -> SQLRETURN {
    return impl::setDescRec(d, RecNumber, Type, SubType, Length, Precision, Scale, Data, StringLength, Indicator);
  });
}

}  // extern "C"
