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

# Sandbox flags 24 are FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_DBUS (8) and
# FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_A11Y (16). There is no accessibility bus
# here, so check whether flatpak-info says the instance was started without one.
BUS="/run/user/$(id -u)/bus"
NO_A11Y="[[ \$(< /.flatpak-info) == *no-a11y-bus=true* ]]"

run_with_sandboxed_bus "${test_builddir}/test-spawn" --sandbox-flags=24 \
    bash -c "test -S ${BUS} && ! ${NO_A11Y}" > spawn.out

assert_file_has_content spawn.out "spawn-exited.*status=0$"

ok "spawn sandbox with buses"

# Without the session bus the caller has no /run/user/$UID/bus, so its socat
# connects to the test bus directly
BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS%%,*}
case "${BUS_ADDRESS}" in
    (unix:path=*)
        CALLER_ARGS="--filesystem=${BUS_ADDRESS#unix:path=}"
        CALLER_CONNECT="unix-connect:${BUS_ADDRESS#unix:path=}"
        ;;
    (unix:abstract=*)
        CALLER_ARGS="--share=network"
        CALLER_CONNECT="abstract-connect:${BUS_ADDRESS#unix:abstract=}"
        ;;
    (*)
        assert_not_reached "Unexpected bus address ${DBUS_SESSION_BUS_ADDRESS}"
        ;;
esac

run_without_buses () {
    BUSSOCK=$(mktemp "${test_tmpdir}/bus.XXXXXX")
    rm -f "${BUSSOCK}"
    run --command=socat --filesystem="${test_tmpdir}" --no-session-bus --no-a11y-bus \
        "${CALLER_ARGS}" org.test.Hello "unix-listen:${BUSSOCK}" "${CALLER_CONNECT}" &
    while [ ! -e "${BUSSOCK}" ]; do sleep 1; done
    DBUS_SESSION_BUS_ADDRESS="unix:path=${BUSSOCK}" "$@"
}

run_without_buses "${test_builddir}/test-spawn" --sandbox-flags=24 \
    bash -c "test ! -e ${BUS} && ${NO_A11Y}" > spawn.out

assert_file_has_content spawn.out "spawn-exited.*status=0$"

ok "spawn sandbox without buses"

run_without_buses "${test_builddir}/test-spawn" \
    bash -c "test ! -e ${BUS} && ${NO_A11Y}" > spawn.out

assert_file_has_content spawn.out "spawn-exited.*status=0$"

ok "spawn without buses"

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
    "${test_builddir}/test-spawn" --sandbox-flags=0 bash -ec "
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
