#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

USE_COLLECTIONS_IN_SERVER=yes
USE_COLLECTIONS_IN_CLIENT=yes

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_revokefs_without_fuse

echo "1..9"

#Regular repo
setup_repo

# Ensure we have the full locale extension:
${FLATPAK} ${U} config  --set languages "*"

${FLATPAK} ${U} install -y test-repo org.test.Hello

mkdir usb_dir

${FLATPAK} ${U} create-usb --destination-repo=repo usb_dir org.test.Hello

assert_has_file usb_dir/repo/config
assert_has_file usb_dir/repo/summary
assert_has_file usb_dir/repo/refs/mirrors/org.test.Collection.test/app/org.test.Hello/${ARCH}/master
assert_has_file usb_dir/repo/refs/mirrors/org.test.Collection.test/runtime/org.test.Hello.Locale/${ARCH}/master
assert_has_file usb_dir/repo/refs/mirrors/org.test.Collection.test/runtime/org.test.Platform/${ARCH}/master
assert_has_file usb_dir/repo/refs/mirrors/org.test.Collection.test/appstream2/${ARCH}

${FLATPAK} ${U} uninstall -y --all

ok "created sideloaded repo"

${FLATPAK} ${U} remote-modify --url="http://no.127.0.0.1:${port}/test" test-repo

if ${FLATPAK} ${U} install -y test-repo org.test.Hello &> /dev/null; then
    assert_not_reached "Should not be able to install with wrong url"
fi

mkdir $FL_DIR/sideload-repos
SIDELOAD_REPO_LINK=$FL_DIR/sideload-repos/sideload1
mkdir $FLATPAK_RUN_DIR/sideload-repos
SIDELOAD_REPO_LINK2=$FLATPAK_RUN_DIR/sideload-repos/sideload1

SIDELOAD_REPO=$(realpath usb_dir/repo)

# This one uses the sideload repo in /run, the rest in the flatpak dir
ln -s $SIDELOAD_REPO $SIDELOAD_REPO_LINK2

${FLATPAK} ${U} install -y test-repo org.test.Hello
assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/master/active/metadata
assert_has_file $FL_DIR/repo/refs/remotes/test-repo/app/org.test.Hello/${ARCH}/master
assert_not_has_file $FL_DIR/repo/refs/mirrors/org.test.Collection.test/app/org.test.Hello/${ARCH}/master

ok "installed sideloaded app"

rm $SIDELOAD_REPO_LINK2

${FLATPAK} ${U} uninstall -y org.test.Hello
${FLATPAK} ${U} install --sideload-repo=${SIDELOAD_REPO} -y test-repo org.test.Hello

assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/master/active/metadata
assert_has_file $FL_DIR/repo/refs/remotes/test-repo/app/org.test.Hello/${ARCH}/master
assert_not_has_file $FL_DIR/repo/refs/mirrors/org.test.Collection.test/app/org.test.Hello/${ARCH}/master

ln -s $SIDELOAD_REPO $SIDELOAD_REPO_LINK

ok "installed w/ runtime sideload option"

# Remove old appstream checkouts so we can update from the sideload
rm -rf $FL_DIR/appstream/test-repo/$ARCH/
rm -rf $FL_DIR/repo/refs/remotes/test-repo/appstream2/$ARCH
ostree prune --repo=$FL_DIR/repo --refs-only

${FLATPAK} ${U} update --appstream test-repo

assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml

ok "updated sideloaded appstream"

${FLATPAK} ${U} remote-modify --url="http://127.0.0.1:${port}/test" test-repo
${FLATPAK} ${U} uninstall -y --all

# Disable sideload repo and "break" online repo
rm $SIDELOAD_REPO_LINK
mv repos/test/objects repos/test/objects.disabled

httpd_clear_log

# Ensure this fails (but still loads summary)
if ${FLATPAK} ${U} install -y test-repo org.test.Hello &> install-error-log; then
    assert_not_reached "Disabled online install broken"
fi
assert_file_has_content install-error-log "Server returned .*404.*"
assert_file_has_content httpd-log "GET .*commit .*404"

