#!/usr/bin/env bash
# Starts the real-Trino test containers used by tests/docker/run_trino_tests.sh (idempotently).
#
#   run_trino.sh [options] [start|stop|remove|status|connstr]
#
# Containers (all attached to the Docker network "trino-odbc-net"):
#   trino-odbc-test    Trino, plain HTTP, no authentication, classic protocol   -> http://localhost:18080
#   trino-odbc-minio   MinIO (S3) storage for the spooled result protocol       -> http://localhost:19000
#   trino-odbc-tls     Trino, HTTPS + file password authenticator (alice/secret)
#                      + spooled protocol via MinIO                             -> https://localhost:18443
#                                                                                  http://localhost:18081
# Commands:
#   start (default)  create or reuse the containers, wait until Trino is ready, verify password
#                    authentication and the spooled protocol, print the connection strings and
#                    write them to <assets dir>/connection.env
#   stop             stop the containers (the next start reuses them)
#   remove           stop and remove the containers and the network (generated assets are kept)
#   status           show the container states
#   connstr          print the connection strings / env file for the containers that are running
#
# Idempotency: a running container whose image and mounted configuration match is reused as is; a
# stopped one is started; one that was created from another image or configuration (or --recreate)
# is removed and created again. TLS assets (certificate, keystore, password file, rendered
# properties) come from gen_tls_assets.sh and are generated once.
#
# Spooled protocol (TLS container only). Trino 483 spools to object storage, so MinIO holds the
# segments; the bucket is created with the minio/mc image ("mc alias set" + "mc mb -p"). Settings
# this setup depends on (details in tests/docker/README.md):
#   fs.segment.encryption=false           MinIO rejects SSE-C segment keys over plain http
#   protocol.spooling.shared-secret-key   must decode to exactly 32 bytes (openssl rand -base64 32)
#   protocol.spooling.retrieval-mode=COORDINATOR_PROXY   clients never need to reach MinIO
#   protocol.spooling.inlining.enabled    set by --inlining (default off)
#
# Options (environment variable and default in brackets):
#   --plain-only | --tls-only  only the plain stack / only MinIO + the TLS stack
#   --inlining on|off          protocol.spooling.inlining.enabled of the TLS container
#                              [TRINO_ODBC_SPOOLING_INLINING=off]. off: every result, however small,
#                              arrives as spooled segments, so every test exercises the driver's
#                              segment download + ack path; on: Trino's default (small results inline)
#   --recreate                 remove existing containers before starting
#   --image IMAGE              Trino image [TRINO_ODBC_TRINO_IMAGE=trinodb/trino:483; when neither present
#                              locally nor pullable: TRINO_ODBC_TRINO_FALLBACK_IMAGE=trinodb/trino:478]
#   --assets-dir DIR           generated files: TLS assets, catalogs, connection.env
#                              [TRINO_ODBC_DOCKER_ASSETS=tests/docker/trino/generated]
#   --timeout SECONDS          startup wait per container [TRINO_ODBC_STARTUP_TIMEOUT=300]
#   --memory SIZE              docker --memory for each Trino container, e.g. 4g [TRINO_ODBC_TRINO_MEMORY, unset]
#   --host NAME                host name written into the connection strings [TRINO_ODBC_TRINO_HOST=localhost]
#   --no-probe                 skip the post-start checks (authenticated query, spooled protocol)
#   -n, --dry-run              print the docker commands and file writes instead of performing them;
#                              read-only Docker queries still run when a daemon is reachable
#   -q, --quiet                only errors and the final connection strings
#   -h, --help
#
# More environment variables: TRINO_ODBC_DOCKER (CLI, default docker), TRINO_ODBC_PYTHON (python3,
#   used by the spooled-protocol probe), TRINO_ODBC_NETWORK,
#   TRINO_ODBC_PLAIN_CONTAINER, TRINO_ODBC_TLS_CONTAINER, TRINO_ODBC_MINIO_CONTAINER,
#   TRINO_ODBC_PLAIN_PORT (18080), TRINO_ODBC_TLS_PORT (18443), TRINO_ODBC_TLS_HTTP_PORT (18081),
#   TRINO_ODBC_MINIO_PORT (19000; empty = not published), TRINO_ODBC_BIND_ADDRESS (e.g. 127.0.0.1;
#   default all interfaces), TRINO_ODBC_MINIO_IMAGE (minio/minio), TRINO_ODBC_MC_IMAGE (minio/mc),
#   TRINO_ODBC_MINIO_ACCESS_KEY / TRINO_ODBC_MINIO_SECRET_KEY (minioadmin), TRINO_ODBC_SPOOLING_BUCKET
#   (spooling), TRINO_ODBC_PLAIN_USER (test), TRINO_ODBC_TEST_CATALOG (tpch), TRINO_ODBC_TEST_SCHEMA
#   (tiny), TRINO_ODBC_PROBE_HOST (localhost; where curl/python reach the published ports) and every
#   gen_tls_assets.sh variable (TRINO_ODBC_TLS_USER, TRINO_ODBC_TLS_PASSWORD, ...).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DOCKER="${TRINO_ODBC_DOCKER:-docker}"
PYTHON="${TRINO_ODBC_PYTHON:-python3}"
PRIMARY_IMAGE="${TRINO_ODBC_TRINO_IMAGE:-trinodb/trino:483}"
FALLBACK_IMAGE="${TRINO_ODBC_TRINO_FALLBACK_IMAGE:-trinodb/trino:478}"
MINIO_IMAGE="${TRINO_ODBC_MINIO_IMAGE:-minio/minio}"
MC_IMAGE="${TRINO_ODBC_MC_IMAGE:-minio/mc}"
NETWORK="${TRINO_ODBC_NETWORK:-trino-odbc-net}"
PLAIN_CONTAINER="${TRINO_ODBC_PLAIN_CONTAINER:-trino-odbc-test}"
TLS_CONTAINER="${TRINO_ODBC_TLS_CONTAINER:-trino-odbc-tls}"
MINIO_CONTAINER="${TRINO_ODBC_MINIO_CONTAINER:-trino-odbc-minio}"
PLAIN_PORT="${TRINO_ODBC_PLAIN_PORT:-18080}"
TLS_PORT="${TRINO_ODBC_TLS_PORT:-18443}"
TLS_HTTP_PORT="${TRINO_ODBC_TLS_HTTP_PORT:-18081}"
MINIO_PORT="${TRINO_ODBC_MINIO_PORT-19000}"
BIND_ADDRESS="${TRINO_ODBC_BIND_ADDRESS:-}"
TRINO_MEMORY="${TRINO_ODBC_TRINO_MEMORY:-}"
HOST_NAME="${TRINO_ODBC_TRINO_HOST:-localhost}"
PROBE_HOST="${TRINO_ODBC_PROBE_HOST:-localhost}"
PLAIN_USER="${TRINO_ODBC_PLAIN_USER:-test}"
TEST_CATALOG="${TRINO_ODBC_TEST_CATALOG:-tpch}"
TEST_SCHEMA="${TRINO_ODBC_TEST_SCHEMA:-tiny}"
MINIO_ACCESS_KEY="${TRINO_ODBC_MINIO_ACCESS_KEY:-minioadmin}"
MINIO_SECRET_KEY="${TRINO_ODBC_MINIO_SECRET_KEY:-minioadmin}"
SPOOLING_BUCKET="${TRINO_ODBC_SPOOLING_BUCKET:-spooling}"
INLINING="${TRINO_ODBC_SPOOLING_INLINING:-off}"
TIMEOUT="${TRINO_ODBC_STARTUP_TIMEOUT:-300}"
ASSETS="${TRINO_ODBC_DOCKER_ASSETS:-$SCRIPT_DIR/generated}"
IMAGE_OVERRIDE=""
IMAGE=""
COMMAND=""
WANT_PLAIN=1
WANT_TLS=1
RECREATE=0
QUIET=0
DRY_RUN=0
PROBE=1
DOCKER_OK=1

