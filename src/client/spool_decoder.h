#pragma once
// Trino spooled protocol: segment codecs (json / json+zstd / json+lz4) and the parallel segment
// fetcher that turns a spooled `data` object into ordered Pages.
#include "client/http_client.h"
#include "result/json_decoder.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trino_odbc {

/// True when the codec for `encoding` is compiled in ("json" always is).
bool isEncodingSupported(std::string_view encoding);
/// Filters a comma-separated encoding list down to the supported ones (order kept, lowercase,
/// duplicates removed). Unknown names are dropped with a warning.
std::string filterSupportedEncodings(std::string_view list);

/// Turns segment bytes into the JSON rows text. `uncompressedSize` is the size announced by the
/// server (0 = unknown; required for lz4, optional for zstd frames that carry it). Throws
/// DriverError(HY000) on corrupt data and DriverError(HYC00) for an encoding not compiled in.
std::string decodeSegmentBytes(std::string_view encoding, std::string_view bytes, size_t uncompressedSize);
/// Same, but may reuse `bytes` for the plain "json" encoding (no copy).
std::string decodeSegmentBytes(std::string_view encoding, std::string&& bytes, size_t uncompressedSize);

/// Downloads / decodes the segments of spooled responses with up to `maxParallel` worker threads
/// and delivers the resulting pages strictly in rowOffset order. Memory is bounded: at most
/// `maxParallel` segments are in flight or decoded-but-undelivered at any time.
///
/// process() is called from one thread at a time (the calling thread during the synchronous phase
/// of a query, the prefetch thread afterwards). shutdown()/the destructor may run on another thread
/// while process() is blocked; process() then throws CancelledError.
class SpoolFetcher {
 public:
  /// Return false to stop delivery (remaining segments are dropped).
  using PageSink = std::function<bool(std::unique_ptr<Page>)>;

  SpoolFetcher(std::shared_ptr<const ConnectionConfig> config, std::shared_ptr<CurlShare> share,
               const std::atomic<bool>* cancel, std::optional<std::chrono::steady_clock::time_point> deadline,
               int maxParallel);
  ~SpoolFetcher();
  SpoolFetcher(const SpoolFetcher&) = delete;
  SpoolFetcher& operator=(const SpoolFetcher&) = delete;

  /// Delivers every segment of `data` as a Page (in rowOffset order). Returns false when the sink
  /// asked to stop. Throws the first segment error (HttpError / DriverError / CancelledError).
  /// `externalCancel` (nullable) is an additional flag polled while waiting for segments — the
  /// statement's SQLCancel request during the synchronous phase of SQLExecute; when it becomes true
  /// the in-flight downloads are aborted and CancelledError is thrown.
  bool process(SpooledData data, const std::vector<ResultColumn>& columns, const PageSink& sink,
               const std::atomic<bool>* externalCancel = nullptr);
  /// Replaces the absolute deadline applied to segment downloads started from now on (nullopt =
  /// none). The query timeout covers only the synchronous execute phase, so the owner clears it
  /// once SQLExecute returns.
  void setDeadline(std::optional<std::chrono::steady_clock::time_point> deadline);
  /// Aborts in-flight downloads and joins the workers. Idempotent; never called from a worker.
  void shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace trino_odbc
