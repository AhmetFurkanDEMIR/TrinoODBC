// Recursive-descent parser and printer for Trino type signatures as they appear in the REST
// protocol's `columns[].type` and in DESCRIBE OUTPUT ("decimal(10,2)", "timestamp(6) with time zone",
// "row(\"my col\" varchar(3), x array(integer))", "interval day to second", "HyperLogLog", ...).
#include "types/trino_type.h"

#include "util/log.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "types";
// Deeper nesting than this is not a real Trino type; it protects the stack from hostile input.
constexpr int kMaxDepth = 64;

bool isAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }
bool isAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
/// Characters allowed inside an unquoted word (type names, unquoted field names). Trino type names
/// are identifiers; '$' and '.' appear in a few plugin-provided names and are harmless to accept.
bool isWordChar(char c) { return isAsciiAlpha(c) || isAsciiDigit(c) || c == '_' || c == '$' || c == '.'; }
bool isWordStart(char c) { return isAsciiAlpha(c) || c == '_' || c == '$'; }

char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/// Lowercases and collapses whitespace runs so "Timestamp   WITH time zone" compares as
/// "timestamp with time zone".
std::string normalizeBaseName(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool pendingSpace = false;
  for (char c : s) {
    if (isAsciiSpace(c)) {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(asciiLower(c));
  }
  return out;
}

/// True when a row field name can be printed without double quotes and re-parsed to the same name
/// (Trino would also keep it unchanged: unquoted identifiers are lowercased by the server).
bool isSimpleIdentifier(std::string_view name) {
  if (name.empty()) return false;
  if (!((name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || isAsciiDigit(c) || c == '_';
  });
}

std::string quoteFieldName(std::string_view name) {
  if (isSimpleIdentifier(name)) return std::string(name);
  std::string out;
  out.reserve(name.size() + 2);
  out.push_back('"');
  for (char c : name) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

class Parser {
 public:
  explicit Parser(std::string_view text) : s_(text) {}

  /// Parses the whole input; false when anything is left over or malformed.
  bool parseAll(TrinoType& out) {
    skipSpaces();
    if (!parseType(out)) return false;
    skipSpaces();
    return pos_ == s_.size();
  }

 private:
  bool atEnd() const { return pos_ >= s_.size(); }
  char peek() const { return atEnd() ? '\0' : s_[pos_]; }
  bool atArgBoundary() const { return atEnd() || peek() == ',' || peek() == ')'; }

  void skipSpaces() {
    while (!atEnd() && isAsciiSpace(s_[pos_])) ++pos_;
  }

  /// One unquoted word (identifier). False when the current character cannot start a word.
  bool parseWord(std::string& word) {
    if (atEnd() || !isWordStart(peek())) return false;
    const size_t start = pos_;
    while (!atEnd() && isWordChar(s_[pos_])) ++pos_;
    word.assign(s_, start, pos_ - start);
    return true;
  }

  /// "name with ""quotes""" -> name with "quotes"
  bool parseQuoted(std::string& name) {
    if (peek() != '"') return false;
    ++pos_;
    name.clear();
    while (true) {
      if (atEnd()) return false;  // unterminated
      const char c = s_[pos_++];
      if (c == '"') {
        if (peek() == '"') {  // escaped quote
          name.push_back('"');
          ++pos_;
          continue;
        }
        return true;
      }
      name.push_back(c);
    }
  }

  /// Non-negative decimal literal with overflow detection.
  bool parseLong(int64_t& value) {
    if (!isAsciiDigit(peek())) return false;
    int64_t acc = 0;
    while (!atEnd() && isAsciiDigit(s_[pos_])) {
      const int digit = s_[pos_] - '0';
      if (acc > (std::numeric_limits<int64_t>::max() - digit) / 10) return false;
      acc = acc * 10 + digit;
      ++pos_;
    }
    value = acc;
    return true;
  }

  /// type := words [ '(' args ')' ] words
  /// The trailing words carry suffixes such as "with time zone" that Trino prints after the
  /// precision: "timestamp(3) with time zone".
  bool parseType(TrinoType& out) {
    if (++depth_ > kMaxDepth) return false;
    struct DepthGuard {
      int& d;
      ~DepthGuard() { --d; }
    } guard{depth_};

    std::string base;
    std::string word;
    while (true) {
      skipSpaces();
      if (atEnd() || peek() == '(' || peek() == ',' || peek() == ')') break;
      if (!parseWord(word)) return false;  // e.g. a stray quote or digit where a name is expected
      if (!base.empty()) base.push_back(' ');
      base += word;
    }
    if (base.empty()) return false;

    std::vector<TrinoTypeArg> args;
    if (peek() == '(') {
      ++pos_;
      if (!parseArgs(args)) return false;
      if (peek() != ')') return false;
      ++pos_;
      // Suffix words after the parameter list ("with time zone").
      while (true) {
        skipSpaces();
        if (atEnd() || !isWordStart(peek())) break;
        parseWord(word);
        base.push_back(' ');
        base += word;
      }
    }

    out.rawType = normalizeBaseName(base);
    out.category = TrinoType::categoryFor(out.rawType);
    out.args = std::move(args);
    return true;
  }

  bool parseArgs(std::vector<TrinoTypeArg>& args) {
    skipSpaces();
    if (peek() == ')') return false;  // "varchar()" is not a Trino type
    while (true) {
      TrinoTypeArg arg;
      if (!parseArg(arg)) return false;
      args.push_back(std::move(arg));
      skipSpaces();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      return peek() == ')';
    }
  }

  /// arg := long | '"' name '"' type | name type | type
  bool parseArg(TrinoTypeArg& arg) {
    skipSpaces();
    if (isAsciiDigit(peek())) {
      arg.kind = TrinoTypeArg::Kind::Long;
      if (!parseLong(arg.longValue)) return false;
      skipSpaces();
      return atArgBoundary();
    }
    if (peek() == '"') {
      std::string name;
      if (!parseQuoted(name)) return false;
      skipSpaces();
      auto type = std::make_shared<TrinoType>();
      if (!parseType(*type)) return false;
      arg.kind = TrinoTypeArg::Kind::NamedType;
      arg.name = std::move(name);
      arg.type = std::move(type);
      return true;
    }

    // Unquoted: either a bare type ("integer", "timestamp with time zone") or an unquoted row
    // field name followed by its type ("x integer"). Older Trino versions print row field names
    // without quotes, so both must work. Try the bare-type reading first: it wins only when the
    // whole text is a known multi-word type, otherwise the first word is the field name.
    const size_t save = pos_;
    {
      auto type = std::make_shared<TrinoType>();
      if (parseType(*type)) {
        skipSpaces();
        if (atArgBoundary() && type->category != TypeCategory::Unknown) {
          arg.kind = TrinoTypeArg::Kind::Type;
          arg.type = std::move(type);
          return true;
        }
      }
    }
    pos_ = save;
    std::string name;
    if (!parseWord(name)) return false;
    skipSpaces();
    if (atArgBoundary()) {
      // A single unknown word: a plugin type such as "HyperLogLog" rather than a field name.
      pos_ = save;
      auto type = std::make_shared<TrinoType>();
      if (!parseType(*type)) return false;
      skipSpaces();
      if (!atArgBoundary()) return false;
      arg.kind = TrinoTypeArg::Kind::Type;
      arg.type = std::move(type);
      return true;
    }
    auto type = std::make_shared<TrinoType>();
    if (!parseType(*type)) return false;
    skipSpaces();
    if (!atArgBoundary()) return false;
    arg.kind = TrinoTypeArg::Kind::NamedType;
    arg.name = std::move(name);
    arg.type = std::move(type);
    return true;
  }

  std::string_view s_;
  size_t pos_ = 0;
  int depth_ = 0;
};

void appendArgs(const TrinoType& t, std::string& out) {
  if (t.args.empty()) return;
  // Trino prints "decimal(10,2)" but "map(varchar, integer)" / "row(a integer, b varchar)".
  const bool allLongs = std::all_of(t.args.begin(), t.args.end(), [](const TrinoTypeArg& a) {
    return a.kind == TrinoTypeArg::Kind::Long;
  });
  const char* sep = allLongs ? "," : ", ";
  out.push_back('(');
  for (size_t i = 0; i < t.args.size(); ++i) {
    if (i > 0) out += sep;
    const TrinoTypeArg& a = t.args[i];
    switch (a.kind) {
      case TrinoTypeArg::Kind::Long:
        out += std::to_string(a.longValue);
        break;
      case TrinoTypeArg::Kind::Type:
        out += a.type ? a.type->toString() : "unknown";
        break;
      case TrinoTypeArg::Kind::NamedType:
        if (!a.name.empty()) {
          out += quoteFieldName(a.name);
          out.push_back(' ');
        }
        out += a.type ? a.type->toString() : "unknown";
        break;
    }
  }
  out.push_back(')');
}

}  // namespace

TrinoType TrinoType::parse(std::string_view typeName) {
  TrinoType result;
  Parser parser(typeName);
  if (parser.parseAll(result)) return result;
  // Contract: never throw; unparsable text is exposed as an opaque text type under its own name.
  TRINO_LOG_DEBUG(kLog, "unparsable Trino type signature: '%.*s'", static_cast<int>(typeName.size()), typeName.data());
  TrinoType unknown;
  unknown.rawType.assign(typeName.data(), typeName.size());
  unknown.category = TypeCategory::Unknown;
  return unknown;
}

std::string TrinoType::toString() const {
  std::string out;
  out.reserve(rawType.size() + 16);
  if (args.empty()) return rawType;
  // Parameters belong to the first word: "timestamp(3) with time zone".
  const size_t space = rawType.find(' ');
  if (space == std::string::npos) {
    out = rawType;
    appendArgs(*this, out);
  } else {
    out.assign(rawType, 0, space);
    appendArgs(*this, out);
    out.append(rawType, space, std::string::npos);
  }
  return out;
}

std::optional<int64_t> TrinoType::length() const {
  if (category != TypeCategory::Varchar && category != TypeCategory::Char) return std::nullopt;
  if (!args.empty() && args[0].kind == TrinoTypeArg::Kind::Long) return args[0].longValue;
  // Plain "varchar" is unbounded; plain "char" is char(1) in Trino.
  if (category == TypeCategory::Char) return int64_t{1};
  return std::nullopt;
}

int TrinoType::decimalPrecision() const {
  if (category == TypeCategory::Decimal && !args.empty() && args[0].kind == TrinoTypeArg::Kind::Long) {
    const int64_t p = args[0].longValue;
    return static_cast<int>(std::min<int64_t>(std::max<int64_t>(p, 1), 38));
  }
  return 38;
}

int TrinoType::decimalScale() const {
  if (category == TypeCategory::Decimal && args.size() >= 2 && args[1].kind == TrinoTypeArg::Kind::Long) {
    const int64_t s = args[1].longValue;
    return static_cast<int>(std::min<int64_t>(std::max<int64_t>(s, 0), 38));
  }
  return 0;
}

int TrinoType::temporalPrecision() const {
  switch (category) {
    case TypeCategory::Time:
    case TypeCategory::TimeWithTimeZone:
    case TypeCategory::Timestamp:
    case TypeCategory::TimestampWithTimeZone:
      if (!args.empty() && args[0].kind == TrinoTypeArg::Kind::Long) {
        return static_cast<int>(std::min<int64_t>(std::max<int64_t>(args[0].longValue, 0), 12));
      }
      return 3;  // Trino's default precision when the server prints no parameter
    default:
      return 0;
  }
}

bool TrinoType::isNumeric() const {
  switch (category) {
    case TypeCategory::TinyInt:
    case TypeCategory::SmallInt:
    case TypeCategory::Integer:
    case TypeCategory::BigInt:
    case TypeCategory::Real:
    case TypeCategory::Double:
    case TypeCategory::Decimal:
      return true;
    default:
      return false;
  }
}

bool TrinoType::isTemporal() const {
  switch (category) {
    case TypeCategory::Date:
    case TypeCategory::Time:
    case TypeCategory::TimeWithTimeZone:
    case TypeCategory::Timestamp:
    case TypeCategory::TimestampWithTimeZone:
      return true;
    default:
      return false;
  }
}

TypeCategory TrinoType::categoryFor(std::string_view baseName) {
  struct Entry {
    const char* name;
    TypeCategory category;
  };
  // Names as Trino prints them plus the SQL aliases it accepts ("int", "double precision").
  static const Entry kTable[] = {
      {"boolean", TypeCategory::Boolean},
      {"tinyint", TypeCategory::TinyInt},
      {"smallint", TypeCategory::SmallInt},
      {"integer", TypeCategory::Integer},
      {"int", TypeCategory::Integer},
      {"bigint", TypeCategory::BigInt},
      {"real", TypeCategory::Real},
      {"double", TypeCategory::Double},
      {"double precision", TypeCategory::Double},
      {"decimal", TypeCategory::Decimal},
      {"varchar", TypeCategory::Varchar},
      {"char", TypeCategory::Char},
      {"varbinary", TypeCategory::Varbinary},
      {"json", TypeCategory::Json},
      {"date", TypeCategory::Date},
      {"time", TypeCategory::Time},
      {"time with time zone", TypeCategory::TimeWithTimeZone},
      {"timestamp", TypeCategory::Timestamp},
      {"timestamp with time zone", TypeCategory::TimestampWithTimeZone},
      {"interval year to month", TypeCategory::IntervalYearToMonth},
      {"interval day to second", TypeCategory::IntervalDayToSecond},
      {"array", TypeCategory::Array},
      {"map", TypeCategory::Map},
      {"row", TypeCategory::Row},
      {"uuid", TypeCategory::Uuid},
      {"ipaddress", TypeCategory::IpAddress},
  };
  const std::string key = normalizeBaseName(baseName);
  for (const Entry& e : kTable) {
    if (key == e.name) return e.category;
  }
  return TypeCategory::Unknown;
}

const char* TrinoType::categoryName(TypeCategory c) {
  switch (c) {
    case TypeCategory::Boolean: return "Boolean";
    case TypeCategory::TinyInt: return "TinyInt";
    case TypeCategory::SmallInt: return "SmallInt";
    case TypeCategory::Integer: return "Integer";
    case TypeCategory::BigInt: return "BigInt";
    case TypeCategory::Real: return "Real";
    case TypeCategory::Double: return "Double";
    case TypeCategory::Decimal: return "Decimal";
    case TypeCategory::Varchar: return "Varchar";
    case TypeCategory::Char: return "Char";
    case TypeCategory::Varbinary: return "Varbinary";
    case TypeCategory::Json: return "Json";
    case TypeCategory::Date: return "Date";
    case TypeCategory::Time: return "Time";
    case TypeCategory::TimeWithTimeZone: return "TimeWithTimeZone";
    case TypeCategory::Timestamp: return "Timestamp";
    case TypeCategory::TimestampWithTimeZone: return "TimestampWithTimeZone";
    case TypeCategory::IntervalYearToMonth: return "IntervalYearToMonth";
    case TypeCategory::IntervalDayToSecond: return "IntervalDayToSecond";
    case TypeCategory::Array: return "Array";
    case TypeCategory::Map: return "Map";
    case TypeCategory::Row: return "Row";
    case TypeCategory::Uuid: return "Uuid";
    case TypeCategory::IpAddress: return "IpAddress";
    case TypeCategory::Unknown: break;
  }
  return "Unknown";
}

}  // namespace trino_odbc
