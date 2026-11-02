// Unit tests for the ODBC escape translator, parameter-marker scanner and statement classifier.
#include "unit/test_framework.h"

#include "sql/escape.h"
#include "util/errors.h"

#include <string>
#include <vector>

using namespace trino_odbc;

namespace {

std::string tr(const std::string& sql) { return translateOdbcEscapes(sql); }

/// SQLSTATE of the DriverError thrown by translating `sql`; "" when nothing was thrown.
std::string translateState(const std::string& sql) {
  try {
    (void)translateOdbcEscapes(sql);
  } catch (const DriverError& e) {
    return e.sqlstate();
  }
  return "";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Pass-through and opaque units
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: plain SQL is returned unchanged") {
  CHECK_EQ(tr("SELECT 1"), std::string("SELECT 1"));
  CHECK_EQ(tr(""), std::string(""));
  CHECK_EQ(tr("  select a, b\nfrom t where c = 'x' -- done\n"), std::string("  select a, b\nfrom t where c = 'x' -- done\n"));
}

TEST_CASE("escape: sequences inside string literals are untouched") {
  CHECK_EQ(tr("SELECT '{fn UCASE(x)}'"), std::string("SELECT '{fn UCASE(x)}'"));
  CHECK_EQ(tr("SELECT 'it''s {d ''2020-01-01''}' FROM t"), std::string("SELECT 'it''s {d ''2020-01-01''}' FROM t"));
  CHECK_EQ(tr("SELECT '{' , {fn UCASE(x)}, '}'"), std::string("SELECT '{' , upper(x), '}'"));
  CHECK_EQ(tr("SELECT U&'{fn x}', X'01'"), std::string("SELECT U&'{fn x}', X'01'"));
}

TEST_CASE("escape: sequences inside quoted identifiers are untouched") {
  CHECK_EQ(tr("SELECT \"{fn UCASE(x)}\" FROM t"), std::string("SELECT \"{fn UCASE(x)}\" FROM t"));
  CHECK_EQ(tr("SELECT \"a\"\"{b}\" FROM t"), std::string("SELECT \"a\"\"{b}\" FROM t"));
}

TEST_CASE("escape: sequences inside comments are untouched") {
  CHECK_EQ(tr("SELECT 1 -- {fn UCASE(x)}\n, {fn LCASE(y)}"), std::string("SELECT 1 -- {fn UCASE(x)}\n, lower(y)"));
  CHECK_EQ(tr("SELECT /* {d '2020-01-01'} */ {d '2020-01-01'}"), std::string("SELECT /* {d '2020-01-01'} */ DATE '2020-01-01'"));
  CHECK_EQ(tr("SELECT 1 -- unterminated { comment"), std::string("SELECT 1 -- unterminated { comment"));
}

TEST_CASE("escape: unterminated string or comment passes through without throwing") {
  CHECK_EQ(tr("SELECT 'abc"), std::string("SELECT 'abc"));
  CHECK_EQ(tr("SELECT 1 /* open {"), std::string("SELECT 1 /* open {"));
  CHECK_EQ(tr("SELECT \"open {"), std::string("SELECT \"open {"));
}

// ---------------------------------------------------------------------------------------------
// Date/time/interval literals
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: {d} {t} {ts} literals") {
  CHECK_EQ(tr("SELECT {d '2020-01-02'}"), std::string("SELECT DATE '2020-01-02'"));
  CHECK_EQ(tr("SELECT {t '12:34:56'}"), std::string("SELECT TIME '12:34:56'"));
  CHECK_EQ(tr("SELECT {ts '2020-01-02 03:04:05.123'}"), std::string("SELECT TIMESTAMP '2020-01-02 03:04:05.123'"));
  CHECK_EQ(tr("SELECT {TS '2020-01-02 03:04:05'}"), std::string("SELECT TIMESTAMP '2020-01-02 03:04:05'"));
  CHECK_EQ(tr("SELECT {  ts   '2020-01-02 03:04:05'  }"), std::string("SELECT TIMESTAMP '2020-01-02 03:04:05'"));
  CHECK_EQ(tr("SELECT {ts'2020-01-02 03:04:05'}"), std::string("SELECT TIMESTAMP '2020-01-02 03:04:05'"));
  CHECK_EQ(tr("SELECT {D'2020-01-02'}, {T '01:02:03'}"), std::string("SELECT DATE '2020-01-02', TIME '01:02:03'"));
}

TEST_CASE("escape: {d} {t} {ts} with non-literal bodies become CASTs") {
  CHECK_EQ(tr("SELECT {d ?}"), std::string("SELECT CAST(? AS date)"));
  CHECK_EQ(tr("SELECT {ts {fn NOW()}}"), std::string("SELECT CAST(localtimestamp AS timestamp)"));
  CHECK_EQ(tr("SELECT {t col}"), std::string("SELECT CAST(col AS time)"));
}

TEST_CASE("escape: {interval}") {
  CHECK_EQ(tr("SELECT {interval '1' DAY}"), std::string("SELECT INTERVAL '1' DAY"));
  CHECK_EQ(tr("SELECT {INTERVAL -'3' MONTH}"), std::string("SELECT INTERVAL -'3' MONTH"));
  CHECK_EQ(tr("SELECT {interval '1 02:03' DAY TO MINUTE}"), std::string("SELECT INTERVAL '1 02:03' DAY TO MINUTE"));
}

