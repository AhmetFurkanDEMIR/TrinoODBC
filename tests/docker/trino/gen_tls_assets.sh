#!/usr/bin/env bash
# Generates the TLS, password and spooling assets of the "trino-odbc-tls" test container.
#
#   gen_tls_assets.sh [options] [OUTPUT_DIR]
#
# Writes into OUTPUT_DIR (default: tests/docker/trino/generated):
#   cert.pem / key.pem      self-signed server certificate (SAN: localhost, 127.0.0.1, ::1, the TLS
#                           container name, host.docker.internal). cert.pem is the file the driver's
#                           TrustedCerts= points at for SSLVerification=Full.
#   keystore.p12            PKCS#12 keystore (alias "trino") for http-server.https.keystore.path
#   password.db             Trino file password authenticator: user:iterations:salt-hex:hash-hex
#                           (PBKDF2-HMAC-SHA1, 1000 iterations, 32-byte hash)
#   config.properties       rendered from tls/config.properties (fresh spooling + internal secrets)
#   spooling-manager.properties   rendered from tls/spooling-manager.properties (MinIO endpoint)
#   password-authenticator.properties   copied from tls/
#   tls-assets.env          the values used, shell-sourceable; read by run_trino.sh
#
# Idempotent: when every file exists and was generated with the same parameters nothing is
# touched (the running container keeps matching its mounted files). --force regenerates.
#
# Options (environment variable and default in brackets):
#   --force                    regenerate even when the assets are present and up to date
#   --user NAME                [TRINO_ODBC_TLS_USER=alice]
#   --password PWD             [TRINO_ODBC_TLS_PASSWORD=secret]
#   --keystore-password PWD    [TRINO_ODBC_KEYSTORE_PASSWORD=changeit]
#   --container-name NAME      TLS container name added to the SAN [TRINO_ODBC_TLS_CONTAINER=trino-odbc-tls]
#   --san LIST                 extra subjectAltName entries, "DNS:name,IP:1.2.3.4" [TRINO_ODBC_TLS_EXTRA_SAN]
#   --days N                   certificate validity [TRINO_ODBC_TLS_CERT_DAYS=3650]
#   --minio-endpoint URL       S3 endpoint as seen from Trino [TRINO_ODBC_MINIO_ENDPOINT=http://trino-odbc-minio:9000]
#   --minio-access-key KEY     [TRINO_ODBC_MINIO_ACCESS_KEY=minioadmin]
#   --minio-secret-key KEY     [TRINO_ODBC_MINIO_SECRET_KEY=minioadmin]
#   --bucket NAME              spooling bucket [TRINO_ODBC_SPOOLING_BUCKET=spooling]
#   -q, --quiet                only print errors
#   -h, --help
#
# Requires openssl (OpenSSL 1.1+/3.x or LibreSSL; TRINO_ODBC_OPENSSL selects the binary) and
# python3 (hashlib only; TRINO_ODBC_PYTHON selects the interpreter).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEMPLATE_DIR="$SCRIPT_DIR/tls"

OUT_DIR=""
FORCE=0
QUIET=0
TLS_USER="${TRINO_ODBC_TLS_USER:-alice}"
TLS_PASSWORD="${TRINO_ODBC_TLS_PASSWORD:-secret}"
KEYSTORE_PASSWORD="${TRINO_ODBC_KEYSTORE_PASSWORD:-changeit}"
CONTAINER_NAME="${TRINO_ODBC_TLS_CONTAINER:-trino-odbc-tls}"
EXTRA_SAN="${TRINO_ODBC_TLS_EXTRA_SAN:-}"
CERT_DAYS="${TRINO_ODBC_TLS_CERT_DAYS:-3650}"
MINIO_ENDPOINT="${TRINO_ODBC_MINIO_ENDPOINT:-http://trino-odbc-minio:9000}"
MINIO_ACCESS_KEY="${TRINO_ODBC_MINIO_ACCESS_KEY:-minioadmin}"
MINIO_SECRET_KEY="${TRINO_ODBC_MINIO_SECRET_KEY:-minioadmin}"
SPOOLING_BUCKET="${TRINO_ODBC_SPOOLING_BUCKET:-spooling}"
OPENSSL="${TRINO_ODBC_OPENSSL:-openssl}"
PYTHON="${TRINO_ODBC_PYTHON:-python3}"
PBKDF2_ITERATIONS=1000

