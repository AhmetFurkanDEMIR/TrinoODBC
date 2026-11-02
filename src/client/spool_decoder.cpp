// Spooled protocol: codecs and the ordered parallel segment fetcher. See spool_decoder.h.
#include "client/spool_decoder.h"

#include "util/log.h"
#include "util/strings.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

#if defined(TRINO_ODBC_HAVE_ZSTD)
#  include <zstd.h>
#endif
#if defined(TRINO_ODBC_HAVE_LZ4)
#  include <lz4.h>
#endif

namespace trino_odbc {

namespace {
constexpr const char* kLog = "spool";
constexpr int kAckTimeoutSec = 5;
// How often process() re-checks an external cancel flag while it waits for a segment.
constexpr int kCancelPollMs = 50;
// Upper bound for a single decompressed segment (guards against a corrupt uncompressedSize).
constexpr size_t kMaxSegmentBytes = size_t(1) << 31;

bool isJson(std::string_view e) { return iequals(e, "json"); }
bool isZstd(std::string_view e) { return iequals(e, "json+zstd"); }
bool isLz4(std::string_view e) { return iequals(e, "json+lz4"); }

[[noreturn]] void notCompiledIn(std::string_view encoding) {
  throw DriverError(sqlstate::kOptionalFeatureNotImplemented,
                    "Spooled encoding '" + std::string(encoding) + "' is not supported by this build of the driver");
}

std::string decompress(std::string_view encoding, std::string_view bytes, size_t uncompressedSize) {
  if (uncompressedSize > kMaxSegmentBytes)
    throw DriverError(sqlstate::kGeneralError, "Spooled segment announces an implausible uncompressed size");
  (void)bytes;  // only referenced by the codecs that are compiled in
  if (isZstd(encoding)) {
#if defined(TRINO_ODBC_HAVE_ZSTD)
    size_t expected = uncompressedSize;
    if (expected == 0) {
      const unsigned long long frameSize = ZSTD_getFrameContentSize(bytes.data(), bytes.size());
      if (frameSize == ZSTD_CONTENTSIZE_ERROR)
        throw DriverError(sqlstate::kGeneralError, "Corrupt zstd segment (not a zstd frame)");
      if (frameSize == ZSTD_CONTENTSIZE_UNKNOWN || frameSize > kMaxSegmentBytes)
        throw DriverError(sqlstate::kGeneralError, "zstd segment without a known uncompressed size");
      expected = static_cast<size_t>(frameSize);
    }
    std::string out;
    out.resize(expected);
    const size_t n = ZSTD_decompress(out.data(), out.size(), bytes.data(), bytes.size());
    if (ZSTD_isError(n))
      throw DriverError(sqlstate::kGeneralError, std::string("Corrupt zstd segment: ") + ZSTD_getErrorName(n));
    if (n != expected)
      throw DriverError(sqlstate::kGeneralError,
                        formatString("zstd segment decompressed to %zu bytes, expected %zu", n, expected));
    return out;
#else
    notCompiledIn(encoding);
#endif
  }
  if (isLz4(encoding)) {
#if defined(TRINO_ODBC_HAVE_LZ4)
    if (uncompressedSize == 0)
      throw DriverError(sqlstate::kGeneralError, "lz4 segment without uncompressedSize metadata");
    if (bytes.size() > static_cast<size_t>(LZ4_MAX_INPUT_SIZE))
      throw DriverError(sqlstate::kGeneralError, "lz4 segment too large");
    std::string out;
    out.resize(uncompressedSize);
    const int n = LZ4_decompress_safe(bytes.data(), out.data(), static_cast<int>(bytes.size()),
                                      static_cast<int>(uncompressedSize));
    if (n < 0) throw DriverError(sqlstate::kGeneralError, "Corrupt lz4 segment");
    if (static_cast<size_t>(n) != uncompressedSize)
      throw DriverError(sqlstate::kGeneralError,
                        formatString("lz4 segment decompressed to %d bytes, expected %zu", n, uncompressedSize));
    return out;
#else
    notCompiledIn(encoding);
#endif
  }
  notCompiledIn(encoding);
}
}  // namespace

bool isEncodingSupported(std::string_view encoding) {
  if (isJson(encoding)) return true;
#if defined(TRINO_ODBC_HAVE_ZSTD)
  if (isZstd(encoding)) return true;
#endif
#if defined(TRINO_ODBC_HAVE_LZ4)
  if (isLz4(encoding)) return true;
#endif
  return false;
}

std::string filterSupportedEncodings(std::string_view list) {
  std::vector<std::string> kept;
  for (const std::string& raw : split(list, ',')) {
    const std::string name = toLower(raw);
    if (!isJson(name) && !isZstd(name) && !isLz4(name)) {
      TRINO_LOG_WARN(kLog, "Ignoring unknown QueryDataEncoding entry '%s'", raw.c_str());
      continue;
    }
    if (!isEncodingSupported(name)) {
      TRINO_LOG_INFO(kLog, "QueryDataEncoding '%s' is not compiled in; skipped", name.c_str());
      continue;
    }
    if (std::find(kept.begin(), kept.end(), name) == kept.end()) kept.push_back(name);
  }
  return join(kept, ",");
}

std::string decodeSegmentBytes(std::string_view encoding, std::string_view bytes, size_t uncompressedSize) {
  if (isJson(encoding)) return std::string(bytes);
  return decompress(encoding, bytes, uncompressedSize);
}

std::string decodeSegmentBytes(std::string_view encoding, std::string&& bytes, size_t uncompressedSize) {
  if (isJson(encoding)) return std::move(bytes);
  return decompress(encoding, bytes, uncompressedSize);
}

// ---- SpoolFetcher -----------------------------------------------------------------------------

struct SpoolFetcher::Impl {
  struct Task {
    SpooledSegment segment;
    std::unique_ptr<Page> page;
    std::exception_ptr error;
    bool taken = false;   // a worker picked it up
    bool done = false;    // page or error is set
  };

