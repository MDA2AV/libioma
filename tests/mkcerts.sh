#!/bin/sh
# Self-signed certificates for the test fixture: `default` (CN=localhost) and one SNI host.
set -e
dir=${1:-tests/certs}
for host in default sni.test; do
    cn=localhost; [ "$host" = default ] || cn=$host
    mkdir -p "$dir/$host"
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj "/CN=$cn" -days 30 \
        -keyout "$dir/$host/key.pem" -out "$dir/$host/cert.pem" 2>/dev/null
done
echo "certificates in $dir"
