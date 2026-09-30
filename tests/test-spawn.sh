#!/bin/bash

set -euo pipefail

# shellcheck source=libtest.sh
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

# It's normal for SpawnStarted to *not* get emitted if the command exits too
# quickly, which means we wouldn't be able to check if we actually received it
# in this test. Thus, we have the test binary write to a FIFO upon receiving
# SpawnStarted, and the inner command will block on that FIFO before exiting.
mkfifo "${test_tmpdir}/spawn-started"
run_with_sandboxed_bus "${test_builddir}/test-spawn" \
    --notify-start \
    --write-after-start="${test_tmpdir}/spawn-started" \
    bash -c "echo ok > '${test_tmpdir}/spawn-output-notify'; head -1 ${test_tmpdir}/spawn-started" > spawn.out

assert_file_has_content spawn.out "spawn-started"
assert_file_has_content spawn.out "spawn-exited.*status=0$"
assert_file_has_content "${test_tmpdir}/spawn-output-notify" "ok"

ok "spawn with notify-start"

done_testing