  std::shared_ptr<const ConnectionConfig> config;
  std::shared_ptr<CurlShare> share;
  const std::atomic<bool>* cancel = nullptr;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  size_t maxParallel = 1;

  std::mutex mutex;
  std::condition_variable workCv;   // workers wait here for tasks / capacity
  std::condition_variable doneCv;   // process() waits here for results
  std::deque<std::shared_ptr<Task>> pending;
  size_t buffered = 0;              // tasks taken by a worker and not yet consumed by process()
  bool stop = false;
  std::exception_ptr fatal;         // a worker could not start (no curl handle)
  std::atomic<bool> abort{false};   // secondary cancel flag: aborts in-flight downloads
  std::vector<std::thread> workers;
  // Valid while process() runs (workers only read them for tasks of that batch).
  const std::vector<ResultColumn>* columns = nullptr;
  std::string encoding;

  void workerMain();
  std::unique_ptr<Page> fetchAndDecode(Task& task, HttpClient& http);
  void ensureWorkers(size_t wanted);
  void abandonBatch(std::vector<std::shared_ptr<Task>>& tasks, std::unique_lock<std::mutex>& lk);
};

SpoolFetcher::SpoolFetcher(std::shared_ptr<const ConnectionConfig> config, std::shared_ptr<CurlShare> share,
                           const std::atomic<bool>* cancel,
                           std::optional<std::chrono::steady_clock::time_point> deadline, int maxParallel)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
  impl_->share = std::move(share);
  impl_->cancel = cancel;
  impl_->deadline = deadline;
  impl_->maxParallel = static_cast<size_t>(std::max(1, maxParallel));
}

SpoolFetcher::~SpoolFetcher() { shutdown(); }

void SpoolFetcher::Impl::ensureWorkers(size_t wanted) {
  // Called with `mutex` held. Workers are created lazily and kept for the query's lifetime so
  // their curl handles (and pooled connections) are reused across responses.
  wanted = std::min(wanted, maxParallel);
  while (workers.size() < wanted) workers.emplace_back([this] { workerMain(); });
}

void SpoolFetcher::Impl::workerMain() {
  std::unique_ptr<HttpClient> http;
  try {
    http = std::make_unique<HttpClient>(config, share, nullptr);
    http->setCancelFlags(cancel, &abort);
    // The deadline is applied per task (under `mutex`), since setDeadline() may change it.
  } catch (...) {
    // Without a transport this worker can never finish a task; fail the whole fetcher so that
    // process() does not wait forever.
    std::lock_guard<std::mutex> g(mutex);
    if (!fatal) fatal = std::current_exception();
    stop = true;
    doneCv.notify_all();
    workCv.notify_all();
    return;
  }
  for (;;) {
    std::shared_ptr<Task> task;
    {
      std::unique_lock<std::mutex> lk(mutex);
      workCv.wait(lk, [&] { return stop || (!pending.empty() && buffered < maxParallel); });
      if (stop) return;
      task = std::move(pending.front());
      pending.pop_front();
      task->taken = true;
      ++buffered;
      // The deadline may have been lifted since the worker started (end of the execute phase).
      http->setDeadline(deadline);
    }
    try {
      task->page = fetchAndDecode(*task, *http);
    } catch (...) {
      task->error = std::current_exception();
    }
    {
      std::lock_guard<std::mutex> g(mutex);
      task->done = true;
    }
    doneCv.notify_all();
  }
}

