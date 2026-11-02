#!/usr/bin/env bash
# Builds tests/docker/Dockerfile.ubuntu: an Ubuntu 24.04 + unixODBC image that compiles the driver,
# runs the unit tests, the mock-server integration tests and an isql smoke test through the driver
# manager. The tests run inside the image build, so a successful build is a passed Linux test run.
#
#   run_linux_tests.sh [options] [-- <extra docker build arguments>]
#
# Options (environment variable and default in brackets):
#   --base-image IMAGE   [TRINO_ODBC_BASE_IMAGE=ubuntu:24.04]; an image that already carries the
#                        toolchain (e.g. one built once with --target toolchain --tag trino-odbc-linux-base)
#                        skips the apt step
#   --tag TAG            name of the resulting image [TRINO_ODBC_LINUX_IMAGE=trino-odbc-linux-test]
#   --target STAGE       stop after a stage: toolchain | build | unit-tests | integration-tests | dm-smoke
#   --build-type TYPE    CMake build type [TRINO_ODBC_BUILD_TYPE=Release]
#   --cmake-args "ARGS"  extra configure arguments [TRINO_ODBC_CMAKE_EXTRA_ARGS], e.g. "-DTRINO_ODBC_WERROR=ON"
#   --platform PLATFORM  docker --platform, e.g. linux/amd64 (emulated builds are slow)
#   --no-cache           rebuild every stage
#   --pull               refresh the base image first
#   --shell              open an interactive shell in the built image afterwards
#   --quiet              hide the build output (only the result line is printed)
#   -h, --help
#
# The build context is the repository streamed through tar: build directories, .git and the generated
# Trino assets (tests/docker/trino/generated, which holds test secrets) are left out, so no
# .dockerignore is needed and the image never contains them.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DOCKERFILE_REL="tests/docker/Dockerfile.ubuntu"
DOCKER="${TRINO_ODBC_DOCKER:-docker}"

BASE_IMAGE="${TRINO_ODBC_BASE_IMAGE:-ubuntu:24.04}"
TAG="${TRINO_ODBC_LINUX_IMAGE:-trino-odbc-linux-test}"
TARGET=""
BUILD_TYPE="${TRINO_ODBC_BUILD_TYPE:-Release}"
CMAKE_ARGS="${TRINO_ODBC_CMAKE_EXTRA_ARGS:-}"
PLATFORM=""
NO_CACHE=0
PULL=0
SHELL_AFTER=0
QUIET=0
EXTRA_ARGS=()

usage() { sed -n '2,/^[^#]/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }
log() { printf 'run_linux_tests: %s\n' "$*"; }
die() { printf 'run_linux_tests: error: %s\n' "$*" >&2; exit 1; }
need_value() { [ $# -ge 2 ] || die "$1 needs a value"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --base-image) need_value "$@"; shift; BASE_IMAGE="$1" ;;
    --base-image=*) BASE_IMAGE="${1#--base-image=}" ;;
    --tag|-t) need_value "$@"; shift; TAG="$1" ;;
    --tag=*) TAG="${1#--tag=}" ;;
    --target) need_value "$@"; shift; TARGET="$1" ;;
    --target=*) TARGET="${1#--target=}" ;;
    --build-type) need_value "$@"; shift; BUILD_TYPE="$1" ;;
    --build-type=*) BUILD_TYPE="${1#--build-type=}" ;;
    --cmake-args) need_value "$@"; shift; CMAKE_ARGS="$1" ;;
    --cmake-args=*) CMAKE_ARGS="${1#--cmake-args=}" ;;
    --platform) need_value "$@"; shift; PLATFORM="$1" ;;
    --platform=*) PLATFORM="${1#--platform=}" ;;
    --no-cache) NO_CACHE=1 ;;
    --pull) PULL=1 ;;
    --shell) SHELL_AFTER=1 ;;
    --quiet|-q) QUIET=1 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; EXTRA_ARGS=("$@"); break ;;
    *) die "unknown argument '$1' (see --help; docker arguments go after --)" ;;
  esac
  shift
done

case "$TARGET" in
  ''|toolchain|build|unit-tests|integration-tests|dm-smoke|final) ;;
  *) die "unknown --target '$TARGET' (toolchain, build, unit-tests, integration-tests, dm-smoke)" ;;
