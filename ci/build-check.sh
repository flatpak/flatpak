#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/build.sh
make check

if test -x /usr/bin/clang; then
    git clean -dfx && git submodule foreach git clean -dfx
    # And now a clang build to find unused variables; perhaps
    # in the future these could parallelize
    export CC=clang
    export CFLAGS='-Werror=unused-variable'
    # We disable introspection because it fails with clang: https://bugzilla.redhat.com/show_bug.cgi?id=1543295
    build --disable-introspection
fi