TEST_CASE("escape: datetime literals inside a WHERE clause keep surrounding text") {
  CHECK_EQ(tr("SELECT * FROM t WHERE d >= {d '2020-01-01'} AND d < {d '2021-01-01'} ORDER BY d"),
           std::string("SELECT * FROM t WHERE d >= DATE '2020-01-01' AND d < DATE '2021-01-01' ORDER BY d"));
}

// ---------------------------------------------------------------------------------------------
// {fn} string functions
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: {fn} string functions with simple renames") {
  CHECK_EQ(tr("{fn UCASE(name)}"), std::string("upper(name)"));
  CHECK_EQ(tr("{fn ucase(name)}"), std::string("upper(name)"));
  CHECK_EQ(tr("{FN LCASE(name)}"), std::string("lower(name)"));
  CHECK_EQ(tr("{fn LENGTH(name)}"), std::string("length(name)"));
  CHECK_EQ(tr("{fn CHAR_LENGTH(name)}"), std::string("length(name)"));
  CHECK_EQ(tr("{fn CHARACTER_LENGTH(name)}"), std::string("length(name)"));
  CHECK_EQ(tr("{fn LTRIM(name)}"), std::string("ltrim(name)"));
  CHECK_EQ(tr("{fn RTRIM(name)}"), std::string("rtrim(name)"));
  CHECK_EQ(tr("{fn SOUNDEX(name)}"), std::string("soundex(name)"));
  CHECK_EQ(tr("{fn CHAR(65)}"), std::string("chr(65)"));
  CHECK_EQ(tr("{fn CONCAT(a, b)}"), std::string("concat(a, b)"));
  CHECK_EQ(tr("{fn CONCAT(a,b,c)}"), std::string("concat(a, b, c)"));
  CHECK_EQ(tr("{fn REPLACE(s, 'a', 'b')}"), std::string("replace(s, 'a', 'b')"));
  CHECK_EQ(tr("{fn SUBSTRING(s, 2, 3)}"), std::string("substr(s, 2, 3)"));
  CHECK_EQ(tr("{fn SUBSTRING(s, 2)}"), std::string("substr(s, 2)"));
  CHECK_EQ(tr("{fn POSITION('a' IN s)}"), std::string("position('a' IN s)"));
}

TEST_CASE("escape: {fn} string functions with rewrites") {
  // nullif keeps ASCII('') a NULL instead of a codepoint() runtime failure (review finding 70).
  CHECK_EQ(tr("{fn ASCII(s)}"), std::string("codepoint(CAST(nullif(substr(s, 1, 1), '') AS varchar(1)))"));
  CHECK_EQ(tr("{fn BIT_LENGTH(s)}"), std::string("(length(to_utf8(s)) * 8)"));
  CHECK_EQ(tr("{fn OCTET_LENGTH(s)}"), std::string("length(to_utf8(s))"));
  CHECK_EQ(tr("{fn LEFT(s, 3)}"), std::string("substr(s, 1, 3)"));
  CHECK_EQ(tr("{fn RIGHT(s, 3)}"), std::string("substr(s, greatest(length(s) - 3 + 1, 1))"));
  CHECK_EQ(tr("{fn RIGHT(s, n + 1)}"), std::string("substr(s, greatest(length(s) - (n + 1) + 1, 1))"));
  CHECK_EQ(tr("{fn LOCATE('a', s)}"), std::string("strpos(s, 'a')"));
  CHECK_EQ(tr("{fn LOCATE('a,b', s)}"), std::string("strpos(s, 'a,b')"));
  CHECK_EQ(tr("{fn LOCATE(a, b, 5)}"),
           std::string("(CASE WHEN strpos(substr(b, 5), a) = 0 THEN 0 ELSE strpos(substr(b, 5), a) + 5 - 1 END)"));
  CHECK_EQ(tr("{fn LOCATE(a, b, x+1)}"),
           std::string("(CASE WHEN strpos(substr(b, (x+1)), a) = 0 THEN 0 ELSE strpos(substr(b, (x+1)), a) + (x+1) - 1 END)"));
  CHECK_EQ(tr("{fn REPEAT('ab', 3)}"), std::string("array_join(repeat('ab', 3), '')"));
  CHECK_EQ(tr("{fn SPACE(4)}"), std::string("array_join(repeat(' ', 4), '')"));
  CHECK_EQ(tr("{fn INSERT(s, 2, 3, 'xy')}"), std::string("concat(substr(s, 1, 2 - 1), 'xy', substr(s, 2 + 3))"));
  CHECK_EQ(tr("{fn INSERT(s, a-1, b*2, 'xy')}"),
           std::string("concat(substr(s, 1, (a-1) - 1), 'xy', substr(s, (a-1) + (b*2)))"));
}

