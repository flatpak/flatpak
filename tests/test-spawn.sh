#!/bin/bash

set -euo pipefail

# shellcheck source=tests/libtest.sh
. "$(dirname "$0")/libtest.sh"

skip_without_bwrap
skip_revokefs_without_fuse

setup_repo test
install_repo

# We can't pass fds via D-Bus over the socat intermediary, so instead use the
# presence of an output file as a marker of success.
run_with_sandboxed_bus "${test_builddir}/test-spawn" \
    bash -c "echo ok > '${test_tmpdir}/spawn-output'" > spawn.out

assert_file_has_content spawn.out "spawn-exited.*status=0$"
assert_file_has_content "${test_tmpdir}/spawn-output" "ok"

ok "spawn"

done_testing
