#pragma once
// Trino statement-response JSON decoding (simdjson on-demand). Turns a `/v1/statement` response
// into typed Pages plus the protocol metadata (columns, stats, error, warnings, spooled segments).
// This header deliberately does not include simdjson so that every consumer stays codec-free.
#include "client/trino_client.h"
#include "result/page.h"
#include "result/row_source.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trino_odbc {

/// Spare bytes a JSON buffer must carry beyond its logical size before it is handed to the decoder
/// (mirrors SIMDJSON_PADDING; json_decoder.cpp static_asserts they agree).
constexpr size_t kJsonPaddingBytes = 64;

/// Ensures `s.capacity() >= s.size() + kJsonPaddingBytes` (no-op when already true).
void ensureJsonPadding(std::string& s);

/// The `error` object of a statement response.
struct TrinoError {
  std::string message;
  int errorCode = 0;
  std::string errorName;
  std::string errorType;
  std::string sqlState;        // empty when Trino did not supply one
  int line = 0, column = 0;    // errorLocation (0 = unknown)
  std::string failureType;     // failureInfo.type (for logs)
  std::string failureMessage;  // failureInfo.message
};

/// One segment of the spooled protocol (`data.segments[]`).
struct SpooledSegment {
  enum class Type { Inline, Spooled };
  Type type = Type::Inline;
  std::string inlineData;      // decoded bytes (base64 already removed) when type == Inline
  std::string uri, ackUri;     // when type == Spooled
  std::vector<std::pair<std::string, std::string>> headers;  // request headers for uri/ackUri
  int64_t segmentSize = 0;     // bytes on the wire (0 = unknown)
  int64_t rowOffset = 0;
  int64_t rowsCount = 0;
  int64_t uncompressedSize = 0;  // 0 = not given (plain json)
};

/// `data` when it is an object rather than an array of rows.
struct SpooledData {
  std::string encoding;        // "json" | "json+zstd" | "json+lz4"
  std::vector<SpooledSegment> segments;
};

struct StatementResponse {
  std::string id, infoUri, nextUri, partialCancelUri;
  std::vector<ResultColumn> columns;
  bool columnsPresent = false;
  std::unique_ptr<Page> page;        // decoded `data` rows (null when there were none)
  bool dataIsSpooled = false;
  SpooledData spooled;
  QueryStats stats;
  bool statsPresent = false;
  std::optional<TrinoError> error;
  std::vector<QueryWarning> warnings;
  std::string updateType;
  std::optional<int64_t> updateCount;
};

/// Parses a full statement response. `knownColumns` (nullable) supplies the column list when the
/// response omits it; when the response carries `columns` they win. `data` rows are decoded into
/// `page` only when the columns are known (from either source); otherwise a DriverError is thrown
/// because typed decoding is impossible. Throws DriverError(HY000) on malformed JSON.
StatementResponse parseStatementResponse(std::string& jsonWithPadding,
                                         const std::vector<ResultColumn>* knownColumns,
                                         const TypeMappingOptions& opts);

/// Decodes a bare JSON array of rows (`[[c1, c2, ...], ...]`) into a Page typed by `columns`.
/// The buffer must be padded (see ensureJsonPadding). Throws DriverError(HY000) on malformed input.
std::unique_ptr<Page> decodeRowsJson(std::string& rowsJsonWithPadding, const std::vector<ResultColumn>& columns);

/// Parses an OAuth2 token-server answer: `{"token":"..."}` / `{"nextUri":"..."}` / `{"error":"..."}`.
/// Missing members are left empty. Returns false when the body is not a JSON object.
bool parseOAuthTokenResponse(std::string& jsonWithPadding, std::string* token, std::string* nextUri,
                             std::string* error);

}  // namespace trino_odbc
