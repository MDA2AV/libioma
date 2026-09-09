#!/bin/sh
# run-suites.sh - the check sequence, in one place: the unit test, then the HTTP fixture with the
# smoke, stress and early-TLS suites against it, then the pipe fixture with the pipe suite. Both
# `make check` and CMake's `check` target run this, so the sequence lives here and nowhere else.
#
#   sh tests/run-suites.sh --unit tests/ioxd-unit --server tests/ioxd-test-server \
#                          --pipe-server tests/ioxd-pipe-server [--port 8099] [--pipe-port 8102]
#
#   --port N          the fixture's first port: N and N+1 plain, N+2 TLS          [8099]
#   --pipe-port N     the pipe fixture's port                                     [8102]
#   --unit PATH       the unit test binary
#   --server PATH     the HTTP fixture (tests/server.c)
#   --pipe-server PATH the pipe fixture (tests/pipe-server.c)
#   --python PY       the python the suites run under                             [python3]
#   --tls-python PY   the python for tls_early.py (needs tlslite-ng)              [--python]
#   --work DIR        scratch: the fixture's log and the certificates it serves   [obj/check]
#   --suite NAME      run only this one (repeatable): unit smoke stress tls pipes
#
# The suites named in one run share the fixture they talk to, and are meant to: a fixture bound to
# a port the suite before it left full of TIME_WAIT connections has some of its new connections
# reset, so restarting one per suite on the same port is not the same thing at all.
#
# A fixture's output goes to a log in the work directory instead of /dev/null, and the log is
# printed - and the run fails - when the fixture exits non-zero, when a worker reported an error,
# or when a worker stopped with connections still open. The certificates the fixture serves are a
# copy of tests/certs, so a suite may rewrite them (smoke.py's TLS reload does).
set -u

tests=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$tests")

port=8099
pipe_port=8102
unit=
server=
pipe_server=
python=python3
tls_python=
work=
suites=

while [ $# -gt 0 ]; do
    case $1 in
    --port)        port=$2;        shift 2 ;;
    --pipe-port)   pipe_port=$2;   shift 2 ;;
    --unit)        unit=$2;        shift 2 ;;
    --server)      server=$2;      shift 2 ;;
    --pipe-server) pipe_server=$2; shift 2 ;;
    --python)      python=$2;      shift 2 ;;
    --tls-python)  tls_python=$2;  shift 2 ;;
    --work)        work=$2;        shift 2 ;;
    --suite)       suites="$suites $2"; shift 2 ;;
    -h|--help)     sed -n '2,26p' "$0"; exit 0 ;;
    *)             echo "run-suites: unknown option $1" >&2; exit 2 ;;
    esac
done
[ -n "$tls_python" ] || tls_python=$python
[ -n "$work" ] || work=$root/obj/check
[ -n "$suites" ] || suites="unit smoke stress tls pipes"
tls_port=$((port + 2))

wanted() {
    for s in $suites; do
        [ "$s" = "$1" ] && return 0
    done
    return 1
}

# A socket probe: the only honest answer to "is it listening".
port_open() {
    "$python" -c "import socket,sys
s = socket.socket()
s.settimeout(0.5)
sys.exit(0 if s.connect_ex(('127.0.0.1', $1)) == 0 else 1)" 2>/dev/null
}

# Up when the port answers AND every worker has said it is listening: each worker opens its own
# SO_REUSEPORT socket, and a reuseport group that grows while connections are arriving resets some
# of them, so waiting for the first worker alone would hand a suite a fixture that is still
# assembling itself.
workers=2

wait_for_fixture() {
    i=0
    while [ $i -lt 100 ]; do
        if port_open "$1" && [ "$(grep -c 'listening on' "$2")" -ge $workers ]; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

# What a worker prints when something went wrong, and the shutdown line with connections left on
# it. A dropped TLS connection is not one of these: tls_early.py corrupts a record on purpose.
log_faults() {
    grep -nE 'io_uring_(setup|enter):|SQ still full|register pbuf ring:|accept :[0-9]+:' "$1"
    grep -nE 'stopping: .* still open' "$1" | grep -v ', 0 still open'
}

rc=0
mkdir -p "$work"

# --- the unit test: no fixture needed ---
if wanted unit && [ -n "$unit" ]; then
    "$unit" || rc=1
fi

# --- the HTTP fixture, with every suite that talks to it ---
if wanted smoke || wanted stress || wanted tls; then
    [ -n "$server" ] || { echo "run-suites: --server is required for the http suites" >&2; exit 2; }
    certs=
    if wanted smoke || wanted tls; then
        sh "$tests/mkcerts.sh" "$tests/certs" >/dev/null || rc=1
        rm -rf "$work/certs"
        cp -r "$tests/certs" "$work/certs"         # the fixture serves a copy: a suite may rewrite it
        certs=$work/certs
    fi
    log=$work/fixture.log
    if [ -n "$certs" ]; then                       # unset means no TLS listener, not an empty path
        IOXD_WORKERS=$workers IOXD_PORT=$port IOXD_CERTS="$certs" "$server" >"$log" 2>&1 &
    else
        IOXD_WORKERS=$workers IOXD_PORT=$port "$server" >"$log" 2>&1 &
    fi
    pid=$!
    if wait_for_fixture "$port" "$log"; then
        wanted smoke  && { IOXD_CERTS="$certs" "$python" "$tests/smoke.py" "$port" || rc=1; }
        wanted stress && { "$python" "$tests/stress.py" "$port" || rc=1; }
        if wanted tls; then
            if port_open "$tls_port"; then
                "$tls_python" "$tests/tls_early.py" "$tls_port" || rc=1
            else
                echo "skip tls_early: nothing listening on $tls_port (a TLS=0 build?)"
            fi
        fi
    else
        echo "FAIL the fixture never listened on $port"
        rc=1
    fi
    kill -INT "$pid" 2>/dev/null
    wait "$pid"; status=$?
    [ $status -eq 0 ] || { echo "FAIL the fixture exited $status"; rc=1; }
    grep -q 'stopping:' "$log" || { echo "FAIL no worker reported stopping"; rc=1; }
    faults=$(log_faults "$log")
    [ -z "$faults" ] || { echo "FAIL the fixture's log: $faults"; rc=1; }
    [ $rc -eq 0 ] || { echo "--- $log ---"; cat "$log"; echo "--- end of $log ---"; }
fi

# --- the pipe fixture ---
if wanted pipes; then
    [ -n "$pipe_server" ] || { echo "run-suites: --pipe-server is required for the pipe suite" >&2; exit 2; }
    log=$work/pipe.log
    inner=0
    "$pipe_server" "$pipe_port" >"$log" 2>&1 &
    pid=$!
    if wait_for_fixture "$pipe_port" "$log"; then
        "$python" "$tests/pipes.py" "$pipe_port" || inner=1
    else
        echo "FAIL the pipe fixture never listened on $pipe_port"
        inner=1
    fi
    kill -INT "$pid" 2>/dev/null
    wait "$pid"; status=$?
    [ $status -eq 0 ] || { echo "FAIL the pipe fixture exited $status"; inner=1; }
    grep -q 'stopping:' "$log" || { echo "FAIL no pipe worker reported stopping"; inner=1; }
    faults=$(log_faults "$log")
    [ -z "$faults" ] || { echo "FAIL the pipe fixture's log: $faults"; inner=1; }
    [ $inner -eq 0 ] || { echo "--- $log ---"; cat "$log"; echo "--- end of $log ---"; rc=1; }
fi

exit $rc
