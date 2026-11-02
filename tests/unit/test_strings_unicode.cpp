// Unit tests for src/util: strings, unicode, log, tz; plus the portable rules of the Windows setup
// dialog (core/platform_setup.h).
#include "unit/test_framework.h"

#include "core/config.h"
#include "core/platform_setup.h"
#include "util/log.h"
#include "util/strings.h"
#include "util/tz.h"
#include "util/unicode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(TRINO_ODBC_WINDOWS)
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

using namespace trino_odbc;

namespace {

// ---- helpers ------------------------------------------------------------------------------------

std::string tempDir() {
#if defined(TRINO_ODBC_WINDOWS)
  char buf[MAX_PATH + 1];
  const DWORD n = GetTempPathA(MAX_PATH + 1, buf);
  std::string dir = n ? std::string(buf, n) : std::string(".\\");
  if (dir.back() != '\\' && dir.back() != '/') dir.push_back('\\');
  return dir;
#else
  const char* t = std::getenv("TMPDIR");
  std::string dir = t && *t ? t : "/tmp";
  if (dir.back() != '/') dir.push_back('/');
  return dir;
#endif
}

void makeDir(const std::string& path) {
#if defined(TRINO_ODBC_WINDOWS)
  _mkdir(path.c_str());
#else
  ::mkdir(path.c_str(), 0700);
#endif
}

void setEnv(const char* name, const std::string& value) {
#if defined(TRINO_ODBC_WINDOWS)
  _putenv_s(name, value.c_str());
#else
  ::setenv(name, value.c_str(), 1);
#endif
}

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return s;
}

int64_t daysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

/// Epoch seconds of a civil instant (interpreted as UTC or as "wall clock as if UTC").
int64_t at(int64_t y, int m, int d, int hh = 0, int mm = 0, int ss = 0) {
  return daysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
}

int32_t offAtUtc(const char* zone, int64_t utc) {
  int32_t off = 123456789;
  const bool ok = TimeZoneDb::global().utcOffsetAtUtc(zone, utc, &off);
  return ok ? off : 123456789;
}

int32_t offAtLocal(const char* zone, int64_t local) {
  int32_t off = 123456789;
  const bool ok = TimeZoneDb::global().utcOffsetAtLocal(zone, local, &off);
  return ok ? off : 123456789;
}

void putU32(std::string& s, uint32_t v) {
  s.push_back(static_cast<char>((v >> 24) & 0xFF));
  s.push_back(static_cast<char>((v >> 16) & 0xFF));
  s.push_back(static_cast<char>((v >> 8) & 0xFF));
  s.push_back(static_cast<char>(v & 0xFF));
}
void putS64(std::string& s, int64_t v) {
  putU32(s, static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32));
  putU32(s, static_cast<uint32_t>(static_cast<uint64_t>(v) & 0xFFFFFFFFu));
}

struct TzType {
  int32_t utoff;
  uint8_t isdst;
};

/// Builds a minimal TZif file: `transitions` are (utc, typeIndex); v2 adds the 64-bit block + footer.
std::string buildTzif(bool v2, const std::vector<std::pair<int64_t, uint8_t>>& transitions,
                      const std::vector<TzType>& types, const std::string& footer) {
  auto header = [&](char version) {
    std::string h = "TZif";
    h.push_back(version);
    h.append(15, '\0');
    putU32(h, 0);  // isutcnt
    putU32(h, 0);  // isstdcnt
    putU32(h, 0);  // leapcnt
    putU32(h, static_cast<uint32_t>(transitions.size()));
    putU32(h, static_cast<uint32_t>(types.size()));
    putU32(h, 4);  // charcnt
    return h;
  };
  auto block = [&](bool wide) {
    std::string b;
    for (const auto& t : transitions) {
      if (wide) putS64(b, t.first);
      else putU32(b, static_cast<uint32_t>(static_cast<int32_t>(t.first)));
    }
    for (const auto& t : transitions) b.push_back(static_cast<char>(t.second));
    for (const auto& ty : types) {
      putU32(b, static_cast<uint32_t>(ty.utoff));
      b.push_back(static_cast<char>(ty.isdst));
      b.push_back('\0');  // desigidx
    }
    b.append("UTC", 3);
    b.push_back('\0');
    return b;
  };
  std::string file = header(v2 ? '2' : '\0') + block(false);
  if (v2) {
    file += header('2') + block(true);
    file += "\n" + footer + "\n";
  }
  return file;
}

std::string wideFromUnits(std::initializer_list<uint32_t> units) {
  WString w;
  for (uint32_t u : units) w.push_back(static_cast<SQLWCHAR>(u));
  return wideToUtf8(w);
}

}  // namespace

// ================================================================================================
// strings
// ================================================================================================

TEST_CASE("strings: trim / case / compare") {
  CHECK_EQ(std::string(trim("  a b \t\n")), std::string("a b"));
  CHECK_EQ(std::string(trim("")), std::string(""));
  CHECK_EQ(std::string(trim("   ")), std::string(""));
  CHECK_EQ(toLower("AbC-Ç"), std::string("abc-Ç"));  // non-ASCII untouched
  CHECK_EQ(toUpper("abc1"), std::string("ABC1"));
  CHECK(iequals("Host", "HOST"));
  CHECK(!iequals("Host", "Hosts"));
  CHECK(startsWith("abcdef", "abc"));
  CHECK(!startsWith("ab", "abc"));
  CHECK(istartsWith("ABCdef", "abc"));
  CHECK(endsWith("file.p12", ".p12"));
  CHECK(iendsWith("file.PFX", ".pfx"));
  CHECK(!iendsWith("x", ".pfx"));
}

TEST_CASE("strings: split / join / replaceAll") {
  auto parts = split(" a , b ,, c ", ',');
  REQUIRE_EQ(parts.size(), size_t(3));
  CHECK_EQ(parts[0], std::string("a"));
  CHECK_EQ(parts[2], std::string("c"));
  auto raw = split("a,,b", ',', false, false);
  REQUIRE_EQ(raw.size(), size_t(3));
  CHECK_EQ(raw[1], std::string(""));
  CHECK_EQ(split("", ';').size(), size_t(0));
  CHECK_EQ(split("", ';', true, false).size(), size_t(1));
  CHECK_EQ(join({"a", "b", "c"}, ", "), std::string("a, b, c"));
  CHECK_EQ(join({}, ","), std::string(""));
  CHECK_EQ(replaceAll("a.b.c", ".", ".."), std::string("a..b..c"));
  CHECK_EQ(replaceAll("aaa", "a", ""), std::string(""));
  CHECK_EQ(replaceAll("abc", "", "x"), std::string("abc"));
}

TEST_CASE("strings: parseBool variants") {
  bool v = false;
  for (const char* t : {"1", "true", "TRUE", "Yes", "on", "y", "T", " true "}) {
    CHECK(parseBool(t, &v));
    CHECK(v);
  }
  for (const char* f : {"0", "false", "No", "OFF", "n", "F"}) {
    v = true;
    CHECK(parseBool(f, &v));
    CHECK(!v);
  }
  CHECK(!parseBool("2", &v));
  CHECK(!parseBool("", &v));
  CHECK(!parseBool("yess", &v));
  CHECK(parseBool("1", nullptr));
}

