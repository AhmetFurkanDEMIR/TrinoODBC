#pragma once
// Small string utilities shared across the driver (ASCII case handling, splitting, parsing,
// URL/base64/hex codecs, SQL quoting).
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace trino_odbc {

std::string_view trim(std::string_view s);
std::string toLower(std::string_view s);   // ASCII only (SQL identifiers / keys)
std::string toUpper(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool startsWith(std::string_view s, std::string_view prefix);
bool istartsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool iendsWith(std::string_view s, std::string_view suffix);
std::vector<std::string> split(std::string_view s, char sep, bool trimParts = true, bool skipEmpty = true);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replaceAll(std::string_view s, std::string_view from, std::string_view to);

/// Accepts 1/0, true/false, yes/no, on/off, y/n, t/f (case-insensitive). Returns false if unparsable.
bool parseBool(std::string_view s, bool* out);
/// Strict decimal integer with optional sign; no trailing garbage.
bool parseInt64(std::string_view s, int64_t* out);
bool parseDouble(std::string_view s, double* out);
/// printf-style formatting into std::string.
std::string formatString(const char* fmt, ...);

struct CaseInsensitiveLess {
  bool operator()(const std::string& a, const std::string& b) const;
};
using KeyValueMap = std::map<std::string, std::string, CaseInsensitiveLess>;

std::string urlEncode(std::string_view s);       // RFC 3986 unreserved kept, space -> %20
std::string urlDecode(std::string_view s);       // also maps '+' to space
std::string base64Encode(std::string_view bytes);
bool base64Decode(std::string_view text, std::string& out);  // tolerant to missing padding/whitespace
std::string hexEncode(std::string_view bytes, bool upper = true);

/// 'it''s'  -> single-quoted SQL literal with quote doubling.
std::string sqlQuoteLiteral(std::string_view s);
/// "na""me" -> double-quoted SQL identifier with quote doubling.
std::string sqlQuoteIdentifier(std::string_view s);

}  // namespace trino_odbc
