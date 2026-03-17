#!/usr/bin/env bash
# gen-certs.sh - Generate a self-signed TLS cert for the Sentinel stream server.
# Run once from the repo root:  bash certs/gen-certs.sh
# Requires OpenSSL 1.1+ on PATH.
#
# Output:
#   certs/sentinel-server.key   - EC private key (prime256v1)
#   certs/sentinel-server.crt   - self-signed X.509 cert (1 year, SAN=localhost/127.0.0.1)
#
# Both files are in .gitignore - never commit them.

set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEY="$DIR/sentinel-server.key"
CRT="$DIR/sentinel-server.crt"
CNF="$DIR/_san.cnf"

cat > "$CNF" <<'EOF'
[req]
distinguished_name = req_distinguished_name
x509_extensions    = v3_req
prompt             = no

[req_distinguished_name]
CN = Sentinel Stream Server

[v3_req]
subjectAltName   = @alt_names
keyUsage         = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth

[alt_names]
DNS.1 = localhost
IP.1  = 127.0.0.1
EOF

echo "Generating EC private key..."
openssl ecparam -name prime256v1 -genkey -noout -out "$KEY"

echo "Generating self-signed certificate (365 days)..."
openssl req -new -x509 -key "$KEY" -out "$CRT" -days 365 -config "$CNF"

rm -f "$CNF"

echo ""
echo "Done. Files written to:"
echo "  $KEY"
echo "  $CRT"
echo ""
echo "Set in server_config.yaml:"
echo "  tls:"
echo "    cert_file: certs/sentinel-server.crt"
echo "    key_file:  certs/sentinel-server.key"
echo ""
echo "Set in client_config.yaml:"
echo "  server:"
echo "    ca_file: certs/sentinel-server.crt"
