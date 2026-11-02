# Docker test environments

Two independent Docker setups back the driver's test suite:

| Setup | Entry point | What it proves |
|---|---|---|
| **Linux build + tests** (Ubuntu 24.04, unixODBC) | `tests/docker/run_linux_tests.sh` | the driver builds with GCC and `-DTRINO_ODBC_DRIVER_MANAGER=unixodbc`, the unit tests and the mock-server integration tests pass on Linux, and the driver works through the unixODBC driver manager (`odbcinst` registration, `isql`, `iusql`) |
| **Real Trino** (plain HTTP + HTTPS/password/spooling) | `tests/docker/run_trino_tests.sh` (containers: `tests/docker/trino/run_trino.sh`) | the integration tests pass against a real Trino 483 coordinator: classic protocol, HTTPS with `SSLVerification=None` and `Full` (`CA` is covered by the mock TLS tests), password (LDAP-style Basic) authentication, password over plain HTTP with `AllowInsecurePassword=1` (`tls-http`), and the spooled result protocol (`json+zstd` / `json+lz4` segments from MinIO, downloaded and acknowledged) |

Files in this directory:

```
tests/docker/
├── Dockerfile.ubuntu            Linux image: toolchain -> build -> unit-tests -> integration-tests -> dm-smoke -> final
├── run_linux_tests.sh           builds Dockerfile.ubuntu (the tests run during the build)
├── run_trino_tests.sh           starts the Trino containers and runs trino_odbc_integration_tests against them
└── trino/
    ├── run_trino.sh             starts/stops/removes the Trino + MinIO containers, writes connection.env
    ├── gen_tls_assets.sh        certificate, PKCS#12 keystore, password file, secrets, rendered properties
    ├── plain/config.properties  coordinator config of the plain container (used as is)
    ├── tls/*.properties         templates for the TLS container (rendered by gen_tls_assets.sh)
    └── generated/               created on first start; holds test secrets (see "Generated files")
```

Every script prints its full option list with `--help`. `run_trino.sh` and `run_trino_tests.sh` also
accept `--dry-run` (`-n`): they print the docker commands and file writes they would perform and
change nothing. The dry run works even without a running Docker daemon.

Requirements on the host: Docker (Docker Desktop or Engine 20.10+), bash (the macOS 3.2 bash is
fine), `openssl`, `python3` and `curl`. For the real-Trino setup, give Docker at least 6 GB of
memory. The image's `jvm.config` sizes each heap at 80% of the memory the container sees, and
without `--memory` both Trino containers see the whole Docker VM. `run_trino.sh --memory 3g` caps
each one.

---

## 1. Linux build and tests (unixODBC)

```sh
tests/docker/run_linux_tests.sh
```

A successful run ends with
`PASSED in <n>s: build, unit tests, mock integration tests and unixODBC isql smoke test`. The tests
run while the image builds, so any failing step fails the build and the script exits non-zero.

| Stage (`--target`) | What happens |
|---|---|
| `toolchain` | `apt-get install` of build-essential, cmake, ninja-build, unixodbc, odbcinst, unixodbc-dev, libcurl4-openssl-dev, libzstd-dev, liblz4-dev, libsimdjson-dev, python3, openssl, tzdata. This stage is skipped when the base image already has every package. |
| `build` | `cmake -G Ninja -DTRINO_ODBC_DRIVER_MANAGER=unixodbc -DTRINO_ODBC_BUILD_TESTS=ON`, build, then `cmake --install` to `/usr/local` |
| `unit-tests` | `/build/tests/trino_odbc_unit_tests` |
| `integration-tests` | `/build/tests/trino_odbc_integration_tests --mock` (starts the Python mock servers inside the build container) |
| `dm-smoke` | `packaging/linux/register_driver.sh` registers the installed driver as `Trino ODBC Driver` and `odbcinst -q -d` must show the installed path. Then `isql` runs against a mock server (port 18090, `--spooling`) through a DSN (`ODBCINI=/etc/trino-odbc-test/odbc.ini`, DSN `TrinoMock`), through a connection string (`isql -k`) and with a data-type query, followed by an `iusql` check of the wide entry points. |
| `final` (default, the last stage) | `dm-smoke` plus labels, `WORKDIR /build` and `CMD /bin/bash`; this is the image `--shell` and `docker run` use. |

