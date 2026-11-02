// ODBC escape-sequence translator, parameter-marker scanner and statement classifier.
//
// The translator is a hand-written lexer rather than a SQL parser: it only needs to know where
// string literals, quoted identifiers and comments begin and end so that braces, commas,
// parentheses and '?' inside them are never interpreted. Everything else is copied verbatim, so
// the output differs from the input only where an escape sequence was rewritten.
#include "sql/escape.h"

#include "util/errors.h"
#include "util/log.h"
#include "util/strings.h"

#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace trino_odbc {
namespace {

constexpr const char* kLog = "escape";
// Escape sequences nest ({fn LEFT({fn UCASE(x)}, 3)}); bound the recursion so a pathological
// statement cannot exhaust the stack.
constexpr int kMaxNesting = 64;
constexpr size_t npos = std::string_view::npos;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

[[noreturn]] void syntaxError(std::string message) {
  throw DriverError(sqlstate::kSyntaxError, std::move(message));
}

// ---------------------------------------------------------------------------------------------
// Lexical skipping. "Opaque" units are string literals, quoted identifiers and comments: the
// translator never looks inside them.
// ---------------------------------------------------------------------------------------------

/// Returns the index just past the quoted unit starting at `pos` (sql[pos] == quote). A doubled
/// quote is an escape. An unterminated unit extends to the end of the text; Trino reports that.
size_t skipQuoted(std::string_view sql, size_t pos, char quote, bool* terminated = nullptr) {
  if (terminated) *terminated = false;
  size_t i = pos + 1;
  while (i < sql.size()) {
    if (sql[i] == quote) {
      if (i + 1 < sql.size() && sql[i + 1] == quote) {
        i += 2;
        continue;
      }
      if (terminated) *terminated = true;
      return i + 1;
    }
    ++i;
  }
  return sql.size();
}

/// If sql[pos] starts an opaque unit, returns the index just past it; otherwise npos.
size_t skipOpaque(std::string_view sql, size_t pos) {
  const char c = sql[pos];
  if (c == '\'' || c == '"') return skipQuoted(sql, pos, c);
  if (c == '-' && pos + 1 < sql.size() && sql[pos + 1] == '-') {
    const size_t nl = sql.find('\n', pos + 2);
    return nl == npos ? sql.size() : nl + 1;
  }
  if (c == '/' && pos + 1 < sql.size() && sql[pos + 1] == '*') {
    const size_t end = sql.find("*/", pos + 2);
    return end == npos ? sql.size() : end + 2;
  }
  return npos;
}

/// True when `text` is exactly one single-quoted string literal (nothing before or after it).
bool isSingleStringLiteral(std::string_view text) {
  if (text.empty() || text[0] != '\'') return false;
  bool terminated = false;
  return skipQuoted(text, 0, '\'', &terminated) == text.size() && terminated;
}

/// Index of the '}' matching the '{' at `open`; braces inside opaque units are ignored.
size_t findMatchingBrace(std::string_view sql, size_t open) {
  int depth = 0;
  size_t i = open;
  while (i < sql.size()) {
    const size_t end = skipOpaque(sql, i);
    if (end != npos) {
      i = end;
      continue;
    }
    if (sql[i] == '{') {
      ++depth;
    } else if (sql[i] == '}') {
      if (--depth == 0) return i;
    }
    ++i;
  }
  syntaxError("Malformed ODBC escape sequence: missing closing '}'");
}

/// Index of the ')' matching the '(' at `open`, or npos when unbalanced.
size_t findMatchingParen(std::string_view text, size_t open) {
  int depth = 0;
  size_t i = open;
  while (i < text.size()) {
    const size_t end = skipOpaque(text, i);
    if (end != npos) {
      i = end;
      continue;
    }
    if (text[i] == '(') {
      ++depth;
    } else if (text[i] == ')') {
      if (--depth == 0) return i;
    }
    ++i;
  }
  return npos;
}

/// Splits a function argument list at top-level commas. Commas inside parentheses, brackets
/// (ARRAY[1,2]), braces, strings, identifiers and comments do not split. Arguments are trimmed;
/// an empty argument such as `f(a, )` is a syntax error.
std::vector<std::string_view> splitTopLevelArgs(std::string_view args) {
  std::vector<std::string_view> out;
  int depth = 0;
  size_t start = 0;
  size_t i = 0;
  while (i < args.size()) {
    const size_t end = skipOpaque(args, i);
    if (end != npos) {
      i = end;
      continue;
    }
    const char c = args[i];
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
    } else if (c == ')' || c == ']' || c == '}') {
      if (--depth < 0) syntaxError("Malformed ODBC escape sequence: unbalanced parentheses in argument list");
    } else if (c == ',' && depth == 0) {
      out.push_back(trim(args.substr(start, i - start)));
      start = i + 1;
    }
    ++i;
  }
  const std::string_view last = trim(args.substr(start));
  if (out.empty() && last.empty()) return out;  // f() has no arguments at all
  out.push_back(last);
  for (const auto& a : out) {
    if (a.empty()) syntaxError("Malformed ODBC escape sequence: empty function argument");
  }
  return out;
}

