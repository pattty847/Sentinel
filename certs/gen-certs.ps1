# gen-certs.ps1 — Generate a self-signed TLS cert for the Sentinel stream server.
# Run once from the repo root:  .\certs\gen-certs.ps1
# Requires OpenSSL on PATH (ships with Git for Windows).
#
# Output:
#   certs/sentinel-server.key   — EC private key (prime256v1)
#   certs/sentinel-server.crt   — self-signed X.509 cert (1 year, SAN=localhost/127.0.0.1)
#
# Both files are in .gitignore — never commit them.

$ErrorActionPreference = "Stop"
$outDir = Join-Path $PSScriptRoot ""
$keyFile = Join-Path $outDir "sentinel-server.key"
$crtFile = Join-Path $outDir "sentinel-server.crt"
$cnfFile = Join-Path $outDir "_san.cnf"

# Minimal OpenSSL config with Subject Alternative Names
@"
[req]
distinguished_name = req_distinguished_name
x509_extensions    = v3_req
prompt             = no

[req_distinguished_name]
CN = Sentinel Stream Server

[v3_req]
subjectAltName = @alt_names
keyUsage       = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth

[alt_names]
DNS.1 = localhost
IP.1  = 127.0.0.1
"@ | Set-Content $cnfFile

Write-Host "Generating EC private key..."
& openssl ecparam -name prime256v1 -genkey -noout -out $keyFile

Write-Host "Generating self-signed certificate (365 days)..."
& openssl req -new -x509 -key $keyFile -out $crtFile -days 365 -config $cnfFile

Remove-Item $cnfFile -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done. Files written to:"
Write-Host "  $keyFile"
Write-Host "  $crtFile"
Write-Host ""
Write-Host "Set in server_config.yaml:"
Write-Host "  tls:"
Write-Host "    cert_file: certs/sentinel-server.crt"
Write-Host "    key_file:  certs/sentinel-server.key"
Write-Host ""
Write-Host "Set in client_config.yaml:"
Write-Host "  server:"
Write-Host "    ca_file: certs/sentinel-server.crt"
