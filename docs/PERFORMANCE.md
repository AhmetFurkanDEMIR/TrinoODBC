# Performance tuning

The driver is designed for large result sets: results are decoded once into column-major pages,
a background thread keeps a bounded queue of pages ahead of the application, and with a Trino
server that has the spooled protocol enabled the data is downloaded as compressed segments in
parallel. This page gives measured figures, explains the knobs and gives recommendations for
Power BI.

## Measured throughput

Measured with `SELECT * FROM tpch.sf1.orders` (1 500 000 rows, 9 columns), block cursor with bound
columns, on an Apple M-series laptop, against Trino 483 from the `tests/docker` setup (coordinator
and MinIO in local Docker containers on the same machine, so there is no real network latency):

| Path | Throughput | Notes |
|---|---|---|
| Classic JSON protocol, plain HTTP | about 300 000 rows/s | every page goes through the coordinator; single fetch thread |
| Spooled protocol (`json+zstd` / `json+lz4`) over TLS | 1.5 - 2.6 million rows/s | 0.6 - 1 s for the 1.5 M rows; 40 segments downloaded in parallel and acknowledged |
| Power BI-style import flow (`test_powerbi_flow`) over TLS + spooling | 1.57 million rows/s | replay of the Power Query ODBC call sequence: `SQLGetInfo` probes, `SQLGetTypeInfo`, navigator catalog calls, then a `SQLFetchScroll` loop over bound `SQL_C_SBIGINT` / `SQL_C_WCHAR` / `SQL_C_DOUBLE` / `SQL_C_TYPE_TIMESTAMP` columns |

Caveats: these are single runs on one laptop, not a benchmark suite. Server-side work (the tpch
connector generates the data), the machine, Docker and the spooling configuration of the test
setup (`protocol.spooling.retrieval-mode=COORDINATOR_PROXY`, inlining disabled,
`fs.segment.encryption=false` because MinIO is reached over plain HTTP) all affect the numbers. On a
real network, throughput depends mostly on latency and bandwidth to the coordinator and the
spooling storage. No throughput has been measured on Windows: the Windows x64 package has been
verified functionally (installation, connection, queries, Power BI Desktop), but not timed, so the
Power BI figure comes from the same call sequence driven by the integration tests on macOS, not
from Power BI itself.

## How a query flows

1. `SQLExecute` sends `POST /v1/statement` and follows `nextUri` on the calling thread until the
   column list and the first page (or the end of the query) have arrived. Errors and metadata are
   therefore always reported synchronously by `SQLExecute` / `SQLExecDirect`.
2. A prefetch thread then continues following `nextUri`, decoding pages into a queue (spooled
   segments are downloaded and decoded by up to `MaxParallelSegments` worker threads and handed
   over in row order). It pauses when the queue holds `PrefetchPages` pages **or**
   `MaxPrefetchBytes` bytes, whichever comes first, and resumes as the application fetches.
   Memory use is therefore bounded regardless of the result size. Internal queries (catalog
   functions, `SELECT version()`, `DESCRIBE`) use neither the prefetch thread nor spooling.
3. `SQLFetch` / `SQLFetchScroll` / `SQLGetData` read from the current page and convert straight
   into the application buffers; no per-cell allocation happens on this path.
4. `SQLCloseCursor`, `SQLFreeStmt(SQL_CLOSE)`, `SQL_ATTR_MAX_ROWS` and `SQLCancel` (also between
   two fetches), as well as freeing the statement or disconnecting, stop the prefetcher and cancel
   the query on the server (`DELETE nextUri`), so abandoning a large result does not keep the
   cluster busy. `IdleFetchTimeout` does the same for a result the application stopped reading
   without closing it. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md#cancelled-queries-keep-running).

## Spooled protocol

Trino 466 and later can spool query results to shared storage and hand the client a list of
segments instead of streaming every page through the coordinator. The driver enables it by
sending `X-Trino-Query-Data-Encoding` (option `Spooling=1`, the default). Servers without the
feature ignore the header and the classic page protocol is used; nothing needs to be configured
on the client for either case.