/// Wraps `arg` in parentheses unless it is a single token (identifier, number, '?' or string
/// literal), so that it can be embedded into arithmetic without precedence surprises.
std::string paren(std::string_view arg) {
  if (isSingleStringLiteral(arg) || arg == "?") return std::string(arg);
  bool simple = !arg.empty();
  for (const char c : arg) {
    if (!isIdentChar(c) && c != '.') {
      simple = false;
      break;
    }
  }
  if (simple) return std::string(arg);
  std::string out;
  out.reserve(arg.size() + 2);
  out.push_back('(');
  out.append(arg.data(), arg.size());
  out.push_back(')');
  return out;
}

std::string joinArgs(const std::vector<std::string_view>& args) {
  std::string out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) out.append(", ");
    out.append(args[i].data(), args[i].size());
  }
  return out;
}

std::string callOf(std::string_view name, const std::vector<std::string_view>& args) {
  std::string out(name);
  out.push_back('(');
  out.append(joinArgs(args));
  out.push_back(')');
  return out;
}

// ---------------------------------------------------------------------------------------------
// Scalar function table
// ---------------------------------------------------------------------------------------------

constexpr int kVariadic = 1 << 20;

/// Functions whose translation is only a rename (or none) with arguments passed through unchanged.
struct SimpleFn {
  const char* odbc;
  const char* trino;
  int minArgs;
  int maxArgs;
};

const SimpleFn kSimpleFns[] = {
    // string
    {"CHAR", "chr", 1, 1},
    {"CHAR_LENGTH", "length", 1, 1},
    {"CHARACTER_LENGTH", "length", 1, 1},
    {"CONCAT", "concat", 1, kVariadic},
    {"LCASE", "lower", 1, 1},
    {"LENGTH", "length", 1, 1},
    {"LTRIM", "ltrim", 1, 1},
    {"POSITION", "position", 1, 1},  // the single argument is `a IN b`, valid Trino syntax
    {"REPLACE", "replace", 2, 3},
    {"RTRIM", "rtrim", 1, 1},
    {"SOUNDEX", "soundex", 1, 1},
    {"SUBSTRING", "substr", 2, 3},
    {"UCASE", "upper", 1, 1},
    // numeric
    {"ABS", "abs", 1, 1},
    {"ACOS", "acos", 1, 1},
    {"ASIN", "asin", 1, 1},
    {"ATAN", "atan", 1, 1},
    {"ATAN2", "atan2", 2, 2},
    {"CEILING", "ceiling", 1, 1},
    {"COS", "cos", 1, 1},
    {"DEGREES", "degrees", 1, 1},
    {"EXP", "exp", 1, 1},
    {"FLOOR", "floor", 1, 1},
    {"LOG", "ln", 1, 1},
    {"LOG10", "log10", 1, 1},
    {"MOD", "mod", 2, 2},
    {"PI", "pi", 0, 0},
    {"POWER", "power", 2, 2},
    {"RADIANS", "radians", 1, 1},
    {"ROUND", "round", 1, 2},
    {"SIGN", "sign", 1, 1},
    {"SIN", "sin", 1, 1},
    {"SQRT", "sqrt", 1, 1},
    {"TAN", "tan", 1, 1},
    {"TRUNCATE", "truncate", 1, 2},
    // time/date
    {"DAYOFMONTH", "day", 1, 1},
    {"DAYOFYEAR", "day_of_year", 1, 1},
    {"EXTRACT", "extract", 1, 1},  // `field FROM x` is valid Trino syntax
    {"HOUR", "hour", 1, 1},
    {"MINUTE", "minute", 1, 1},
    {"MONTH", "month", 1, 1},
    {"QUARTER", "quarter", 1, 1},
    {"SECOND", "second", 1, 1},
    {"WEEK", "week", 1, 1},
    {"YEAR", "year", 1, 1},
    // system
    {"IFNULL", "coalesce", 2, kVariadic},
};