LABEL_ROLE="trino-odbc.role"
LABEL_SPEC="trino-odbc.spec"

usage() { sed -n '2,/^[^#]/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }
log() { [ "$QUIET" = 1 ] || printf 'run_trino: %s\n' "$*"; }
warn() { printf 'run_trino: warning: %s\n' "$*" >&2; }
die() { printf 'run_trino: error: %s\n' "$*" >&2; exit 1; }
need_value() { [ $# -ge 2 ] || die "$1 needs a value"; }

while [ $# -gt 0 ]; do
  case "$1" in
    start|stop|remove|status|connstr) [ -z "$COMMAND" ] || die "only one command allowed"; COMMAND="$1" ;;
    --plain-only) WANT_TLS=0 ;;
    --tls-only) WANT_PLAIN=0 ;;
    --inlining) need_value "$@"; shift; INLINING="$1" ;;
    --inlining=*) INLINING="${1#--inlining=}" ;;
    --recreate) RECREATE=1 ;;
    --image) need_value "$@"; shift; IMAGE_OVERRIDE="$1" ;;
    --image=*) IMAGE_OVERRIDE="${1#--image=}" ;;
    --assets-dir) need_value "$@"; shift; ASSETS="$1" ;;
    --assets-dir=*) ASSETS="${1#--assets-dir=}" ;;
    --timeout) need_value "$@"; shift; TIMEOUT="$1" ;;
    --timeout=*) TIMEOUT="${1#--timeout=}" ;;
    --memory) need_value "$@"; shift; TRINO_MEMORY="$1" ;;
    --memory=*) TRINO_MEMORY="${1#--memory=}" ;;
    --host) need_value "$@"; shift; HOST_NAME="$1" ;;
    --host=*) HOST_NAME="${1#--host=}" ;;
    --no-probe) PROBE=0 ;;
    -n|--dry-run) DRY_RUN=1 ;;
    -q|--quiet) QUIET=1 ;;
    -h|--help) usage; exit 0 ;;
    -*) die "unknown option '$1' (see --help)" ;;
    *) die "unexpected argument '$1' (see --help)" ;;
  esac
  shift
done
[ -n "$COMMAND" ] || COMMAND=start
[ "$WANT_PLAIN" = 1 ] || [ "$WANT_TLS" = 1 ] || die "--plain-only and --tls-only exclude each other"
case "$TIMEOUT" in ''|*[!0-9]*) die "--timeout must be a number of seconds" ;; esac
for p in "$PLAIN_PORT" "$TLS_PORT" "$TLS_HTTP_PORT"; do
  case "$p" in ''|*[!0-9]*) die "port '$p' is not a number" ;; esac
done
case "$MINIO_PORT" in *[!0-9]*) die "TRINO_ODBC_MINIO_PORT '$MINIO_PORT' is not a number" ;; esac
case "$(printf '%s' "$INLINING" | tr '[:upper:]' '[:lower:]')" in
  on|true|yes|1|enabled) INLINING=on ;;
  off|false|no|0|disabled) INLINING=off ;;
  *) die "--inlining must be 'on' or 'off' (got '$INLINING')" ;;
esac
[ -n "$ASSETS" ] || die "--assets-dir must not be empty"

# ---- helpers -------------------------------------------------------------------------------------
# A dry run must still work where Docker is not installed or the daemon is down (e.g. to review
# the commands on a CI runner before granting it Docker access): every container then counts as
# missing and every image as absent.
if ! command -v "$DOCKER" >/dev/null 2>&1; then
  [ "$DRY_RUN" = 1 ] || die "'$DOCKER' not found (install Docker or set TRINO_ODBC_DOCKER)"
  DOCKER_OK=0
elif ! "$DOCKER" info >/dev/null 2>&1; then
  [ "$DRY_RUN" = 1 ] || die "cannot talk to the Docker daemon ('$DOCKER info' failed)"
  DOCKER_OK=0
fi
if [ "$DOCKER_OK" = 0 ]; then warn "no reachable Docker daemon: the dry run assumes that no container or image exists"; fi

# Dry-run output goes to a duplicate of stdout so that it survives the ">/dev/null" of the wrapped
# docker commands.
exec 3>&1
shell_quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
# Copy-pasteable rendering of a command line (plain words stay unquoted for readability).
quote_cmd() {
  local out="" a
  for a in "$@"; do
    case "$a" in
      ''|*[!A-Za-z0-9_./:=@%+,-]*) out="$out $(shell_quote "$a")" ;;
      *) out="$out $a" ;;
    esac
  done
  printf '%s' "${out# }"
}
plan() { printf 'run_trino: [dry-run] %s\n' "$*" >&3; }
# Runs a command that changes Docker state, or only shows it in dry-run mode.
run_or_show() {
  if [ "$DRY_RUN" = 1 ]; then plan "$(quote_cmd "$@")"; return 0; fi
  "$@"
}