Useful options:

```sh
tests/docker/run_linux_tests.sh --target unit-tests            # stop after the unit tests
tests/docker/run_linux_tests.sh --target toolchain --tag trino-odbc-linux-base   # build a toolchain image once
tests/docker/run_linux_tests.sh --base-image trino-odbc-linux-base   # reuse it: no apt, works offline
tests/docker/run_linux_tests.sh --cmake-args "-DTRINO_ODBC_WERROR=ON" --build-type Debug
tests/docker/run_linux_tests.sh --platform linux/amd64         # x86-64 build on an ARM host (emulated, slow)
tests/docker/run_linux_tests.sh --no-cache                     # rebuild every stage
tests/docker/run_linux_tests.sh --shell                        # afterwards: shell in the built image
```

`--target` accepts `toolchain`, `build`, `unit-tests`, `integration-tests`, `dm-smoke` and `final`.
Extra `docker build` arguments go after `--`.

The build context is streamed through `tar`. It leaves out `.git`, build directories and
`tests/docker/trino/generated`, which holds the test secrets, so no `.dockerignore` is needed. A
plain `docker build -f tests/docker/Dockerfile.ubuntu .` also works, but its context includes those
directories.

Inside the resulting image (`trino-odbc-linux-test`), the driver is at `/build/libtrinoodbc.so`
and is also installed and registered under `/usr/local/lib`. To rerun a suite interactively:

```sh
docker run --rm -it trino-odbc-linux-test
/build/tests/trino_odbc_integration_tests --mock --filter fetch
printf 'SELECT version()\n' | isql -v -b TrinoMock      # needs a mock: python3 /src/tests/mock_trino/mock_trino.py --port 18090 &
```

---

## 2. Real Trino tests

### Quick start

```sh
# 1. build the driver and the integration test binary on the host
cmake -S . -B build -DTRINO_ODBC_BUILD_TESTS=ON && cmake --build build

# 2. start the containers (first start: ~1 minute) and run plain, tls and tls-full
tests/docker/run_trino_tests.sh
```

`run_trino_tests.sh` calls `trino/run_trino.sh start`, which is idempotent: running containers
with an unchanged configuration are reused. It then runs `trino_odbc_integration_tests --connstr
"<connection string>"` once per configuration and prints a PASS/FAIL summary. The exit status is 0
only when every configuration passes. The containers keep running afterwards; pass `--stop` or
`--remove` to take them down.

### Containers and ports

```
                 host                                  docker network "trino-odbc-net"
  ODBC driver ── localhost:18080 ──> trino-odbc-test  :8080   plain HTTP, no auth, classic protocol
             ├── localhost:18443 ──> trino-odbc-tls   :8443   HTTPS + password auth + spooling ──┐
             └── localhost:18081 ──> trino-odbc-tls   :8080   HTTP, password allowed (insecure)  │ s3 (http)
                 localhost:19000 ──> trino-odbc-minio :9000   MinIO, bucket "spooling"  <────────┘
```

| Container | Image | Host port → container port | Purpose |
|---|---|---|---|
| `trino-odbc-test` | `trinodb/trino:483` (fallback `trinodb/trino:478`) | 18080 → 8080 | no authentication, classic protocol. This is the driver's default path. |
| `trino-odbc-tls` | same image | 18443 → 8443 (HTTPS), 18081 → 8080 (HTTP) | `http-server.authentication.type=PASSWORD` with the file authenticator, self-signed certificate, spooled protocol |
| `trino-odbc-minio` | `minio/minio` | 19000 → 9000 | object storage for spooled segments. Only the TLS coordinator talks to it; the host port exists for debugging. |