TEST_CASE("strings: parseInt64 / parseDouble") {
  int64_t i = 0;
  CHECK(parseInt64("42", &i));
  CHECK_EQ(i, int64_t(42));
  CHECK(parseInt64("-7", &i));
  CHECK_EQ(i, int64_t(-7));
  CHECK(parseInt64("+7", &i));
  CHECK_EQ(i, int64_t(7));
  CHECK(parseInt64("9223372036854775807", &i));
  CHECK_EQ(i, INT64_MAX);
  CHECK(parseInt64("-9223372036854775808", &i));
  CHECK_EQ(i, INT64_MIN);
  CHECK(!parseInt64("9223372036854775808", &i));
  CHECK(!parseInt64("-9223372036854775809", &i));
  CHECK(!parseInt64("", &i));
  CHECK(!parseInt64("-", &i));
  CHECK(!parseInt64("12a", &i));
  CHECK(!parseInt64(" 12", &i));
  CHECK(!parseInt64("1.0", &i));

  double d = 0;
  CHECK(parseDouble("1.5", &d));
  CHECK_EQ(d, 1.5);
  CHECK(parseDouble("-2.5e3", &d));
  CHECK_EQ(d, -2500.0);
  CHECK(parseDouble(" 7 ", &d));
  CHECK_EQ(d, 7.0);
  CHECK(parseDouble(".5", &d));
  CHECK_EQ(d, 0.5);
  CHECK(parseDouble("5.", &d));
  CHECK_EQ(d, 5.0);
  CHECK(parseDouble("NaN", &d));
  CHECK(std::isnan(d));
  CHECK(parseDouble("Infinity", &d));
  CHECK(std::isinf(d) && d > 0);
  CHECK(parseDouble("-Infinity", &d));
  CHECK(std::isinf(d) && d < 0);
  CHECK(parseDouble("1e400", &d));
  CHECK(std::isinf(d) && d > 0);
  CHECK(!parseDouble("", &d));
  CHECK(!parseDouble(".", &d));
  CHECK(!parseDouble("1,5", &d));
  CHECK(!parseDouble("1e", &d));
  CHECK(!parseDouble("0x10", &d));
  CHECK(!parseDouble("12abc", &d));
}

TEST_CASE("strings: formatString short and long") {
  CHECK_EQ(formatString("%s=%d", "a", 5), std::string("a=5"));
  CHECK_EQ(formatString(""), std::string(""));
  const std::string big(2000, 'x');
  const std::string out = formatString("[%s]", big.c_str());
  CHECK_EQ(out.size(), size_t(2002));
  CHECK(out.front() == '[' && out.back() == ']');
  CHECK_EQ(formatString(nullptr), std::string(""));
}

TEST_CASE("strings: url encode/decode") {
  CHECK_EQ(urlEncode("abc-_.~"), std::string("abc-_.~"));
  CHECK_EQ(urlEncode("a b&c=d/é"), std::string("a%20b%26c%3Dd%2F%C3%A9"));
  CHECK_EQ(urlDecode("a%20b%26c%3Dd%2F%C3%A9"), std::string("a b&c=d/é"));
  CHECK_EQ(urlDecode("a+b"), std::string("a b"));
  CHECK_EQ(urlDecode("100%"), std::string("100%"));
  CHECK_EQ(urlDecode("%zz%4"), std::string("%zz%4"));
  CHECK_EQ(urlDecode(urlEncode("k=v;x,y")), std::string("k=v;x,y"));
}

TEST_CASE("strings: base64 round trips and tolerant decoding") {
  std::string out;
  for (size_t n = 0; n < 40; ++n) {
    std::string bytes;
    for (size_t i = 0; i < n; ++i) bytes.push_back(static_cast<char>((i * 37 + n) & 0xFF));
    const std::string enc = base64Encode(bytes);
    CHECK_EQ(enc.size() % 4, size_t(0));
    REQUIRE(base64Decode(enc, out));
    CHECK(out == bytes);
    // without padding
    std::string unpadded = enc;
    while (!unpadded.empty() && unpadded.back() == '=') unpadded.pop_back();
    REQUIRE(base64Decode(unpadded, out));
    CHECK(out == bytes);
  }
  CHECK_EQ(base64Encode(""), std::string(""));
  CHECK_EQ(base64Encode("f"), std::string("Zg=="));
  CHECK_EQ(base64Encode("fo"), std::string("Zm8="));
  CHECK_EQ(base64Encode("foo"), std::string("Zm9v"));
  CHECK_EQ(base64Encode("\xff\xfe\xfd"), std::string("//79"));
  CHECK(base64Decode("Zm 9v\nYm\tFy", out));
  CHECK_EQ(out, std::string("foobar"));
  CHECK(base64Decode("__79", out));  // url-safe alphabet
  CHECK_EQ(out, std::string("\xff\xfe\xfd"));
  CHECK(!base64Decode("Zm9v*", out));
  CHECK(!base64Decode("Z", out));       // lone symbol cannot form a byte
  CHECK(!base64Decode("Zg==Zg", out));  // data after padding
  CHECK(base64Decode("", out));
  CHECK(out.empty());
  CHECK_EQ(hexEncode("\x01\xab"), std::string("01AB"));
  CHECK_EQ(hexEncode("\x01\xab", false), std::string("01ab"));
}

TEST_CASE("strings: sql quoting and case-insensitive map") {
  CHECK_EQ(sqlQuoteLiteral("it's"), std::string("'it''s'"));
  CHECK_EQ(sqlQuoteLiteral(""), std::string("''"));
  CHECK_EQ(sqlQuoteIdentifier("na\"me"), std::string("\"na\"\"me\""));
  KeyValueMap m;
  m["Host"] = "a";
  m["HOST"] = "b";
  CHECK_EQ(m.size(), size_t(1));
  CHECK_EQ(m["host"], std::string("b"));
  CaseInsensitiveLess less;
  CHECK(less("abc", "abd"));
  CHECK(less("ab", "abc"));
  CHECK(!less("ABC", "abc"));
  CHECK(!less("abc", "ABC"));
}

// ================================================================================================
// unicode
// ================================================================================================

TEST_CASE("unicode: utf8 validation") {
  CHECK(utf8IsValid(""));
  CHECK(utf8IsValid("plain ascii"));
  CHECK(utf8IsValid("Türkçe ğüşiöç 日本語 😀"));
  CHECK(!utf8IsValid("\xC0\x80"));              // overlong NUL
  CHECK(!utf8IsValid("\xE0\x80\x80"));          // overlong
  CHECK(!utf8IsValid("\xED\xA0\x80"));          // UTF-16 surrogate
  CHECK(!utf8IsValid("\xF4\x90\x80\x80"));      // > U+10FFFF
  CHECK(!utf8IsValid("\xF5\x80\x80\x80"));
  CHECK(!utf8IsValid("\xE2\x82"));              // truncated
  CHECK(!utf8IsValid("\x80"));                  // stray continuation
  CHECK(!utf8IsValid("abc\xFF"));
  CHECK(utf8IsValid("\xF0\x9F\x98\x80"));       // U+1F600
  CHECK(utf8IsValid("\xF4\x8F\xBF\xBF"));       // U+10FFFF
}