# Prints KEY from a shell-sourceable env file without polluting this shell.
env_value() { ( set +u; . "$1" >/dev/null 2>&1; eval "printf '%s' \"\${$2:-}\"" ); }
# Brace-quotes a connection-string value when it contains characters that would end it.
cs_value() {
  case "$1" in
    *'}'*) die "value '$1' cannot be used in a connection string (contains '}')" ;;
    *';'*|*'='*|*'{'*|' '*) printf '{%s}' "$1" ;;
    *) printf '%s' "$1" ;;
  esac
}
# SHA-256 of stdin with whatever tool the host has.
sha256_stdin() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 | awk '{print $1}'
  elif command -v openssl >/dev/null 2>&1; then openssl dgst -sha256 | awk '{print $NF}'
  else cksum | awk '{print $1 "-" $2}'
  fi
}
publish() {  # HOST-PORT CONTAINER-PORT -> "-p" argument
  if [ -n "$BIND_ADDRESS" ]; then printf '%s:%s:%s' "$BIND_ADDRESS" "$1" "$2"; else printf '%s:%s' "$1" "$2"; fi
}
# Makes ASSETS absolute and symlink-free: docker needs that for bind sources, and connection.env
# must name the certificate with a path valid from any working directory.
absolute_assets() {
  if [ -d "$ASSETS" ]; then
    ASSETS="$(cd "$ASSETS" && pwd -P)"
  else
    case "$ASSETS" in /*) ;; *) ASSETS="$PWD/$ASSETS" ;; esac
  fi
}

container_state() {
  [ "$DOCKER_OK" = 1 ] || { printf 'missing'; return 0; }
  "$DOCKER" container inspect -f '{{.State.Status}}' "$1" 2>/dev/null || printf 'missing'
}
container_label() {
  [ "$DOCKER_OK" = 1 ] || return 0
  "$DOCKER" container inspect -f "{{index .Config.Labels \"$2\"}}" "$1" 2>/dev/null || true
}
container_running() { [ "$(container_state "$1")" = running ]; }

# Creates/starts/reuses NAME so that it runs with the configuration identified by SPEC (a hash
# stored as a label at creation). CREATE_FN performs the docker run.
ensure_container() {
  local name=$1 spec=$2 create_fn=$3 state label
  state="$(container_state "$name")"
  if [ "$state" != missing ]; then
    label="$(container_label "$name" "$LABEL_SPEC")"
    if [ "$RECREATE" = 1 ]; then
      log "removing $name (--recreate)"
      run_or_show "$DOCKER" rm -f -v "$name" >/dev/null
    elif [ "$label" != "$spec" ]; then
      log "recreating $name: its image or configuration differs from the current files"
      run_or_show "$DOCKER" rm -f -v "$name" >/dev/null
    else
      case "$state" in
        running) log "reusing running container $name"; return 0 ;;
        paused) log "unpausing $name"; run_or_show "$DOCKER" unpause "$name" >/dev/null; return 0 ;;
        exited|created) log "starting existing container $name"; run_or_show "$DOCKER" start "$name" >/dev/null; return 0 ;;
        *) log "recreating $name (state: $state)"; run_or_show "$DOCKER" rm -f -v "$name" >/dev/null ;;
      esac
    fi
  fi
  log "creating container $name"
  "$create_fn"
}

dump_logs() { "$DOCKER" logs --tail 60 "$1" 2>&1 | sed 's/^/    | /' >&2 || true; }

# Waits until Trino in NAME printed "SERVER STARTED" for its current run, then (when curl is
# available) until /v1/info reports starting=false; otherwise until the image health check passes.
wait_for_trino() {
  local name=$1 url=$2 started deadline now state
  if [ "$DRY_RUN" = 1 ]; then plan "would wait up to ${TIMEOUT}s for $name (SERVER STARTED, then $url/v1/info starting=false)"; return 0; fi
  started="$("$DOCKER" container inspect -f '{{.State.StartedAt}}' "$name")"
  deadline=$(( $(date +%s) + TIMEOUT ))
  log "waiting for $name to log SERVER STARTED (timeout ${TIMEOUT}s)"
  while :; do
    state="$(container_state "$name")"
    if [ "$state" != running ]; then
      dump_logs "$name"
      die "$name is $state (see log excerpt above; 'docker logs $name' has the rest)"
    fi
    if "$DOCKER" logs --since "$started" "$name" 2>&1 | grep -q 'SERVER STARTED'; then break; fi
    now=$(date +%s)
    if [ "$now" -ge "$deadline" ]; then
      dump_logs "$name"
      die "$name did not start within ${TIMEOUT}s (raise TRINO_ODBC_STARTUP_TIMEOUT or give Docker more memory)"
    fi
    sleep 2
  done
  if command -v curl >/dev/null 2>&1; then
    while ! curl -sk --max-time 5 "$url/v1/info" 2>/dev/null | grep -q '"starting":false'; do
      [ "$(date +%s)" -lt "$deadline" ] || die "$url/v1/info does not report starting=false (is $PROBE_HOST the Docker host? see TRINO_ODBC_PROBE_HOST)"
      sleep 1
    done
  elif [ "$("$DOCKER" container inspect -f '{{if .State.Health}}yes{{end}}' "$name")" = yes ]; then
    while [ "$("$DOCKER" container inspect -f '{{.State.Health.Status}}' "$name")" != healthy ]; do
      [ "$(date +%s)" -lt "$deadline" ] || die "$name did not become healthy within ${TIMEOUT}s"
      sleep 2
    done
  fi
  log "$name is ready ($url)"
}

# Verifies the password authenticator end to end with one authenticated query (curl only).
probe_tls_auth() {
  local user=$1 password=$2 url=$3 out i=0
  if [ "$DRY_RUN" = 1 ]; then plan "would verify password authentication ($user) with POST $url/v1/statement"; return 0; fi
  if ! command -v curl >/dev/null 2>&1; then warn "curl not found; password authentication probe skipped"; return 0; fi
  while :; do
    out="$(curl -sk --max-time 15 -u "$user:$password" -H "X-Trino-User: $user" -X POST --data 'SELECT 1' \
             "$url/v1/statement" 2>/dev/null || true)"
    case "$out" in
      *'"id"'*) log "password authentication verified ($user @ $url)"; return 0 ;;
    esac
    i=$((i + 1))
    if [ "$i" -ge 5 ]; then
      die "authenticated query against $url failed: ${out:-<no response>}"
    fi
    sleep 2
  done
}

# Python (stdlib only) client for one spooled query: POST with X-Trino-Query-Data-Encoding: json,
# follow nextUri, decode inline segments, GET + DELETE(ack) spooled ones exactly like the driver
# (segment headers only, no Trino credentials). It turns the silent misconfigurations of this setup
# into a start failure: SSE-C against plain-http MinIO (query error), a spooling key of the wrong
# size (query error) and inlining that stayed on (no spooled segment at all).
IFS= read -r -d '' SPOOLING_PROBE_PY <<'PY' || true
import base64, json, os, ssl, sys, time, urllib.error, urllib.request

url = os.environ["RT_PROBE_URL"].rstrip("/")
user = os.environ["RT_PROBE_USER"]
password = os.environ["RT_PROBE_PASSWORD"]
require_spooled = os.environ.get("RT_PROBE_REQUIRE_SPOOLED") == "1"
deadline = time.time() + float(os.environ.get("RT_PROBE_TIMEOUT") or "120")
sql = "SELECT nationkey, name FROM tpch.tiny.nation"
expected_rows = 25

# Self-signed server certificate: its verification is what the tls-full test run covers.
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
token = base64.b64encode(("%s:%s" % (user, password)).encode("utf-8")).decode("ascii")
trino_headers = {"X-Trino-User": user, "X-Trino-Source": "run_trino.sh-probe", "Authorization": "Basic " + token}


def fail(message):
    sys.stdout.write(message + "\n")
    sys.exit(1)


def request(method, uri, body=None, headers=None, fatal=True):
    """One HTTP exchange; 502/503/504 are retried until the deadline (Trino warming up)."""
    while True:
        req = urllib.request.Request(uri, data=body, method=method, headers=headers or {})
        try:
            with urllib.request.urlopen(req, context=ctx, timeout=30) as resp:
                return resp.read()
        except urllib.error.HTTPError as e:
            if e.code in (502, 503, 504) and time.time() < deadline:
                time.sleep(1)
                continue
            message = "%s %s -> HTTP %d %s" % (method, uri, e.code, e.read().decode("utf-8", "replace")[:400])
        except (urllib.error.URLError, OSError) as e:
            message = "%s %s -> %s" % (method, uri, e)
        if fatal:
            fail(message)
        return None


doc = json.loads(request("POST", url + "/v1/statement", sql.encode("utf-8"),
                         dict(trino_headers, **{"X-Trino-Query-Data-Encoding": "json"})))
rows = inline = spooled = acked = 0
classic = False
while True:
    error = doc.get("error")
    if error:
        fail("query failed: %s: %s" % (error.get("errorName"), error.get("message")))
    data = doc.get("data")
    if isinstance(data, list):
        classic = True
        rows += len(data)
    elif isinstance(data, dict):
        for segment in data.get("segments") or []:
            kind = segment.get("type")
            if kind == "inline":
                inline += 1
                rows += len(json.loads(base64.b64decode(segment["data"])))
            elif kind == "spooled":
                headers = {name: ", ".join(value) if isinstance(value, list) else str(value)
                           for name, value in (segment.get("headers") or {}).items()}
                rows += len(json.loads(request("GET", segment["uri"], headers=headers)))
                spooled += 1
                if segment.get("ackUri") and request("DELETE", segment["ackUri"], headers=headers, fatal=False) is not None:
                    acked += 1
            else:
                fail("unknown segment type %r" % (kind,))
    next_uri = doc.get("nextUri")
    if not next_uri:
        break
    if time.time() > deadline:
        fail("query did not finish before the probe timeout")
    doc = json.loads(request("GET", next_uri, headers=trino_headers))

if classic:
    fail("the server answered with the classic protocol (data array): protocol.spooling.enabled is not in effect")
if rows != expected_rows:
    fail("expected %d rows, decoded %d" % (expected_rows, rows))
if require_spooled and spooled == 0:
    fail("all %d segment(s) came inline although protocol.spooling.inlining.enabled=false; check "
         "spooling-manager.properties (s3.*, fs.segment.encryption=false) and look for 'falling back to "
         "inlining' in the server log" % inline)
print("spooled protocol verified: %d rows, %d spooled segment(s) downloaded (%d acknowledged), %d inline"
      % (rows, spooled, acked, inline))
PY

probe_spooling() {  # URL USER PASSWORD
  local out require=0
  [ "$INLINING" = on ] || require=1
  if [ "$DRY_RUN" = 1 ]; then
    plan "would verify the spooled protocol with $PYTHON (SELECT from tpch.tiny.nation via $1, $([ "$require" = 1 ] && printf 'spooled segments required' || printf 'inline segments allowed'))"
    return 0
  fi
  if ! command -v "$PYTHON" >/dev/null 2>&1; then warn "'$PYTHON' not found; spooled-protocol probe skipped"; return 0; fi
  if out="$(RT_PROBE_URL="$1" RT_PROBE_USER="$2" RT_PROBE_PASSWORD="$3" RT_PROBE_REQUIRE_SPOOLED="$require" \
            RT_PROBE_TIMEOUT="$TIMEOUT" "$PYTHON" -c "$SPOOLING_PROBE_PY" 2>&1)"; then
    log "$out"
  else
    dump_logs "$TLS_CONTAINER"
    die "spooled-protocol probe against $1 failed: $out"
  fi
}

# ---- image ---------------------------------------------------------------------------------------
image_present() { [ "$DOCKER_OK" = 1 ] && "$DOCKER" image inspect "$1" >/dev/null 2>&1; }
image_pull() {
  log "pulling $1"
  if [ "$DRY_RUN" = 1 ]; then plan "$(quote_cmd "$DOCKER" pull "$1")"; return 0; fi
  "$DOCKER" pull "$1" >/dev/null 2>&1
}

resolve_image() {
  local candidate
  if [ -n "$IMAGE_OVERRIDE" ]; then
    image_present "$IMAGE_OVERRIDE" || image_pull "$IMAGE_OVERRIDE" || die "image '$IMAGE_OVERRIDE' is neither present nor pullable"
    IMAGE="$IMAGE_OVERRIDE"
  else
    for candidate in "$PRIMARY_IMAGE" "$FALLBACK_IMAGE"; do
      if image_present "$candidate"; then IMAGE="$candidate"; break; fi
    done
    if [ -z "$IMAGE" ]; then
      for candidate in "$PRIMARY_IMAGE" "$FALLBACK_IMAGE"; do
        if image_pull "$candidate"; then IMAGE="$candidate"; break; fi
      done
    fi
    [ -n "$IMAGE" ] || die "neither $PRIMARY_IMAGE nor $FALLBACK_IMAGE is available (docker pull failed)"
    [ "$IMAGE" = "$PRIMARY_IMAGE" ] || warn "using fallback image $IMAGE ($PRIMARY_IMAGE unavailable)"
  fi
  log "Trino image: $IMAGE"
}

# ---- assets --------------------------------------------------------------------------------------
write_if_different() {  # FILE CONTENT
  if [ "$DRY_RUN" = 1 ]; then
    if [ ! -f "$1" ] || [ "$(cat "$1")" != "$2" ]; then plan "would write $1"; fi
    return 0
  fi
  if [ ! -f "$1" ] || [ "$(cat "$1")" != "$2" ]; then printf '%s\n' "$2" > "$1"; fi
  chmod 644 "$1"
}

# Both containers get the same explicit catalogs (tpch for the read-only tests, memory for DML/DDL)
# instead of whatever the image ships with.
TPCH_CATALOG="connector.name=tpch
tpch.splits-per-node=4"
MEMORY_CATALOG="connector.name=memory"

ensure_catalogs() {
  if [ "$DRY_RUN" = 1 ]; then
    [ -d "$ASSETS/catalog" ] || plan "would create $ASSETS/catalog"
  else
    mkdir -p "$ASSETS/catalog"
    chmod 755 "$ASSETS/catalog"
  fi
  write_if_different "$ASSETS/catalog/tpch.properties" "$TPCH_CATALOG"
  write_if_different "$ASSETS/catalog/memory.properties" "$MEMORY_CATALOG"
}

ensure_tls_assets() {
  [ -x "$SCRIPT_DIR/gen_tls_assets.sh" ] || die "$SCRIPT_DIR/gen_tls_assets.sh missing or not executable"
  local gen_args=()
  if [ "$QUIET" = 1 ]; then gen_args+=(--quiet); fi
  gen_args+=("$ASSETS")
  local gen_env=(TRINO_ODBC_TLS_CONTAINER="$TLS_CONTAINER"
                 TRINO_ODBC_MINIO_ENDPOINT="${TRINO_ODBC_MINIO_ENDPOINT:-http://$MINIO_CONTAINER:9000}"
                 TRINO_ODBC_MINIO_ACCESS_KEY="$MINIO_ACCESS_KEY" TRINO_ODBC_MINIO_SECRET_KEY="$MINIO_SECRET_KEY"
                 TRINO_ODBC_SPOOLING_BUCKET="$SPOOLING_BUCKET")
  if [ "$DRY_RUN" = 1 ]; then
    # gen_tls_assets.sh writes files, so it is only shown; the credentials of existing assets (or
    # the defaults it would use) feed the connection strings printed at the end.
    plan "$(quote_cmd env "${gen_env[@]}" "$SCRIPT_DIR/gen_tls_assets.sh" "${gen_args[@]}")"
    if [ -f "$ASSETS/tls-assets.env" ]; then
      TLS_USER="$(env_value "$ASSETS/tls-assets.env" TRINO_ODBC_TLS_USER)"
      TLS_PASSWORD="$(env_value "$ASSETS/tls-assets.env" TRINO_ODBC_TLS_PASSWORD)"
    fi
    [ -n "$TLS_USER" ] || TLS_USER="${TRINO_ODBC_TLS_USER:-alice}"
    [ -n "$TLS_PASSWORD" ] || TLS_PASSWORD="${TRINO_ODBC_TLS_PASSWORD:-secret}"
    return 0
  fi
  env "${gen_env[@]}" "$SCRIPT_DIR/gen_tls_assets.sh" "${gen_args[@]}"
  TLS_USER="$(env_value "$ASSETS/tls-assets.env" TRINO_ODBC_TLS_USER)"
  TLS_PASSWORD="$(env_value "$ASSETS/tls-assets.env" TRINO_ODBC_TLS_PASSWORD)"
  [ -n "$TLS_USER" ] && [ -n "$TLS_PASSWORD" ] || die "$ASSETS/tls-assets.env is incomplete; rerun gen_tls_assets.sh --force"
}

# The mounted coordinator configuration = gen_tls_assets.sh's rendered config.properties plus the
# options of this script. It is derived into a file of its own so that gen_tls_assets.sh keeps
# owning (and fingerprinting) config.properties, and so that changing --inlining changes the
# container spec (and therefore recreates the container) without regenerating secrets.
TLS_CONFIG_EFFECTIVE="config.effective.properties"
INLINING_KEY_RE='^[[:space:]]*protocol\.spooling\.inlining\.enabled([[:space:]]*[=:]|[[:space:]])'

effective_tls_config() {
  local base="$ASSETS/config.properties" flag=false
  if [ "$INLINING" = on ]; then flag=true; fi
  # Drop any inlining line of the base so the property is defined once (duplicates are rejected).
  if [ -f "$base" ]; then grep -v -E "$INLINING_KEY_RE" "$base" || true; fi
  printf '# ---- appended by run_trino.sh (--inlining %s) ----\n' "$INLINING"
  printf '# off: every result travels as spooled segments (download + ack path of the driver).\n'
  printf 'protocol.spooling.inlining.enabled=%s\n' "$flag"
}

ensure_tls_config() {
  if [ "$DRY_RUN" = 0 ] && [ ! -f "$ASSETS/config.properties" ]; then
    die "$ASSETS/config.properties missing (gen_tls_assets.sh did not render it)"
  fi
  write_if_different "$ASSETS/$TLS_CONFIG_EFFECTIVE" "$(effective_tls_config)"
}

# Host file (in ASSETS) : file name under /etc/trino in the TLS container.
TLS_MOUNTS="$TLS_CONFIG_EFFECTIVE:config.properties password-authenticator.properties:password-authenticator.properties spooling-manager.properties:spooling-manager.properties password.db:password.db keystore.p12:keystore.p12"

# ---- network -------------------------------------------------------------------------------------
ensure_network() {
  if [ "$DOCKER_OK" = 0 ] || ! "$DOCKER" network inspect "$NETWORK" >/dev/null 2>&1; then
    log "creating network $NETWORK"
    run_or_show "$DOCKER" network create "$NETWORK" >/dev/null
  fi
}

# ---- plain Trino ---------------------------------------------------------------------------------
PLAIN_CONFIG="$SCRIPT_DIR/plain/config.properties"

# The catalogs are hashed from their content (identical bytes to the files written by
# ensure_catalogs), so a dry run computes the same spec before the files exist.
plain_spec() {
  {
    printf 'plain image=%s port=%s bind=%s memory=%s\n' "$IMAGE" "$PLAIN_PORT" "$BIND_ADDRESS" "$TRINO_MEMORY"
    cat "$PLAIN_CONFIG"
    printf '%s\n' "$TPCH_CATALOG" "$MEMORY_CATALOG"
  } | sha256_stdin
}

create_plain() {
  local args
  args=(run -d --name "$PLAIN_CONTAINER" --network "$NETWORK" -p "$(publish "$PLAIN_PORT" 8080)"
        --label "$LABEL_ROLE=plain" --label "$LABEL_SPEC=$PLAIN_SPEC"
        -v "$PLAIN_CONFIG:/etc/trino/config.properties:ro"
        -v "$ASSETS/catalog:/etc/trino/catalog:ro")
  [ -z "$TRINO_MEMORY" ] || args+=(--memory "$TRINO_MEMORY")
  run_or_show "$DOCKER" "${args[@]}" "$IMAGE" >/dev/null \
    || die "docker run failed for $PLAIN_CONTAINER (port $PLAIN_PORT in use? set TRINO_ODBC_PLAIN_PORT)"
}

# ---- MinIO ---------------------------------------------------------------------------------------
minio_spec() {
  printf 'minio image=%s port=%s bind=%s access=%s secret=%s\n' "$MINIO_IMAGE" "$MINIO_PORT" "$BIND_ADDRESS" \
    "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" | sha256_stdin
}

create_minio() {
  local args
  args=(run -d --name "$MINIO_CONTAINER" --network "$NETWORK"
        --label "$LABEL_ROLE=minio" --label "$LABEL_SPEC=$MINIO_SPEC"
        -e "MINIO_ROOT_USER=$MINIO_ACCESS_KEY" -e "MINIO_ROOT_PASSWORD=$MINIO_SECRET_KEY")
  if [ -n "$MINIO_PORT" ] && [ "$MINIO_PORT" != 0 ]; then args+=(-p "$(publish "$MINIO_PORT" 9000)"); fi
  run_or_show "$DOCKER" "${args[@]}" "$MINIO_IMAGE" server /data >/dev/null \
    || die "docker run failed for $MINIO_CONTAINER (port $MINIO_PORT in use? set TRINO_ODBC_MINIO_PORT)"
}

# One throw-away mc run: register the server ("mc alias set", which also fails until MinIO answers,
# so the loop below doubles as the readiness wait) and create the bucket ("mc mb -p" succeeds when
# it already exists). Values are passed as positional parameters, never spliced into the script.
MC_SCRIPT='mc alias set local "$1" "$2" "$3" >/dev/null && mc mb -p "local/$4"'

# Creates the spooling bucket with the minio/mc image on the shared network. Only when that image
# is neither present nor pullable does it fall back to the mc binary inside the MinIO container.
ensure_bucket() {
  local deadline err="" mode=image
  if ! image_present "$MC_IMAGE"; then
    if ! image_pull "$MC_IMAGE"; then
      if "$DOCKER" exec "$MINIO_CONTAINER" sh -c 'command -v mc' >/dev/null 2>&1; then
        warn "$MC_IMAGE unavailable; using the mc client inside $MINIO_CONTAINER"
        mode=exec
      else
        die "$MC_IMAGE is neither present nor pullable and $MINIO_CONTAINER has no mc client"
      fi
    fi
  fi
  log "creating bucket $SPOOLING_BUCKET on $MINIO_CONTAINER (mc alias set + mc mb -p)"
  if [ "$DRY_RUN" = 1 ]; then
    plan "$(quote_cmd "$DOCKER" run --rm --network "$NETWORK" --entrypoint sh "$MC_IMAGE" -c "$MC_SCRIPT" mc-setup \
                      "http://$MINIO_CONTAINER:9000" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" "$SPOOLING_BUCKET")"
    return 0
  fi
  deadline=$(( $(date +%s) + TIMEOUT ))
  while :; do
    if [ "$mode" = image ]; then
      err="$("$DOCKER" run --rm --network "$NETWORK" --entrypoint sh "$MC_IMAGE" -c "$MC_SCRIPT" mc-setup \
               "http://$MINIO_CONTAINER:9000" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" "$SPOOLING_BUCKET" 2>&1)" && break
    else
      err="$("$DOCKER" exec "$MINIO_CONTAINER" sh -c "$MC_SCRIPT" mc-setup \
               "http://127.0.0.1:9000" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" "$SPOOLING_BUCKET" 2>&1)" && break
    fi
    container_running "$MINIO_CONTAINER" || { dump_logs "$MINIO_CONTAINER"; die "$MINIO_CONTAINER stopped"; }
    [ "$(date +%s)" -lt "$deadline" ] || die "could not create bucket $SPOOLING_BUCKET: $err"
    sleep 2
  done
  log "bucket $SPOOLING_BUCKET ready"
}

# ---- TLS Trino -----------------------------------------------------------------------------------
TLS_USER=""
TLS_PASSWORD=""

# Hash of everything the TLS container is created from. The effective config is hashed from the
# content it has (or, in a dry run, would have), so --inlining changes are detected either way.
tls_spec() {
  local m src
  for m in $TLS_MOUNTS; do
    src="${m%%:*}"
    if [ "$src" != "$TLS_CONFIG_EFFECTIVE" ] && [ ! -f "$ASSETS/$src" ]; then printf 'pending-assets'; return 0; fi
  done
  [ -f "$ASSETS/config.properties" ] || { printf 'pending-assets'; return 0; }
  {
    printf 'tls image=%s https=%s http=%s bind=%s memory=%s\n' "$IMAGE" "$TLS_PORT" "$TLS_HTTP_PORT" "$BIND_ADDRESS" "$TRINO_MEMORY"
    for m in $TLS_MOUNTS; do
      src="${m%%:*}"
      if [ "$src" = "$TLS_CONFIG_EFFECTIVE" ]; then printf '%s\n' "$(effective_tls_config)"; else cat "$ASSETS/$src"; fi
    done
    printf '%s\n' "$TPCH_CATALOG" "$MEMORY_CATALOG"
  } | sha256_stdin
}

create_tls() {
  local args m
  args=(run -d --name "$TLS_CONTAINER" --network "$NETWORK"
        -p "$(publish "$TLS_PORT" 8443)" -p "$(publish "$TLS_HTTP_PORT" 8080)"
        --label "$LABEL_ROLE=tls" --label "$LABEL_SPEC=$TLS_SPEC")
  for m in $TLS_MOUNTS; do args+=(-v "$ASSETS/${m%%:*}:/etc/trino/${m#*:}:ro"); done
  args+=(-v "$ASSETS/catalog:/etc/trino/catalog:ro")
  [ -z "$TRINO_MEMORY" ] || args+=(--memory "$TRINO_MEMORY")
  run_or_show "$DOCKER" "${args[@]}" "$IMAGE" >/dev/null \
    || die "docker run failed for $TLS_CONTAINER (ports $TLS_PORT/$TLS_HTTP_PORT in use? set TRINO_ODBC_TLS_PORT / TRINO_ODBC_TLS_HTTP_PORT)"
}

# ---- connection strings --------------------------------------------------------------------------
tail_keys() { printf 'Catalog=%s;Schema=%s' "$(cs_value "$TEST_CATALOG")" "$(cs_value "$TEST_SCHEMA")"; }
plain_connstr() { printf 'Host=%s;Port=%s;UID=%s;%s' "$(cs_value "$HOST_NAME")" "$PLAIN_PORT" "$(cs_value "$PLAIN_USER")" "$(tail_keys)"; }
tls_connstr() {
  printf 'Host=%s;Port=%s;SSL=1;SSLVerification=None;AuthenticationType=LDAP;UID=%s;PWD=%s;%s' \
    "$(cs_value "$HOST_NAME")" "$TLS_PORT" "$(cs_value "$TLS_USER")" "$(cs_value "$TLS_PASSWORD")" "$(tail_keys)"
}
tls_full_connstr() {
  printf 'Host=%s;Port=%s;SSL=1;SSLVerification=Full;TrustedCerts=%s;AuthenticationType=LDAP;UID=%s;PWD=%s;%s' \
    "$(cs_value "$HOST_NAME")" "$TLS_PORT" "$(cs_value "$ASSETS/cert.pem")" "$(cs_value "$TLS_USER")" \
    "$(cs_value "$TLS_PASSWORD")" "$(tail_keys)"
}
tls_http_connstr() {
  printf 'Host=%s;Port=%s;AuthenticationType=LDAP;UID=%s;PWD=%s;AllowInsecurePassword=1;%s' \
    "$(cs_value "$HOST_NAME")" "$TLS_HTTP_PORT" "$(cs_value "$TLS_USER")" "$(cs_value "$TLS_PASSWORD")" "$(tail_keys)"
}

# Loads the TLS credentials of an existing container's assets (for connstr/status without a start).
load_tls_credentials() {
  [ -f "$ASSETS/tls-assets.env" ] || return 1
  TLS_USER="$(env_value "$ASSETS/tls-assets.env" TRINO_ODBC_TLS_USER)"
  TLS_PASSWORD="$(env_value "$ASSETS/tls-assets.env" TRINO_ODBC_TLS_PASSWORD)"
  [ -n "$TLS_USER" ] && [ -n "$TLS_PASSWORD" ] && [ -f "$ASSETS/cert.pem" ]
}

# Inlining state of the TLS container: what this run configured, or (connstr) what the
# config.properties mounted into the running container says (a container created by an older
# version of this script mounts config.properties itself, which leaves Trino's default: on).
tls_inlining_state() {
  local value src="$ASSETS/$TLS_CONFIG_EFFECTIVE" mounted
  if [ "$COMMAND" = start ]; then printf '%s' "$INLINING"; return 0; fi
  if [ "$DOCKER_OK" = 1 ]; then
    mounted="$("$DOCKER" container inspect -f '{{range .Mounts}}{{if eq .Destination "/etc/trino/config.properties"}}{{.Source}}{{end}}{{end}}' "$TLS_CONTAINER" 2>/dev/null || true)"
    if [ -n "$mounted" ] && [ -f "$mounted" ]; then src="$mounted"; fi
  fi
  [ -f "$src" ] || { printf 'unknown'; return 0; }
  value="$(grep -E "$INLINING_KEY_RE" "$src" | tail -1 | sed 's/^[^=:]*[=:][[:space:]]*//' || true)"
  case "$value" in
    true|'') printf 'on' ;;
    false) printf 'off' ;;
    *) printf 'unknown' ;;
  esac
}