usage() { sed -n '2,/^[^#]/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }
log() { [ "$QUIET" = 1 ] || printf 'gen_tls_assets: %s\n' "$*"; }
die() { printf 'gen_tls_assets: error: %s\n' "$*" >&2; exit 1; }

need_value() { [ $# -ge 2 ] || die "$1 needs a value"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --force) FORCE=1 ;;
    --user) need_value "$@"; shift; TLS_USER="$1" ;;
    --user=*) TLS_USER="${1#--user=}" ;;
    --password) need_value "$@"; shift; TLS_PASSWORD="$1" ;;
    --password=*) TLS_PASSWORD="${1#--password=}" ;;
    --keystore-password) need_value "$@"; shift; KEYSTORE_PASSWORD="$1" ;;
    --keystore-password=*) KEYSTORE_PASSWORD="${1#--keystore-password=}" ;;
    --container-name) need_value "$@"; shift; CONTAINER_NAME="$1" ;;
    --container-name=*) CONTAINER_NAME="${1#--container-name=}" ;;
    --san) need_value "$@"; shift; EXTRA_SAN="$1" ;;
    --san=*) EXTRA_SAN="${1#--san=}" ;;
    --days) need_value "$@"; shift; CERT_DAYS="$1" ;;
    --days=*) CERT_DAYS="${1#--days=}" ;;
    --minio-endpoint) need_value "$@"; shift; MINIO_ENDPOINT="$1" ;;
    --minio-endpoint=*) MINIO_ENDPOINT="${1#--minio-endpoint=}" ;;
    --minio-access-key) need_value "$@"; shift; MINIO_ACCESS_KEY="$1" ;;
    --minio-access-key=*) MINIO_ACCESS_KEY="${1#--minio-access-key=}" ;;
    --minio-secret-key) need_value "$@"; shift; MINIO_SECRET_KEY="$1" ;;
    --minio-secret-key=*) MINIO_SECRET_KEY="${1#--minio-secret-key=}" ;;
    --bucket) need_value "$@"; shift; SPOOLING_BUCKET="$1" ;;
    --bucket=*) SPOOLING_BUCKET="${1#--bucket=}" ;;
    -q|--quiet) QUIET=1 ;;
    -h|--help) usage; exit 0 ;;
    -*) die "unknown option '$1' (see --help)" ;;
    *) [ -z "$OUT_DIR" ] || die "unexpected argument '$1'"; OUT_DIR="$1" ;;
  esac
  shift
done
[ -n "$OUT_DIR" ] || OUT_DIR="${TRINO_ODBC_DOCKER_ASSETS:-$SCRIPT_DIR/generated}"

# ---- validation ----------------------------------------------------------------------------------
command -v "$OPENSSL" >/dev/null 2>&1 || die "'$OPENSSL' not found (install openssl or set TRINO_ODBC_OPENSSL)"
command -v "$PYTHON" >/dev/null 2>&1 || die "'$PYTHON' not found (install python3 or set TRINO_ODBC_PYTHON)"
[ -f "$TEMPLATE_DIR/config.properties" ] || die "template $TEMPLATE_DIR/config.properties missing"
[ -f "$TEMPLATE_DIR/spooling-manager.properties" ] || die "template $TEMPLATE_DIR/spooling-manager.properties missing"
[ -f "$TEMPLATE_DIR/password-authenticator.properties" ] || die "$TEMPLATE_DIR/password-authenticator.properties missing"
case "$TLS_USER" in
  ''|*:*|*' '*) die "user name must be non-empty and contain no ':' or spaces (password.db format)" ;;
esac
[ -n "$TLS_PASSWORD" ] || die "password must not be empty"
[ -n "$KEYSTORE_PASSWORD" ] || die "keystore password must not be empty"
case "$CERT_DAYS" in ''|*[!0-9]*) die "--days must be a positive integer" ;; esac
# Values land in Java .properties files and in ODBC connection strings; keep them plain.
for v in "$TLS_PASSWORD" "$KEYSTORE_PASSWORD" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" "$SPOOLING_BUCKET" "$MINIO_ENDPOINT"; do
  case "$v" in
    *'\'*|*$'\n'*|*'}'*) die "value '$v' must not contain backslashes, braces or newlines" ;;
  esac
done

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd -P)"

SAN="DNS:localhost,IP:127.0.0.1,IP:::1,DNS:${CONTAINER_NAME},DNS:host.docker.internal"
[ -z "$EXTRA_SAN" ] || SAN="$SAN,$EXTRA_SAN"

ASSET_FILES="cert.pem key.pem keystore.p12 password.db config.properties spooling-manager.properties password-authenticator.properties tls-assets.env"

# Every parameter that influences the generated files. Secrets are random per generation, so the
# fingerprint only tells whether a regeneration would produce a *different kind* of setup.
FINGERPRINT="$(printf '%s\n' "user=$TLS_USER" "password=$TLS_PASSWORD" "keystore=$KEYSTORE_PASSWORD" \
  "san=$SAN" "days=$CERT_DAYS" "minio=$MINIO_ENDPOINT" "access=$MINIO_ACCESS_KEY" "secret=$MINIO_SECRET_KEY" \
  "bucket=$SPOOLING_BUCKET" | "$OPENSSL" dgst -sha256 | awk '{print $NF}')"