// ---------------------------------------------------------------------------------------------
// {fn} numeric functions
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: {fn} numeric functions") {
  CHECK_EQ(tr("{fn ABS(x)}"), std::string("abs(x)"));
  CHECK_EQ(tr("{fn ACOS(x)}"), std::string("acos(x)"));
  CHECK_EQ(tr("{fn ATAN2(y, x)}"), std::string("atan2(y, x)"));
  CHECK_EQ(tr("{fn CEILING(x)}"), std::string("ceiling(x)"));
  CHECK_EQ(tr("{fn COT(x)}"), std::string("(1 / tan(x))"));
  CHECK_EQ(tr("{fn DEGREES(x)}"), std::string("degrees(x)"));
  CHECK_EQ(tr("{fn EXP(x)}"), std::string("exp(x)"));
  CHECK_EQ(tr("{fn FLOOR(x)}"), std::string("floor(x)"));
  CHECK_EQ(tr("{fn LOG(x)}"), std::string("ln(x)"));
  CHECK_EQ(tr("{fn LOG10(x)}"), std::string("log10(x)"));
  CHECK_EQ(tr("{fn MOD(a, b)}"), std::string("mod(a, b)"));
  CHECK_EQ(tr("{fn PI()}"), std::string("pi()"));
  CHECK_EQ(tr("{fn POWER(a, 2)}"), std::string("power(a, 2)"));
  CHECK_EQ(tr("{fn RADIANS(x)}"), std::string("radians(x)"));
  CHECK_EQ(tr("{fn RAND()}"), std::string("random()"));
  CHECK_EQ(tr("{fn RAND(42)}"), std::string("random()"));
  CHECK_EQ(tr("{fn ROUND(x, 2)}"), std::string("round(x, 2)"));
  CHECK_EQ(tr("{fn ROUND(x)}"), std::string("round(x)"));
  CHECK_EQ(tr("{fn SIGN(x)}"), std::string("sign(x)"));
  CHECK_EQ(tr("{fn SQRT(x)}"), std::string("sqrt(x)"));
  CHECK_EQ(tr("{fn TAN(x)}"), std::string("tan(x)"));
  CHECK_EQ(tr("{fn TRUNCATE(x)}"), std::string("truncate(x)"));
  CHECK_EQ(tr("{fn TRUNCATE(x, 2)}"), std::string("truncate(x, 2)"));
}

// ---------------------------------------------------------------------------------------------
// {fn} time/date functions
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: {fn} current date/time functions") {
  CHECK_EQ(tr("{fn CURDATE()}"), std::string("current_date"));
  CHECK_EQ(tr("{fn CURRENT_DATE()}"), std::string("current_date"));
  CHECK_EQ(tr("{fn CURRENT_DATE}"), std::string("current_date"));
  CHECK_EQ(tr("{fn CURTIME()}"), std::string("localtime"));
  CHECK_EQ(tr("{fn CURRENT_TIME()}"), std::string("localtime"));
  CHECK_EQ(tr("{fn CURRENT_TIME(3)}"), std::string("localtime(3)"));
  CHECK_EQ(tr("{fn NOW()}"), std::string("localtimestamp"));
  CHECK_EQ(tr("{fn CURRENT_TIMESTAMP()}"), std::string("localtimestamp"));
  CHECK_EQ(tr("{fn CURRENT_TIMESTAMP(6)}"), std::string("localtimestamp(6)"));
}

TEST_CASE("escape: {fn} date part functions") {
  CHECK_EQ(tr("{fn DAYNAME(d)}"), std::string("date_format(d, '%W')"));
  CHECK_EQ(tr("{fn MONTHNAME(d)}"), std::string("date_format(d, '%M')"));
  CHECK_EQ(tr("{fn DAYOFMONTH(d)}"), std::string("day(d)"));
  CHECK_EQ(tr("{fn DAYOFWEEK(d)}"), std::string("((day_of_week(d) % 7) + 1)"));
  CHECK_EQ(tr("{fn DAYOFYEAR(d)}"), std::string("day_of_year(d)"));
  CHECK_EQ(tr("{fn HOUR(t)}"), std::string("hour(t)"));
  CHECK_EQ(tr("{fn MINUTE(t)}"), std::string("minute(t)"));
  CHECK_EQ(tr("{fn SECOND(t)}"), std::string("second(t)"));
  CHECK_EQ(tr("{fn MONTH(d)}"), std::string("month(d)"));
  CHECK_EQ(tr("{fn QUARTER(d)}"), std::string("quarter(d)"));
  CHECK_EQ(tr("{fn WEEK(d)}"), std::string("week(d)"));
  CHECK_EQ(tr("{fn YEAR(d)}"), std::string("year(d)"));
  CHECK_EQ(tr("{fn EXTRACT(YEAR FROM d)}"), std::string("extract(YEAR FROM d)"));
}