# Prints the connection strings of the running containers and writes <assets>/connection.env.
report_connections() {
  local env_file="$ASSETS/connection.env" have_plain=0 have_tls=0 image inlining="" content
  if [ "$DRY_RUN" = 1 ] && [ "$COMMAND" = start ]; then
    # Nothing was started: report what a real start would produce.
    have_plain=$WANT_PLAIN
    have_tls=$WANT_TLS
  else
    if container_running "$PLAIN_CONTAINER"; then have_plain=1; fi
    if container_running "$TLS_CONTAINER"; then
      if [ -n "$TLS_USER" ] || load_tls_credentials; then have_tls=1; else warn "$TLS_CONTAINER is running but $ASSETS/tls-assets.env is missing; TLS connection strings omitted"; fi
    fi
  fi
  [ "$have_plain" = 1 ] || [ "$have_tls" = 1 ] || die "no Trino test container is running (run '$0 start')"
  image="$IMAGE"
  if [ -z "$image" ]; then
    if [ "$have_plain" = 1 ]; then image="$("$DOCKER" container inspect -f '{{.Config.Image}}' "$PLAIN_CONTAINER")"
    else image="$("$DOCKER" container inspect -f '{{.Config.Image}}' "$TLS_CONTAINER")"; fi
  fi
  [ "$have_tls" = 0 ] || inlining="$(tls_inlining_state)"
  content="$(
    echo "# Generated by tests/docker/trino/run_trino.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ); sourced by run_trino_tests.sh."
    echo "TRINO_ODBC_TRINO_IMAGE=$(shell_quote "$image")"
    echo "TRINO_ODBC_TRINO_HOST=$(shell_quote "$HOST_NAME")"
    if [ "$have_plain" = 1 ]; then
      echo "TRINO_ODBC_PLAIN_URL=$(shell_quote "http://$HOST_NAME:$PLAIN_PORT")"
      echo "TRINO_ODBC_PLAIN_CONNSTR=$(shell_quote "$(plain_connstr)")"
    fi
    if [ "$have_tls" = 1 ]; then
      echo "TRINO_ODBC_TLS_URL=$(shell_quote "https://$HOST_NAME:$TLS_PORT")"
      echo "TRINO_ODBC_TLS_HTTP_URL=$(shell_quote "http://$HOST_NAME:$TLS_HTTP_PORT")"
      echo "TRINO_ODBC_TLS_CERT=$(shell_quote "$ASSETS/cert.pem")"
      echo "TRINO_ODBC_TLS_USER=$(shell_quote "$TLS_USER")"
      echo "TRINO_ODBC_TLS_PASSWORD=$(shell_quote "$TLS_PASSWORD")"
      echo "TRINO_ODBC_TLS_SPOOLING_INLINING=$(shell_quote "$inlining")"
      echo "TRINO_ODBC_TLS_CONNSTR=$(shell_quote "$(tls_connstr)")"
      echo "TRINO_ODBC_TLS_FULL_CONNSTR=$(shell_quote "$(tls_full_connstr)")"
      echo "TRINO_ODBC_TLS_HTTP_CONNSTR=$(shell_quote "$(tls_http_connstr)")"
    fi
  )"
  if [ "$DRY_RUN" = 1 ]; then
    plan "would write $env_file"
  else
    mkdir -p "$ASSETS"
    printf '%s\n' "$content" > "$env_file"
    chmod 644 "$env_file"
  fi

  echo
  if [ "$DRY_RUN" = 1 ]; then
    echo "Dry run (image $image). Connection strings a start would produce for trino_odbc_integration_tests --connstr:"
  else
    echo "Trino test containers ready (image $image). Connection strings for trino_odbc_integration_tests --connstr:"
  fi
  if [ "$have_plain" = 1 ]; then
    echo "  plain (http, no auth)                 $(plain_connstr)"
  fi
  if [ "$have_tls" = 1 ]; then
    echo "  tls (password auth, no verification)  $(tls_connstr)"
    echo "  tls-full (trusted cert + hostname)    $(tls_full_connstr)"
    echo "  tls-http (password over plain http)   $(tls_http_connstr)"
    echo "  certificate                           $ASSETS/cert.pem"
    echo "  spooled protocol (tls containers)     inlining $inlining"
  fi
  echo "  env file                              $env_file"
  echo
}

