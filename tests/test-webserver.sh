#!/bin/bash

set -euo pipefail

dir=$1
cmd=${2:-python3 -m http.server 0}
test_tmpdir=$(pwd)

[ "$dir" != "" ] && cd ${dir}
echo "Running web server: PYTHONUNBUFFERED=1 setsid $cmd" >&2
touch ${test_tmpdir}/httpd-output
env PYTHONUNBUFFERED=1 setsid $cmd >${test_tmpdir}/httpd-output &
child_pid=$!
echo "Web server pid: $child_pid" >&2

for x in $(seq 300); do
    echo "Waiting for web server ($x/300)..." >&2
    # Snapshot the output
    cp ${test_tmpdir}/httpd-output{,.tmp}
    sed -ne 's/^/# httpd-output.tmp: /' < ${test_tmpdir}/httpd-output.tmp >&2
    echo >&2
    # If it's non-empty, see whether it matches our regexp
    if test -s ${test_tmpdir}/httpd-output.tmp; then
        sed -e 's,Serving HTTP on 0.0.0.0 port \([0-9]*\) (http://0.0.0.0:[0-9]*/) \.\.\.,\1,' < ${test_tmpdir}/httpd-output.tmp > ${test_tmpdir}/httpd-port
        if ! cmp ${test_tmpdir}/httpd-output.tmp ${test_tmpdir}/httpd-port 1>/dev/null; then
            # If so, we've successfully extracted the port
            break
        fi
    fi
    sleep 0.1
done
port=$(cat ${test_tmpdir}/httpd-port)
echo "http://127.0.0.1:${port}" > ${test_tmpdir}/httpd-address
echo "$child_pid" > ${test_tmpdir}/httpd-pid
echo "Started web server '$cmd': process $child_pid on port $port" >&2