TEST_CASE("escape: {fn} TIMESTAMPADD / TIMESTAMPDIFF") {
  CHECK_EQ(tr("{fn TIMESTAMPADD(SQL_TSI_DAY, 1, d)}"), std::string("date_add('day', 1, d)"));
  // SQL_TSI_FRAC_SECOND counts billionths of a second (review finding 35).
  CHECK_EQ(tr("{fn TIMESTAMPADD(SQL_TSI_FRAC_SECOND, 500000000, ts)}"),
           std::string("date_add('millisecond', (500000000) / 1000000, ts)"));
  CHECK_EQ(tr("{fn TIMESTAMPADD(SQL_TSI_SECOND, 5, ts)}"), std::string("date_add('second', 5, ts)"));
  CHECK_EQ(tr("{fn TIMESTAMPADD(SQL_TSI_MINUTE, 5, ts)}"), std::string("date_add('minute', 5, ts)"));
  CHECK_EQ(tr("{fn TIMESTAMPADD(SQL_TSI_HOUR, 5, ts)}"), std::string("date_add('hour', 5, ts)"));
  CHECK_EQ(tr("{fn TIMESTAMPADD(SQL_TSI_WEEK, 5, ts)}"), std::string("date_add('week', 5, ts)"));
  CHECK_EQ(tr("{fn TIMESTAMPADD(SQL_TSI_MONTH, 5, ts)}"), std::string("date_add('month', 5, ts)"));
  CHECK_EQ(tr("{fn TIMESTAMPADD(SQL_TSI_QUARTER, 5, ts)}"), std::string("date_add('quarter', 5, ts)"));
  CHECK_EQ(tr("{fn TIMESTAMPADD(sql_tsi_year, -1, ts)}"), std::string("date_add('year', -1, ts)"));
  CHECK_EQ(tr("{fn TIMESTAMPADD(DAY, 1, d)}"), std::string("date_add('day', 1, d)"));
  CHECK_EQ(tr("{fn TIMESTAMPDIFF(SQL_TSI_DAY, a, b)}"), std::string("date_diff('day', a, b)"));
  CHECK_EQ(tr("{fn TIMESTAMPDIFF(SQL_TSI_FRAC_SECOND, a, b)}"), std::string("(date_diff('millisecond', a, b) * 1000000)"));
  CHECK_EQ(tr("{fn TIMESTAMPDIFF(SQL_TSI_YEAR, {d '2020-01-01'}, {fn CURDATE()})}"),
           std::string("date_diff('year', DATE '2020-01-01', current_date)"));
  CHECK_EQ(translateState("{fn TIMESTAMPADD(SQL_TSI_FORTNIGHT, 1, d)}"), std::string("42000"));
  CHECK_EQ(translateState("{fn TIMESTAMPDIFF(SQL_TSI_DAY, a)}"), std::string("42000"));
}

// ---------------------------------------------------------------------------------------------
// {fn} system functions and CONVERT
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: {fn} system functions") {
  CHECK_EQ(tr("{fn DATABASE()}"), std::string("current_catalog"));
  CHECK_EQ(tr("{fn USER()}"), std::string("current_user"));
  CHECK_EQ(tr("{fn IFNULL(a, 0)}"), std::string("coalesce(a, 0)"));
}

TEST_CASE("escape: {fn CONVERT} type map") {
  CHECK_EQ(tr("{fn CONVERT(x, SQL_BIGINT)}"), std::string("CAST(x AS bigint)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_BINARY)}"), std::string("CAST(x AS varbinary)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_VARBINARY)}"), std::string("CAST(x AS varbinary)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_LONGVARBINARY)}"), std::string("CAST(x AS varbinary)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_BIT)}"), std::string("CAST(x AS boolean)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_CHAR)}"), std::string("CAST(x AS varchar)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_VARCHAR)}"), std::string("CAST(x AS varchar)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_LONGVARCHAR)}"), std::string("CAST(x AS varchar)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_WCHAR)}"), std::string("CAST(x AS varchar)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_WVARCHAR)}"), std::string("CAST(x AS varchar)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_WLONGVARCHAR)}"), std::string("CAST(x AS varchar)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_DATE)}"), std::string("CAST(x AS date)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_TYPE_DATE)}"), std::string("CAST(x AS date)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_DECIMAL)}"), std::string("CAST(x AS decimal(38,10))"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_NUMERIC)}"), std::string("CAST(x AS decimal(38,10))"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_DOUBLE)}"), std::string("CAST(x AS double)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_FLOAT)}"), std::string("CAST(x AS double)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_INTEGER)}"), std::string("CAST(x AS integer)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_REAL)}"), std::string("CAST(x AS real)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_SMALLINT)}"), std::string("CAST(x AS smallint)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_TINYINT)}"), std::string("CAST(x AS tinyint)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_TIME)}"), std::string("CAST(x AS time)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_TYPE_TIME)}"), std::string("CAST(x AS time)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_TIMESTAMP)}"), std::string("CAST(x AS timestamp)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_TYPE_TIMESTAMP)}"), std::string("CAST(x AS timestamp)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_GUID)}"), std::string("CAST(x AS uuid)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_INTERVAL_YEAR_TO_MONTH)}"), std::string("CAST(x AS interval year to month)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_INTERVAL_DAY_TO_SECOND)}"), std::string("CAST(x AS interval day to second)"));
}