const SimpleFn* findSimpleFn(const std::string& upperName) {
  for (const auto& fn : kSimpleFns) {
    if (upperName == fn.odbc) return &fn;
  }
  return nullptr;
}

/// Functions that may legally be written without parentheses ({fn CURRENT_DATE}).
bool isNiladic(const std::string& upperName) {
  static const char* const kNames[] = {"CURRENT_DATE", "CURDATE", "CURRENT_TIME", "CURTIME", "CURRENT_TIMESTAMP",
                                       "NOW",          "PI",      "RAND",         "DATABASE", "USER"};
  for (const char* n : kNames) {
    if (upperName == n) return true;
  }
  return false;
}

void requireArgs(const std::string& fn, const std::vector<std::string_view>& args, int minArgs, int maxArgs) {
  const int n = static_cast<int>(args.size());
  if (n < minArgs || n > maxArgs) {
    syntaxError("Wrong number of arguments for ODBC scalar function " + fn + ": expected " +
                (minArgs == maxArgs ? std::to_string(minArgs)
                 : maxArgs == kVariadic ? "at least " + std::to_string(minArgs)
                                        : std::to_string(minArgs) + " to " + std::to_string(maxArgs)) +
                ", got " + std::to_string(n));
  }
}

/// Maps an ODBC TIMESTAMPADD/TIMESTAMPDIFF interval name (with or without the SQL_TSI_ prefix)
/// to a Trino date_add/date_diff unit string.
std::string intervalUnit(std::string_view token, const std::string& fn) {
  std::string u = toUpper(trim(token));
  if (startsWith(u, "SQL_TSI_")) u.erase(0, 8);
  if (u == "FRAC_SECOND") return "millisecond";  // Trino's finest unit; callers rescale billionths
  if (u == "SECOND") return "second";
  if (u == "MINUTE") return "minute";
  if (u == "HOUR") return "hour";
  if (u == "DAY") return "day";
  if (u == "WEEK") return "week";
  if (u == "MONTH") return "month";
  if (u == "QUARTER") return "quarter";
  if (u == "YEAR") return "year";
  syntaxError("Unknown interval type '" + std::string(trim(token)) + "' in ODBC scalar function " + fn);
}

/// ODBC SQL type name / code -> Trino type for {fn CONVERT}.
struct ConvertType {
  const char* odbcName;  // without the SQL_ prefix
  int code;              // ODBC type code (0 = none)
  const char* trino;
  bool takesSuffix;      // whether a (n) / (p,s) suffix on the ODBC name may be carried over
};