| Option | Default | Effect |
|---|---|---|
| `Spooling` | `1` | Ask for spooled results. Set `0` to force the classic protocol (for example to work around a misconfigured spooling store). |
| `QueryDataEncoding` | `json+zstd,json+lz4,json` | Preferred encodings in order. Only codecs compiled into the driver are offered (`json` always is); the server picks the first one it supports. `json+zstd` gives the smallest transfers, `json+lz4` the fastest decompression. |
| `MaxParallelSegments` | `4` | Spooled segments are downloaded by up to this many worker threads per query and delivered in row order; at most this many decoded segments wait for the consumer. Raise it on high-latency links to object storage; lower it on shared desktops. |
| `TargetResultSize` | `16` MB | `targetResultSize=<N>MB` appended to every `nextUri` of application queries (max 128 MB, larger values are clamped). Larger pages mean fewer round trips; the queue limits below still bound memory. Segment sizes of the spooled protocol are governed mainly by the server's spooling configuration. |

Details of the segment handling:

* Segments are acknowledged (`DELETE ackUri`, 5 s timeout, failures only logged) only after they
  were decoded successfully, so the server can reclaim storage while the driver is still fetching
  later segments.
* Segment downloads do not carry the connection's `Authorization` header; they send only the
  headers Trino attached to the segment (plus `ExtraHeaders`).
* A segment without `uncompressedSize` in its metadata is plain JSON even when the response
  encoding names a codec (Trino only compresses segments above a size threshold).
* Inline segments: Trino 483 serialises every inline segment of one response with the same base64
  payload (all inline segments concatenated); the driver slices it by each segment's
  `segmentSize`. Size and row-count mismatches are reported as `HY000`.

Server side, spooling needs `protocol.spooling.enabled=true` in `config.properties` and a
`spooling-manager.properties` pointing at a filesystem/object store; see the Trino documentation
for the exact properties of your version.

## Prefetch queue

| Option | Default | Guidance |
|---|---|---|
| `PrefetchPages` | `4` | Pages queued ahead of the application. 2-8 is plenty; more only helps when the application processes rows in bursts. |
| `MaxPrefetchBytes` (`PrefetchBufferMB`) | `64` MB | Upper bound of queued, decoded data. Raise it together with `TargetResultSize` for very wide rows so that at least two pages fit. |

Rule of thumb: `MaxPrefetchBytes` should be at least `2 x TargetResultSize`; otherwise the
fetcher stalls after a single page.

## HTTP

* `Compression=1` (default) sends `Accept-Encoding` with every encoding the linked libcurl
  decodes (gzip/deflate, plus br/zstd when available), which matters for the JSON pages of the
  classic protocol; keep it on unless the coordinator is CPU-bound and the network is fast.
* `RequestTimeout` (default 60 s) bounds each HTTP request, not the whole query. A page that
  takes longer than this to produce fails the query with `HYT00`; raise it (or set 0) for queries
  whose first page takes minutes, and use `QueryTimeout` / `SQL_ATTR_QUERY_TIMEOUT` to bound the
  query as a whole.
* `MaxRetries` (default 5) retries 502/503/504 and transient connection failures with back-off
  (100 ms doubling to 2 s, plus up to 99 ms jitter). Failures after a `POST` was sent are not
  retried, because the query may already be running. A coordinator behind a load balancer that
  returns 503 while restarting is survived transparently.
* `ValidateConnection=0` skips the `SELECT version()` round trip at connect; useful for tools that
  open many short-lived connections (`SQL_DBMS_VER` is then reported as `00.00.0000`).
* The connection's HTTP handles share DNS cache, TLS session and connection pool, so subsequent
  requests reuse the TLS connection.

## Fetching from the application

* **Bind columns and fetch blocks.** `SQL_ATTR_ROW_ARRAY_SIZE` of 1 000-10 000 rows with bound
  columns (`SQLBindCol`, column-wise or row-wise) is far cheaper than one `SQLFetch` per row and
  than `SQLGetData` per cell. There is no upper limit on the rowset size.
* **Choose the C type that matches the column**: `SQL_C_SBIGINT` for bigint, `SQL_C_DOUBLE` for
  double, `SQL_C_TYPE_TIMESTAMP` for timestamps. Converting numbers to text (`SQL_C_CHAR`) costs
  formatting per cell.
* **Wide vs narrow strings.** Data is stored as UTF-8. `SQL_C_WCHAR` is transcoded directly into
  the application buffer; `SQL_C_CHAR` is a copy on Unix (`AnsiEncoding=utf8`) but a code-page
  conversion on Windows (`AnsiEncoding=system`). Unicode applications should bind `SQL_C_WCHAR`.