all_present() {
  local f
  for f in $ASSET_FILES; do [ -s "$OUT_DIR/$f" ] || return 1; done
  return 0
}

# Prints the value of KEY from the env file (sourced in a subshell so quoting is honoured).
env_value() { ( set +u; . "$1" >/dev/null 2>&1; eval "printf '%s' \"\${$2:-}\"" ); }

if [ "$FORCE" = 0 ] && all_present; then
  if [ "$(env_value "$OUT_DIR/tls-assets.env" TRINO_ODBC_TLS_ASSETS_FINGERPRINT)" = "$FINGERPRINT" ]; then
    log "assets already present in $OUT_DIR (same parameters; --force regenerates)"
    exit 0
  fi
  log "assets in $OUT_DIR were generated with different parameters; regenerating"
elif [ "$FORCE" = 0 ]; then
  log "generating assets in $OUT_DIR"
else
  log "regenerating assets in $OUT_DIR (--force)"
fi

# ---- generation into a staging directory, moved into place at the end --------------------------
STAGE="$(mktemp -d "$OUT_DIR/.gen.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

# 1. Certificate + key. A config file instead of -addext: LibreSSL and old OpenSSL lack -addext.
#    CA:TRUE lets every TLS backend (OpenSSL, Schannel, Secure Transport) accept the self-signed
#    certificate as a trust anchor when it is handed over through TrustedCerts=.
cat > "$STAGE/openssl.cnf" <<EOF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = localhost
O = Trino ODBC tests
[ext]
subjectAltName = $SAN
basicConstraints = critical, CA:TRUE
keyUsage = critical, digitalSignature, keyEncipherment, keyCertSign
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
EOF
if ! "$OPENSSL" req -x509 -newkey rsa:2048 -sha256 -nodes -days "$CERT_DAYS" \
      -keyout "$STAGE/key.pem" -out "$STAGE/cert.pem" -config "$STAGE/openssl.cnf" >"$STAGE/openssl.log" 2>&1; then
  cat "$STAGE/openssl.log" >&2
  die "openssl req failed"
fi
chmod 600 "$STAGE/key.pem"

# 2. PKCS#12 keystore for Trino's http-server. Explicit AES/SHA-256 algorithms: LibreSSL and
#    OpenSSL 1.x default to RC2/3DES bags, which recent JDKs reject.
export TRINO_ODBC_GEN_P12_PASS="$KEYSTORE_PASSWORD"
if ! "$OPENSSL" pkcs12 -export -in "$STAGE/cert.pem" -inkey "$STAGE/key.pem" -out "$STAGE/keystore.p12" \
      -name trino -passout env:TRINO_ODBC_GEN_P12_PASS \
      -keypbe AES-256-CBC -certpbe AES-256-CBC -macalg sha256 >"$STAGE/openssl.log" 2>&1; then
  cat "$STAGE/openssl.log" >&2
  die "openssl pkcs12 -export failed"
fi
# Verify the keystore opens with the password we are going to put into config.properties.
"$OPENSSL" pkcs12 -in "$STAGE/keystore.p12" -passin env:TRINO_ODBC_GEN_P12_PASS -nokeys -noout >/dev/null 2>&1 \
  || die "generated keystore.p12 cannot be read back"
unset TRINO_ODBC_GEN_P12_PASS
# The keystore and the password file are read by the unprivileged "trino" user inside the
# container; on a native-Linux Docker host the bind mount keeps the host mode bits.
chmod 644 "$STAGE/keystore.p12"

# 3. Password file (PBKDF2 line, exactly what Trino's file authenticator parses).
export TRINO_ODBC_GEN_USER="$TLS_USER" TRINO_ODBC_GEN_PASSWORD="$TLS_PASSWORD" TRINO_ODBC_GEN_ITERATIONS="$PBKDF2_ITERATIONS"
"$PYTHON" - > "$STAGE/password.db" <<'EOF' || die "python3 could not write password.db"
import hashlib, os, sys
user = os.environ["TRINO_ODBC_GEN_USER"]
password = os.environ["TRINO_ODBC_GEN_PASSWORD"].encode("utf-8")
iterations = int(os.environ["TRINO_ODBC_GEN_ITERATIONS"])
salt = os.urandom(16)
digest = hashlib.pbkdf2_hmac("sha1", password, salt, iterations, 32)
sys.stdout.write("%s:%d:%s:%s\n" % (user, iterations, salt.hex(), digest.hex()))
EOF
unset TRINO_ODBC_GEN_USER TRINO_ODBC_GEN_PASSWORD TRINO_ODBC_GEN_ITERATIONS
grep -q "^$TLS_USER:$PBKDF2_ITERATIONS:[0-9a-f]\{32\}:[0-9a-f]\{64\}$" "$STAGE/password.db" || die "password.db has an unexpected format"
chmod 644 "$STAGE/password.db"