const ConvertType kConvertTypes[] = {
    {"BIGINT", -5, "bigint", false},
    {"BINARY", -2, "varbinary", false},
    {"VARBINARY", -3, "varbinary", false},
    {"LONGVARBINARY", -4, "varbinary", false},
    {"BIT", -7, "boolean", false},
    {"BOOLEAN", 16, "boolean", false},
    {"CHAR", 1, "varchar", true},
    {"VARCHAR", 12, "varchar", true},
    {"LONGVARCHAR", -1, "varchar", true},
    {"WCHAR", -8, "varchar", true},
    {"WVARCHAR", -9, "varchar", true},
    {"WLONGVARCHAR", -10, "varchar", true},
    {"DATE", 9, "date", false},
    {"TYPE_DATE", 91, "date", false},
    {"DECIMAL", 3, "decimal", true},
    {"NUMERIC", 2, "decimal", true},
    {"DOUBLE", 8, "double", false},
    {"FLOAT", 6, "double", false},
    {"INTEGER", 4, "integer", false},
    {"REAL", 7, "real", false},
    {"SMALLINT", 5, "smallint", false},
    {"TINYINT", -6, "tinyint", false},
    {"TIME", 10, "time", true},
    {"TYPE_TIME", 92, "time", true},
    {"TIMESTAMP", 11, "timestamp", true},
    {"TYPE_TIMESTAMP", 93, "timestamp", true},
    {"GUID", -11, "uuid", false},
    {"INTERVAL_YEAR", 101, "interval year to month", false},
    {"INTERVAL_MONTH", 102, "interval year to month", false},
    {"INTERVAL_YEAR_TO_MONTH", 107, "interval year to month", false},
    {"INTERVAL_DAY", 103, "interval day to second", false},
    {"INTERVAL_HOUR", 104, "interval day to second", false},
    {"INTERVAL_MINUTE", 105, "interval day to second", false},
    {"INTERVAL_SECOND", 106, "interval day to second", false},
    {"INTERVAL_DAY_TO_HOUR", 108, "interval day to second", false},
    {"INTERVAL_DAY_TO_MINUTE", 109, "interval day to second", false},
    {"INTERVAL_DAY_TO_SECOND", 110, "interval day to second", false},
    {"INTERVAL_HOUR_TO_MINUTE", 111, "interval day to second", false},
    {"INTERVAL_HOUR_TO_SECOND", 112, "interval day to second", false},
    {"INTERVAL_MINUTE_TO_SECOND", 113, "interval day to second", false},
};

/// Renders the target type of {fn CONVERT(expr, type)}. Accepts SQL_xxx names, bare names,
/// numeric ODBC type codes and an optional (n)/(p,s) suffix on character/decimal/temporal types.
std::string convertTargetType(std::string_view typeToken) {
  const std::string_view full = trim(typeToken);
  std::string_view base = full;
  std::string_view suffix;
  const size_t lp = full.find('(');
  if (lp != npos) {
    base = trim(full.substr(0, lp));
    suffix = trim(full.substr(lp));
    if (suffix.empty() || suffix.back() != ')') syntaxError("Malformed type '" + std::string(full) + "' in ODBC CONVERT");
  }
  std::string upper = toUpper(base);
  if (startsWith(upper, "SQL_")) upper.erase(0, 4);
  int64_t code = 0;
  const bool numeric = parseInt64(upper, &code);
  for (const auto& t : kConvertTypes) {
    if (numeric ? (t.code == code) : (upper == t.odbcName)) {
      std::string out = t.trino;
      if (!suffix.empty() && t.takesSuffix) {
        out.append(suffix.data(), suffix.size());
      } else if (out == "decimal") {
        out = "decimal(38,10)";  // ODBC gives no precision; use a wide default
      }
      return out;
    }
  }
  syntaxError("Unknown SQL type '" + std::string(full) + "' in ODBC CONVERT");
}

// ---------------------------------------------------------------------------------------------
// Translator
// ---------------------------------------------------------------------------------------------

class Translator {
 public:
  explicit Translator(std::vector<std::string>* warnings) : warnings_(warnings) {}

  /// Copies `sql` replacing every top-level escape sequence; opaque units are copied verbatim.
  std::string translate(std::string_view sql, int depth) {
    if (depth > kMaxNesting) syntaxError("ODBC escape sequences nested too deeply");
    std::string out;
    out.reserve(sql.size());
    size_t i = 0;
    while (i < sql.size()) {
      const size_t end = skipOpaque(sql, i);
      if (end != npos) {
        out.append(sql.data() + i, end - i);
        i = end;
        continue;
      }
      const char c = sql[i];
      if (c == '{') {
        const size_t close = findMatchingBrace(sql, i);
        out.append(translateEscape(sql.substr(i + 1, close - i - 1), depth + 1));
        i = close + 1;
        continue;
      }
      if (c == '}') syntaxError("Malformed ODBC escape sequence: unexpected '}'");
      out.push_back(c);
      ++i;
    }
    return out;
  }