TEST_CASE("escape: {fn CONVERT} accepts case, codes, bare names and suffixes") {
  CHECK_EQ(tr("{fn convert(x, sql_varchar)}"), std::string("CAST(x AS varchar)"));
  CHECK_EQ(tr("{fn CONVERT(x, VARCHAR)}"), std::string("CAST(x AS varchar)"));
  CHECK_EQ(tr("{fn CONVERT(x, 12)}"), std::string("CAST(x AS varchar)"));
  CHECK_EQ(tr("{fn CONVERT(x, -5)}"), std::string("CAST(x AS bigint)"));
  CHECK_EQ(tr("{fn CONVERT(x, 93)}"), std::string("CAST(x AS timestamp)"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_VARCHAR(20))}"), std::string("CAST(x AS varchar(20))"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_DECIMAL(10, 2))}"), std::string("CAST(x AS decimal(10, 2))"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_TIMESTAMP(3))}"), std::string("CAST(x AS timestamp(3))"));
  CHECK_EQ(tr("{fn CONVERT(x, SQL_INTEGER(5))}"), std::string("CAST(x AS integer)"));
  CHECK_EQ(tr("{fn CONVERT(?, SQL_INTEGER)}"), std::string("CAST(? AS integer)"));
  CHECK_EQ(tr("{fn CONVERT(a + b, SQL_DOUBLE)}"), std::string("CAST(a + b AS double)"));
  CHECK_EQ(translateState("{fn CONVERT(x, SQL_NOPE)}"), std::string("42000"));
  CHECK_EQ(translateState("{fn CONVERT(x)}"), std::string("42000"));
  CHECK_EQ(translateState("{fn CONVERT(x, SQL_VARCHAR(}"), std::string("42000"));
}

// ---------------------------------------------------------------------------------------------
// Nesting
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: nested {fn} sequences") {
  CHECK_EQ(tr("{fn LEFT({fn UCASE(name)}, 3)}"), std::string("substr(upper(name), 1, 3)"));
  CHECK_EQ(tr("SELECT {fn LEFT({fn UCASE(name)}, 3)} AS x FROM t"), std::string("SELECT substr(upper(name), 1, 3) AS x FROM t"));
  CHECK_EQ(tr("{fn CONVERT({fn NOW()}, SQL_DATE)}"), std::string("CAST(localtimestamp AS date)"));
  CHECK_EQ(tr("{fn TIMESTAMPADD(SQL_TSI_DAY, 1, {d '2020-01-01'})}"), std::string("date_add('day', 1, DATE '2020-01-01')"));
  CHECK_EQ(tr("{fn IFNULL({fn LTRIM({fn RTRIM({fn LCASE(s)})})}, {fn SPACE(1)})}"),
           std::string("coalesce(ltrim(rtrim(lower(s))), array_join(repeat(' ', 1), ''))"));
  CHECK_EQ(tr("{fn LOCATE({fn UCASE('a')}, {fn UCASE(s)}, {fn ABS(n)})}"),
           std::string("(CASE WHEN strpos(substr(upper(s), (abs(n))), upper('a')) = 0 THEN 0 ELSE strpos(substr(upper(s), (abs(n))), upper('a')) + (abs(n)) - 1 END)"));
}

TEST_CASE("escape: comments and strings inside function arguments") {
  CHECK_EQ(tr("{fn UCASE(/* c */ x)}"), std::string("upper(/* c */ x)"));
  CHECK_EQ(tr("{fn CONCAT('a)b', \"we,ird\")}"), std::string("concat('a)b', \"we,ird\")"));
  CHECK_EQ(tr("{fn UCASE(f(a, b))}"), std::string("upper(f(a, b))"));
  CHECK_EQ(tr("{fn UCASE(ARRAY[1, 2][1])}"), std::string("upper(ARRAY[1, 2][1])"));
  CHECK_EQ(tr("{fn UCASE(CASE WHEN a THEN 'x,y' ELSE 'z' END)}"), std::string("upper(CASE WHEN a THEN 'x,y' ELSE 'z' END)"));
}

TEST_CASE("escape: several escapes in one statement keep every other character") {
  const std::string in = "SELECT {fn UCASE(a)},{fn LCASE(b)} , {d '2020-01-01'}\n\tFROM t WHERE x = {fn CONVERT(?, SQL_INTEGER)}";
  const std::string out = "SELECT upper(a),lower(b) , DATE '2020-01-01'\n\tFROM t WHERE x = CAST(? AS integer)";
  CHECK_EQ(tr(in), out);
}

TEST_CASE("escape: many escapes in one statement") {
  std::string in = "SELECT ";
  std::string expected = "SELECT ";
  for (int i = 0; i < 2000; ++i) {
    if (i) {
      in += ", ";
      expected += ", ";
    }
    in += "{fn UCASE(c" + std::to_string(i) + ")}";
    expected += "upper(c" + std::to_string(i) + ")";
  }
  CHECK_EQ(tr(in), expected);
}

// ---------------------------------------------------------------------------------------------
// {oj}, {escape}, {call}
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: {oj}") {
  CHECK_EQ(tr("SELECT * FROM {oj a LEFT OUTER JOIN b ON a.id = b.id}"), std::string("SELECT * FROM a LEFT OUTER JOIN b ON a.id = b.id"));
  CHECK_EQ(tr("SELECT * FROM {OJ a LEFT OUTER JOIN b ON {fn UCASE(a.k)} = b.k}"),
           std::string("SELECT * FROM a LEFT OUTER JOIN b ON upper(a.k) = b.k"));
  CHECK_EQ(translateState("SELECT * FROM {oj }"), std::string("42000"));
}