Both Trino containers mount the same catalogs from `generated/catalog`: `tpch` for the read-only
tests (`tpch.tiny.*`, and `tpch.sf1.orders` with 1.5M rows for the large fetch) and `memory` for
DML/DDL. Ports, names and images can be changed with environment variables such as
`TRINO_ODBC_PLAIN_PORT`, `TRINO_ODBC_TLS_PORT`, `TRINO_ODBC_TLS_HTTP_PORT`, `TRINO_ODBC_MINIO_PORT`
and `TRINO_ODBC_TRINO_IMAGE` (see `run_trino.sh --help`). By default, published ports listen on
every interface. On a shared network, set `TRINO_ODBC_BIND_ADDRESS=127.0.0.1`.

### Credentials

These are test-only values and must never be reused anywhere else.

| What | Value | Where it is set |
|---|---|---|
| Plain container user | `test` (any name works; no password: a `PWD` without `SSL=0` makes the driver switch to TLS, which the plain port does not speak, and with `SSL=0` the driver refuses to send it unless `AllowInsecurePassword=1`) | `TRINO_ODBC_PLAIN_USER` |
| TLS container user / password | `alice` / `secret` | `TRINO_ODBC_TLS_USER`, `TRINO_ODBC_TLS_PASSWORD` (read by `gen_tls_assets.sh`; changing them regenerates the assets) |
| `password.db` line | `alice:1000:<salt hex>:<hash hex>` (PBKDF2-HMAC-SHA1, 1000 iterations, 32-byte hash) | generated |
| Keystore password | `changeit` | `TRINO_ODBC_KEYSTORE_PASSWORD` |
| MinIO access / secret key | `minioadmin` / `minioadmin` | `TRINO_ODBC_MINIO_ACCESS_KEY`, `TRINO_ODBC_MINIO_SECRET_KEY` |
| `internal-communication.shared-secret`, `protocol.spooling.shared-secret-key` | random per generation | generated |

### Connection strings

`run_trino.sh start` prints these strings and writes them to `generated/connection.env`
(`TRINO_ODBC_PLAIN_CONNSTR`, `TRINO_ODBC_TLS_CONNSTR`, `TRINO_ODBC_TLS_FULL_CONNSTR`,
`TRINO_ODBC_TLS_HTTP_CONNSTR`, plus URLs, certificate path and inlining state). `run_trino.sh
connstr` prints them again for the containers that are running.

| Name (`--only`) | Connection string |
|---|---|
| `plain` | `Host=localhost;Port=18080;UID=test;Catalog=tpch;Schema=tiny` |
| `tls` | `Host=localhost;Port=18443;SSL=1;SSLVerification=None;AuthenticationType=LDAP;UID=alice;PWD=secret;Catalog=tpch;Schema=tiny` |
| `tls-full` | `Host=localhost;Port=18443;SSL=1;SSLVerification=Full;TrustedCerts=<repo>/tests/docker/trino/generated/cert.pem;AuthenticationType=LDAP;UID=alice;PWD=secret;Catalog=tpch;Schema=tiny` |
| `tls-http` (not in the default set) | `Host=localhost;Port=18081;AuthenticationType=LDAP;UID=alice;PWD=secret;AllowInsecurePassword=1;Catalog=tpch;Schema=tiny` |

The integration harness loads the driver library directly, so these strings have no `Driver=` key.
Add one when you go through a driver manager:

```sh
# unixODBC (driver registered by packaging/linux/register_driver.sh)
isql -v -k "Driver=Trino ODBC Driver;Host=localhost;Port=18080;UID=test;Catalog=tpch;Schema=tiny"
# iODBC on macOS, pointing at the build output
iodbctest "DRIVER=$PWD/build/libtrinoodbc.dylib;Host=localhost;Port=18443;SSL=1;SSLVerification=None;AuthenticationType=LDAP;UID=alice;PWD=secret;Catalog=tpch;Schema=tiny"
# the integration tests against one configuration, by hand
TRINO_ODBC_DRIVER=$PWD/build/libtrinoodbc.dylib build/tests/trino_odbc_integration_tests \
    --connstr "Host=localhost;Port=18080;UID=test;Catalog=tpch;Schema=tiny" --filter query
```

The same settings as a DSN in `odbc.ini`:

```ini
[TrinoDockerTLS]
Driver=Trino ODBC Driver
Host=localhost
Port=18443
SSL=1
SSLVerification=Full
TrustedCerts=/path/to/odbc/tests/docker/trino/generated/cert.pem
AuthenticationType=LDAP
UID=alice
PWD=secret
Catalog=tpch
Schema=tiny
```