 private:
  /// `body` is the text between the braces.
  std::string translateEscape(std::string_view body, int depth) {
    const std::string_view b = trim(body);
    if (b.empty()) syntaxError("Malformed ODBC escape sequence: empty braces");
    if (b[0] == '?') {
      // {? = call proc(...)}: Trino procedures have no return value.
      const std::string_view rest = trim(b.substr(1));
      if (!rest.empty() && rest[0] == '=' && istartsWith(trim(rest.substr(1)), "call")) {
        throw DriverError(sqlstate::kOptionalFeatureNotImplemented,
                          "Procedure return values ({? = call ...}) are not supported by Trino");
      }
      syntaxError("Malformed ODBC escape sequence: '" + std::string(b) + "'");
    }
    size_t k = 0;
    while (k < b.size() && isIdentChar(b[k])) ++k;
    const std::string keyword = toUpper(b.substr(0, k));
    const std::string_view rest = trim(b.substr(k));

    if (keyword == "D") return literalEscape(rest, "DATE", "date", depth);
    if (keyword == "T") return literalEscape(rest, "TIME", "time", depth);
    if (keyword == "TS") return literalEscape(rest, "TIMESTAMP", "timestamp", depth);
    if (keyword == "INTERVAL") {
      if (rest.empty()) syntaxError("Malformed ODBC escape sequence: empty {interval}");
      return "INTERVAL " + translate(rest, depth);
    }
    if (keyword == "FN") return translateFunction(rest, depth);
    if (keyword == "OJ") {
      if (rest.empty()) syntaxError("Malformed ODBC escape sequence: empty {oj}");
      return translate(rest, depth);
    }
    if (keyword == "ESCAPE") {
      if (!isSingleStringLiteral(rest) && rest != "?") {
        syntaxError("Malformed ODBC escape sequence: {escape} requires a quoted character");
      }
      return "ESCAPE " + std::string(rest);
    }
    if (keyword == "CALL") {
      const std::string call = translate(rest, depth);
      const std::string_view c = trim(call);
      if (c.empty()) syntaxError("Malformed ODBC escape sequence: empty {call}");
      std::string out = "CALL ";
      out.append(c.data(), c.size());
      if (c.find('(') == npos) out.append("()");  // Trino requires the argument list
      return out;
    }
    syntaxError("Unknown ODBC escape keyword '" + std::string(b.substr(0, k)) + "'");
  }

  /// {d 'x'} -> DATE 'x'; a non-literal body ({d ?}, {ts {fn NOW()}}) becomes a CAST.
  std::string literalEscape(std::string_view rest, const char* keyword, const char* castType, int depth) {
    const std::string inner = translate(rest, depth);
    const std::string_view t = trim(inner);
    if (t.empty()) syntaxError(std::string("Malformed ODBC escape sequence: empty {") + keyword + "}");
    std::string out;
    if (isSingleStringLiteral(t)) {
      out = keyword;
      out.push_back(' ');
      out.append(t.data(), t.size());
    } else {
      out = "CAST(";
      out.append(t.data(), t.size());
      out.append(" AS ").append(castType).push_back(')');
    }
    return out;
  }

  /// `text` is `NAME(args)` (possibly with nested escapes, resolved first).
  std::string translateFunction(std::string_view text, int depth) {
    const std::string translated = translate(text, depth);
    const std::string_view t = trim(translated);
    size_t k = 0;
    while (k < t.size() && isIdentChar(t[k])) ++k;
    if (k == 0 || !isIdentStart(t[0])) syntaxError("Malformed ODBC escape sequence: {fn} without a function name");
    const std::string_view name = t.substr(0, k);
    const std::string upper = toUpper(name);
    const std::string_view rest = trim(t.substr(k));
    std::vector<std::string_view> args;
    if (!rest.empty()) {
      if (rest[0] != '(') syntaxError("Malformed ODBC escape sequence: expected '(' after {fn " + upper + "}");
      const size_t close = findMatchingParen(rest, 0);
      if (close == npos) syntaxError("Malformed ODBC escape sequence: missing ')' in {fn " + upper + "}");
      if (!trim(rest.substr(close + 1)).empty()) {
        syntaxError("Malformed ODBC escape sequence: unexpected text after {fn " + upper + "(...)}");
      }
      args = splitTopLevelArgs(rest.substr(1, close - 1));
    } else if (!isNiladic(upper) && findSimpleFn(upper) != nullptr) {
      syntaxError("Malformed ODBC escape sequence: {fn " + upper + "} requires an argument list");
    }
    return renderFunction(upper, name, args);
  }

