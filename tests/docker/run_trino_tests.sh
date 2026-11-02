#!/usr/bin/env bash
# Runs the ODBC integration tests against real Trino containers (started by trino/run_trino.sh).
#
#   run_trino_tests.sh [options] [INTEGRATION_TEST_BINARY] [-- <extra arguments for the binary>]
#
# trino_odbc_integration_tests is executed once per configuration:
#   plain     Host=localhost;Port=18080;UID=test;Catalog=tpch;Schema=tiny
#   tls       Host=localhost;Port=18443;SSL=1;SSLVerification=None;AuthenticationType=LDAP;UID=alice;PWD=secret;Catalog=tpch;Schema=tiny
#   tls-full  as tls but SSLVerification=Full;TrustedCerts=<generated cert.pem>
#   tls-http  (only when selected with --only) password auth over the TLS container's plain http port
#             18081 with AllowInsecurePassword=1
# The exact strings come from <assets>/connection.env written by run_trino.sh, so every port, user
# or host override of that script applies here too. The TLS container serves every result through
# the spooled protocol (run_trino.sh --inlining off, the default), so the tls* runs cover segment
# download + ack, the plain run the classic protocol. The exit status is 0 only when every selected
# configuration passes; the output of each run is also saved under --log-dir.
#
# Options (environment variable and default in brackets):
#   --build-dir DIR    directory holding tests/trino_odbc_integration_tests [TRINO_ODBC_BUILD_DIR;
#                      otherwise the first of build, build-macos, build-linux, build-*, cmake-build-*
#                      under the repository that contains the binary]
#   --driver PATH      driver library to load [TRINO_ODBC_DRIVER; default: the path compiled into the
#                      test binary]
#   --only LIST        comma-separated subset of plain,tls,tls-full,tls-http [default: plain,tls,tls-full]
#   --filter TEXT      only tests whose name contains TEXT (forwarded as --filter)
#   --loglevel N       driver LogLevel for the runs (forwarded as --loglevel)
#   --inlining on|off  forwarded to run_trino.sh [TRINO_ODBC_SPOOLING_INLINING=off]
#   --recreate         forwarded to run_trino.sh: recreate the containers before the run
#   --no-start         do not run run_trino.sh; use the containers/connection.env as they are
#   --stop | --remove  stop / remove the containers after the run (default: keep them running)
#   --log-dir DIR      per-configuration logs [TRINO_ODBC_TEST_LOGDIR=<assets>/test-logs]
#   --assets-dir DIR   generated-assets directory of run_trino.sh [TRINO_ODBC_DOCKER_ASSETS]
#   --keep-going       run the remaining configurations after a failure (the default)
#   --fail-fast        stop at the first failing configuration
#   -n, --dry-run      show what would run (run_trino.sh --dry-run, test command lines) and exit;
#                      nothing is started, stopped, written or executed
#   -h, --help
# Any other run_trino.sh variable (TRINO_ODBC_TRINO_IMAGE, TRINO_ODBC_PLAIN_PORT, ...) is honoured.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_TRINO="$SCRIPT_DIR/trino/run_trino.sh"

BINARY=""
BUILD_DIR="${TRINO_ODBC_BUILD_DIR:-}"
DRIVER="${TRINO_ODBC_DRIVER:-}"
ONLY="plain,tls,tls-full"
FILTER=""
LOGLEVEL=""
INLINING=""
RECREATE=0
START=1
AFTER=""
LOG_DIR="${TRINO_ODBC_TEST_LOGDIR:-}"
ASSETS="${TRINO_ODBC_DOCKER_ASSETS:-$SCRIPT_DIR/trino/generated}"
FAIL_FAST=0
DRY_RUN=0
EXTRA_ARGS=()

