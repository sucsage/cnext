#!/bin/sh
# =====================================================================
# gen-dev-cert.sh — produce a self-signed cert/key pair for local dev.
#
# Writes into  certs/cert.pem  +  certs/key.pem  by default (override
# with CERT_DIR=path). Both files are gitignored — never committed.
#
# Consumers replace the dev cert with a real one by dropping their own
# cert.pem / key.pem into the same directory. No rebuild required.
# =====================================================================
set -eu

CERT_DIR=${CERT_DIR:-certs}
CERT=$CERT_DIR/cert.pem
KEY=$CERT_DIR/key.pem

mkdir -p "$CERT_DIR"

if [ -f "$CERT" ] && [ -f "$KEY" ]; then
    echo "[gen-dev-cert] $CERT + $KEY already exist — skipping"
    exit 0
fi

command -v openssl >/dev/null 2>&1 || {
    echo "[gen-dev-cert] openssl not found — install openssl first" >&2
    exit 1
}

TMP_CFG=$(mktemp)
trap 'rm -f "$TMP_CFG"' EXIT

cat >"$TMP_CFG" <<'EOF'
[req]
distinguished_name = dn
req_extensions     = v3_req
prompt             = no

[dn]
CN = localhost

[v3_req]
subjectAltName   = @alt_names
extendedKeyUsage = serverAuth

[alt_names]
DNS.1 = localhost
DNS.2 = cnext.local
IP.1  = 127.0.0.1
IP.2  = ::1
EOF

openssl req -x509 -nodes -days 3650 \
    -newkey ed25519 \
    -keyout "$KEY" -out "$CERT" \
    -config "$TMP_CFG" -extensions v3_req 2>/dev/null

echo "[gen-dev-cert] wrote $CERT + $KEY (ed25519, 10y, SAN=localhost,127.0.0.1)"