TEST_CASE("escape: {escape}") {
  CHECK_EQ(tr("SELECT * FROM t WHERE n LIKE 'a\\%' {escape '\\'}"), std::string("SELECT * FROM t WHERE n LIKE 'a\\%' ESCAPE '\\'"));
  CHECK_EQ(tr("... LIKE ? {ESCAPE '!'}"), std::string("... LIKE ? ESCAPE '!'"));
  CHECK_EQ(translateState("LIKE 'x' {escape \\}"), std::string("42000"));
}

TEST_CASE("escape: {call}") {
  CHECK_EQ(tr("{call system.runtime.kill_query('q', 'why')}"), std::string("CALL system.runtime.kill_query('q', 'why')"));
  CHECK_EQ(tr("{CALL proc}"), std::string("CALL proc()"));
  CHECK_EQ(tr("{call sch.proc(?, {d '2020-01-01'})}"), std::string("CALL sch.proc(?, DATE '2020-01-01')"));
  CHECK_EQ(translateState("{call}"), std::string("42000"));
}

TEST_CASE("escape: {? = call} is HYC00") {
  CHECK_THROWS_AS(translateOdbcEscapes("{? = call proc(1)}"), DriverError);
  CHECK_EQ(translateState("{? = call proc(1)}"), std::string("HYC00"));
  CHECK_EQ(translateState("{?=CALL proc}"), std::string("HYC00"));
  CHECK_EQ(translateState("{? proc}"), std::string("42000"));
}

// ---------------------------------------------------------------------------------------------
// Unknown functions and warnings
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: unknown {fn} passes through with a warning") {
  std::vector<std::string> warnings;
  CHECK_EQ(translateOdbcEscapes("SELECT {fn FOO(a, b)}", &warnings), std::string("SELECT FOO(a, b)"));
  REQUIRE_EQ(warnings.size(), static_cast<size_t>(1));
  CHECK(warnings[0].find("FOO") != std::string::npos);
  warnings.clear();
  CHECK_EQ(translateOdbcEscapes("{fn regexp_like(s, 'x')}", &warnings), std::string("regexp_like(s, 'x')"));
  CHECK_EQ(warnings.size(), static_cast<size_t>(1));
  CHECK_EQ(translateOdbcEscapes("{fn bar()}", &warnings), std::string("bar()"));
  CHECK_EQ(warnings.size(), static_cast<size_t>(2));
  // null warnings vector must be tolerated
  CHECK_EQ(translateOdbcEscapes("{fn FOO(x)}", nullptr), std::string("FOO(x)"));
}

TEST_CASE("escape: known functions produce no warnings") {
  std::vector<std::string> warnings;
  (void)translateOdbcEscapes("SELECT {fn UCASE(a)}, {fn LEFT(b, 1)}, {d '2020-01-01'}", &warnings);
  CHECK(warnings.empty());
}

// ---------------------------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------------------------

TEST_CASE("escape: malformed sequences raise 42000") {
  CHECK_THROWS_AS(translateOdbcEscapes("SELECT {fn UCASE(x)"), DriverError);
  CHECK_EQ(translateState("SELECT {fn UCASE(x)"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn UCASE(x}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT }"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {   }"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn (x)}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {bogus 1}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn UCASE(x) + 1}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn UCASE x}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn UCASE}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn LEFT(a)}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn UCASE(a, b)}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn UCASE(a, )}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn UCASE(a))}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {d }"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {interval}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn UCASE({fn LCASE(x)}"), std::string("42000"));
  CHECK_EQ(translateState("SELECT {fn UCASE('unterminated)}"), std::string("42000"));
}

TEST_CASE("escape: pathological nesting is rejected, moderate nesting works") {
  std::string deep;
  for (int i = 0; i < 100; ++i) deep += "{fn ABS(";
  deep += "x";
  for (int i = 0; i < 100; ++i) deep += ")}";
  CHECK_EQ(translateState(deep), std::string("42000"));

  std::string ok;
  std::string expected;
  for (int i = 0; i < 20; ++i) {
    ok += "{fn ABS(";
    expected += "abs(";
  }
  ok += "x";
  expected += "x";
  for (int i = 0; i < 20; ++i) {
    ok += ")}";
    expected += ")";
  }
  CHECK_EQ(tr(ok), expected);
}

// ---------------------------------------------------------------------------------------------
// Parameter markers
// ---------------------------------------------------------------------------------------------