  std::string renderFunction(const std::string& fn, std::string_view rawName, const std::vector<std::string_view>& args) {
    if (const SimpleFn* simple = findSimpleFn(fn)) {
      requireArgs(fn, args, simple->minArgs, simple->maxArgs);
      return callOf(simple->trino, args);
    }
    // string
    if (fn == "ASCII") {
      requireArgs(fn, args, 1, 1);
      // codepoint() rejects anything but a single character, so an empty string must become NULL
      // (what ODBC drivers return for ASCII('')) instead of failing the whole query.
      return "codepoint(CAST(nullif(substr(" + std::string(args[0]) + ", 1, 1), '') AS varchar(1)))";
    }
    if (fn == "BIT_LENGTH") {
      requireArgs(fn, args, 1, 1);
      return "(length(to_utf8(" + std::string(args[0]) + ")) * 8)";
    }
    if (fn == "OCTET_LENGTH") {
      requireArgs(fn, args, 1, 1);
      return "length(to_utf8(" + std::string(args[0]) + "))";
    }
    if (fn == "INSERT") {
      requireArgs(fn, args, 4, 4);
      const std::string s(args[0]), start = paren(args[1]), len = paren(args[2]), s2(args[3]);
      return "concat(substr(" + s + ", 1, " + start + " - 1), " + s2 + ", substr(" + s + ", " + start + " + " + len + "))";
    }
    if (fn == "LEFT") {
      requireArgs(fn, args, 2, 2);
      return "substr(" + std::string(args[0]) + ", 1, " + std::string(args[1]) + ")";
    }
    if (fn == "RIGHT") {
      requireArgs(fn, args, 2, 2);
      const std::string s(args[0]), n = paren(args[1]);
      return "substr(" + s + ", greatest(length(" + s + ") - " + n + " + 1, 1))";
    }
    if (fn == "LOCATE") {
      requireArgs(fn, args, 2, 3);
      const std::string a(args[0]), b(args[1]);
      if (args.size() == 2) return "strpos(" + b + ", " + a + ")";
      const std::string s = paren(args[2]);
      const std::string pos = "strpos(substr(" + b + ", " + s + "), " + a + ")";
      return "(CASE WHEN " + pos + " = 0 THEN 0 ELSE " + pos + " + " + s + " - 1 END)";
    }
    if (fn == "REPEAT") {
      requireArgs(fn, args, 2, 2);
      return "array_join(repeat(" + std::string(args[0]) + ", " + std::string(args[1]) + "), '')";
    }
    if (fn == "SPACE") {
      requireArgs(fn, args, 1, 1);
      return "array_join(repeat(' ', " + std::string(args[0]) + "), '')";
    }
    // numeric
    if (fn == "COT") {
      requireArgs(fn, args, 1, 1);
      return "(1 / tan(" + std::string(args[0]) + "))";
    }
    if (fn == "RAND") {
      requireArgs(fn, args, 0, 1);  // the optional seed cannot be honoured; random() is unseeded
      return "random()";
    }
    // time/date
    if (fn == "CURDATE" || fn == "CURRENT_DATE") {
      requireArgs(fn, args, 0, 0);
      return "current_date";
    }
    if (fn == "CURTIME" || fn == "CURRENT_TIME") {
      requireArgs(fn, args, 0, 1);
      return args.empty() ? "localtime" : "localtime(" + std::string(args[0]) + ")";
    }
    if (fn == "NOW" || fn == "CURRENT_TIMESTAMP") {
      requireArgs(fn, args, 0, 1);
      return args.empty() ? "localtimestamp" : "localtimestamp(" + std::string(args[0]) + ")";
    }
    if (fn == "DAYNAME") {
      requireArgs(fn, args, 1, 1);
      return "date_format(" + std::string(args[0]) + ", '%W')";
    }
    if (fn == "MONTHNAME") {
      requireArgs(fn, args, 1, 1);
      return "date_format(" + std::string(args[0]) + ", '%M')";
    }
    if (fn == "DAYOFWEEK") {
      requireArgs(fn, args, 1, 1);
      // ODBC: 1 = Sunday .. 7 = Saturday; Trino day_of_week: 1 = Monday .. 7 = Sunday.
      return "((day_of_week(" + std::string(args[0]) + ") % 7) + 1)";
    }
    if (fn == "TIMESTAMPADD") {
      requireArgs(fn, args, 3, 3);
      const std::string unit = intervalUnit(args[0], fn);
      if (unit == "millisecond") {
        // SQL_TSI_FRAC_SECOND counts billionths of a second; Trino's finest unit is the millisecond.
        return "date_add('millisecond', (" + std::string(args[1]) + ") / 1000000, " + std::string(args[2]) + ")";
      }
      return "date_add('" + unit + "', " + std::string(args[1]) + ", " + std::string(args[2]) + ")";
    }
    if (fn == "TIMESTAMPDIFF") {
      requireArgs(fn, args, 3, 3);
      const std::string unit = intervalUnit(args[0], fn);
      if (unit == "millisecond") {
        // Result in billionths of a second (SQL_TSI_FRAC_SECOND), computed at millisecond resolution.
        return "(date_diff('millisecond', " + std::string(args[1]) + ", " + std::string(args[2]) + ") * 1000000)";
      }
      return "date_diff('" + unit + "', " + std::string(args[1]) + ", " + std::string(args[2]) + ")";
    }
    // system
    if (fn == "DATABASE") {
      requireArgs(fn, args, 0, 0);
      return "current_catalog";
    }
    if (fn == "USER") {
      requireArgs(fn, args, 0, 0);
      return "current_user";
    }
    if (fn == "CONVERT") {
      requireArgs(fn, args, 2, 2);
      return "CAST(" + std::string(args[0]) + " AS " + convertTargetType(args[1]) + ")";
    }
    // Unknown: Trino may well have a function of that name, so pass it through and warn.
    const std::string message = "Unknown ODBC scalar function '" + std::string(rawName) + "' passed through to Trino unchanged";
    TRINO_LOG_WARN(kLog, "%s", message.c_str());
    if (warnings_) warnings_->push_back(message);
    return callOf(rawName, args);
  }