# ---- commands ------------------------------------------------------------------------------------
cmd_start() {
  if [ "$DRY_RUN" = 1 ]; then plan "nothing below is executed; containers, network and files stay as they are"; fi
  resolve_image
  if [ "$DRY_RUN" = 1 ]; then
    [ -d "$ASSETS" ] || plan "would create $ASSETS"
  else
    mkdir -p "$ASSETS"
  fi
  absolute_assets
  ensure_network
  ensure_catalogs
  if [ "$WANT_PLAIN" = 1 ]; then
    PLAIN_SPEC="$(plain_spec)"
    ensure_container "$PLAIN_CONTAINER" "$PLAIN_SPEC" create_plain
  fi
  if [ "$WANT_TLS" = 1 ]; then
    ensure_tls_assets
    ensure_tls_config
    MINIO_SPEC="$(minio_spec)"
    ensure_container "$MINIO_CONTAINER" "$MINIO_SPEC" create_minio
    ensure_bucket
    TLS_SPEC="$(tls_spec)"
    ensure_container "$TLS_CONTAINER" "$TLS_SPEC" create_tls
  fi
  # Both Trinos boot in parallel; wait for them one after the other.
  if [ "$WANT_PLAIN" = 1 ]; then wait_for_trino "$PLAIN_CONTAINER" "http://$PROBE_HOST:$PLAIN_PORT"; fi
  if [ "$WANT_TLS" = 1 ]; then
    wait_for_trino "$TLS_CONTAINER" "https://$PROBE_HOST:$TLS_PORT"
    if [ "$PROBE" = 1 ]; then
      probe_tls_auth "$TLS_USER" "$TLS_PASSWORD" "https://$PROBE_HOST:$TLS_PORT"
      probe_spooling "https://$PROBE_HOST:$TLS_PORT" "$TLS_USER" "$TLS_PASSWORD"
    fi
  fi
  report_connections
}

