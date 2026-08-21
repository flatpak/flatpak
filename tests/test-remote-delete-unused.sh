#!/bin/bash

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_revokefs_without_fuse

echo "1..6"

setup_repo
install_repo
port=$(cat httpd-port)
SELECTOR=${U:---system}

add_remote () {
    local installation=$1
    shift
    ${FLATPAK} ${installation} remote-add --no-gpg-verify "$1" "http://127.0.0.1:${port}/test" >&2
}

run_interactive () {
    local answer=$1
    local installation=$2
    shift
    shift
    printf '%s\n' "${answer}" | python3 -c \
        'import os, pty, sys; raise SystemExit(os.waitstatus_to_exitcode(pty.spawn(sys.argv[1:])))' \
        ${FLATPAK} ${installation} "$@"
}

add_remote "${SELECTOR}" unused-one
add_remote "${SELECTOR}" unused-two
run_interactive y "${SELECTOR}" remote-delete --unused > delete-log
${FLATPAK} ${SELECTOR} remotes > remotes
assert_file_has_content remotes "^test-repo"
assert_not_file_has_content remotes "unused-one"
assert_not_file_has_content remotes "unused-two"
test "$(grep -c "Remove all unused remotes?" delete-log)" = 1
assert_file_has_content delete-log "Remove all unused remotes[?] \[Y/n\]"
assert_file_has_content delete-log "Removed 2 unused remotes\."
ok "delete multiple unused remotes with one confirmation"

add_remote "${SELECTOR}" unused-declined
run_interactive n "${SELECTOR}" remote-delete --unused > delete-log
${FLATPAK} ${SELECTOR} remotes > remotes
assert_file_has_content remotes "^unused-declined"
ok "decline without deleting remotes"

${FLATPAK} ${SELECTOR} remote-delete --force unused-declined
${FLATPAK} ${SELECTOR} remote-delete --unused > delete-log
assert_file_empty delete-log
ok "no unused remotes exits without prompting"

if ${FLATPAK} ${SELECTOR} remote-delete --unused test-repo > delete-log 2>&1; then
    assert_not_reached "--unused with NAME should fail"
fi
assert_file_has_content delete-log "NAME and --unused are mutually exclusive"
ok "reject --unused with NAME"

add_remote "${SELECTOR}" unused-selected
add_remote "${INVERT_U}" unused-other
run_interactive y "${SELECTOR}" remote-delete --unused > delete-log
${FLATPAK} ${SELECTOR} remotes > selected-remotes
${FLATPAK} ${INVERT_U} remotes > other-remotes
assert_not_file_has_content selected-remotes "unused-selected"
assert_file_has_content other-remotes "^unused-other"
ok "respect explicit installation selector"

run_interactive y remote-delete --unused > delete-log
${FLATPAK} ${INVERT_U} remotes > other-remotes
assert_not_file_has_content other-remotes "unused-other"
assert_file_has_content delete-log "unused-other"
ok "search standard installations by default"
