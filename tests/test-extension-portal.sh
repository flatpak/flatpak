#!/bin/bash
# shellcheck disable=SC2154
#
# Copyright (C) 2026 Flatpak contributors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

# shellcheck source=libtest.sh
. "$(dirname "$0")"/libtest.sh

skip_without_bwrap
skip_revokefs_without_fuse

echo "1..8"

setup_repo
install_repo

# Create an extension ref that belongs to the test app (org.test.Hello)
# This mimics what a plugin would look like: org.test.Hello.Plugin
make_test_extension () {
    local ID=$1
    local BRANCH=${2:-master}

    local DIR
    DIR=$(mktemp -d)

    cat > ${DIR}/metadata <<EOF
[Runtime]
name=${ID}
EOF
    mkdir -p ${DIR}/usr
    mkdir -p ${DIR}/files
    touch ${DIR}/usr/exists
    touch ${DIR}/usr/extension-${ID}

    ${FLATPAK} build-export --no-update-summary --runtime ${GPGARGS:-${FL_GPGARGS}} repos/test ${DIR} ${BRANCH} >&2
    update_repo
    rm -rf ${DIR}
}

# Create extension refs in the test repo
make_test_extension org.test.Hello.Plugin master

# Export an additional extension for later
make_test_extension org.test.Hello.Plugin2 master

TEST_APP_ID=org.test.Hello
TEST_EXTENSION_REF="runtime/org.test.Hello.Plugin/${ARCH}/master"

# Test 1: List extensions before installing any - verify returned fields
run_with_sandboxed_bus \
    env TEST_APP_ID=${TEST_APP_ID} TEST_EXTENSION_REF=${TEST_EXTENSION_REF} \
    ${test_builddir}/test-extension-portal list ext.pid > list-before-install.out

# Should contain our extensions from the remote (not installed)
assert_file_has_content list-before-install.out "name=org.test.Hello.Plugin "
assert_file_has_content list-before-install.out "name=org.test.Hello.Plugin2 "
# Neither plugin should be installed yet
assert_file_has_content list-before-install.out "name=org.test.Hello.Plugin arch=${ARCH} branch=master installed=0"
assert_file_has_content list-before-install.out "name=org.test.Hello.Plugin2 .* installed=0"
# Each extension should have origin set
assert_file_has_content list-before-install.out "name=org.test.Hello.Plugin .* origin=test-repo"
assert_file_has_content list-before-install.out "name=org.test.Hello.Plugin2 .* origin=test-repo"
# Locale is pre-installed with the app
assert_file_has_content list-before-install.out "name=org.test.Hello.Locale .* installed=1"
# Should not list extensions that don't belong to the app
assert_not_file_has_content list-before-install.out "name=org.test.Platform "

ok "list extensions returns correct fields"

# Test 2: Install an extension and verify it is actually installed
run_with_sandboxed_bus \
    env TEST_APP_ID=${TEST_APP_ID} TEST_EXTENSION_REF=${TEST_EXTENSION_REF} \
    ${test_builddir}/test-extension-portal install ext.pid > install.out

# Verify the progress signal reported successful completion (status=2 is DONE)
assert_file_has_content install.out "extension_progress status=2"
assert_not_file_has_content install.out "extension_progress status=3"

# Verify the extension is actually installed via flatpak list
${FLATPAK} ${U} list -a --columns=ref > list-after-portal-install.out
assert_file_has_content list-after-portal-install.out "org\.test\.Hello\.Plugin/"

ok "install extension"

# Test 3: List extensions after install - verify installed flag and commit
run_with_sandboxed_bus \
    env TEST_APP_ID=${TEST_APP_ID} TEST_EXTENSION_REF=${TEST_EXTENSION_REF} \
    ${test_builddir}/test-extension-portal list ext.pid > list-after-install.out

# Plugin should now be installed with a non-empty commit
assert_file_has_content list-after-install.out "name=org.test.Hello.Plugin arch=${ARCH} branch=master installed=1 origin=test-repo commit=.\+"
# Plugin2 should still be not installed
assert_file_has_content list-after-install.out "name=org.test.Hello.Plugin2 .* installed=0"