TEST_CASE("markers: count ignores strings, identifiers and comments") {
  CHECK_EQ(countParameterMarkers("SELECT 1"), static_cast<size_t>(0));
  CHECK_EQ(countParameterMarkers("SELECT ?"), static_cast<size_t>(1));
  CHECK_EQ(countParameterMarkers("SELECT ?, ?, ?"), static_cast<size_t>(3));
  CHECK_EQ(countParameterMarkers("SELECT '?'"), static_cast<size_t>(0));
  CHECK_EQ(countParameterMarkers("SELECT 'it''s ?', ?"), static_cast<size_t>(1));
  CHECK_EQ(countParameterMarkers("SELECT \"?\" FROM t WHERE a = ?"), static_cast<size_t>(1));
  CHECK_EQ(countParameterMarkers("SELECT 1 -- ?\n"), static_cast<size_t>(0));
  CHECK_EQ(countParameterMarkers("SELECT /* ? */ ? -- ?"), static_cast<size_t>(1));
  CHECK_EQ(countParameterMarkers("SELECT ? , '?' , \"?\" , -- ?\n /* ? */ ?"), static_cast<size_t>(2));
  CHECK_EQ(countParameterMarkers("SELECT {fn UCASE(?)} FROM t WHERE x = {fn CONVERT(?, SQL_INTEGER)}"), static_cast<size_t>(2));
  CHECK_EQ(countParameterMarkers("SELECT upper(?) FROM t WHERE x = CAST(? AS integer)"), static_cast<size_t>(2));
  CHECK_EQ(countParameterMarkers("??"), static_cast<size_t>(2));
  CHECK_EQ(countParameterMarkers("SELECT '?"), static_cast<size_t>(0));
}

TEST_CASE("markers: split yields n+1 pieces that rebuild the SQL") {
  const std::string sql = "INSERT INTO t VALUES (?, '?', ?, /* ? */ ?)";
  auto pieces = splitOnParameterMarkers(sql);
  REQUIRE_EQ(pieces.size(), static_cast<size_t>(4));
  CHECK_EQ(std::string(pieces[0]), std::string("INSERT INTO t VALUES ("));
  CHECK_EQ(std::string(pieces[1]), std::string(", '?', "));
  CHECK_EQ(std::string(pieces[2]), std::string(", /* ? */ "));
  CHECK_EQ(std::string(pieces[3]), std::string(")"));
  std::string rebuilt;
  for (size_t i = 0; i < pieces.size(); ++i) {
    rebuilt.append(pieces[i].data(), pieces[i].size());
    if (i + 1 < pieces.size()) rebuilt += "X";
  }
  CHECK_EQ(rebuilt, std::string("INSERT INTO t VALUES (X, '?', X, /* ? */ X)"));
  CHECK_EQ(pieces.size(), countParameterMarkers(sql) + 1);
}

TEST_CASE("markers: split without markers or at the edges") {
  auto none = splitOnParameterMarkers("SELECT 1");
  REQUIRE_EQ(none.size(), static_cast<size_t>(1));
  CHECK_EQ(std::string(none[0]), std::string("SELECT 1"));

  auto edges = splitOnParameterMarkers("?a?");
  REQUIRE_EQ(edges.size(), static_cast<size_t>(3));
  CHECK_EQ(std::string(edges[0]), std::string(""));
  CHECK_EQ(std::string(edges[1]), std::string("a"));
  CHECK_EQ(std::string(edges[2]), std::string(""));

  auto empty = splitOnParameterMarkers("");
  REQUIRE_EQ(empty.size(), static_cast<size_t>(1));
  CHECK(empty[0].empty());
}

// ---------------------------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------------------------

TEST_CASE("classify: statements returning rows") {
  const char* rows[] = {
      "SELECT 1",
      "  select a from t",
      "/* c */ WITH t AS (SELECT 1) SELECT * FROM t",
      "-- comment\nSELECT 1",
      "(SELECT 1) UNION (SELECT 2)",
      "((SELECT 1))",
      "EXPLAIN SELECT 1",
      "EXPLAIN ANALYZE SELECT 1",
      "SHOW CATALOGS",
      "SHOW TABLES FROM x.y",
      "DESCRIBE t",
      "DESC t",
      "DESCRIBE INPUT s",
      "DESCRIBE OUTPUT s",
      "VALUES (1, 2)",
      "TABLE t",
  };
  for (const char* sql : rows) {
    const SqlKind k = classifySql(sql);
    CHECK(k.returnsRows);
    CHECK(!k.isDml);
    CHECK(!k.isDdl);
    CHECK(!k.isTransaction);
    CHECK(!k.isUse);
    CHECK(!k.isSet);
    CHECK(!k.isPrepare);
  }
  CHECK_EQ(classifySql("  select a from t").firstKeyword, std::string("SELECT"));
  CHECK_EQ(classifySql("(SELECT 1) UNION (SELECT 2)").firstKeyword, std::string("SELECT"));
  CHECK_EQ(classifySql("/* c */ WITH t AS (SELECT 1) SELECT * FROM t").firstKeyword, std::string("WITH"));
}

TEST_CASE("classify: DML") {
  const char* dml[] = {"INSERT INTO t VALUES (1)", "update t set a = 1", "DELETE FROM t WHERE 1 = 1",
                       "MERGE INTO t USING s ON t.id = s.id WHEN MATCHED THEN DELETE"};
  for (const char* sql : dml) {
    const SqlKind k = classifySql(sql);
    CHECK(k.isDml);
    CHECK(!k.returnsRows);
    CHECK(!k.isDdl);
  }
  CHECK_EQ(classifySql("update t set a = 1").firstKeyword, std::string("UPDATE"));
}