std::unique_ptr<Page> SpoolFetcher::Impl::fetchAndDecode(Task& task, HttpClient& http) {
  SpooledSegment& seg = task.segment;
  std::string bytes;
  if (seg.type == SpooledSegment::Type::Inline) {
    bytes = std::move(seg.inlineData);
  } else {
    HttpRequest req;
    req.method = HttpRequest::Method::Get;
    req.url = seg.uri;
    req.headers = seg.headers;
    req.sendAuth = false;  // segment URIs carry their own credentials in `headers`
    req.idempotent = true;
    HttpResponse resp = http.performWithRetry(req, config->maxRetries);
    HttpClient::requireSuccess(resp, seg.uri);
    bytes = std::move(resp.body);
  }
  if (seg.segmentSize > 0 && bytes.size() != static_cast<size_t>(seg.segmentSize))
    throw DriverError(sqlstate::kGeneralError,
                      formatString("Spooled segment at row %lld has %zu bytes, expected %lld",
                                   static_cast<long long>(seg.rowOffset), bytes.size(),
                                   static_cast<long long>(seg.segmentSize)));
  // Trino only compresses segments above a size threshold; a segment without uncompressedSize in its
  // metadata is stored as plain JSON even when the response encoding names a codec.
  const std::string_view effectiveEncoding = seg.uncompressedSize > 0 ? encoding : std::string_view("json");
  std::string json = decodeSegmentBytes(effectiveEncoding, std::move(bytes),
                                        seg.uncompressedSize > 0 ? static_cast<size_t>(seg.uncompressedSize) : 0);
  std::unique_ptr<Page> page = decodeRowsJson(json, *columns);
  if (seg.rowsCount > 0 && page->rowCount != static_cast<size_t>(seg.rowsCount))
    throw DriverError(sqlstate::kGeneralError,
                      formatString("Spooled segment at row %lld decoded %zu rows, expected %lld",
                                   static_cast<long long>(seg.rowOffset), page->rowCount,
                                   static_cast<long long>(seg.rowsCount)));
  TRINO_LOG_TRACE(kLog, "segment rowOffset=%lld rows=%zu bytes=%zu decoded", static_cast<long long>(seg.rowOffset),
                  page->rowCount, json.size());
  // Acknowledge only after the data is safely decoded: the server may delete the segment.
  if (seg.type == SpooledSegment::Type::Spooled && !seg.ackUri.empty()) {
    HttpRequest ack;
    ack.method = HttpRequest::Method::Delete;
    ack.url = seg.ackUri;
    ack.headers = seg.headers;
    ack.sendAuth = false;
    ack.timeoutSec = kAckTimeoutSec;
    try {
      HttpResponse resp = http.perform(ack);
      // Ack URIs may be presigned: log them redacted.
      if (!resp.ok())
        TRINO_LOG_DEBUG(kLog, "segment ack %s -> HTTP %ld", redactUrlForLog(seg.ackUri, true).c_str(), resp.status);
    } catch (const DriverError& e) {
      TRINO_LOG_DEBUG(kLog, "segment ack %s failed: %s", redactUrlForLog(seg.ackUri, true).c_str(), e.what());
    }
  }
  return page;
}

void SpoolFetcher::Impl::abandonBatch(std::vector<std::shared_ptr<Task>>& tasks, std::unique_lock<std::mutex>& lk) {
  // Called with `lk` held. Drops what nobody started, aborts what is in flight, and waits for the
  // workers to settle so the next batch (if any) starts from a clean state.
  pending.clear();
  abort.store(true);
  for (auto& t : tasks) {
    if (t->taken) doneCv.wait(lk, [&] { return t->done || stop; });
  }
  buffered = 0;
  abort.store(false);
  workCv.notify_all();
}