ln -s $SIDELOAD_REPO $SIDELOAD_REPO_LINK

${FLATPAK} ${U} install -y test-repo org.test.Hello
assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/master/active/metadata

# Re-enable online repo
mv repos/test/objects.disabled repos/test/objects

ok "installed sideloaded app when online"

OLD_COMMIT=$(cat repos/test/refs/heads/app/org.test.Hello/${ARCH}/master)

# Make new app version
make_updated_app
update_repo

NEW_COMMIT=$(cat repos/test/refs/heads/app/org.test.Hello/$ARCH/master)

# (Re)install the new version (online), ensure we used sideload repo for objects shared between new and old version
SHARED_OBJECT=$(ostree ls --repo=repos/test -C app/org.test.Hello/$ARCH/master /files/share/applications/org.test.Hello.desktop | awk '{ print $5 }')
SHARED_OBJECT_PATH=$(commit_to_path $SHARED_OBJECT filez)
${FLATPAK} ${U} uninstall -y app/org.test.Hello/$ARCH/master

httpd_clear_log
${FLATPAK} ${U} install -y app/org.test.Hello/$ARCH/master

assert_not_file_has_content httpd-log "GET /test/${SHARED_OBJECT_PATH}"

# Prepare sideload repo for NEW_COMMIT
${FLATPAK} ${U} create-usb --destination-repo=repo2 usb_dir org.test.Hello

UPDATED_COMMIT=$( ${FLATPAK} ${U} info --show-commit app/org.test.Hello/${ARCH}/master )
assert_streq "$NEW_COMMIT" "$UPDATED_COMMIT"

# Update again should do nothing
${FLATPAK} ${U} update -y
UPDATED_COMMIT=$( ${FLATPAK} ${U} info --show-commit app/org.test.Hello/${ARCH}/master )
assert_streq "$NEW_COMMIT" "$UPDATED_COMMIT"

# Ensure that offline update don't downgrade to older version
${FLATPAK} ${U} remote-modify --url="http://no.127.0.0.1:${port}/test" test-repo
${FLATPAK} ${U} update -y
UPDATED_COMMIT=$( ${FLATPAK} ${U} info --show-commit app/org.test.Hello/${ARCH}/master )
assert_streq "$NEW_COMMIT" "$UPDATED_COMMIT"

ok "updates from sideload don't go backwards"

if [ x${USE_SYSTEMDIR-} == xyes ]; then
    # --commit + --system works only as root, so lets just "fake it" by installing the current sideload version
    ${FLATPAK} ${U} uninstall -y --no-related org.test.Hello
    ${FLATPAK} ${U} install -y org.test.Hello
else
    # Try (offline) to update to the old version
    ${FLATPAK} ${U} update -y --commit $OLD_COMMIT org.test.Hello
fi

UPDATED_COMMIT=$( ${FLATPAK} ${U} info --show-commit app/org.test.Hello/${ARCH}/master )
assert_streq "$OLD_COMMIT" "$UPDATED_COMMIT"

ok "update to explicit old version"

# Switch to updated usb repo
mv usb_dir/repo usb_dir/repo.old
mv usb_dir/repo2 usb_dir/repo

# Try with --sideload-repo instead of config
rm $SIDELOAD_REPO_LINK

# And update to it (offline)
${FLATPAK} ${U} update -y --sideload-repo=${SIDELOAD_REPO}

UPDATED_COMMIT=$( ${FLATPAK} ${U} info --show-commit app/org.test.Hello/${ARCH}/master )
assert_streq "$NEW_COMMIT" "$UPDATED_COMMIT"

# Re enable (go online)
${FLATPAK} ${U} remote-modify --url="http://127.0.0.1:${port}/test" test-repo

ok "update offline to new version"

assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=true$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=false$'

ostree --repo=$FL_DIR/repo config set --group='remote "test-repo"' gpg-verify-summary false

assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=false$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=true$'

${FLATPAK} ${U} update -y

assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=true$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=false$'

ok "migrate to gpg-verify-summary"