usage() { sed -n '2,/^[^#]/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }
log() { printf 'run_trino_tests: %s\n' "$*"; }
plan() { printf 'run_trino_tests: [dry-run] %s\n' "$*"; }
die() { printf 'run_trino_tests: error: %s\n' "$*" >&2; exit 1; }
need_value() { [ $# -ge 2 ] || die "$1 needs a value"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) need_value "$@"; shift; BUILD_DIR="$1" ;;
    --build-dir=*) BUILD_DIR="${1#--build-dir=}" ;;
    --driver) need_value "$@"; shift; DRIVER="$1" ;;
    --driver=*) DRIVER="${1#--driver=}" ;;
    --only) need_value "$@"; shift; ONLY="$1" ;;
    --only=*) ONLY="${1#--only=}" ;;
    --filter|-f) need_value "$@"; shift; FILTER="$1" ;;
    --filter=*) FILTER="${1#--filter=}" ;;
    --loglevel) need_value "$@"; shift; LOGLEVEL="$1" ;;
    --loglevel=*) LOGLEVEL="${1#--loglevel=}" ;;
    --inlining) need_value "$@"; shift; INLINING="$1" ;;
    --inlining=*) INLINING="${1#--inlining=}" ;;
    --recreate) RECREATE=1 ;;
    --no-start) START=0 ;;
    --stop) AFTER=stop ;;
    --remove) AFTER=remove ;;
    --log-dir) need_value "$@"; shift; LOG_DIR="$1" ;;
    --log-dir=*) LOG_DIR="${1#--log-dir=}" ;;
    --assets-dir) need_value "$@"; shift; ASSETS="$1" ;;
    --assets-dir=*) ASSETS="${1#--assets-dir=}" ;;
    --keep-going) FAIL_FAST=0 ;;
    --fail-fast) FAIL_FAST=1 ;;
    -n|--dry-run) DRY_RUN=1 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; EXTRA_ARGS=("$@"); break ;;
    -*) die "unknown option '$1' (see --help)" ;;
    *) [ -z "$BINARY" ] || die "unexpected argument '$1' (use -- before extra test arguments)"; BINARY="$1" ;;
  esac
  shift
done
case "$LOGLEVEL" in ''|[0-5]) ;; *) die "--loglevel must be 0..5" ;; esac
[ -n "$ASSETS" ] || die "--assets-dir must not be empty"
if [ "$START" = 0 ] && { [ -n "$INLINING" ] || [ "$RECREATE" = 1 ]; }; then
  die "--inlining/--recreate configure the containers and cannot be combined with --no-start"
fi

# ---- configurations ------------------------------------------------------------------------------
NEED_PLAIN=0
NEED_TLS=0
SELECTED=()
IFS=',' read -r -a SELECTED <<< "$ONLY"
[ "${#SELECTED[@]}" -gt 0 ] || die "--only needs at least one configuration"
for cfg in "${SELECTED[@]}"; do
  case "$cfg" in
    plain) NEED_PLAIN=1 ;;
    tls|tls-full|tls-http) NEED_TLS=1 ;;
    *) die "unknown configuration '$cfg' (plain, tls, tls-full, tls-http)" ;;
  esac
done

