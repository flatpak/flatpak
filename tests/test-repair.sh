#!/bin/bash
#
# Copyright (C) 2021 Matthew Leeds <mwleeds@protonmail.com>
#
# SPDX-License-Identifier: LGPL-2.0-or-later

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..1"

setup_repo
${FLATPAK} ${U} install -y test-repo org.test.Hello

# delete the object for files/bin/hello.sh
rm ${FL_DIR}/repo/objects/0d/30582c0ac8a2f89f23c0f62e548ba7853f5285d21848dd503460a567b5d253.file

# dry run repair shouldn't replace the missing file
${FLATPAK} ${U} repair --dry-run
assert_not_has_file ${FL_DIR}/repo/objects/0d/30582c0ac8a2f89f23c0f62e548ba7853f5285d21848dd503460a567b5d253.file

# normal repair should replace the missing file
${FLATPAK} ${U} repair
assert_has_file ${FL_DIR}/repo/objects/0d/30582c0ac8a2f89f23c0f62e548ba7853f5285d21848dd503460a567b5d253.file

# app should've been reinstalled
${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "org\.test\.Hello/"

# clean up
${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello
${FLATPAK} ${U} remote-delete test-repo

ok "repair command handles missing files"
