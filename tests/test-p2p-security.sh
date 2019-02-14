#!/bin/bash
#
# Copyright (C) 2019 Matthew Leeds <matthew.leeds@endlessm.com>
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
# License along with this library; if not, see <https://www.gnu.org/licenses/>.

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_without_p2p
skip_without_ostree_version 2019 2

# Test that for an app A installed from a remote R1, updates for the app are
# verified using the keyring associated with remote R1, even if remote R2 has
# configured the same collection ID as R1 and attempts to serve an update for
# A. In other words, ensure that we only have to trust the remote that is
# trusted at install time to provide good updates.

echo "1..1"

# First install from a legitimate remote
setup_repo test org.test.Collection
install_repo test

INITIAL_COMMIT=$(${FLATPAK} ${U} info -c org.test.Hello)
if [ "x${INITIAL_COMMIT}" == "x" ]; then
    assert_not_reached "Can't get installed commit of org.test.Hello"
fi

GPG_KEY1=$(ostree --repo=${FL_DIR}/repo show ${INITIAL_COMMIT} | sed -n "s/^.*RSA key ID \([[:xdigit:]]\+\)$/\1/p")

# Then add a malicious remote which has the same collection ID but a different GPG key
GPGPUBKEY="${FL_GPG_HOMEDIR2}/pubring.gpg" GPGARGS="${FL_GPGARGS2}" setup_repo test-impostor org.test.Collection

# Create an update signed by the malicious remote
sleep 1 # Ensure the timestamp on the new commit is later
GPGARGS="${FL_GPGARGS2}" make_updated_app test-impostor org.test.Collection

# Updating should fail because the signatures aren't from the legitimate remote
if G_MESSAGES_DEBUG=all ${FLATPAK} ${U} update -y org.test.Hello >failed-p2p-update-log; then
    assert_not_reached "Update of org.test.Hello was successful despite malicious commit"
fi
assert_file_has_content failed-p2p-update-log "GPG signatures found, but none are in trusted keyring"

COMMIT_AFTER_FAILED_UPDATE=$(${FLATPAK} ${U} info -c org.test.Hello)
if [ "x${INITIAL_COMMIT}" != "x${COMMIT_AFTER_FAILED_UPDATE}" ]; then
    assert_not_reached "org.test.Hello was updated from the malicious remote"
fi

# The legitimate remote should still be able to serve an update
sleep 1 # Ensure the timestamp on the new commit is later
make_updated_app test org.test.Collection
UPDATE_COMMIT=$(ostree --repo=repos/test rev-parse app/org.test.Hello/${ARCH}/master)
${FLATPAK} ${U} update -y org.test.Hello
COMMIT_AFTER_SUCCESSFUL_UPDATE=$(${FLATPAK} ${U} info -c org.test.Hello)
if [ "x${UPDATE_COMMIT}" != "x${COMMIT_AFTER_SUCCESSFUL_UPDATE}" ]; then
    assert_not_reached "org.test.Hello was not updated"
fi

GPG_KEY2=$(ostree --repo=${FL_DIR}/repo show ${COMMIT_AFTER_SUCCESSFUL_UPDATE} | sed -n "s/^.*RSA key ID \([[:xdigit:]]\+\)$/\1/p")
if [ "x${GPG_KEY1}" != "x${GPG_KEY2}" ]; then
    assert_not_reached "Updated commit not signed with the same GPG key"
fi

echo "ok updates are verified with the correct remote's keyring"