# Record the installed commit for the update test
OLD_PLUGIN_COMMIT=$(${FLATPAK} ${U} info --show-commit runtime/org.test.Hello.Plugin/${ARCH}/master)

ok "list shows installed extension with commit"

# Test 4: Update the extension (creates an update by re-exporting)
make_test_extension org.test.Hello.Plugin master

run_with_sandboxed_bus \
    env TEST_APP_ID=${TEST_APP_ID} TEST_EXTENSION_REF=${TEST_EXTENSION_REF} \
    ${test_builddir}/test-extension-portal update ext.pid > update.out

# Verify the progress signal reported successful completion
assert_file_has_content update.out "extension_progress status=2"
assert_not_file_has_content update.out "extension_progress status=3"

# Verify the commit actually changed
NEW_PLUGIN_COMMIT=$(${FLATPAK} ${U} info --show-commit runtime/org.test.Hello.Plugin/${ARCH}/master)
assert_not_streq "$OLD_PLUGIN_COMMIT" "$NEW_PLUGIN_COMMIT"

ok "update extension"

# Test 5: Uninstall the extension and verify it is actually removed
run_with_sandboxed_bus \
    env TEST_APP_ID=${TEST_APP_ID} TEST_EXTENSION_REF=${TEST_EXTENSION_REF} \
    ${test_builddir}/test-extension-portal uninstall ext.pid > uninstall.out

# Verify the progress signal reported successful completion
assert_file_has_content uninstall.out "extension_progress status=2"
assert_not_file_has_content uninstall.out "extension_progress status=3"

# Verify the extension is actually uninstalled via flatpak list
${FLATPAK} ${U} list -a --columns=ref > list-after-portal-uninstall.out
assert_not_file_has_content list-after-portal-uninstall.out "org\.test\.Hello\.Plugin/"

ok "uninstall extension"

# Test 6: List extensions after uninstall - should show not installed again
run_with_sandboxed_bus \
    env TEST_APP_ID=${TEST_APP_ID} TEST_EXTENSION_REF=${TEST_EXTENSION_REF} \
    ${test_builddir}/test-extension-portal list ext.pid > list-after-uninstall.out

# Plugin should now be not installed, with empty commit
assert_file_has_content list-after-uninstall.out "name=org.test.Hello.Plugin .* installed=0"
assert_file_has_content list-after-uninstall.out "name=org.test.Hello.Plugin .* commit= "
# Locale should still be installed
assert_file_has_content list-after-uninstall.out "name=org.test.Hello.Locale .* installed=1"

ok "list shows uninstalled extension"

# Test 7: Install a ref that doesn't belong to this app (should be rejected with AccessDenied)
run_with_sandboxed_bus \
    env TEST_APP_ID=${TEST_APP_ID} TEST_EXTENSION_REF=${TEST_EXTENSION_REF} \
    ${test_builddir}/test-extension-portal install-bad-ref ext.pid > install-bad-ref.out

# Verify the D-Bus error was AccessDenied with the expected message
assert_file_has_content install-bad-ref.out "AccessDenied"
assert_file_has_content install-bad-ref.out "does not belong to app"

# Verify that nothing was actually installed (the bad ref is org.other.App.Extension)
${FLATPAK} ${U} list -a --columns=ref > list-after-bad-ref.out
assert_not_file_has_content list-after-bad-ref.out "org\.other\.App\.Extension"

ok "install bad ref rejected"

# Test 8: Install a ref that belongs to the app but doesn't exist in any remote
run_with_sandboxed_bus \
    env TEST_APP_ID=${TEST_APP_ID} TEST_EXTENSION_REF=${TEST_EXTENSION_REF} \
    ${test_builddir}/test-extension-portal install-not-found ext.pid > install-not-found.out

# Verify the progress signal reported FileNotFound error with expected message
assert_file_has_content install-not-found.out "FileNotFound"
assert_file_has_content install-not-found.out "not found in any configured remote"

# Verify that the non-existent extension was not installed
${FLATPAK} ${U} list -a --columns=ref > list-after-not-found.out
assert_not_file_has_content list-after-not-found.out "org\.test\.Hello\.NonExistent"

ok "install not-found ref returns error"