TEST_CASE("classify: DDL") {
  const char* ddl[] = {"CREATE TABLE t (a int)", "DROP TABLE t", "ALTER TABLE t ADD COLUMN b int",
                       "COMMENT ON TABLE t IS 'x'", "GRANT SELECT ON t TO u", "REVOKE SELECT ON t FROM u",
                       "DENY SELECT ON t TO u", "ANALYZE t", "REFRESH MATERIALIZED VIEW v", "TRUNCATE TABLE t"};
  for (const char* sql : ddl) {
    const SqlKind k = classifySql(sql);
    CHECK(k.isDdl);
    CHECK(!k.isDml);
    CHECK(!k.returnsRows);
  }
}

TEST_CASE("classify: transactions, USE, SET/RESET, PREPARE") {
  CHECK(classifySql("START TRANSACTION").isTransaction);
  CHECK(classifySql("START TRANSACTION ISOLATION LEVEL SERIALIZABLE").isTransaction);
  CHECK(classifySql("COMMIT").isTransaction);
  CHECK(classifySql("commit work").isTransaction);
  CHECK(classifySql("ROLLBACK").isTransaction);
  CHECK(!classifySql("COMMIT").returnsRows);

  CHECK(classifySql("USE hive.default").isUse);
  CHECK(!classifySql("USE hive.default").returnsRows);

  CHECK(classifySql("SET SESSION query_max_run_time = '1h'").isSet);
  CHECK(classifySql("SET ROLE admin").isSet);
  CHECK(classifySql("SET PATH a.b").isSet);
  CHECK(classifySql("SET TIME ZONE 'UTC'").isSet);
  CHECK(classifySql("SET SESSION AUTHORIZATION bob").isSet);
  CHECK(classifySql("RESET SESSION query_max_run_time").isSet);
  CHECK(classifySql("RESET SESSION AUTHORIZATION").isSet);
  CHECK(!classifySql("SET ROLE admin").returnsRows);

  const SqlKind prep = classifySql("PREPARE s FROM SELECT 1");
  CHECK(prep.isPrepare);
  CHECK(!prep.returnsRows);
  CHECK(classifySql("DEALLOCATE PREPARE s").isPrepare);
  CHECK(!classifySql("DEALLOCATE PREPARE s").returnsRows);

  const SqlKind exec = classifySql("EXECUTE s USING 1");
  CHECK(exec.isPrepare);
  CHECK(exec.returnsRows);
  CHECK(!exec.isDml);
  CHECK_EQ(exec.firstKeyword, std::string("EXECUTE"));
}

TEST_CASE("classify: EXECUTE IMMEDIATE classifies the inline statement") {
  const SqlKind ins = classifySql("EXECUTE IMMEDIATE 'INSERT INTO t VALUES (''a'')' USING 1");
  CHECK(ins.isPrepare);
  CHECK(ins.isDml);
  CHECK(!ins.returnsRows);
  CHECK_EQ(ins.firstKeyword, std::string("EXECUTE"));

  const SqlKind sel = classifySql("execute immediate 'select * from t where a = ?' using 1");
  CHECK(sel.isPrepare);
  CHECK(sel.returnsRows);
  CHECK(!sel.isDml);

  const SqlKind ddl = classifySql("EXECUTE IMMEDIATE '  /* x */ CREATE TABLE t (a int)'");
  CHECK(ddl.isDdl);
  CHECK(!ddl.returnsRows);

  // Unterminated literal: fall back to the generic EXECUTE classification.
  const SqlKind broken = classifySql("EXECUTE IMMEDIATE 'INSERT INTO t");
  CHECK(broken.isPrepare);
  CHECK(broken.returnsRows);
  CHECK(!broken.isDml);
}

TEST_CASE("classify: CALL and unknown statements have no flags") {
  const SqlKind call = classifySql("CALL system.runtime.kill_query('q')");
  CHECK(!call.returnsRows);
  CHECK(!call.isDml);
  CHECK(!call.isDdl);
  CHECK(!call.isTransaction);
  CHECK(!call.isUse);
  CHECK(!call.isSet);
  CHECK(!call.isPrepare);
  CHECK_EQ(call.firstKeyword, std::string("CALL"));

  const SqlKind empty = classifySql("");
  CHECK(!empty.returnsRows);
  CHECK_EQ(empty.firstKeyword, std::string(""));
  CHECK_EQ(classifySql("   /* only a comment */  ").firstKeyword, std::string(""));
  CHECK_EQ(classifySql("-- only\n").firstKeyword, std::string(""));
  CHECK(!classifySql("42").returnsRows);
}

TEST_CASE("classify: firstKeyword") {
  CHECK_EQ(firstKeyword("select 1"), std::string("SELECT"));
  CHECK_EQ(firstKeyword("  \n\t SELECT 1"), std::string("SELECT"));
  CHECK_EQ(firstKeyword("-- c\n/* d */ Insert into t"), std::string("INSERT"));
  CHECK_EQ(firstKeyword("(select 1)"), std::string("SELECT"));
  CHECK_EQ(firstKeyword("with_x_1 y"), std::string("WITH_X_1"));
  CHECK_EQ(firstKeyword(""), std::string(""));
  CHECK_EQ(firstKeyword("/* unterminated"), std::string(""));
  CHECK_EQ(firstKeyword("'string first'"), std::string(""));
}
