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

# Use a real runtime caller so the portal reads [Runtime] metadata through
# /proc, rather than the mock application identity used by test-portal.
if run_with_sandboxed_bus_for "runtime/org.test.Platform/${ARCH}/master" \
    "${test_builddir}/test-spawn" bash -c 'exit 0' > runtime-spawn.out 2>&1; then
    assert_not_reached "runtime spawn without --sandbox succeeded"
fi
assert_file_has_content runtime-spawn.out "Runtime callers must use a sandbox"
ok "runtime spawn requires sandbox"

touch "${test_tmpdir}/runtime-parent-only"
parent_network_namespace=$(readlink /proc/self/ns/net)
run_with_sandboxed_bus_for "runtime/org.test.Platform/${ARCH}/master" \
    "${test_builddir}/test-spawn" --sandbox bash -ec "
        case \"\$(cat /.flatpak-info)\" in
            *'sandbox=true'*) ;;
            *) exit 1 ;;
        esac
        test ! -e '${test_tmpdir}/runtime-parent-only'
        test ! -S '/run/user/$(id -u)/bus'
        test \"\$(readlink /proc/self/ns/net)\" != '${parent_network_namespace}'
    " > runtime-spawn.out
assert_file_has_content runtime-spawn.out "spawn-exited.*status=0$"
ok "runtime spawn preserves sandbox isolation"

done_testing
