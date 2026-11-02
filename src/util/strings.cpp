// String helpers shared by every module. Everything here is ASCII-oriented on purpose: it operates on
// SQL identifiers, connection-string keys, header values and codecs, never on user-visible text.
#include "util/strings.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <locale>
#include <sstream>

namespace trino_odbc {

namespace {

inline bool isAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }
inline char asciiUpper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c; }

/// RFC 3986 unreserved characters are the only ones that survive urlEncode unchanged.
inline bool isUnreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

inline int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

constexpr char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Decodes one base64 symbol; accepts the URL-safe alphabet too so tokens from either encoder work.
inline int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

}  // namespace

std::string_view trim(std::string_view s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && isAsciiSpace(s[b])) ++b;
  while (e > b && isAsciiSpace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::string toLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = asciiLower(c);
  return out;
}

std::string toUpper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = asciiUpper(c);
  return out;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (asciiLower(a[i]) != asciiLower(b[i])) return false;
  }
  return true;
}

bool startsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool istartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool iendsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

std::vector<std::string> split(std::string_view s, char sep, bool trimParts, bool skipEmpty) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t pos = s.find(sep, start);
    std::string_view part = s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
    if (trimParts) part = trim(part);
    if (!(skipEmpty && part.empty())) parts.emplace_back(part);
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return parts;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  size_t total = 0;
  for (const auto& p : parts) total += p.size() + sep.size();
  std::string out;
  out.reserve(total);
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out.append(sep);
    out.append(parts[i]);
  }
  return out;
}

std::string replaceAll(std::string_view s, std::string_view from, std::string_view to) {
  if (from.empty()) return std::string(s);
  std::string out;
  out.reserve(s.size());
  size_t start = 0;
  while (true) {
    const size_t pos = s.find(from, start);
    if (pos == std::string_view::npos) {
      out.append(s.substr(start));
      break;
    }
    out.append(s.substr(start, pos - start));
    out.append(to);
    start = pos + from.size();
  }
  return out;
}

bool parseBool(std::string_view s, bool* out) {
  const std::string_view t = trim(s);
  static constexpr std::string_view kTrue[] = {"1", "true", "yes", "on", "y", "t"};
  static constexpr std::string_view kFalse[] = {"0", "false", "no", "off", "n", "f"};
  for (const auto& v : kTrue) {
    if (iequals(t, v)) {
      if (out) *out = true;
      return true;
    }
  }
  for (const auto& v : kFalse) {
    if (iequals(t, v)) {
      if (out) *out = false;
      return true;
    }
  }
  return false;
}

bool parseInt64(std::string_view s, int64_t* out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool negative = false;
  if (s[0] == '+' || s[0] == '-') {
    negative = s[0] == '-';
    i = 1;
  }
  if (i >= s.size()) return false;
  // Accumulate as a negative magnitude so INT64_MIN is representable without overflow tricks.
  int64_t acc = 0;
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return false;
    const int d = c - '0';
    if (acc < (kMin + d) / 10) return false;  // would overflow
    acc = acc * 10 - d;
  }
  if (!negative) {
    if (acc == kMin) return false;
    acc = -acc;
  }
  if (out) *out = acc;
  return true;
}