# ---- locate the integration test binary ---------------------------------------------------------
find_binary() {
  local d
  for d in "$ROOT/build" "$ROOT/build-macos" "$ROOT/build-linux" "$ROOT"/build-* "$ROOT"/cmake-build-* "$ROOT/out/build"/*; do
    if [ -x "$d/tests/trino_odbc_integration_tests" ]; then printf '%s' "$d/tests/trino_odbc_integration_tests"; return 0; fi
    if [ -x "$d/tests/trino_odbc_integration_tests.exe" ]; then printf '%s' "$d/tests/trino_odbc_integration_tests.exe"; return 0; fi
  done
  return 1
}

BINARY_PROBLEM=""
if [ -z "$BINARY" ]; then
  if [ -n "$BUILD_DIR" ]; then
    for candidate in "$BUILD_DIR/tests/trino_odbc_integration_tests" "$BUILD_DIR/trino_odbc_integration_tests"; do
      if [ -x "$candidate" ]; then BINARY="$candidate"; break; fi
    done
    [ -n "$BINARY" ] || BINARY_PROBLEM="no trino_odbc_integration_tests under $BUILD_DIR (build with -DTRINO_ODBC_BUILD_TESTS=ON)"
  else
    BINARY="$(find_binary)" || BINARY_PROBLEM="trino_odbc_integration_tests not found; pass its path, --build-dir, or build the project first"
  fi
fi
if [ -z "$BINARY_PROBLEM" ] && [ ! -x "$BINARY" ]; then BINARY_PROBLEM="$BINARY is not an executable"; fi
if [ -n "$BINARY_PROBLEM" ]; then
  # A dry run is useful before the first build too; it only reports the problem.
  [ "$DRY_RUN" = 1 ] || die "$BINARY_PROBLEM"
  plan "$BINARY_PROBLEM"
  BINARY="${BINARY:-<trino_odbc_integration_tests>}"
else
  case "$BINARY" in
    /*) ;;
    *) BINARY="$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")" ;;
  esac
fi
if [ -n "$DRIVER" ]; then
  if [ ! -f "$DRIVER" ]; then
    [ "$DRY_RUN" = 1 ] || die "driver library $DRIVER not found"
    plan "driver library $DRIVER not found"
  fi
  case "$DRIVER" in /*) ;; *) DRIVER="$PWD/$DRIVER" ;; esac
  export TRINO_ODBC_DRIVER="$DRIVER"
fi

# ---- containers ----------------------------------------------------------------------------------
[ -x "$RUN_TRINO" ] || die "$RUN_TRINO missing or not executable"
trino_args() {  # common run_trino.sh arguments for the selected configurations
  printf '%s\n' --assets-dir "$ASSETS"
  if [ "$NEED_TLS" = 0 ]; then printf '%s\n' --plain-only; elif [ "$NEED_PLAIN" = 0 ]; then printf '%s\n' --tls-only; fi
  if [ "$DRY_RUN" = 1 ]; then printf '%s\n' --dry-run; fi
}
TRINO_ARGS=()
while IFS= read -r a; do TRINO_ARGS+=("$a"); done < <(trino_args)

if [ "$START" = 1 ]; then
  START_ARGS=("${TRINO_ARGS[@]}")
  if [ -n "$INLINING" ]; then START_ARGS+=(--inlining "$INLINING"); fi
  if [ "$RECREATE" = 1 ]; then START_ARGS+=(--recreate); fi
  "$RUN_TRINO" "${START_ARGS[@]}" start
fi
ENV_FILE="$ASSETS/connection.env"
if [ -f "$ENV_FILE" ]; then
  # shellcheck disable=SC1090
  . "$ENV_FILE"
  if [ "$DRY_RUN" = 1 ]; then plan "connection strings below come from the existing $ENV_FILE (a start rewrites it)"; fi
elif [ "$DRY_RUN" = 1 ]; then
  plan "$ENV_FILE does not exist yet; run_trino.sh start writes it (connection strings as printed above)"
else
  die "$ENV_FILE not found (run $RUN_TRINO start first, or drop --no-start)"
fi

[ -n "$LOG_DIR" ] || LOG_DIR="$ASSETS/test-logs"
if [ "$DRY_RUN" = 0 ]; then mkdir -p "$LOG_DIR"; fi

# ---- run -----------------------------------------------------------------------------------------
TEST_ARGS=()
[ -z "$FILTER" ] || TEST_ARGS+=(--filter "$FILTER")
[ -z "$LOGLEVEL" ] || TEST_ARGS+=(--loglevel "$LOGLEVEL")
if [ "${#EXTRA_ARGS[@]}" -gt 0 ]; then TEST_ARGS+=("${EXTRA_ARGS[@]}"); fi

mask_password() { printf '%s' "$1" | sed -e 's/PWD={[^}]*}/PWD={***}/g' -e 's/PWD=[^;]*/PWD=***/g'; }

RESULT_NAMES=()
RESULT_CODES=()
RESULT_NOTES=()
RESULT_SECONDS=()

run_config() {  # NAME CONNSTR
  local name=$1 connstr=$2 logfile rc started summary
  logfile="$LOG_DIR/$name.log"   # separate statement: `local a=$1 b=$a` expands $a before assigning it
  if [ "$DRY_RUN" = 1 ]; then
    plan "[$name] (cd $(dirname "$BINARY") && $BINARY --connstr '$(mask_password "$connstr")'${TEST_ARGS[*]+ ${TEST_ARGS[*]}}) | tee $logfile"
    return 0
  fi
  echo
  echo "================================================================================"
  echo "== [$name] $BINARY --connstr '$(mask_password "$connstr")'${TEST_ARGS[*]+ ${TEST_ARGS[*]}}"
  echo "== log: $logfile"
  echo "================================================================================"
  started=$(date +%s)
  set +e
  ( cd "$(dirname "$BINARY")" && "$BINARY" --connstr "$connstr" ${TEST_ARGS[@]+"${TEST_ARGS[@]}"} 2>&1 ) | tee "$logfile"
  rc=${PIPESTATUS[0]}
  set -e
  summary="$(grep -E '^[0-9]+ tests run, ' "$logfile" | tail -1 || true)"
  RESULT_NAMES+=("$name")
  RESULT_CODES+=("$rc")
  RESULT_NOTES+=("${summary:-no summary line (crashed or aborted?)}")
  RESULT_SECONDS+=("$(( $(date +%s) - started ))")
  return "$rc"
}

connstr_for() {  # NAME -> connection string or "" when the container is unavailable
  case "$1" in
    plain) printf '%s' "${TRINO_ODBC_PLAIN_CONNSTR:-}" ;;
    tls) printf '%s' "${TRINO_ODBC_TLS_CONNSTR:-}" ;;
    tls-full) printf '%s' "${TRINO_ODBC_TLS_FULL_CONNSTR:-}" ;;
    tls-http) printf '%s' "${TRINO_ODBC_TLS_HTTP_CONNSTR:-}" ;;
  esac
}