The certificate's subjectAltName covers `localhost`, `127.0.0.1`, `::1`, `trino-odbc-tls` and
`host.docker.internal`. With `SSLVerification=Full`, `Host` must be one of these names. You can add
more names with `TRINO_ODBC_TLS_EXTRA_SAN="DNS:myhost,IP:10.0.0.5" tests/docker/trino/gen_tls_assets.sh --force`
and then `run_trino.sh start`, which recreates the TLS container.

### The spooled protocol setup

The TLS coordinator serves results through Trino's spooled protocol, so the tls\* runs exercise the
driver's segment download, decoding and `DELETE ackUri` path. The driver asks for
`QueryDataEncoding=json+zstd,json+lz4,json` by default, so a driver built with zstd and lz4
receives compressed segments. The plain container keeps the classic
protocol. Trino 483's filesystem spooling manager stores segments in object storage (it has no
local-directory location), which is why MinIO runs next to it. `run_trino.sh` creates the bucket
with the `minio/mc` image on the shared network (`mc alias set local http://trino-odbc-minio:9000
...` then `mc mb -p local/spooling`).

Each setting below was verified on Trino 483. Getting one wrong either fails queries or quietly
bypasses the code path under test:

| Setting | File | Why |
|---|---|---|
| `fs.segment.encryption=false` | `spooling-manager.properties` | By default segments are encrypted with SSE-C keys, and MinIO over plain HTTP rejects them: *"Requests specifying Server Side Encryption with Customer provided keys must be made over a secure connection"*. |
| `protocol.spooling.shared-secret-key=<base64>` | `config.properties` | The key must decode to exactly 32 bytes. `gen_tls_assets.sh` uses `openssl rand -base64 32` and checks the decoded length. |
| `protocol.spooling.retrieval-mode=COORDINATOR_PROXY` | `config.properties` | Segment URIs point at the coordinator (`https://localhost:18443/v1/spooled/...`), so the ODBC client never needs to reach MinIO. |
| `protocol.spooling.inlining.enabled=false` | `config.effective.properties` (appended by `run_trino.sh`) | With Trino's default inlining, most results (even 15,000 rows of `tpch.tiny.orders`) arrive as *inline* segments, and the download + ack path would go untested. Controlled by `run_trino.sh --inlining on\|off` or `TRINO_ODBC_SPOOLING_INLINING`; the default for this setup is `off`. |
| `s3.endpoint=http://trino-odbc-minio:9000`, `s3.path-style-access=true`, `s3.region=us-east-1`, `fs.location=s3://spooling/` | `spooling-manager.properties` | MinIO on the shared Docker network |

After every `start`, `run_trino.sh` checks the setup end to end:

1. It waits for `SERVER STARTED` and for `/v1/info` to report `"starting":false`.
2. It runs an authenticated `SELECT 1` as `alice`.
3. A small Python client runs `SELECT nationkey, name FROM tpch.tiny.nation` with
   `X-Trino-Query-Data-Encoding: json`. It downloads every spooled segment with the segment headers
   only (no Trino credentials, as the driver does), acknowledges each one, and requires 25 decoded
   rows. With `--inlining off`, it also requires at least one spooled segment.

A misconfiguration therefore fails `start` with a clear message instead of surfacing later as a
test failure. Use `--no-probe` to skip these checks (for example when the host cannot reach the
published ports).

To test the driver against Trino's default behaviour, where small results come inline, use
`--inlining on`. Trino 483 sends all inline segments of a response as one shared payload that the
client slices by each segment's `segmentSize`, and this run covers that path:

```sh
tests/docker/run_trino_tests.sh --inlining on        # recreates trino-odbc-tls with inlining enabled
tests/docker/run_trino_tests.sh --inlining off       # back to the default of this setup
```

### Generated files (`tests/docker/trino/generated/`)

