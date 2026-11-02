#!/usr/bin/env python3
"""Creates a self-signed TLS certificate/key pair for the mock Trino server.

Uses the ``openssl`` command-line tool (no third-party Python modules). The certificate carries
Subject Alternative Names for ``localhost`` and ``127.0.0.1`` so the driver's ``SSLVerification=Full``
mode (hostname check) passes when the mock is addressed either way.

Usage::

    python3 gen_cert.py <directory> [--force]

Writes ``<directory>/cert.pem`` and ``<directory>/key.pem``; existing files are kept unless
``--force`` is given. Prints the certificate path on success, exits non-zero when openssl is
missing or fails.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def generate(directory: str, force: bool = False, days: int = 3650) -> str:
    os.makedirs(directory, exist_ok=True)
    cert = os.path.join(directory, "cert.pem")
    key = os.path.join(directory, "key.pem")
    if not force and os.path.isfile(cert) and os.path.isfile(key):
        return cert
    openssl = shutil.which("openssl")
    if not openssl:
        raise RuntimeError("openssl executable not found in PATH")

    # A config file instead of -addext: LibreSSL (macOS) and old OpenSSL builds lack -addext.
    config = (
        "[req]\n"
        "distinguished_name = dn\n"
        "x509_extensions = ext\n"
        "prompt = no\n"
        "[dn]\n"
        "CN = localhost\n"
        "O = Trino ODBC mock\n"
        "[ext]\n"
        "subjectAltName = DNS:localhost,IP:127.0.0.1,IP:::1\n"
        "basicConstraints = CA:TRUE\n"
        "keyUsage = digitalSignature, keyEncipherment, keyCertSign\n"
        "extendedKeyUsage = serverAuth\n"
    )
    fd, config_path = tempfile.mkstemp(prefix="mock_trino_cert_", suffix=".cnf")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(config)
        cmd = [openssl, "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes", "-days", str(days),
               "-keyout", key, "-out", cert, "-config", config_path]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise RuntimeError("openssl failed (%d): %s" % (proc.returncode, proc.stderr.decode("utf-8", "replace").strip()))
    finally:
        try:
            os.unlink(config_path)
        except OSError:
            pass
    try:
        os.chmod(key, 0o600)
    except OSError:
        pass
    return cert


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Generate a self-signed localhost certificate")
    parser.add_argument("directory")
    parser.add_argument("--force", action="store_true", help="overwrite existing files")
    parser.add_argument("--days", type=int, default=3650)
    args = parser.parse_args(argv)
    try:
        cert = generate(args.directory, args.force, args.days)
    except RuntimeError as e:
        sys.stderr.write("gen_cert: %s\n" % e)
        return 1
    print(cert)
    return 0


if __name__ == "__main__":
    sys.exit(main())