log "binary: $BINARY"
log "driver: ${TRINO_ODBC_DRIVER:-<compiled into the test binary>}"
log "Trino image: ${TRINO_ODBC_TRINO_IMAGE:-unknown}"
if [ "$NEED_TLS" = 1 ]; then log "TLS container spooling inlining: ${TRINO_ODBC_TLS_SPOOLING_INLINING:-unknown}"; fi
FAILED=0
for cfg in "${SELECTED[@]}"; do
  connstr="$(connstr_for "$cfg")"
  if [ -z "$connstr" ]; then
    if [ "$DRY_RUN" = 1 ]; then plan "[$cfg] no connection string yet (written by run_trino.sh start)"; continue; fi
    die "no connection string for '$cfg' in $ENV_FILE (is the matching container running?)"
  fi
  if [ "$cfg" = tls-full ] && [ ! -f "${TRINO_ODBC_TLS_CERT:-}" ]; then
    if [ "$DRY_RUN" = 1 ]; then plan "[$cfg] certificate ${TRINO_ODBC_TLS_CERT:-<unset>} is missing"; continue; fi
    die "certificate ${TRINO_ODBC_TLS_CERT:-<unset>} for tls-full is missing"
  fi
  if ! run_config "$cfg" "$connstr"; then
    FAILED=$((FAILED + 1))
    if [ "$FAIL_FAST" = 1 ]; then log "stopping after the first failure (--fail-fast)"; break; fi
  fi
done

if [ "$DRY_RUN" = 1 ]; then
  if [ -n "$AFTER" ]; then "$RUN_TRINO" "${TRINO_ARGS[@]}" "$AFTER"; fi
  log "dry run finished; nothing was executed"
  exit 0
fi

# ---- summary -------------------------------------------------------------------------------------
echo
echo "================================================================================"
echo "== summary"
echo "================================================================================"
i=0
while [ "$i" -lt "${#RESULT_NAMES[@]}" ]; do
  if [ "${RESULT_CODES[$i]}" = 0 ]; then status=PASS; else status="FAIL (exit ${RESULT_CODES[$i]})"; fi
  printf '  %-9s %-14s %5ss  %s\n' "${RESULT_NAMES[$i]}" "$status" "${RESULT_SECONDS[$i]}" "${RESULT_NOTES[$i]}"
  i=$((i + 1))
done
echo "  logs: $LOG_DIR"

if [ -n "$AFTER" ]; then
  "$RUN_TRINO" "${TRINO_ARGS[@]}" "$AFTER"
fi

if [ "$FAILED" -gt 0 ]; then
  log "$FAILED configuration(s) failed"
  exit 1
fi
log "all configurations passed"
