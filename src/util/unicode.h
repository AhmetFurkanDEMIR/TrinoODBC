#pragma once
// UTF-8 <-> SQLWCHAR (UTF-16 or UTF-32 depending on the driver manager) <-> narrow/ANSI conversions,
// plus the ODBC "copy into application buffer" helpers. All internal strings are UTF-8.
#include "platform.h"

#include <string>
#include <string_view>

namespace trino_odbc {

using WString = std::basic_string<SQLWCHAR>;

/// Encoding used for SQLCHAR data (narrow API strings and SQL_C_CHAR column data).
enum class AnsiEncoding { Utf8, System };

/// System code page on Windows, UTF-8 elsewhere.
AnsiEncoding defaultAnsiEncoding();

// ---- Wide (SQLWCHAR) <-> UTF-8 ------------------------------------------------------------
/// `lengthUnits` is a count of SQLWCHAR units or SQL_NTS. Invalid sequences become U+FFFD.
std::string wideToUtf8(const SQLWCHAR* s, SQLLEN lengthUnits);
inline std::string wideToUtf8(const SQLWCHAR* s, SQLSMALLINT lengthUnits) {
  return wideToUtf8(s, static_cast<SQLLEN>(lengthUnits));
}
inline std::string wideToUtf8(const SQLWCHAR* s, SQLINTEGER lengthUnits) {
  return wideToUtf8(s, static_cast<SQLLEN>(lengthUnits));
}
std::string wideToUtf8(const WString& s);
WString utf8ToWide(std::string_view utf8);
/// SQLWCHAR units needed to encode `utf8` (no terminator).
size_t wideUnitsFor(std::string_view utf8);
/// Transcodes as many *complete* code points as fit into out[0..capUnits) without splitting a
/// surrogate pair. Returns units written; *consumedBytes receives the UTF-8 bytes consumed.
/// No terminator is written. `out` may be null when capUnits == 0.
size_t utf8ToWideChunk(std::string_view utf8, SQLWCHAR* out, size_t capUnits, size_t* consumedBytes);
/// Units until NUL.
size_t wideStrLen(const SQLWCHAR* s);

// ---- Narrow (SQLCHAR) <-> UTF-8 -----------------------------------------------------------
/// `lengthBytes` is a byte count or SQL_NTS.
std::string narrowToUtf8(const SQLCHAR* s, SQLLEN lengthBytes, AnsiEncoding enc);
inline std::string narrowToUtf8(const SQLCHAR* s, SQLSMALLINT lengthBytes, AnsiEncoding enc) {
  return narrowToUtf8(s, static_cast<SQLLEN>(lengthBytes), enc);
}
inline std::string narrowToUtf8(const SQLCHAR* s, SQLINTEGER lengthBytes, AnsiEncoding enc) {
  return narrowToUtf8(s, static_cast<SQLLEN>(lengthBytes), enc);
}
std::string utf8ToNarrow(std::string_view utf8, AnsiEncoding enc);

// ---- UTF-8 helpers ---------------------------------------------------------------------------
/// Largest byte count <= maxBytes at which `utf8` can be cut without splitting a code point.
size_t utf8SafeCut(std::string_view utf8, size_t maxBytes);
size_t utf8CodePoints(std::string_view utf8);
bool utf8IsValid(std::string_view utf8);
/// Decodes one code point at `pos`; advances pos. Returns U+FFFD on malformed input.
uint32_t utf8Next(std::string_view s, size_t& pos);
/// Appends the UTF-8 encoding of `cp`.
void utf8Append(std::string& out, uint32_t cp);

// ---- ODBC copy-out helpers -------------------------------------------------------------------
// Semantics shared by all: if buf is null nothing is copied but the length is still reported; the
// output is always NUL-terminated when the buffer has room for at least the terminator; a length
// pointer may be null; the return value is true when truncation happened (caller adds 01004).
bool copyNarrowOutImpl(std::string_view utf8, SQLCHAR* buf, SQLLEN bufBytes, SQLLEN* outBytes, AnsiEncoding enc);
bool copyWideOutUnitsImpl(std::string_view utf8, SQLWCHAR* buf, SQLLEN bufUnits, SQLLEN* outUnits);

/// Narrow buffer; bufBytes / *outBytes in bytes (excludes terminator).
template <typename LenT>
bool copyNarrowOut(std::string_view utf8, SQLCHAR* buf, SQLLEN bufBytes, LenT* outBytes, AnsiEncoding enc) {
  SQLLEN n = 0;
  const bool truncated = copyNarrowOutImpl(utf8, buf, bufBytes, &n, enc);
  if (outBytes) *outBytes = static_cast<LenT>(n);
  return truncated;
}
/// Wide buffer where lengths are in SQLWCHAR units (SQLGetDiagRecW, SQLDescribeColW, SQLGetCursorNameW ...).
template <typename LenT>
bool copyWideOutChars(std::string_view utf8, SQLWCHAR* buf, SQLLEN bufUnits, LenT* outUnits) {
  SQLLEN n = 0;
  const bool truncated = copyWideOutUnitsImpl(utf8, buf, bufUnits, &n);
  if (outUnits) *outUnits = static_cast<LenT>(n);
  return truncated;
}
/// Wide buffer where lengths are in BYTES (SQLGetInfoW, SQLColAttributeW, SQLGetConnectAttrW ...).
template <typename LenT>
bool copyWideOutBytes(std::string_view utf8, SQLWCHAR* buf, SQLLEN bufBytes, LenT* outBytes) {
  SQLLEN n = 0;
  const SQLLEN units = bufBytes < 0 ? bufBytes : bufBytes / static_cast<SQLLEN>(sizeof(SQLWCHAR));
  const bool truncated = copyWideOutUnitsImpl(utf8, buf, units, &n);
  if (outBytes) *outBytes = static_cast<LenT>(n * static_cast<SQLLEN>(sizeof(SQLWCHAR)));
  return truncated;
}

}  // namespace trino_odbc
