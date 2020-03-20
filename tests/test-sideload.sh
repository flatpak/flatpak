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

echo "1..3"

#Regular repo
setup_repo

${FLATPAK} ${U} install -y test-repo org.test.Hello
# Ensure we have the full locale extension:
${FLATPAK} ${U} update -y --subpath=

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

SIDELOAD_REPO=$(realpath usb_dir/repo)
${FLATPAK} ${U} config --set sideload-repos ${SIDELOAD_REPO}

${FLATPAK} ${U} config --get sideload-repos > sideload-repos
assert_file_has_content sideload-repos ${SIDELOAD_REPO}

${FLATPAK} ${U} install -y test-repo org.test.Hello
assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/master/active/metadata
assert_has_file $FL_DIR/repo/refs/remotes/test-repo/app/org.test.Hello/${ARCH}/master
assert_not_has_file $FL_DIR/repo/refs/mirrors/org.test.Collection.test/app/org.test.Hello/${ARCH}/master

ok "installed sideloaded app"

# Remove old appstream checkouts so we can update from the sideload
rm -rf $FL_DIR/appstream/test-repo/$ARCH/
rm -rf $FL_DIR/repo/refs/remotes/test-repo/appstream2/$ARCH
ostree prune --repo=$FL_DIR/repo --refs-only

${FLATPAK} ${U} update --appstream test-repo

assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml

ok "updated sideloaded appstream"