# 4. Secrets. The spooling key must be 256 bits, base64 encoded (Trino validates the decoded size).
SPOOLING_KEY="$("$OPENSSL" rand -base64 32 | tr -d '\n')"
# (openssl's base64 decoder needs the terminating newline, hence printf '%s\n'.)
decoded_len="$(printf '%s\n' "$SPOOLING_KEY" | "$OPENSSL" base64 -d | wc -c | tr -d ' ')"
[ "$decoded_len" = 32 ] || die "spooling secret does not decode to 32 bytes (got $decoded_len)"
INTERNAL_SECRET="$("$OPENSSL" rand -hex 32 | tr -d '\n')"
[ ${#INTERNAL_SECRET} -eq 64 ] || die "could not generate the internal shared secret"

# 5. Render the templates.
sed_escape() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }
render() {  # TEMPLATE OUTPUT
  sed -e "s|@KEYSTORE_PASSWORD@|$(sed_escape "$KEYSTORE_PASSWORD")|g" \
      -e "s|@INTERNAL_SHARED_SECRET@|$(sed_escape "$INTERNAL_SECRET")|g" \
      -e "s|@SPOOLING_SHARED_SECRET_KEY@|$(sed_escape "$SPOOLING_KEY")|g" \
      -e "s|@MINIO_ENDPOINT@|$(sed_escape "$MINIO_ENDPOINT")|g" \
      -e "s|@MINIO_ACCESS_KEY@|$(sed_escape "$MINIO_ACCESS_KEY")|g" \
      -e "s|@MINIO_SECRET_KEY@|$(sed_escape "$MINIO_SECRET_KEY")|g" \
      -e "s|@SPOOLING_BUCKET@|$(sed_escape "$SPOOLING_BUCKET")|g" \
      "$1" > "$2"
  if grep -q '@[A-Z_][A-Z_]*@' "$2"; then
    die "unreplaced placeholder in $(basename "$2"): $(grep -o '@[A-Z_][A-Z_]*@' "$2" | sort -u | tr '\n' ' ')"
  fi
  chmod 644 "$2"
}
render "$TEMPLATE_DIR/config.properties" "$STAGE/config.properties"
render "$TEMPLATE_DIR/spooling-manager.properties" "$STAGE/spooling-manager.properties"
cp "$TEMPLATE_DIR/password-authenticator.properties" "$STAGE/password-authenticator.properties"
chmod 644 "$STAGE/password-authenticator.properties"

# 6. Parameter record for run_trino.sh (single-quoted, so any character except ' round-trips).
shell_quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
{
  echo "# Generated by tests/docker/trino/gen_tls_assets.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ); do not edit."
  echo "TRINO_ODBC_TLS_USER=$(shell_quote "$TLS_USER")"
  echo "TRINO_ODBC_TLS_PASSWORD=$(shell_quote "$TLS_PASSWORD")"
  echo "TRINO_ODBC_KEYSTORE_PASSWORD=$(shell_quote "$KEYSTORE_PASSWORD")"
  echo "TRINO_ODBC_TLS_CERT=$(shell_quote "$OUT_DIR/cert.pem")"
  echo "TRINO_ODBC_TLS_KEYSTORE=$(shell_quote "$OUT_DIR/keystore.p12")"
  echo "TRINO_ODBC_TLS_SAN=$(shell_quote "$SAN")"
  echo "TRINO_ODBC_TLS_CONTAINER=$(shell_quote "$CONTAINER_NAME")"
  echo "TRINO_ODBC_MINIO_ENDPOINT=$(shell_quote "$MINIO_ENDPOINT")"
  echo "TRINO_ODBC_MINIO_ACCESS_KEY=$(shell_quote "$MINIO_ACCESS_KEY")"
  echo "TRINO_ODBC_MINIO_SECRET_KEY=$(shell_quote "$MINIO_SECRET_KEY")"
  echo "TRINO_ODBC_SPOOLING_BUCKET=$(shell_quote "$SPOOLING_BUCKET")"
  echo "TRINO_ODBC_TLS_ASSETS_FINGERPRINT=$(shell_quote "$FINGERPRINT")"
} > "$STAGE/tls-assets.env"
chmod 644 "$STAGE/tls-assets.env"

# 7. Move into place (per file; a failure between files is repaired by the next run because
#    all_present() then fails).
for f in $ASSET_FILES; do
  mv -f "$STAGE/$f" "$OUT_DIR/$f"
done

log "wrote $OUT_DIR/{$(printf '%s' "$ASSET_FILES" | tr ' ' ',')}"
log "user=$TLS_USER  cert SAN=$SAN"
log "TrustedCerts=$OUT_DIR/cert.pem"