selected_containers() {
  # TLS first so that a stop/remove takes the dependants down before MinIO.
  if [ "$WANT_TLS" = 1 ]; then printf '%s\n' "$TLS_CONTAINER" "$MINIO_CONTAINER"; fi
  if [ "$WANT_PLAIN" = 1 ]; then printf '%s\n' "$PLAIN_CONTAINER"; fi
  return 0
}

cmd_stop() {
  local c
  for c in $(selected_containers); do
    if container_running "$c" || { [ "$DRY_RUN" = 1 ] && [ "$DOCKER_OK" = 0 ]; }; then
      log "stopping $c"
      run_or_show "$DOCKER" stop "$c" >/dev/null
    else
      log "$c is not running"
    fi
  done
}

cmd_remove() {
  local c
  for c in $(selected_containers); do
    if [ "$(container_state "$c")" != missing ] || { [ "$DRY_RUN" = 1 ] && [ "$DOCKER_OK" = 0 ]; }; then
      log "removing $c"
      run_or_show "$DOCKER" rm -f -v "$c" >/dev/null
    fi
  done
  if [ "$WANT_PLAIN" = 1 ] && [ "$WANT_TLS" = 1 ]; then
    if [ "$DRY_RUN" = 1 ]; then
      plan "would remove network $NETWORK if no other container is attached to it"
    elif "$DOCKER" network inspect "$NETWORK" >/dev/null 2>&1; then
      if [ "$("$DOCKER" network inspect -f '{{len .Containers}}' "$NETWORK")" = 0 ]; then
        log "removing network $NETWORK"
        "$DOCKER" network rm "$NETWORK" >/dev/null
      else
        log "network $NETWORK still has other containers attached; kept"
      fi
    fi
  fi
  log "generated assets in $ASSETS were kept (delete the directory to regenerate certificates and secrets)"
}

cmd_status() {
  local c state health image
  printf '%-18s %-10s %-10s %s\n' CONTAINER STATE HEALTH IMAGE
  for c in "$PLAIN_CONTAINER" "$MINIO_CONTAINER" "$TLS_CONTAINER"; do
    state="$(container_state "$c")"
    if [ "$state" = missing ]; then
      printf '%-18s %-10s %-10s %s\n' "$c" missing - -
      continue
    fi
    health="$("$DOCKER" container inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}-{{end}}' "$c")"
    image="$("$DOCKER" container inspect -f '{{.Config.Image}}' "$c")"
    printf '%-18s %-10s %-10s %s\n' "$c" "$state" "$health" "$image"
  done
}

case "$COMMAND" in
  start) cmd_start ;;
  stop) cmd_stop ;;
  remove) absolute_assets; cmd_remove ;;
  status) cmd_status ;;
  connstr) absolute_assets; report_connections ;;
esac
