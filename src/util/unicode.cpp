// Hand-written UTF-8 / UTF-16 / UTF-32 transcoding. No std::codecvt (deprecated, slow, and wrong about
// surrogates on some runtimes) and no assumption about sizeof(SQLWCHAR): the driver manager decides
// whether SQLWCHAR carries UTF-16 code units (Windows, unixODBC) or UTF-32 code points (iODBC).
#include "util/unicode.h"

#include <cstring>
#include <type_traits>

namespace trino_odbc {

namespace {

constexpr uint32_t kReplacement = 0xFFFD;
constexpr uint32_t kMaxCodePoint = 0x10FFFF;

// SQLWCHAR may be wchar_t, which is signed on Linux/macOS: always widen through the unsigned twin.
using WideUnit = std::make_unsigned_t<SQLWCHAR>;

inline uint32_t unitValue(SQLWCHAR u) { return static_cast<uint32_t>(static_cast<WideUnit>(u)); }
inline bool isHighSurrogate(uint32_t v) { return v >= 0xD800 && v <= 0xDBFF; }
inline bool isLowSurrogate(uint32_t v) { return v >= 0xDC00 && v <= 0xDFFF; }
inline bool isSurrogate(uint32_t v) { return v >= 0xD800 && v <= 0xDFFF; }
inline bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

/// Number of UTF-16 units a code point needs (UTF-32 always needs one).
inline size_t unitsForCodePoint(uint32_t cp) { return (kWideIsUtf16 && cp >= 0x10000) ? 2 : 1; }

/// Decodes the code point at s[pos] following the Unicode "maximal subpart" rule: a malformed sequence
/// yields one U+FFFD and consumes exactly the bytes that formed a valid prefix (at least one byte).
inline uint32_t decodeUtf8(const char* s, size_t size, size_t& pos) {
  const unsigned char b0 = static_cast<unsigned char>(s[pos]);
  if (b0 < 0x80) {
    ++pos;
    return b0;
  }
  size_t need;
  uint32_t cp;
  unsigned char lo = 0x80, hi = 0xBF;  // bounds for the second byte
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    need = 1;
    cp = b0 & 0x1F;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    need = 2;
    cp = b0 & 0x0F;
    if (b0 == 0xE0) lo = 0xA0;        // reject overlong
    else if (b0 == 0xED) hi = 0x9F;   // reject surrogates
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    need = 3;
    cp = b0 & 0x07;
    if (b0 == 0xF0) lo = 0x90;        // reject overlong
    else if (b0 == 0xF4) hi = 0x8F;   // reject > U+10FFFF
  } else {
    ++pos;  // stray continuation byte, 0xC0/0xC1 or 0xF5..0xFF
    return kReplacement;
  }
  size_t i = pos + 1;
  for (size_t k = 0; k < need; ++k, ++i) {
    if (i >= size) {
      pos = i;
      return kReplacement;
    }
    const unsigned char b = static_cast<unsigned char>(s[i]);
    const bool ok = k == 0 ? (b >= lo && b <= hi) : isContinuation(b);
    if (!ok) {
      pos = i;
      return kReplacement;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  pos = i;
  return cp;
}

inline void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

/// Writes `cp` as one or two SQLWCHAR units; the caller guarantees the room.
inline size_t putWide(SQLWCHAR* out, uint32_t cp) {
  if (kWideIsUtf16 && cp >= 0x10000) {
    const uint32_t v = cp - 0x10000;
    out[0] = static_cast<SQLWCHAR>(0xD800 | (v >> 10));
    out[1] = static_cast<SQLWCHAR>(0xDC00 | (v & 0x3FF));
    return 2;
  }
  out[0] = static_cast<SQLWCHAR>(cp);
  return 1;
}

/// Decodes wide units s[0..n) to UTF-8 (UTF-16 with pairing, or UTF-32 with range checks).
void appendWideToUtf8(std::string& out, const SQLWCHAR* s, size_t n) {
  out.reserve(out.size() + n);
  for (size_t i = 0; i < n; ++i) {
    uint32_t v = unitValue(s[i]);
    if (kWideIsUtf16) {
      if (isHighSurrogate(v)) {
        if (i + 1 < n && isLowSurrogate(unitValue(s[i + 1]))) {
          v = 0x10000 + ((v - 0xD800) << 10) + (unitValue(s[i + 1]) - 0xDC00);
          ++i;
        } else {
          v = kReplacement;
        }
      } else if (isLowSurrogate(v)) {
        v = kReplacement;
      }
    } else if (v > kMaxCodePoint || isSurrogate(v)) {
      v = kReplacement;
    }
    appendUtf8(out, v);
  }
}

/// Replaces malformed sequences with U+FFFD; returns the input untouched when it is already valid.
std::string sanitizeUtf8(std::string_view s) {
  if (utf8IsValid(s)) return std::string(s);
  std::string out;
  out.reserve(s.size() + 8);
  size_t pos = 0;
  while (pos < s.size()) appendUtf8(out, decodeUtf8(s.data(), s.size(), pos));
  return out;
}

#if defined(TRINO_ODBC_WINDOWS)
std::wstring utf8ToWin(std::string_view utf8) {
  // Go through our own decoder so malformed input becomes U+FFFD instead of failing the Win32 call.
  std::wstring out;
  out.reserve(utf8.size());
  size_t pos = 0;
  while (pos < utf8.size()) {
    const uint32_t cp = decodeUtf8(utf8.data(), utf8.size(), pos);
    if (cp >= 0x10000) {
      const uint32_t v = cp - 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 | (v >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 | (v & 0x3FF)));
    } else {
      out.push_back(static_cast<wchar_t>(cp));
    }
  }
  return out;
}

std::string systemToUtf8(const char* s, size_t n) {
  if (n == 0) return std::string();
  const int len = static_cast<int>(n);
  const int wn = MultiByteToWideChar(CP_ACP, 0, s, len, nullptr, 0);
  if (wn <= 0) return sanitizeUtf8(std::string_view(s, n));
  std::wstring wide(static_cast<size_t>(wn), L'\0');
  MultiByteToWideChar(CP_ACP, 0, s, len, &wide[0], wn);
  std::string out;
  static_assert(sizeof(wchar_t) == 2, "Windows wchar_t is UTF-16");
  out.reserve(n);
  for (size_t i = 0; i < wide.size(); ++i) {
    uint32_t v = static_cast<uint16_t>(wide[i]);
    if (isHighSurrogate(v) && i + 1 < wide.size() && isLowSurrogate(static_cast<uint16_t>(wide[i + 1]))) {
      v = 0x10000 + ((v - 0xD800) << 10) + (static_cast<uint16_t>(wide[i + 1]) - 0xDC00);
      ++i;
    } else if (isSurrogate(v)) {
      v = kReplacement;
    }
    appendUtf8(out, v);
  }
  return out;
}

std::string utf8ToSystem(std::string_view utf8) {
  if (utf8.empty()) return std::string();
  const std::wstring wide = utf8ToWin(utf8);
  if (wide.empty()) return std::string();
  const int wn = static_cast<int>(wide.size());
  const int n = WideCharToMultiByte(CP_ACP, 0, wide.data(), wn, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_ACP, 0, wide.data(), wn, &out[0], n, nullptr, nullptr);
  return out;
}

/// Largest byte count <= maxBytes that does not split a DBCS character of the active code page.
size_t systemSafeCut(std::string_view bytes, size_t maxBytes) {
  if (bytes.size() <= maxBytes) return bytes.size();
  if (GetACP() == CP_UTF8) return utf8SafeCut(bytes, maxBytes);
  size_t i = 0;
  while (i < bytes.size()) {
    const size_t len = (IsDBCSLeadByteEx(CP_ACP, static_cast<BYTE>(bytes[i])) && i + 1 < bytes.size()) ? 2 : 1;
    if (i + len > maxBytes) break;
    i += len;
  }
  return i;
}
#endif

}  // namespace

AnsiEncoding defaultAnsiEncoding() {
#if defined(TRINO_ODBC_WINDOWS)
  return AnsiEncoding::System;
#else
  return AnsiEncoding::Utf8;
#endif
}

// ---- Wide (SQLWCHAR) <-> UTF-8 ------------------------------------------------------------

size_t wideStrLen(const SQLWCHAR* s) {
  if (!s) return 0;
  size_t n = 0;
  while (s[n] != 0) ++n;
  return n;
}

std::string wideToUtf8(const SQLWCHAR* s, SQLLEN lengthUnits) {
  std::string out;
  if (!s) return out;
  // SQL_NTS is the documented sentinel; any other negative length is an application bug that we
  // treat the same way rather than reading a huge span.
  const size_t n = lengthUnits < 0 ? wideStrLen(s) : static_cast<size_t>(lengthUnits);
  appendWideToUtf8(out, s, n);
  return out;
}

std::string wideToUtf8(const WString& s) {
  std::string out;
  appendWideToUtf8(out, s.data(), s.size());
  return out;
}

WString utf8ToWide(std::string_view utf8) {
  WString out;
  out.reserve(utf8.size());
  size_t pos = 0;
  SQLWCHAR tmp[2];
  while (pos < utf8.size()) {
    const uint32_t cp = decodeUtf8(utf8.data(), utf8.size(), pos);
    const size_t n = putWide(tmp, cp);
    out.append(tmp, n);
  }
  return out;
}

size_t wideUnitsFor(std::string_view utf8) {
  size_t units = 0;
  size_t pos = 0;
  while (pos < utf8.size()) {
    const unsigned char b = static_cast<unsigned char>(utf8[pos]);
    if (b < 0x80) {  // ASCII fast path
      ++pos;
      ++units;
      continue;
    }
    units += unitsForCodePoint(decodeUtf8(utf8.data(), utf8.size(), pos));
  }
  return units;
}

size_t utf8ToWideChunk(std::string_view utf8, SQLWCHAR* out, size_t capUnits, size_t* consumedBytes) {
  size_t written = 0;
  size_t pos = 0;
  if (out && capUnits > 0) {
    while (pos < utf8.size()) {
      const unsigned char b = static_cast<unsigned char>(utf8[pos]);
      if (b < 0x80) {
        if (written + 1 > capUnits) break;
        out[written++] = static_cast<SQLWCHAR>(b);
        ++pos;
        continue;
      }
      size_t next = pos;
      const uint32_t cp = decodeUtf8(utf8.data(), utf8.size(), next);
      if (written + unitsForCodePoint(cp) > capUnits) break;  // never split a surrogate pair
      written += putWide(out + written, cp);
      pos = next;
    }
  }
  if (consumedBytes) *consumedBytes = pos;
  return written;
}

// ---- Narrow (SQLCHAR) <-> UTF-8 -----------------------------------------------------------

std::string narrowToUtf8(const SQLCHAR* s, SQLLEN lengthBytes, AnsiEncoding enc) {
  if (!s) return std::string();
  const char* p = reinterpret_cast<const char*>(s);
  const size_t n = lengthBytes < 0 ? std::strlen(p) : static_cast<size_t>(lengthBytes);
#if defined(TRINO_ODBC_WINDOWS)
  if (enc == AnsiEncoding::System) return systemToUtf8(p, n);
#else
  (void)enc;  // the system encoding is UTF-8 on every non-Windows platform we support
#endif
  return sanitizeUtf8(std::string_view(p, n));
}

std::string utf8ToNarrow(std::string_view utf8, AnsiEncoding enc) {
#if defined(TRINO_ODBC_WINDOWS)
  if (enc == AnsiEncoding::System) return utf8ToSystem(utf8);
#else
  (void)enc;
#endif
  return std::string(utf8);
}

// ---- UTF-8 helpers ---------------------------------------------------------------------------

size_t utf8SafeCut(std::string_view utf8, size_t maxBytes) {
  if (utf8.size() <= maxBytes) return utf8.size();
  size_t cut = maxBytes;
  // Back up over continuation bytes to the lead byte, then verify the full sequence would fit.
  size_t lead = cut;
  while (lead > 0 && isContinuation(static_cast<unsigned char>(utf8[lead]))) --lead;
  if (lead == cut) return cut;  // cut lands exactly on a boundary
  const unsigned char b0 = static_cast<unsigned char>(utf8[lead]);
  size_t seqLen = 1;
  if (b0 >= 0xC0 && b0 <= 0xDF) seqLen = 2;
  else if (b0 >= 0xE0 && b0 <= 0xEF) seqLen = 3;
  else if (b0 >= 0xF0 && b0 <= 0xF7) seqLen = 4;
  if (lead + seqLen <= cut) return cut;  // whole sequence already before the cut (malformed trailing bytes)
  return lead;
}

size_t utf8CodePoints(std::string_view utf8) {
  size_t count = 0;
  size_t pos = 0;
  while (pos < utf8.size()) {
    const unsigned char b = static_cast<unsigned char>(utf8[pos]);
    if (b < 0x80) ++pos;
    else decodeUtf8(utf8.data(), utf8.size(), pos);
    ++count;
  }
  return count;
}

bool utf8IsValid(std::string_view utf8) {
  const size_t size = utf8.size();
  const char* s = utf8.data();
  size_t i = 0;
  while (i < size) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
      ++i;
      continue;
    }
    size_t need;
    unsigned char lo = 0x80, hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
      need = 1;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
      need = 2;
      if (b0 == 0xE0) lo = 0xA0;
      else if (b0 == 0xED) hi = 0x9F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      need = 3;
      if (b0 == 0xF0) lo = 0x90;
      else if (b0 == 0xF4) hi = 0x8F;
    } else {
      return false;
    }
    if (i + need >= size) return false;  // truncated sequence
    const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    if (b1 < lo || b1 > hi) return false;
    for (size_t k = 2; k <= need; ++k) {
      if (!isContinuation(static_cast<unsigned char>(s[i + k]))) return false;
    }
    i += need + 1;
  }
  return true;
}

