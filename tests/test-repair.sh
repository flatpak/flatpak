#!/bin/bash
#
# Copyright (C) 2021 Matthew Leeds <mwleeds@protonmail.com>
#
# SPDX-License-Identifier: LGPL-2.0-or-later

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..2"

setup_repo
${FLATPAK} ${U} install -y test-repo org.test.Hello >&2

# delete the object for files/bin/hello.sh
rm ${FL_DIR}/repo/objects/0d/30582c0ac8a2f89f23c0f62e548ba7853f5285d21848dd503460a567b5d253.file

# dry run repair shouldn't replace the missing file
${FLATPAK} ${U} repair --dry-run >&2
assert_not_has_file ${FL_DIR}/repo/objects/0d/30582c0ac8a2f89f23c0f62e548ba7853f5285d21848dd503460a567b5d253.file

# normal repair should replace the missing file
${FLATPAK} ${U} repair >&2
assert_has_file ${FL_DIR}/repo/objects/0d/30582c0ac8a2f89f23c0f62e548ba7853f5285d21848dd503460a567b5d253.file

# app should've been reinstalled
${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "org\.test\.Hello/"

ok "repair command handles missing files"

# Test that flatpak repair --reinstall-all does not change pin state
# https://github.com/flatpak/flatpak/issues/6565
# Reuse the repo and installation from the previous test.

# Clear any pins to ensure a known-empty state before repair
${FLATPAK} ${U} pin --remove "runtime/org.test.Platform/$ARCH/master" &>/dev/null || true

${FLATPAK} ${U} pin > pins-before
${FLATPAK} ${U} repair --reinstall-all >&2
${FLATPAK} ${U} pin > pins-after

# Pin state must be identical after repair
diff -q pins-before pins-after >/dev/null || \
    assert_not_reached "repair --reinstall-all changed pin state"
rm pins-before pins-after

# Clean up
${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello >&2
${FLATPAK} ${U} remote-delete test-repo >&2

ok "repair --reinstall-all preserves pin state"