* **Long text.** `SQLGetData` retrieves character/binary data in chunks of any size (`01004` for
  each partial chunk, then `SQL_NO_DATA`); for columns you always read entirely, a bound buffer of
  the right size is faster. `SQL_C_CHAR` chunks are cut at byte boundaries, so a very small buffer
  can split a multi-byte UTF-8 sequence across two calls (concatenate the chunks before decoding);
  a `SQL_C_WCHAR` buffer too small to hold a single character (for example one UTF-16 unit plus the
  terminator for a surrogate pair) returns `HY090`.
* **Limit early.** `SQL_ATTR_MAX_ROWS` stops fetching and cancels the query after N rows; adding
  `LIMIT` to the SQL is better still because the cluster does less work.
* `SQLPrepare` + `SQLNumResultCols`/`SQLDescribeCol` before `SQLExecute` costs one extra
  `DESCRIBE OUTPUT` round trip; tools that only need metadata after execution avoid it.

## Type-mapping choices that affect speed

* `DefaultStringColumnLength` (65535) is the size reported for unbounded `varchar`. Tools that
  allocate buffers from the reported size (Excel, some ETL tools) allocate 128 KB per wide string
  cell per row of the rowset; lower it (e.g. 4000) when your data is short, but values longer than
  the limit are then truncated with `01004`.
* `DecimalAsDouble=1` avoids `SQL_NUMERIC_STRUCT` handling in tools that fall back to text for
  decimals; precision is lost beyond about 15 digits.
* `IntervalMode=string` (default) is cheaper than the struct conversion and is what most tools
  expect.

## Power BI

* **Import mode:** the whole table is fetched once. Use `TargetResultSize=32` or `64`,
  `MaxPrefetchBytes=128` or more, keep `Spooling=1`, and set `RequestTimeout=0` (or several minutes)
  for long-running extractions. Filter and aggregate in Power Query so the steps fold into the SQL
  Trino runs (see [POWERBI.md](POWERBI.md)).
* **DirectQuery** (custom connector): every visual runs a query; latency matters more than
  throughput. The defaults are fine; set `QueryTimeout` to what a dashboard user tolerates and
  make sure the queries fold (check the *View Native Query* option).
* Power BI fetches with `SQL_ATTR_ROW_ARRAY_SIZE` and bound columns, mostly as `SQL_C_WCHAR`,
  `SQL_C_SBIGINT`, `SQL_C_DOUBLE` and `SQL_C_TYPE_TIMESTAMP`; the defaults are tuned for that
  pattern.
* The *Command timeout* advanced option maps to `SQL_ATTR_QUERY_TIMEOUT`, which the driver
  enforces by cancelling the query; the *Connection timeout* maps to `SQL_ATTR_LOGIN_TIMEOUT`
  (`ConnectTimeout`), which only takes effect when set before connecting (a later set is accepted
  and ignored).
* A cancelled or abandoned refresh stops its Trino query (see
  [POWERBI.md](POWERBI.md#7-cancelling-a-refresh-or-a-preview)); `IdleFetchTimeout=600` also
  covers statements Power BI leaves open without reading them, which otherwise keep a prefetch
  buffer and a running query until Trino's `query.client.timeout` ends it.
* Gateway machines refresh several datasets concurrently: each running query may hold up to
  `MaxPrefetchBytes` of decoded pages plus up to `MaxParallelSegments` downloaded/decoded segments.
  Size the machine accordingly or lower these values in the gateway DSN.
* The driver has been used from Power BI Desktop on Windows, but the tuning advice on this page
  (timeouts, prefetch sizes, gateway sizing) has not been measured in Power BI or on a gateway.

## Measuring

Raise `LogLevel` to 4 (debug) to see the HTTP traffic of a query and to 5 (trace) to see every
ODBC call with its return code. The Trino query id recorded in the log lets you look up the
query's statistics in the Trino web UI. The integration test suite prints rows/second for a
250 000-row fetch through the mock server (`tests/integration`, `--mock`), for `tpch.sf1.orders`
against a real cluster and for the Power BI-style import (`test_powerbi_flow`). The benchmark tool
`build/trino_odbc_bench "Driver=<path to the driver library>;Host=...;..." [--sql q] [--array-size N]
[--narrow] [--getdata]` loads the driver without a driver manager and reports rows/s and bytes/s.