esac
[ -f "$ROOT/$DOCKERFILE_REL" ] || die "$ROOT/$DOCKERFILE_REL not found"
[ -f "$ROOT/CMakeLists.txt" ] || die "$ROOT does not look like the repository root"
command -v "$DOCKER" >/dev/null 2>&1 || die "'$DOCKER' not found (install Docker or set TRINO_ODBC_DOCKER)"
"$DOCKER" info >/dev/null 2>&1 || die "cannot talk to the Docker daemon ('$DOCKER info' failed)"
command -v tar >/dev/null 2>&1 || die "tar not found"
command -v gzip >/dev/null 2>&1 || die "gzip not found"

# The context: every file of the repository except build output, VCS metadata and generated
# secrets. COPYFILE_DISABLE keeps macOS bsdtar from adding ._* resource-fork entries. The stream is
# gzipped because docker sniffs only the first bytes of stdin to tell a context archive from a
# Dockerfile, and a tar that starts with a pax extended header (bsdtar writes one for files with
# extended attributes) is misread as a Dockerfile; the gzip magic is unambiguous.
context_tar() {
  COPYFILE_DISABLE=1 tar -C "$ROOT" \
    --exclude='./.git' --exclude='./.idea' --exclude='./.vs' --exclude='./.vscode' \
    --exclude='./build' --exclude='./build-*' --exclude='./cmake-build-*' --exclude='./out' \
    --exclude='./tests/docker/trino/generated' \
    --exclude='*.o' --exclude='*.obj' --exclude='*.pyc' --exclude='__pycache__' \
    -cf - . | gzip -1
}

BUILD_ARGS=(build -f "$DOCKERFILE_REL" -t "$TAG"
            --build-arg "BASE_IMAGE=$BASE_IMAGE"
            --build-arg "BUILD_TYPE=$BUILD_TYPE"
            --build-arg "CMAKE_EXTRA_ARGS=$CMAKE_ARGS")
[ -z "$TARGET" ] || BUILD_ARGS+=(--target "$TARGET")
[ -z "$PLATFORM" ] || BUILD_ARGS+=(--platform "$PLATFORM")
[ "$NO_CACHE" = 0 ] || BUILD_ARGS+=(--no-cache)
[ "$PULL" = 0 ] || BUILD_ARGS+=(--pull)
# BuildKit hides step output by default; plain progress keeps the test logs visible (and greppable
# in CI). The legacy builder (DOCKER_BUILDKIT=0) prints everything and rejects --progress.
if [ "${DOCKER_BUILDKIT:-1}" != 0 ] && [ "$QUIET" = 0 ]; then
  BUILD_ARGS+=(--progress "${TRINO_ODBC_DOCKER_PROGRESS:-plain}")
fi
[ "$QUIET" = 0 ] || BUILD_ARGS+=(--quiet)
if [ "${#EXTRA_ARGS[@]}" -gt 0 ]; then BUILD_ARGS+=("${EXTRA_ARGS[@]}"); fi

log "repository: $ROOT"
log "base image: $BASE_IMAGE   tag: $TAG   build type: $BUILD_TYPE${TARGET:+   target: $TARGET}${CMAKE_ARGS:+   cmake: $CMAKE_ARGS}"
started=$(date +%s)
if context_tar | "$DOCKER" "${BUILD_ARGS[@]}" -; then
  elapsed=$(( $(date +%s) - started ))
  if [ -z "$TARGET" ] || [ "$TARGET" = dm-smoke ] || [ "$TARGET" = final ]; then
    log "PASSED in ${elapsed}s: build, unit tests, mock integration tests and unixODBC isql smoke test (image $TAG)"
  else
    log "stage '$TARGET' built in ${elapsed}s (image $TAG)"
  fi
else
  elapsed=$(( $(date +%s) - started ))
  die "FAILED after ${elapsed}s (see the output above; rerun with --no-cache to force every stage)"
fi

if [ "$SHELL_AFTER" = 1 ]; then
  log "opening a shell in $TAG (driver: /build/libtrinoodbc.so, registered as 'Trino ODBC Driver')"
  exec "$DOCKER" run --rm -it "$TAG" /bin/bash
fi