| File | Written by | Content |
|---|---|---|
| `cert.pem`, `key.pem` | `gen_tls_assets.sh` | self-signed server certificate (CA:TRUE, so it can serve as `TrustedCerts`) and key |
| `keystore.p12` | `gen_tls_assets.sh` | PKCS#12 keystore (alias `trino`) → `http-server.https.keystore.path` |
| `password.db` | `gen_tls_assets.sh` | file authenticator users |
| `config.properties`, `spooling-manager.properties`, `password-authenticator.properties` | `gen_tls_assets.sh` | rendered from `tls/` with fresh secrets |
| `config.effective.properties` | `run_trino.sh` | `config.properties` plus the `--inlining` setting; this is the file mounted as `/etc/trino/config.properties` |
| `tls-assets.env` | `gen_tls_assets.sh` | the parameters used (user, password, SAN, MinIO settings, fingerprint) |
| `catalog/tpch.properties`, `catalog/memory.properties` | `run_trino.sh` | catalogs of both Trino containers |
| `connection.env` | `run_trino.sh` | connection strings, sourced by `run_trino_tests.sh` |
| `test-logs/<config>.log` | `run_trino_tests.sh` | full output of each test run |

The assets are generated once and reused while their parameters stay the same. To start over with
new secrets, delete the directory, or run `gen_tls_assets.sh --force` and then `run_trino.sh start`.
Every container records a hash of its image, ports and mounted files, so a changed file makes the
next `start` recreate exactly the containers it affects. Keep `generated/` out of version control.

### `run_trino.sh` reference

```sh
tests/docker/trino/run_trino.sh [start]      # create/reuse, wait, verify, print connection strings
tests/docker/trino/run_trino.sh status       # container states
tests/docker/trino/run_trino.sh connstr      # connection strings of the running containers
tests/docker/trino/run_trino.sh stop         # stop (the next start reuses them)
tests/docker/trino/run_trino.sh remove       # remove containers + network; generated files are kept
tests/docker/trino/run_trino.sh --dry-run start    # show every docker command / file write, change nothing
```

The most useful options are `--plain-only`, `--tls-only`, `--inlining on|off`, `--recreate`,
`--image IMAGE`, `--memory 4g`, `--timeout SECONDS`, `--host NAME` (the host written into the
connection strings) and `--assets-dir DIR`. `TRINO_ODBC_PROBE_HOST` sets the host that `curl` and
the probe use to reach the published ports; the default is `localhost`. When Docker runs on another
machine, set it to that machine's name and pass `--host` with the same value.

### `run_trino_tests.sh` reference

```sh
tests/docker/run_trino_tests.sh                               # plain, tls, tls-full
tests/docker/run_trino_tests.sh --only tls-http               # password over plain HTTP (AllowInsecurePassword=1)
tests/docker/run_trino_tests.sh --only plain --filter fetch   # one configuration, a subset of tests
tests/docker/run_trino_tests.sh --build-dir build-linux --driver build-linux/libtrinoodbc.so
tests/docker/run_trino_tests.sh --no-start                    # use the running containers as they are
tests/docker/run_trino_tests.sh --loglevel 4 -- --list        # driver debug log; extra args go to the binary
tests/docker/run_trino_tests.sh --remove                      # tear everything down afterwards
tests/docker/run_trino_tests.sh --dry-run                     # show the plan only
```

Without an explicit binary, the script uses the first `tests/trino_odbc_integration_tests` found
under `build`, `build-macos`, `build-linux`, `build-*`, `cmake-build-*` or `out/build/*`. CMake only
creates that target when every `tests/integration/*.cpp` source is present; the configure output
says which one is missing.

Against a real server the harness detects that it is not talking to the mock (`SQL_DBMS_VER`
without `mock`). It then skips the tests that need mock-only tables and failure modes, and uses
`tpch.tiny` / `tpch.sf1` equivalents where meaningful. Each run ends with `N tests run, F failed,
S skipped, ...`, and the summary table shows that line for every configuration.

### Checking a container by hand

```sh
curl -s  http://localhost:18080/v1/info                                   # plain: {"starting":false,...}
curl -sk https://localhost:18443/v1/info                                  # TLS
curl -sk -u alice:secret -H 'X-Trino-User: alice' --data 'SELECT 1' https://localhost:18443/v1/statement
docker logs trino-odbc-tls 2>&1 | grep -iE 'spool|error' | tail
docker exec -it trino-odbc-tls trino --server https://localhost:8443 --insecure --user alice --password
```

### Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Query error *"Requests specifying Server Side Encryption with Customer provided keys must be made over a secure connection"* | `fs.segment.encryption=false` is missing from `spooling-manager.properties`, for example after editing the template or using an old `generated/` directory. Run `gen_tls_assets.sh --force`, then `run_trino.sh start`. |
| Trino rejects `protocol.spooling.shared-secret-key` (startup failure or failing spooled queries) | The key does not decode to exactly 32 bytes, for example after a hand-edited value. Regenerate it with `gen_tls_assets.sh --force`, then `run_trino.sh start`. |
| Probe: *"all N segment(s) came inline although protocol.spooling.inlining.enabled=false"* | Spooling to MinIO failed and Trino fell back to inlining (search `docker logs trino-odbc-tls` for `falling back to inlining`). Check the `s3.*` settings and that `trino-odbc-minio` is running with the `spooling` bucket. |
| Probe: *"the server answered with the classic protocol"* | The running coordinator does not have spooling enabled. This happens with an image override (`--image`) whose Trino predates the spooled protocol, or with a hand-edited mounted config. Use an image with the spooled protocol (the default images, 483 and 478, both have it) and run `run_trino.sh --recreate --tls-only start`. |
| `401 Password not allowed for insecure authentication` | Basic credentials were sent over HTTP to a server that forbids that. Use the `tls` strings, or port 18081 with `AllowInsecurePassword=1` (`tls-http`); the plain container needs no password. |
| `tls-full` fails certificate verification | `Host` is not in the certificate's SAN list, or `TrustedCerts` is not the `generated/cert.pem` of the running container. After deleting `generated/`, run `start` again so that the container and the file match. |
| `docker run failed ... port in use` | Another process holds 18080/18443/18081/19000. Pick other ports with `TRINO_ODBC_PLAIN_PORT` and the related variables; the connection strings follow automatically. |
| Container exits or never logs `SERVER STARTED` | Usually memory. Give Docker more, or cap each container with `--memory 3g`. The script prints the last 60 log lines. |
| `trinodb/trino:483` not available | The script falls back to `trinodb/trino:478` and warns. `--image` or `TRINO_ODBC_TRINO_IMAGE` selects another image. |
| `minio/mc` cannot be pulled (offline) | The bucket is created with the `mc` binary inside the MinIO container instead, and the script warns. |

### Benchmark against the containers

`trino_odbc_bench` (built with the project, see
[docs/INSTALL-LINUX.md](../../docs/INSTALL-LINUX.md#benchmarking)) loads the driver directly and
fetches `tpch.sf1.orders` (1.5M rows, 9 columns) by default:

```sh
cmake --build build --target trino_odbc_bench
# classic protocol, plain HTTP
build/trino_odbc_bench "Driver=$PWD/build/libtrinoodbc.dylib;Host=localhost;Port=18080;UID=test"
# spooled protocol over TLS (the password turns TLS on)
build/trino_odbc_bench "Driver=$PWD/build/libtrinoodbc.dylib;Host=localhost;Port=18443;SSLVerification=None;UID=alice;PWD=secret"
```

Use `libtrinoodbc.so` on Linux. On an Apple M-series laptop the plain run gave about 300k rows/s
and the TLS + spooling run 1.5-2.6M rows/s (0.6-1 s for 1.5M rows, 40 segments downloaded in
parallel and acknowledged).

### Cleanup

```sh
tests/docker/trino/run_trino.sh remove       # containers + network
rm -rf tests/docker/trino/generated          # certificates, secrets, logs (regenerated on the next start)
docker image rm trino-odbc-linux-test        # the Linux test image
```

### CI outline

```sh
set -e
tests/docker/run_linux_tests.sh --quiet                        # Linux build + unit + mock + unixODBC
cmake -S . -B build -DTRINO_ODBC_BUILD_TESTS=ON && cmake --build build
tests/docker/run_trino_tests.sh --fail-fast --remove           # real Trino, all default configurations
```