TEST_CASE("unicode: utf8Next / codepoints / append") {
  const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  size_t pos = 0;
  CHECK_EQ(utf8Next(s, pos), uint32_t('a'));
  CHECK_EQ(utf8Next(s, pos), uint32_t(0xE9));
  CHECK_EQ(utf8Next(s, pos), uint32_t(0x20AC));
  CHECK_EQ(utf8Next(s, pos), uint32_t(0x1F600));
  CHECK_EQ(pos, s.size());
  CHECK_EQ(utf8CodePoints(s), size_t(4));
  CHECK_EQ(utf8CodePoints(""), size_t(0));

  // malformed: truncated 3-byte sequence followed by 'X' -> one U+FFFD then 'X'
  const std::string bad = "\xE2\x82X";
  pos = 0;
  CHECK_EQ(utf8Next(bad, pos), uint32_t(0xFFFD));
  CHECK_EQ(pos, size_t(2));
  CHECK_EQ(utf8Next(bad, pos), uint32_t('X'));
  // stray continuation bytes: one U+FFFD each
  const std::string stray = "\x80\x80";
  CHECK_EQ(utf8CodePoints(stray), size_t(2));
  pos = 0;
  CHECK_EQ(utf8Next(stray, pos), uint32_t(0xFFFD));
  CHECK_EQ(pos, size_t(1));
  // overlong / surrogate are rejected at the second byte
  pos = 0;
  CHECK_EQ(utf8Next("\xED\xA0\x80", pos), uint32_t(0xFFFD));
  CHECK_EQ(pos, size_t(1));

  std::string out;
  utf8Append(out, 0x41);
  utf8Append(out, 0xE9);
  utf8Append(out, 0x20AC);
  utf8Append(out, 0x1F600);
  CHECK_EQ(out, s.substr(0, 0) + "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
  out.clear();
  utf8Append(out, 0xD800);   // surrogate -> U+FFFD
  utf8Append(out, 0x110000); // out of range -> U+FFFD
  CHECK_EQ(out, std::string("\xEF\xBF\xBD\xEF\xBF\xBD"));
}

TEST_CASE("unicode: utf8SafeCut never splits a code point") {
  const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // 1+2+3+4 bytes
  CHECK_EQ(utf8SafeCut(s, 100), s.size());
  CHECK_EQ(utf8SafeCut(s, 10), size_t(10));
  CHECK_EQ(utf8SafeCut(s, 9), size_t(6));
  CHECK_EQ(utf8SafeCut(s, 8), size_t(6));
  CHECK_EQ(utf8SafeCut(s, 7), size_t(6));
  CHECK_EQ(utf8SafeCut(s, 6), size_t(6));
  CHECK_EQ(utf8SafeCut(s, 5), size_t(3));
  CHECK_EQ(utf8SafeCut(s, 4), size_t(3));
  CHECK_EQ(utf8SafeCut(s, 3), size_t(3));
  CHECK_EQ(utf8SafeCut(s, 2), size_t(1));
  CHECK_EQ(utf8SafeCut(s, 1), size_t(1));
  CHECK_EQ(utf8SafeCut(s, 0), size_t(0));
  CHECK_EQ(utf8SafeCut("", 0), size_t(0));
}

TEST_CASE("unicode: wide <-> utf8 round trip incl. 4-byte characters") {
  const std::string s = "Türkçe ğüşiöç 日本語 😀 \xF4\x8F\xBF\xBF";
  const WString w = utf8ToWide(s);
  const size_t expectedUnits = kWideIsUtf16 ? utf8CodePoints(s) + 2 : utf8CodePoints(s);
  CHECK_EQ(w.size(), expectedUnits);
  CHECK_EQ(wideUnitsFor(s), expectedUnits);
  CHECK_EQ(wideToUtf8(w), s);
  CHECK_EQ(wideToUtf8(w.c_str(), static_cast<SQLLEN>(SQL_NTS)), s);
  CHECK_EQ(wideToUtf8(w.c_str(), static_cast<SQLLEN>(w.size())), s);
  CHECK_EQ(wideToUtf8(w.c_str(), static_cast<SQLSMALLINT>(1)), std::string("T"));
  CHECK_EQ(wideStrLen(w.c_str()), w.size());
  CHECK_EQ(wideToUtf8(nullptr, static_cast<SQLLEN>(SQL_NTS)), std::string(""));
  CHECK_EQ(utf8ToWide("").size(), size_t(0));
  if (kWideIsUtf16) {
    const WString emoji = utf8ToWide("\xF0\x9F\x98\x80");
    REQUIRE_EQ(emoji.size(), size_t(2));
    CHECK_EQ(static_cast<uint32_t>(static_cast<uint16_t>(emoji[0])), uint32_t(0xD83D));
    CHECK_EQ(static_cast<uint32_t>(static_cast<uint16_t>(emoji[1])), uint32_t(0xDE00));
  }
}

TEST_CASE("unicode: invalid wide input becomes U+FFFD") {
  if (kWideIsUtf16) {
    CHECK_EQ(wideFromUnits({'a', 0xD83D, 'b'}), std::string("a\xEF\xBF\xBD" "b"));   // lone high
    CHECK_EQ(wideFromUnits({0xDE00, 'b'}), std::string("\xEF\xBF\xBD" "b"));         // lone low
    CHECK_EQ(wideFromUnits({0xD83D}), std::string("\xEF\xBF\xBD"));                  // high at end
    CHECK_EQ(wideFromUnits({0xD83D, 0xDE00}), std::string("\xF0\x9F\x98\x80"));      // proper pair
  } else {
    CHECK_EQ(wideFromUnits({'a', 0xD800, 'b'}), std::string("a\xEF\xBF\xBD" "b"));
    CHECK_EQ(wideFromUnits({0x110000}), std::string("\xEF\xBF\xBD"));
    CHECK_EQ(wideFromUnits({0x1F600}), std::string("\xF0\x9F\x98\x80"));
  }
  // invalid UTF-8 going out to wide is replaced, not dropped
  const WString w = utf8ToWide("a\xFF" "b\xE2\x82");
  CHECK_EQ(wideToUtf8(w), std::string("a\xEF\xBF\xBD" "b\xEF\xBF\xBD"));
}

TEST_CASE("unicode: utf8ToWideChunk does not split code points or surrogate pairs") {
  const std::string s = "a\xF0\x9F\x98\x80" "b";  // a, U+1F600, b
  SQLWCHAR buf[8];
  size_t consumed = 0;
  size_t n = utf8ToWideChunk(s, buf, 2, &consumed);
  if (kWideIsUtf16) {
    CHECK_EQ(n, size_t(1));  // the pair needs 2 units; only 'a' fits
    CHECK_EQ(consumed, size_t(1));
  } else {
    CHECK_EQ(n, size_t(2));
    CHECK_EQ(consumed, size_t(5));
  }
  n = utf8ToWideChunk(s, buf, 3, &consumed);
  if (kWideIsUtf16) {
    CHECK_EQ(n, size_t(3));
    CHECK_EQ(consumed, size_t(5));
  } else {
    CHECK_EQ(n, size_t(3));
    CHECK_EQ(consumed, size_t(6));
  }
  n = utf8ToWideChunk(s, buf, 8, &consumed);
  CHECK_EQ(consumed, s.size());
  CHECK_EQ(n, wideUnitsFor(s));
  // resuming from the consumed offset yields the remainder exactly
  size_t c1 = 0;
  const size_t n1 = utf8ToWideChunk(s, buf, 1, &c1);
  CHECK_EQ(n1, size_t(1));
  CHECK_EQ(c1, size_t(1));
  size_t c2 = 0;
  const size_t n2 = utf8ToWideChunk(std::string_view(s).substr(c1), buf, 8, &c2);
  CHECK_EQ(n2, wideUnitsFor(s) - 1);
  CHECK_EQ(c1 + c2, s.size());
  // zero capacity / null output
  n = utf8ToWideChunk(s, nullptr, 0, &consumed);
  CHECK_EQ(n, size_t(0));
  CHECK_EQ(consumed, size_t(0));
  n = utf8ToWideChunk("", buf, 8, &consumed);
  CHECK_EQ(n, size_t(0));
}

TEST_CASE("unicode: copyWideOutChars / copyWideOutBytes truncation semantics") {
  const std::string s = "h\xC3\xA9llo";  // héllo, 5 code points
  SQLWCHAR buf[16];
  SQLSMALLINT outUnits = -1;
  // fits
  CHECK(!copyWideOutChars(s, buf, 16, &outUnits));
  CHECK_EQ(outUnits, SQLSMALLINT(5));
  CHECK_EQ(wideToUtf8(buf, static_cast<SQLLEN>(SQL_NTS)), s);
  // exactly fits with terminator
  CHECK(!copyWideOutChars(s, buf, 6, &outUnits));
  CHECK_EQ(wideToUtf8(buf, static_cast<SQLLEN>(SQL_NTS)), s);
  // truncated: 3 units -> 2 chars + NUL, length still reports the full size
  std::memset(buf, 0x7F, sizeof(buf));
  CHECK(copyWideOutChars(s, buf, 3, &outUnits));
  CHECK_EQ(outUnits, SQLSMALLINT(5));
  CHECK_EQ(wideToUtf8(buf, static_cast<SQLLEN>(SQL_NTS)), std::string("h\xC3\xA9"));
  CHECK_EQ(static_cast<uint32_t>(buf[2]), uint32_t(0));
  // only the terminator fits
  CHECK(copyWideOutChars(s, buf, 1, &outUnits));
  CHECK_EQ(static_cast<uint32_t>(buf[0]), uint32_t(0));
  // zero-length buffer: nothing written, truncation reported
  buf[0] = static_cast<SQLWCHAR>('Q');
  CHECK(copyWideOutChars(s, buf, 0, &outUnits));
  CHECK_EQ(static_cast<uint32_t>(buf[0]), uint32_t('Q'));
  CHECK_EQ(outUnits, SQLSMALLINT(5));
  // null buffer: length only, no truncation flag
  CHECK(!copyWideOutChars(s, nullptr, 0, &outUnits));
  CHECK_EQ(outUnits, SQLSMALLINT(5));
  // null length pointer is fine
  CHECK(!copyWideOutChars(s, buf, 16, static_cast<SQLINTEGER*>(nullptr)));
  // empty string into 1-unit buffer is not truncation
  CHECK(!copyWideOutChars("", buf, 1, &outUnits));
  CHECK_EQ(outUnits, SQLSMALLINT(0));

  // byte-counted variant
  const SQLLEN W = static_cast<SQLLEN>(sizeof(SQLWCHAR));
  SQLINTEGER outBytes = -1;
  CHECK(!copyWideOutBytes(s, buf, 16 * W, &outBytes));
  CHECK_EQ(outBytes, static_cast<SQLINTEGER>(5 * W));
  std::memset(buf, 0x7F, sizeof(buf));
  CHECK(copyWideOutBytes(s, buf, 3 * W + 1, &outBytes));  // odd byte count rounds down to 3 units
  CHECK_EQ(outBytes, static_cast<SQLINTEGER>(5 * W));
  CHECK_EQ(wideToUtf8(buf, static_cast<SQLLEN>(SQL_NTS)), std::string("h\xC3\xA9"));
  CHECK(copyWideOutBytes(s, buf, W - 1, &outBytes));  // less than one unit: nothing written
  CHECK(copyWideOutBytes(s, buf, 0, &outBytes));
  CHECK(!copyWideOutBytes(s, nullptr, 0, &outBytes));
  CHECK_EQ(outBytes, static_cast<SQLINTEGER>(5 * W));
  // surrogate pair never split by a byte-sized buffer
  if (kWideIsUtf16) {
    const std::string emoji = "a\xF0\x9F\x98\x80";
    CHECK(copyWideOutBytes(emoji, buf, 3 * W, &outBytes));  // 2 units usable: 'a' + (pair does not fit)
    CHECK_EQ(wideToUtf8(buf, static_cast<SQLLEN>(SQL_NTS)), std::string("a"));
    CHECK_EQ(outBytes, static_cast<SQLINTEGER>(3 * W));
    CHECK(!copyWideOutBytes(emoji, buf, 4 * W, &outBytes));
    CHECK_EQ(wideToUtf8(buf, static_cast<SQLLEN>(SQL_NTS)), emoji);
  }
}

TEST_CASE("unicode: copyNarrowOut truncation semantics (UTF-8)") {
  const std::string s = "h\xC3\xA9llo";  // 6 bytes
  SQLCHAR buf[16];
  SQLLEN outBytes = -1;
  CHECK(!copyNarrowOut(s, buf, 16, &outBytes, AnsiEncoding::Utf8));
  CHECK_EQ(outBytes, SQLLEN(6));
  CHECK_EQ(std::string(reinterpret_cast<char*>(buf)), s);
  CHECK(!copyNarrowOut(s, buf, 7, &outBytes, AnsiEncoding::Utf8));
  CHECK_EQ(std::string(reinterpret_cast<char*>(buf)), s);
  // cap 3 bytes: "hé" would be 3 bytes -> fits; cap 2 -> only "h"
  CHECK(copyNarrowOut(s, buf, 4, &outBytes, AnsiEncoding::Utf8));
  CHECK_EQ(std::string(reinterpret_cast<char*>(buf)), std::string("h\xC3\xA9"));
  CHECK_EQ(outBytes, SQLLEN(6));
  CHECK(copyNarrowOut(s, buf, 3, &outBytes, AnsiEncoding::Utf8));
  CHECK_EQ(std::string(reinterpret_cast<char*>(buf)), std::string("h"));
  CHECK(copyNarrowOut(s, buf, 1, &outBytes, AnsiEncoding::Utf8));
  CHECK_EQ(buf[0], SQLCHAR(0));
  buf[0] = 'Q';
  CHECK(copyNarrowOut(s, buf, 0, &outBytes, AnsiEncoding::Utf8));
  CHECK_EQ(buf[0], SQLCHAR('Q'));
  CHECK(!copyNarrowOut(s, nullptr, 0, &outBytes, AnsiEncoding::Utf8));
  CHECK_EQ(outBytes, SQLLEN(6));
  SQLSMALLINT small = -1;
  CHECK(!copyNarrowOut("", buf, 1, &small, AnsiEncoding::Utf8));
  CHECK_EQ(small, SQLSMALLINT(0));
  CHECK_EQ(buf[0], SQLCHAR(0));
}

TEST_CASE("unicode: narrowToUtf8 / utf8ToNarrow") {
  const char* text = "abc \xC3\xA9";
  CHECK_EQ(narrowToUtf8(reinterpret_cast<const SQLCHAR*>(text), static_cast<SQLLEN>(SQL_NTS), AnsiEncoding::Utf8), std::string(text));
  CHECK_EQ(narrowToUtf8(reinterpret_cast<const SQLCHAR*>(text), static_cast<SQLINTEGER>(3), AnsiEncoding::Utf8),
           std::string("abc"));
  CHECK_EQ(narrowToUtf8(nullptr, static_cast<SQLLEN>(SQL_NTS), AnsiEncoding::Utf8), std::string(""));
  // malformed narrow UTF-8 is repaired instead of poisoning internal strings
  CHECK_EQ(narrowToUtf8(reinterpret_cast<const SQLCHAR*>("a\xFF" "b"), static_cast<SQLLEN>(SQL_NTS), AnsiEncoding::Utf8),
           std::string("a\xEF\xBF\xBD" "b"));
  CHECK_EQ(utf8ToNarrow(text, AnsiEncoding::Utf8), std::string(text));
#if defined(TRINO_ODBC_WINDOWS)
  CHECK_EQ(defaultAnsiEncoding() == AnsiEncoding::System, true);
  // ASCII survives the system code page on every Windows locale
  CHECK_EQ(utf8ToNarrow("plain", AnsiEncoding::System), std::string("plain"));
  CHECK_EQ(narrowToUtf8(reinterpret_cast<const SQLCHAR*>("plain"), static_cast<SQLLEN>(SQL_NTS), AnsiEncoding::System), std::string("plain"));
#else
  CHECK(defaultAnsiEncoding() == AnsiEncoding::Utf8);
  CHECK_EQ(utf8ToNarrow(text, AnsiEncoding::System), std::string(text));
#endif
}

// ================================================================================================
// log
// ================================================================================================

TEST_CASE("log: mask and level names") {
  CHECK_EQ(Logger::mask(""), std::string(""));
  CHECK_EQ(Logger::mask("short"), std::string("***"));
  CHECK_EQ(Logger::mask("1234567"), std::string("***"));
  CHECK_EQ(Logger::mask("abcdefyz"), std::string("ab***yz"));
  CHECK_EQ(Logger::mask("a-very-long-secret-token"), std::string("a-***en"));
  CHECK_EQ(std::string(Logger::levelName(LogLevel::Error)), std::string("ERROR"));
  CHECK_EQ(std::string(Logger::levelName(LogLevel::Trace)), std::string("TRACE"));
  CHECK_EQ(std::string(Logger::levelName(LogLevel::Off)), std::string("OFF"));
}

TEST_CASE("log: file sink, line format, level semantics") {
  Logger& log = Logger::global();
  log.configureFromEnvironment();
  const std::string dir = tempDir() + "trino_odbc_unit_log";
  makeDir(dir);
  log.configure(5, dir);
  if (!log.enabled(LogLevel::Trace)) return;  // environment forced a lower level; nothing to verify
  CHECK(log.enabled(LogLevel::Error));
  CHECK_EQ(log.level(), 5);
  const std::string path = log.path();
  REQUIRE(!path.empty());
  CHECK(path.find("trino_odbc_") != std::string::npos);
  CHECK(endsWith(path, ".log"));

  log.log(LogLevel::Trace, "unit", "hello world");
  log.logf(LogLevel::Debug, "unit", "value=%d name=%s", 42, "x");
  log.log(LogLevel::Info, "unit", "multi\nline");
  const std::string big(3000, 'z');
  log.logf(LogLevel::Warn, "unit", "%s", big.c_str());
  TRINO_LOG_ERROR("unit", "macro %d", 7);

  const std::string content = readFile(path);
  REQUIRE(!content.empty());
  CHECK(content.find("TRACE unit: hello world\n") != std::string::npos);
  CHECK(content.find("DEBUG unit: value=42 name=x\n") != std::string::npos);
  CHECK(content.find("INFO  unit: multi line\n") != std::string::npos);
  CHECK(content.find("WARN  unit: " + big + "\n") != std::string::npos);
  CHECK(content.find("ERROR unit: macro 7\n") != std::string::npos);
  // timestamp "YYYY-MM-DD HH:MM:SS.mmm [tid] LEVEL"
  const size_t p = content.find("TRACE unit: hello world");
  REQUIRE(p != std::string::npos);
  const size_t lineStart = content.rfind('\n', p) == std::string::npos ? 0 : content.rfind('\n', p) + 1;
  const std::string line = content.substr(lineStart, p - lineStart);
  REQUIRE(line.size() > 24);
  CHECK(line[4] == '-' && line[7] == '-' && line[10] == ' ' && line[13] == ':' && line[16] == ':' && line[19] == '.');
  CHECK(line[23] == ' ' && line[24] == '[');
  CHECK(line.find("] ") != std::string::npos);
  // The sink is opened shared: another process (a second driver instance, a log viewer) can open the
  // same file for reading and appending while this one holds it. std::fopen shares on every CRT, so
  // on Windows these fail when the sink was opened exclusively (_wfopen_s).
  {
    std::FILE* reader = std::fopen(path.c_str(), "rb");
    CHECK(reader != nullptr);
    if (reader) std::fclose(reader);
    std::FILE* writer = std::fopen(path.c_str(), "ab");
    CHECK(writer != nullptr);
    if (writer) {
      std::fputs("second writer line\n", writer);
      std::fclose(writer);
    }
    log.log(LogLevel::Info, "unit", "after second writer");
    const std::string shared = readFile(path);
    CHECK(shared.find("second writer line\n") != std::string::npos);
    CHECK(shared.find("INFO  unit: after second writer\n") != std::string::npos);
  }
  // a lower configure() call never lowers the level; a different path never replaces the first one
  log.configure(1, dir + "/other.log");
  CHECK_EQ(log.level(), 5);
  CHECK_EQ(log.path(), path);
}

// ================================================================================================
// tz
// ================================================================================================

TEST_CASE("tz: parseFixedOffset") {
  int32_t off = -1;
  CHECK(parseFixedOffset("Z", &off));
  CHECK_EQ(off, 0);
  CHECK(parseFixedOffset("utc", &off));
  CHECK_EQ(off, 0);
  CHECK(parseFixedOffset("GMT", &off));
  CHECK(parseFixedOffset("+05:30", &off));
  CHECK_EQ(off, 19800);
  CHECK(parseFixedOffset("-0800", &off));
  CHECK_EQ(off, -28800);
  CHECK(parseFixedOffset("+5", &off));
  CHECK_EQ(off, 18000);
  CHECK(parseFixedOffset("-05", &off));
  CHECK_EQ(off, -18000);
  CHECK(parseFixedOffset("+5:45", &off));
  CHECK_EQ(off, 20700);
  CHECK(parseFixedOffset("+01:02:03", &off));
  CHECK_EQ(off, 3723);
  CHECK(parseFixedOffset("+010203", &off));
  CHECK_EQ(off, 3723);
  CHECK(parseFixedOffset("UTC+02:00", &off));
  CHECK_EQ(off, 7200);
  CHECK(parseFixedOffset("GMT-8", &off));
  CHECK_EQ(off, -28800);
  CHECK(parseFixedOffset(" +00:00 ", &off));
  CHECK_EQ(off, 0);
  CHECK(parseFixedOffset("+18:00", &off));
  CHECK(!parseFixedOffset("+18:01", &off));
  CHECK(!parseFixedOffset("+19", &off));
  CHECK(!parseFixedOffset("+05:60", &off));
  CHECK(!parseFixedOffset("05:30", &off));
  CHECK(!parseFixedOffset("+", &off));
  CHECK(!parseFixedOffset("", &off));
  CHECK(!parseFixedOffset("+5:3", &off));
  CHECK(!parseFixedOffset("+123", &off));
  CHECK(!parseFixedOffset("America/New_York", &off));
  CHECK(!parseFixedOffset("+0a:00", &off));
}

TEST_CASE("tz: Europe/Istanbul (default session zone) always resolves to +03:00 today") {
  int32_t off = 0;
  CHECK(fixedZoneFallback("Europe/Istanbul", &off));
  CHECK_EQ(off, 10800);
  CHECK(fixedZoneFallback(" asia/istanbul ", &off));
  CHECK(fixedZoneFallback("Turkey", &off));
  CHECK(!fixedZoneFallback("Europe/Berlin", &off));
  CHECK(!fixedZoneFallback("", &off));
  // Whatever backend answers (TZif, ICU, Windows rules or the fixed fallback), the offset since
  // 2016 is +03:00 in winter and summer.
  TimeZoneDb& db = TimeZoneDb::global();
  CHECK(db.isKnown("Europe/Istanbul"));
  CHECK_EQ(offAtUtc("Europe/Istanbul", at(2024, 1, 15)), 10800);
  CHECK_EQ(offAtUtc("Europe/Istanbul", at(2024, 7, 15)), 10800);
  CHECK_EQ(offAtLocal("Europe/Istanbul", at(2026, 3, 29)), 10800);
}

TEST_CASE("tz: builtin zones resolve without files") {
  TimeZoneDb& db = TimeZoneDb::global();
  for (const char* name : {"UTC", "Z", "GMT", "Etc/UTC", "Etc/GMT", "utc", "Zulu", "Etc/GMT0", "GMT+0"}) {
    CHECK(db.isKnown(name));
    CHECK_EQ(offAtUtc(name, at(2024, 6, 1)), 0);
    CHECK_EQ(offAtLocal(name, at(2024, 6, 1)), 0);
  }
  CHECK_EQ(offAtUtc("Etc/GMT+5", at(2024, 1, 1)), -18000);   // POSIX sign
  CHECK_EQ(offAtUtc("Etc/GMT-14", at(2024, 1, 1)), 50400);
  CHECK(!db.isKnown("Etc/GMT+15"));
  CHECK_EQ(offAtUtc("+05:30", at(2024, 1, 1)), 19800);
  CHECK_EQ(offAtLocal("+05:30", at(2024, 1, 1)), 19800);
  CHECK_EQ(offAtUtc("-08:00", 0), -28800);
  CHECK_EQ(offAtUtc("UTC+01:00", 0), 3600);
  CHECK(!db.isKnown("Nowhere/Nothing"));
  CHECK(!db.isKnown(""));
  CHECK(!db.isKnown("../etc/passwd"));
  CHECK(!db.isKnown("/etc/localtime"));
  int32_t off = 7;
  CHECK(!db.utcOffsetAtUtc("Nowhere/Nothing", 0, &off));
  CHECK_EQ(off, 7);  // untouched on failure
  CHECK(db.utcOffsetAtUtc("UTC", 0, nullptr));
}

TEST_CASE("tz: POSIX TZ strings with DST rules (no files)") {
  const char* z = "EST5EDT,M3.2.0,M11.1.0";  // US rules
  REQUIRE(TimeZoneDb::global().isKnown(z));
  // 2024: DST starts Mar 10 02:00 EST (07:00Z), ends Nov 3 02:00 EDT (06:00Z)
  CHECK_EQ(offAtUtc(z, at(2024, 3, 10, 6, 59, 59)), -18000);
  CHECK_EQ(offAtUtc(z, at(2024, 3, 10, 7, 0, 0)), -14400);
  CHECK_EQ(offAtUtc(z, at(2024, 7, 4, 12)), -14400);
  CHECK_EQ(offAtUtc(z, at(2024, 11, 3, 5, 59, 59)), -14400);
  CHECK_EQ(offAtUtc(z, at(2024, 11, 3, 6, 0, 0)), -18000);
  CHECK_EQ(offAtUtc(z, at(2024, 12, 25)), -18000);
  CHECK_EQ(offAtUtc(z, at(1985, 7, 1)), -14400);   // rule applies to any year
  CHECK_EQ(offAtUtc(z, at(2100, 1, 1)), -18000);
  // wall clock: unambiguous
  CHECK_EQ(offAtLocal(z, at(2024, 7, 4, 12)), -14400);
  CHECK_EQ(offAtLocal(z, at(2024, 1, 4, 12)), -18000);
  // gap 02:00-03:00 on Mar 10: earlier (pre-transition) offset
  CHECK_EQ(offAtLocal(z, at(2024, 3, 10, 2, 30)), -18000);
  CHECK_EQ(offAtLocal(z, at(2024, 3, 10, 1, 59, 59)), -18000);
  CHECK_EQ(offAtLocal(z, at(2024, 3, 10, 3, 0, 0)), -14400);
  // overlap 01:00-02:00 on Nov 3: earlier offset (EDT)
  CHECK_EQ(offAtLocal(z, at(2024, 11, 3, 1, 30)), -14400);
  CHECK_EQ(offAtLocal(z, at(2024, 11, 3, 0, 59, 59)), -14400);
  CHECK_EQ(offAtLocal(z, at(2024, 11, 3, 2, 0, 0)), -18000);

  // southern hemisphere, explicit dst offset and times: Australia/Sydney style
  const char* syd = "AEST-10AEDT,M10.1.0,M4.1.0/3";
  REQUIRE(TimeZoneDb::global().isKnown(syd));
  CHECK_EQ(offAtUtc(syd, at(2024, 1, 15)), 39600);   // DST in January
  CHECK_EQ(offAtUtc(syd, at(2024, 7, 15)), 36000);
  CHECK_EQ(offAtUtc(syd, at(2024, 4, 6, 15, 59, 59)), 39600);
  CHECK_EQ(offAtUtc(syd, at(2024, 4, 6, 16, 0, 0)), 36000);
  CHECK_EQ(offAtUtc(syd, at(2024, 10, 5, 15, 59, 59)), 36000);
  CHECK_EQ(offAtUtc(syd, at(2024, 10, 5, 16, 0, 0)), 39600);

  // quoted names, Julian day rules, no-DST
  CHECK_EQ(offAtUtc("<+0330>-3:30", at(2024, 1, 1)), 12600);
  CHECK_EQ(offAtUtc("IST-5:30", at(2024, 1, 1)), 19800);
  CHECK_EQ(offAtUtc("XXX3YYY,J60,J300", at(2024, 3, 1, 5, 0, 0)), -7200);   // J60 = Mar 1 even in leap years; DST starts 02:00 local std = 05:00 UTC
  CHECK_EQ(offAtUtc("XXX3YYY,J60,J300", at(2024, 3, 1, 4, 59, 59)), -10800);
  CHECK_EQ(offAtUtc("XXX3YYY,J60,J300", at(2024, 2, 29, 12)), -10800);
  // dst named without rule -> US default rule
  CHECK_EQ(offAtUtc("EST5EDT", at(2024, 7, 4)), -14400);
  CHECK_EQ(offAtUtc("EST5EDT", at(2024, 1, 4)), -18000);
  // TZif v3 extended hours (Troll station style)
  CHECK(TimeZoneDb::global().isKnown("<+00>0<+02>-2,M3.5.0/1,M10.5.0/3"));
  // invalid strings
  CHECK(!TimeZoneDb::global().isKnown("EST5EDT,M13.1.0,M11.1.0"));
  CHECK(!TimeZoneDb::global().isKnown("E5"));
  CHECK(!TimeZoneDb::global().isKnown("EST5EDT,M3.2.0"));
}

TEST_CASE("tz: synthetic TZif files via TZDIR (v2 footer rule, v1 table only, garbage)") {
  const std::string root = tempDir() + "trino_odbc_unit_tzdir";
  makeDir(root);
  makeDir(root + "/Test");
  // Type 0: +0 (pre-1970), type 1: +1h (CET). One transition at epoch 0, footer with EU rules.
  const std::string v2 = buildTzif(true, {{0, 1}}, {{0, 0}, {3600, 0}}, "CET-1CEST,M3.5.0,M10.5.0/3");
  {
    std::ofstream out(root + "/Test/Synthetic", std::ios::binary);
    out.write(v2.data(), static_cast<std::streamsize>(v2.size()));
  }
  const std::string v1 = buildTzif(false, {{0, 1}, {1000000, 0}}, {{0, 0}, {3600, 0}}, "");
  {
    std::ofstream out(root + "/Test/OldFormat", std::ios::binary);
    out.write(v1.data(), static_cast<std::streamsize>(v1.size()));
  }
  {
    std::ofstream out(root + "/Test/Garbage", std::ios::binary);
    out << "this is not a zone file";
  }
  setEnv("TZDIR", root);
  TimeZoneDb& db = TimeZoneDb::global();

  REQUIRE(db.isKnown("Test/Synthetic"));
  CHECK_EQ(offAtUtc("Test/Synthetic", -1), 0);       // before the first transition: type 0
  CHECK_EQ(offAtUtc("Test/Synthetic", 0), 3600);     // after the last transition: footer rule (winter)
  CHECK_EQ(offAtUtc("Test/Synthetic", at(2024, 1, 15)), 3600);
  // EU DST 2024: Mar 31 01:00Z -> +2, Oct 27 01:00Z -> +1
  CHECK_EQ(offAtUtc("Test/Synthetic", at(2024, 3, 31, 0, 59, 59)), 3600);
  CHECK_EQ(offAtUtc("Test/Synthetic", at(2024, 3, 31, 1, 0, 0)), 7200);
  CHECK_EQ(offAtUtc("Test/Synthetic", at(2024, 7, 1)), 7200);
  CHECK_EQ(offAtUtc("Test/Synthetic", at(2024, 10, 27, 0, 59, 59)), 7200);
  CHECK_EQ(offAtUtc("Test/Synthetic", at(2024, 10, 27, 1, 0, 0)), 3600);
  // wall clock gap 02:00-03:00 on Mar 31 -> pre-transition +1; overlap 02:00-03:00 on Oct 27 -> +2
  CHECK_EQ(offAtLocal("Test/Synthetic", at(2024, 3, 31, 2, 30)), 3600);
  CHECK_EQ(offAtLocal("Test/Synthetic", at(2024, 3, 31, 3, 0)), 7200);
  CHECK_EQ(offAtLocal("Test/Synthetic", at(2024, 10, 27, 2, 30)), 7200);
  CHECK_EQ(offAtLocal("Test/Synthetic", at(2024, 10, 27, 3, 0)), 3600);
  CHECK_EQ(offAtLocal("Test/Synthetic", at(2024, 10, 27, 1, 59)), 7200);
  // wall clock just after the table transition at epoch 0 (local 01:00): offset +1
  CHECK_EQ(offAtLocal("Test/Synthetic", 3600), 3600);
  CHECK_EQ(offAtLocal("Test/Synthetic", -3600), 0);

  REQUIRE(db.isKnown("Test/OldFormat"));
  CHECK_EQ(offAtUtc("Test/OldFormat", -5), 0);
  CHECK_EQ(offAtUtc("Test/OldFormat", 5), 3600);
  CHECK_EQ(offAtUtc("Test/OldFormat", 999999), 3600);
  CHECK_EQ(offAtUtc("Test/OldFormat", 1000000), 0);
  CHECK_EQ(offAtUtc("Test/OldFormat", at(2050, 1, 1)), 0);  // no footer: last type persists
  CHECK_EQ(offAtLocal("Test/OldFormat", 1000), 0);           // 00:16 local is inside the 00:00-01:00 gap -> pre-transition offset
  CHECK_EQ(offAtLocal("Test/OldFormat", 5000), 3600);        // 01:23 local -> +1 period

  CHECK(!db.isKnown("Test/Garbage"));
  CHECK(!db.isKnown("Test/Missing"));
}

TEST_CASE("tz: system zoneinfo database (skipped when absent)") {
  TimeZoneDb& db = TimeZoneDb::global();
  if (!db.isKnown("America/New_York")) return;
  const char* ny = "America/New_York";
  CHECK_EQ(offAtUtc(ny, at(2024, 3, 10, 6, 59, 59)), -18000);
  CHECK_EQ(offAtUtc(ny, at(2024, 3, 10, 7, 0, 0)), -14400);
  CHECK_EQ(offAtUtc(ny, at(2024, 11, 3, 5, 59, 59)), -14400);
  CHECK_EQ(offAtUtc(ny, at(2024, 11, 3, 6, 0, 0)), -18000);
  CHECK_EQ(offAtUtc(ny, at(1999, 7, 1)), -14400);
  CHECK_EQ(offAtUtc(ny, at(1970, 1, 1)), -18000);
  // beyond the transition table -> footer rule
  CHECK_EQ(offAtUtc(ny, at(2050, 7, 1)), -14400);
  CHECK_EQ(offAtUtc(ny, at(2050, 1, 15)), -18000);
  CHECK_EQ(offAtUtc(ny, at(2200, 7, 1)), -14400);
  CHECK_EQ(offAtLocal(ny, at(2024, 3, 10, 2, 30)), -18000);   // gap
  CHECK_EQ(offAtLocal(ny, at(2024, 11, 3, 1, 30)), -14400);   // overlap -> earlier offset
  CHECK_EQ(offAtLocal(ny, at(2024, 7, 1, 12)), -14400);
  CHECK_EQ(offAtLocal(ny, at(2024, 12, 1, 12)), -18000);

  if (db.isKnown("Europe/Istanbul")) {
    CHECK_EQ(offAtUtc("Europe/Istanbul", at(2024, 1, 15)), 10800);  // permanent +3 since 2016
    CHECK_EQ(offAtUtc("Europe/Istanbul", at(2024, 7, 15)), 10800);
    CHECK_EQ(offAtUtc("Europe/Istanbul", at(2010, 1, 15)), 7200);
    CHECK_EQ(offAtUtc("Europe/Istanbul", at(2010, 7, 15)), 10800);
  }
  if (db.isKnown("Australia/Sydney")) {
    CHECK_EQ(offAtUtc("Australia/Sydney", at(2024, 4, 6, 15, 59, 59)), 39600);
    CHECK_EQ(offAtUtc("Australia/Sydney", at(2024, 4, 6, 16, 0, 0)), 36000);
    CHECK_EQ(offAtUtc("Australia/Sydney", at(2024, 10, 5, 15, 59, 59)), 36000);
    CHECK_EQ(offAtUtc("Australia/Sydney", at(2024, 10, 5, 16, 0, 0)), 39600);
    CHECK_EQ(offAtUtc("Australia/Sydney", at(2060, 1, 15)), 39600);
  }
  if (db.isKnown("Asia/Kolkata")) {
    CHECK_EQ(offAtUtc("Asia/Kolkata", at(2024, 6, 1)), 19800);
    CHECK_EQ(offAtLocal("Asia/Kolkata", at(2024, 6, 1)), 19800);
  }
  if (db.isKnown("Asia/Kathmandu")) CHECK_EQ(offAtUtc("Asia/Kathmandu", at(2024, 6, 1)), 20700);
  if (db.isKnown("Pacific/Chatham")) {
    CHECK_EQ(offAtUtc("Pacific/Chatham", at(2024, 7, 1)), 45900);   // +12:45
    CHECK_EQ(offAtUtc("Pacific/Chatham", at(2024, 1, 1)), 49500);   // +13:45 DST
  }
  // the OS zone, when detectable, is always a resolvable name
  const std::string sys = TimeZoneDb::systemZoneName();
  if (!sys.empty()) CHECK(db.isKnown(sys));
}

TEST_CASE("tz: system zone is a resolvable name or empty (header then omitted)") {
  // Without a usable backend for the OS zone (e.g. no icu.dll on Windows) detection must degrade to
  // "" instead of returning a name the driver cannot convert with; never a garbage name.
  const std::string sys = TimeZoneDb::systemZoneName();
  if (!sys.empty()) CHECK(TimeZoneDb::global().isKnown(sys));
  CHECK_EQ(TimeZoneDb::systemZoneName(), sys);  // cached, stable
}

// ================================================================================================
// platform setup dialog rules (core/platform_setup.h)
// ================================================================================================

namespace {

/// What the Windows dialog does on OK when the user did not touch the SSL / auth controls: it shows
/// the effective values and writes them back through applyTransportChoice.
KeyValueMap saveUntouched(KeyValueMap values) {
  const bool authExplicit = values.count("AuthenticationType") != 0;
  const bool sslExplicit = values.count("SSL") != 0;
  const DerivedTransport shown = deriveTransport(values, true, true);
  TransportChoice c;
  c.auth = authTypeName(shown.auth);
  c.authExplicit = authExplicit;
  c.ssl = shown.ssl;
  c.sslExplicit = sslExplicit;
  applyTransportChoice(values, c);
  return values;
}

}  // namespace

TEST_CASE("setup: dialog shows the implied SSL / authentication, not the defaults") {
  KeyValueMap v{{"Host", "trino"}, {"Port", "443"}, {"UID", "u"}, {"PWD", "p"}};
  DerivedTransport t = deriveTransport(v, true, true);
  CHECK(t.auth == AuthType::Ldap);
  CHECK(t.ssl);
  KeyValueMap jwt{{"Host", "trino"}, {"UID", "u"}, {"AccessToken", "tok"}};
  t = deriveTransport(jwt, true, true);
  CHECK(t.auth == AuthType::Jwt);
  CHECK(t.ssl);
  KeyValueMap plain{{"Host", "trino"}, {"UID", "u"}};
  t = deriveTransport(plain, true, true);
  CHECK(t.auth == AuthType::None);
  CHECK(!t.ssl);
  // an explicit key wins, and honour=false yields the implied value
  KeyValueMap forced{{"Host", "trino"}, {"PWD", "p"}, {"SSL", "0"}, {"AllowInsecurePassword", "1"}};
  CHECK(!deriveTransport(forced, true, true).ssl);
  KeyValueMap forcedOn{{"Host", "trino"}, {"PWD", "p"}, {"SSL", "1"}};
  CHECK(deriveTransport(forcedOn, true, false).ssl);  // implied by LDAP too
  KeyValueMap aliasKeys{{"Server", "trino"}, {"Password", "p"}, {"UseSSL", "0"}, {"AllowInsecurePassword", "1"}};
  CHECK(deriveTransport(aliasKeys, true, true).auth == AuthType::Ldap);
  CHECK(!deriveTransport(aliasKeys, true, true).ssl);
  CHECK(deriveTransport(aliasKeys, true, false).ssl == false);  // AllowInsecurePassword: no implied TLS
}

TEST_CASE("setup: saving an untouched dialog keeps implied SSL / authentication implicit") {
  // The finding's DSN: Port=443 + PWD connects over https with Basic auth; OK without changes must
  // not store SSL=0 / AuthenticationType=None.
  KeyValueMap v = saveUntouched({{"Host", "trino"}, {"Port", "443"}, {"UID", "u"}, {"PWD", "p"}});
  CHECK(v.count("SSL") == 0);
  CHECK(v.count("AuthenticationType") == 0);
  ConnectionConfig cfg;
  cfg.applyAll(v);
  cfg.finalize();
  CHECK(cfg.ssl);
  CHECK(cfg.auth == AuthType::Ldap);

  v = saveUntouched({{"Host", "trino"}, {"UID", "u"}, {"AccessToken", "tok"}});
  CHECK(v.count("SSL") == 0);
  CHECK(v.count("AuthenticationType") == 0);

  v = saveUntouched({{"Host", "trino"}, {"UID", "u"}});
  CHECK(v.count("SSL") == 0);
  CHECK(v.count("AuthenticationType") == 0);

  // explicit keys stay explicit even when they equal the implied value
  v = saveUntouched({{"Host", "trino"}, {"PWD", "p"}, {"SSL", "1"}, {"AuthenticationType", "LDAP"}});
  CHECK_EQ(v["SSL"], std::string("1"));
  CHECK_EQ(v["AuthenticationType"], std::string("LDAP"));
}

TEST_CASE("setup: user changes to SSL / authentication are written explicitly") {
  const KeyValueMap base{{"Host", "trino"}, {"Port", "443"}, {"UID", "u"}, {"PWD", "p"}};
  // unticking SSL
  KeyValueMap v = base;
  TransportChoice c;
  c.auth = "LDAP";
  c.ssl = false;
  c.sslTouched = true;
  applyTransportChoice(v, c);
  CHECK_EQ(v["SSL"], std::string("0"));
  CHECK(v.count("AuthenticationType") == 0);
  // clicking the checkbox twice (back to the implied state) is still an explicit choice
  v = base;
  c = TransportChoice{};
  c.auth = "LDAP";
  c.ssl = true;
  c.sslTouched = true;
  applyTransportChoice(v, c);
  CHECK_EQ(v["SSL"], std::string("1"));
  // choosing another authentication type; SSL follows the new implied value and stays implicit
  v = KeyValueMap{{"Host", "trino"}, {"UID", "u"}, {"PWD", "p"}};
  c = TransportChoice{};
  c.auth = "None";
  c.authTouched = true;
  c.ssl = false;  // Port 8080 + no authentication implies http
  applyTransportChoice(v, c);
  CHECK_EQ(v["AuthenticationType"], std::string("None"));
  CHECK(v.count("SSL") == 0);
  // a shown value that differs from the implied one is not silently dropped
  v = KeyValueMap{{"Host", "trino"}, {"UID", "u"}};
  c = TransportChoice{};
  c.auth = "Kerberos";
  c.ssl = true;
  applyTransportChoice(v, c);
  CHECK_EQ(v["AuthenticationType"], std::string("Kerberos"));
  CHECK(v.count("SSL") == 0);  // Kerberos implies TLS
  // an explicit key removed by nobody is kept even when the user left the control alone
  v = KeyValueMap{{"Host", "trino"}, {"SSL", "0"}};
  c = TransportChoice{};
  c.auth = "None";
  c.ssl = false;
  c.sslExplicit = true;
  applyTransportChoice(v, c);
  CHECK_EQ(v["SSL"], std::string("0"));
}

TEST_CASE("setup: connection prompt starts without placeholder SSL / AuthenticationType") {
  ConnectionConfig cfg;
  cfg.applyAll(ConnectionConfig::parseConnectionString("Driver={Trino ODBC Driver};Host=h;UID=u;AccessToken=t"));
  KeyValueMap v = promptDialogValues(cfg);
  CHECK(v.count("SSL") == 0);
  CHECK(v.count("AuthenticationType") == 0);
  CHECK_EQ(v["Host"], std::string("h"));
  CHECK_EQ(v["AccessToken"], std::string("t"));
  // pressing Connect without changes, then finalize: the token is still sent over TLS
  KeyValueMap saved = saveUntouched(v);
  ConnectionConfig after;
  after.applyAll(ConnectionConfig::parseConnectionString("Driver={Trino ODBC Driver};Host=h;UID=u;AccessToken=t"));
  std::string applyError;
  for (const auto& kv : saved) CHECK(after.applyKeyValue(kv.first, kv.second, &applyError));
  after.finalize();
  CHECK(after.auth == AuthType::Jwt);
  CHECK(after.ssl);

  ConnectionConfig explicitCfg;
  explicitCfg.applyAll(ConnectionConfig::parseConnectionString("Host=h;UID=u;SSL=1;AuthenticationType=None"));
  v = promptDialogValues(explicitCfg);
  CHECK(v.count("SSL") == 1);
  CHECK(v.count("AuthenticationType") == 1);
}

TEST_CASE("setup: adding or renaming onto an existing DSN needs confirmation") {
  CHECK(needsOverwriteConfirmation("", "Sales", true));         // ODBC_ADD_DSN with an existing name
  CHECK(!needsOverwriteConfirmation("", "Sales", false));       // new name
  CHECK(needsOverwriteConfirmation("Trino", "Sales", true));    // rename onto another DSN
  CHECK(!needsOverwriteConfirmation("Trino", "Other", false));  // rename to a free name
  CHECK(!needsOverwriteConfirmation("Trino", "Trino", true));   // editing in place
  CHECK(!needsOverwriteConfirmation("Trino", "TRINO", true));   // DSN names are case-insensitive
  CHECK(!needsOverwriteConfirmation("", "", true));
}