uint32_t utf8Next(std::string_view s, size_t& pos) {
  if (pos >= s.size()) return kReplacement;
  return decodeUtf8(s.data(), s.size(), pos);
}

void utf8Append(std::string& out, uint32_t cp) {
  if (cp > kMaxCodePoint || isSurrogate(cp)) cp = kReplacement;
  appendUtf8(out, cp);
}

// ---- ODBC copy-out helpers -------------------------------------------------------------------

bool copyNarrowOutImpl(std::string_view utf8, SQLCHAR* buf, SQLLEN bufBytes, SQLLEN* outBytes, AnsiEncoding enc) {
#if defined(TRINO_ODBC_WINDOWS)
  std::string converted;
  std::string_view bytes = utf8;
  if (enc == AnsiEncoding::System) {
    converted = utf8ToSystem(utf8);
    bytes = converted;
  }
#else
  (void)enc;
  const std::string_view bytes = utf8;
#endif
  if (outBytes) *outBytes = static_cast<SQLLEN>(bytes.size());
  if (!buf) return false;
  if (bufBytes <= 0) return !bytes.empty();
  const size_t cap = static_cast<size_t>(bufBytes) - 1;  // room for the terminator
  size_t n;
  if (bytes.size() <= cap) {
    n = bytes.size();
  } else {
#if defined(TRINO_ODBC_WINDOWS)
    n = enc == AnsiEncoding::System ? systemSafeCut(bytes, cap) : utf8SafeCut(bytes, cap);
#else
    n = utf8SafeCut(bytes, cap);
#endif
  }
  if (n) std::memcpy(buf, bytes.data(), n);
  buf[n] = 0;
  return n < bytes.size();
}

bool copyWideOutUnitsImpl(std::string_view utf8, SQLWCHAR* buf, SQLLEN bufUnits, SQLLEN* outUnits) {
  if (outUnits) *outUnits = static_cast<SQLLEN>(wideUnitsFor(utf8));
  if (!buf) return false;
  if (bufUnits <= 0) return !utf8.empty();
  const size_t cap = static_cast<size_t>(bufUnits) - 1;
  size_t consumed = 0;
  const size_t written = utf8ToWideChunk(utf8, buf, cap, &consumed);
  buf[written] = 0;
  return consumed < utf8.size();
}

}  // namespace trino_odbc