  std::vector<std::string>* warnings_;
};

// ---------------------------------------------------------------------------------------------
// Classification helpers
// ---------------------------------------------------------------------------------------------

/// Advances past whitespace and comments.
size_t skipBlanks(std::string_view sql, size_t pos) {
  while (pos < sql.size()) {
    if (isSpace(sql[pos])) {
      ++pos;
      continue;
    }
    if ((sql[pos] == '-' || sql[pos] == '/') && pos + 1 < sql.size() && (sql[pos + 1] == '-' || sql[pos + 1] == '*')) {
      const size_t end = skipOpaque(sql, pos);
      if (end != npos) {
        pos = end;
        continue;
      }
    }
    break;
  }
  return pos;
}

/// Reads the next bare word (identifier characters) after blanks; empty when none. Advances `pos`.
std::string_view readWord(std::string_view sql, size_t& pos) {
  pos = skipBlanks(sql, pos);
  const size_t start = pos;
  while (pos < sql.size() && isIdentChar(sql[pos])) ++pos;
  return sql.substr(start, pos - start);
}

/// Position of the statement's first keyword: blanks, comments and opening parentheses
/// ("(SELECT 1) UNION (SELECT 2)") are skipped.
size_t firstKeywordPos(std::string_view sql) {
  size_t pos = 0;
  for (;;) {
    pos = skipBlanks(sql, pos);
    if (pos < sql.size() && sql[pos] == '(') {
      ++pos;
      continue;
    }
    return pos;
  }
}

bool isOneOf(const std::string& w, std::initializer_list<const char*> names) {
  for (const char* n : names) {
    if (w == n) return true;
  }
  return false;
}

}  // namespace