/// Trino (observed on 483) serialises every inline segment of one response with the *same* base64
/// payload: the concatenation of all inline segments in row order. Each segment's own bytes are the
/// `segmentSize` bytes starting after the previous inline segments' bytes. Slice accordingly; a
/// segment whose payload already has exactly segmentSize bytes is left untouched.
void sliceSharedInlineData(std::vector<SpooledSegment>& segments) {
  size_t sharedOffset = 0;
  size_t sharedLength = 0;
  for (SpooledSegment& seg : segments) {
    if (seg.type != SpooledSegment::Type::Inline) continue;
    const size_t size = seg.segmentSize > 0 ? static_cast<size_t>(seg.segmentSize) : seg.inlineData.size();
    if (seg.inlineData.size() == size) {
      sharedOffset = 0;
      sharedLength = 0;
      continue;
    }
    if (seg.inlineData.size() != sharedLength) {
      sharedOffset = 0;
      sharedLength = seg.inlineData.size();
    }
    if (seg.inlineData.size() < size || sharedOffset + size > seg.inlineData.size())
      throw DriverError(sqlstate::kGeneralError,
                        formatString("Spooled segment at row %lld has %zu bytes, expected %zu",
                                     static_cast<long long>(seg.rowOffset), seg.inlineData.size(), size));
    seg.inlineData = seg.inlineData.substr(sharedOffset, size);
    sharedOffset += size;
  }
}

void SpoolFetcher::setDeadline(std::optional<std::chrono::steady_clock::time_point> deadline) {
  std::lock_guard<std::mutex> g(impl_->mutex);
  impl_->deadline = deadline;
}

bool SpoolFetcher::process(SpooledData data, const std::vector<ResultColumn>& columns, const PageSink& sink,
                           const std::atomic<bool>* externalCancel) {
  Impl& im = *impl_;
  if (data.segments.empty()) return true;
  const auto externallyCancelled = [externalCancel] {
    return externalCancel != nullptr && externalCancel->load(std::memory_order_relaxed);
  };
  if (externallyCancelled()) throw CancelledError();
  std::stable_sort(data.segments.begin(), data.segments.end(),
                   [](const SpooledSegment& a, const SpooledSegment& b) { return a.rowOffset < b.rowOffset; });
  if (!isEncodingSupported(data.encoding))
    throw DriverError(sqlstate::kOptionalFeatureNotImplemented,
                      "Server chose spooled encoding '" + data.encoding + "' which this driver did not offer");
  sliceSharedInlineData(data.segments);

  std::vector<std::shared_ptr<Impl::Task>> tasks;
  tasks.reserve(data.segments.size());
  {
    std::unique_lock<std::mutex> lk(im.mutex);
    if (im.stop) {
      if (im.fatal) std::rethrow_exception(im.fatal);
      throw CancelledError();
    }
    im.columns = &columns;
    im.encoding = data.encoding;
    for (SpooledSegment& seg : data.segments) {
      auto t = std::make_shared<Impl::Task>();
      t->segment = std::move(seg);
      tasks.push_back(t);
      im.pending.push_back(t);
    }
    im.ensureWorkers(tasks.size());
  }
  im.workCv.notify_all();

  for (size_t i = 0; i < tasks.size(); ++i) {
    std::shared_ptr<Impl::Task>& t = tasks[i];
    std::unique_lock<std::mutex> lk(im.mutex);
    if (externalCancel) {
      // The external flag is set without notifying doneCv, so poll it in short slices.
      while (!im.doneCv.wait_for(lk, std::chrono::milliseconds(kCancelPollMs), [&] { return t->done || im.stop; })) {
        if (externallyCancelled()) {
          im.abandonBatch(tasks, lk);  // aborts the in-flight downloads and waits for the workers
          throw CancelledError();
        }
      }
    } else {
      im.doneCv.wait(lk, [&] { return t->done || im.stop; });
    }
    if (!t->done) {
      // shutdown() from another thread (close during fetch): nothing to clean, workers are gone.
      if (im.fatal) std::rethrow_exception(im.fatal);
      throw CancelledError();
    }
    --im.buffered;
    im.workCv.notify_one();
    if (t->error) {
      std::exception_ptr err = t->error;
      im.abandonBatch(tasks, lk);
      std::rethrow_exception(err);
    }
    std::unique_ptr<Page> page = std::move(t->page);
    lk.unlock();
    if (!sink(std::move(page))) {
      std::unique_lock<std::mutex> lk2(im.mutex);
      im.abandonBatch(tasks, lk2);
      return false;
    }
  }
  return true;
}

void SpoolFetcher::shutdown() {
  Impl& im = *impl_;
  std::vector<std::thread> workers;
  {
    std::lock_guard<std::mutex> g(im.mutex);
    im.stop = true;
    im.abort.store(true);
    im.pending.clear();
    workers.swap(im.workers);
  }
  im.workCv.notify_all();
  im.doneCv.notify_all();
  for (std::thread& t : workers) {
    if (t.joinable()) t.join();
  }
}

}  // namespace trino_odbc