bool parseDouble(std::string_view s, double* out) {
  const std::string_view t = trim(s);
  if (t.empty()) return false;
  // Trino spells the specials "NaN", "Infinity", "-Infinity"; std::num_get does not parse them.
  std::string_view mag = t;
  bool negative = false;
  if (mag[0] == '+' || mag[0] == '-') {
    negative = mag[0] == '-';
    mag.remove_prefix(1);
  }
  if (iequals(mag, "nan")) {
    if (out) *out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  if (iequals(mag, "inf") || iequals(mag, "infinity")) {
    if (out) *out = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    return true;
  }
  // Validate the shape ourselves so hex floats / locale quirks cannot sneak through.
  size_t i = 0;
  size_t digits = 0;
  while (i < mag.size() && mag[i] >= '0' && mag[i] <= '9') { ++i; ++digits; }
  if (i < mag.size() && mag[i] == '.') {
    ++i;
    while (i < mag.size() && mag[i] >= '0' && mag[i] <= '9') { ++i; ++digits; }
  }
  if (digits == 0) return false;
  if (i < mag.size() && (mag[i] == 'e' || mag[i] == 'E')) {
    ++i;
    if (i < mag.size() && (mag[i] == '+' || mag[i] == '-')) ++i;
    size_t expDigits = 0;
    while (i < mag.size() && mag[i] >= '0' && mag[i] <= '9') { ++i; ++expDigits; }
    if (expDigits == 0) return false;
  }
  if (i != mag.size()) return false;
  // The classic locale keeps '.' as the decimal point even when the host application changed LC_NUMERIC
  // (Excel / Power BI do that), which is why strtod is not used here.
  std::istringstream in{std::string(t)};
  in.imbue(std::locale::classic());
  double v = 0;
  in >> v;
  if (in.fail()) {
    // Overflow: num_get reports failure while strtod would give +-inf. Reproduce strtod's answer.
    if (!in.bad()) {
      v = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
      if (out) *out = v;
      return true;
    }
    return false;
  }
  if (out) *out = v;
  return true;
}

std::string formatString(const char* fmt, ...) {
  if (!fmt) return std::string();
  va_list args;
  va_start(args, fmt);
  va_list probe;
  va_copy(probe, args);
  char stackBuf[512];
  const int n = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, probe);
  va_end(probe);
  std::string out;
  if (n < 0) {
    va_end(args);
    return out;
  }
  if (static_cast<size_t>(n) < sizeof(stackBuf)) {
    out.assign(stackBuf, static_cast<size_t>(n));
  } else {
    out.resize(static_cast<size_t>(n));
    std::vsnprintf(&out[0], static_cast<size_t>(n) + 1, fmt, args);
  }
  va_end(args);
  return out;
}

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i) {
    const unsigned char ca = static_cast<unsigned char>(asciiLower(a[i]));
    const unsigned char cb = static_cast<unsigned char>(asciiLower(b[i]));
    if (ca != cb) return ca < cb;
  }
  return a.size() < b.size();
}

std::string urlEncode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() + s.size() / 4);
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (isUnreserved(c)) {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string urlDecode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < s.size()) {
      const int hi = hexValue(s[i + 1]);
      const int lo = hexValue(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
      } else {
        out.push_back(c);  // malformed escape: keep literally, never drop data
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string base64Encode(std::string_view bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                       (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                       static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
    out.push_back(kBase64Alphabet[(v >> 18) & 63]);
    out.push_back(kBase64Alphabet[(v >> 12) & 63]);
    out.push_back(kBase64Alphabet[(v >> 6) & 63]);
    out.push_back(kBase64Alphabet[v & 63]);
    i += 3;
  }
  const size_t rest = bytes.size() - i;
  if (rest == 1) {
    const uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
    out.push_back(kBase64Alphabet[(v >> 18) & 63]);
    out.push_back(kBase64Alphabet[(v >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (rest == 2) {
    const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                       (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
    out.push_back(kBase64Alphabet[(v >> 18) & 63]);
    out.push_back(kBase64Alphabet[(v >> 12) & 63]);
    out.push_back(kBase64Alphabet[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

bool base64Decode(std::string_view text, std::string& out) {
  out.clear();
  out.reserve((text.size() / 4) * 3 + 3);
  uint32_t acc = 0;
  int bits = 0;
  bool paddingSeen = false;
  for (const char c : text) {
    if (isAsciiSpace(c)) continue;  // tolerate wrapped / indented input
    if (c == '=') {
      paddingSeen = true;
      continue;
    }
    if (paddingSeen) return false;  // data after padding is malformed
    const int v = base64Value(c);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  // A single leftover symbol carries only 6 bits: no byte can be formed, the input was cut mid-byte.
  if (bits >= 6) return false;
  // Non-zero leftover bits mean the final symbol encodes bits that no byte owns (non-canonical input);
  // tolerated, as most decoders do, because the bytes produced are still exactly what the encoder wrote.
  return true;
}

std::string hexEncode(std::string_view bytes, bool upper) {
  const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const char ch : bytes) {
    const unsigned char c = static_cast<unsigned char>(ch);
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 0x0F]);
  }
  return out;
}

std::string sqlQuoteLiteral(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (const char c : s) {
    if (c == '\'') out.push_back('\'');
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

std::string sqlQuoteIdentifier(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const char c : s) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

}  // namespace trino_odbc