std::string translateOdbcEscapes(std::string_view sql, std::vector<std::string>* warnings) {
  // Fast path: a statement without braces (the overwhelmingly common case) needs no rewriting.
  if (sql.find('{') == npos && sql.find('}') == npos) return std::string(sql);
  Translator translator(warnings);
  std::string out = translator.translate(sql, 0);
  if (TRINO_LOG_ENABLED(LogLevel::Trace) && out != sql) {
    TRINO_LOG_TRACE(kLog, "translated escapes: %s", out.c_str());
  }
  return out;
}

size_t countParameterMarkers(std::string_view sql) {
  size_t count = 0;
  size_t i = 0;
  while (i < sql.size()) {
    const size_t end = skipOpaque(sql, i);
    if (end != npos) {
      i = end;
      continue;
    }
    if (sql[i] == '?') ++count;
    ++i;
  }
  return count;
}

std::vector<std::string_view> splitOnParameterMarkers(std::string_view sql) {
  std::vector<std::string_view> pieces;
  size_t start = 0;
  size_t i = 0;
  while (i < sql.size()) {
    const size_t end = skipOpaque(sql, i);
    if (end != npos) {
      i = end;
      continue;
    }
    if (sql[i] == '?') {
      pieces.push_back(sql.substr(start, i - start));
      start = i + 1;
    }
    ++i;
  }
  pieces.push_back(sql.substr(start));
  return pieces;
}

std::string firstKeyword(std::string_view sql) {
  size_t pos = firstKeywordPos(sql);
  return toUpper(readWord(sql, pos));
}

SqlKind classifySql(std::string_view sql) {
  SqlKind kind;
  size_t pos = firstKeywordPos(sql);
  const std::string w = toUpper(readWord(sql, pos));
  kind.firstKeyword = w;
  if (isOneOf(w, {"SELECT", "WITH", "SHOW", "DESCRIBE", "DESC", "EXPLAIN", "VALUES", "TABLE"})) {
    kind.returnsRows = true;
  } else if (isOneOf(w, {"INSERT", "UPDATE", "DELETE", "MERGE"})) {
    kind.isDml = true;
  } else if (isOneOf(w, {"CREATE", "DROP", "ALTER", "COMMENT", "GRANT", "REVOKE", "DENY", "ANALYZE", "REFRESH",
                         "TRUNCATE"})) {
    kind.isDdl = true;
  } else if (isOneOf(w, {"START", "COMMIT", "ROLLBACK"})) {
    kind.isTransaction = true;
  } else if (w == "USE") {
    kind.isUse = true;
  } else if (isOneOf(w, {"SET", "RESET"})) {
    kind.isSet = true;  // SET SESSION / SET ROLE / SET PATH / SET TIME ZONE / SET SESSION AUTHORIZATION / RESET ...
  } else if (isOneOf(w, {"PREPARE", "DEALLOCATE"})) {
    kind.isPrepare = true;
  } else if (w == "EXECUTE") {
    kind.isPrepare = true;
    // EXECUTE name: whether rows come back depends on the prepared statement, which we cannot
    // see here; assume a result set so the statement layer keeps looking for columns.
    kind.returnsRows = true;
    // EXECUTE IMMEDIATE '<sql>' carries the statement inline, so classify that instead.
    size_t p = pos;
    if (iequals(readWord(sql, p), "IMMEDIATE")) {
      p = skipBlanks(sql, p);
      if (p < sql.size() && sql[p] == '\'') {
        bool terminated = false;
        const size_t end = skipQuoted(sql, p, '\'', &terminated);
        if (terminated) {
          const std::string inner = replaceAll(sql.substr(p + 1, end - p - 2), "''", "'");
          const SqlKind innerKind = classifySql(inner);
          kind.returnsRows = innerKind.returnsRows;
          kind.isDml = innerKind.isDml;
          kind.isDdl = innerKind.isDdl;
          kind.isTransaction = innerKind.isTransaction;
          kind.isUse = innerKind.isUse;
          kind.isSet = innerKind.isSet;
        }
      }
    }
  }
  // CALL and anything unrecognised: every flag stays false.
  return kind;
}

}  // namespace trino_odbc
