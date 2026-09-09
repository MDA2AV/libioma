#!/bin/sh
# Self-signed certificates for the test fixture: `default` (CN=localhost), one SNI host, and a
# wildcard host, so the store's exact match, its fallback and its `_.example.com` -> *.example.com
# rule are all exercised.
#
#   sh tests/mkcerts.sh [dir]              make the three, each only when missing or about to expire
#   sh tests/mkcerts.sh [dir] host ...     remake exactly those, whatever their state
#
# They are short-lived on purpose, so a stale checkout is not served an expired certificate: one
# within three days of its end date is made again.
set -e
dir=${1:-tests/certs}
[ $# -gt 0 ] && shift || true

# One host: `default` is RSA (what most clients and test suites assume), the rest ECDSA, so both
# key types serve. $force remakes it even when what is there is still good.
make_host() {
    host=$1
    force=${2:-no}
    cert="$dir/$host/cert.pem"
    if [ "$force" = no ] && [ -f "$cert" ] && openssl x509 -in "$cert" -noout -checkend 259200 >/dev/null 2>&1; then
        return 0                                  # 3 days: still good for a while
    fi
    mkdir -p "$dir/$host"
    case $host in
    default) key="-newkey rsa:2048" ; cn=localhost ;;
    _.*)     key="-newkey ec -pkeyopt ec_paramgen_curve:prime256v1" ; cn="*.${host#_.}" ;;
    *)       key="-newkey ec -pkeyopt ec_paramgen_curve:prime256v1" ; cn=$host ;;
    esac
    # shellcheck disable=SC2086
    openssl req -x509 $key -nodes -subj "/CN=$cn" -days 30 \
        -keyout "$dir/$host/key.pem" -out "$dir/$host/cert.pem" 2>/dev/null
}

if [ $# -gt 0 ]; then
    for host in "$@"; do
        make_host "$host" force
    done
    echo "certificates for $* in $dir"
else
    make_host default
    make_host sni.test
    make_host _.example.com
    echo "certificates in $dir"
fi
