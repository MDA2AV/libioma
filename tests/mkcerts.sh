#!/bin/sh
# Self-signed certificates for the test fixture: `default` (CN=localhost) and one SNI host.
set -e
dir=${1:-tests/certs}
# default: RSA (what most clients and test suites assume); sni.test: ECDSA, so both key types serve
mkdir -p "$dir/default" "$dir/sni.test"
openssl req -x509 -newkey rsa:2048 -nodes -subj "/CN=localhost" -days 30 \
    -keyout "$dir/default/key.pem" -out "$dir/default/cert.pem" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj "/CN=sni.test" -days 30 \
    -keyout "$dir/sni.test/key.pem" -out "$dir/sni.test/cert.pem" 2>/dev/null
echo "certificates in $dir"
