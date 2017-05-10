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

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_without_user_xattrs

echo "1..4"

#Regular repo
setup_repo

#unsigned repo
GPGPUBKEY="" GPG_ARGS="" setup_repo test-no-gpg

#alternative gpg key repo
GPGPUBKEY="${FL_GPG_HOMEDIR2}/pubring.gpg" GPGARGS="${FL_GPGARGS2}" setup_repo test-gpg2

#remote with missing GPG key
port=$(cat httpd-port-main)
flatpak remote-add ${U} test-missing-gpg-repo "http://127.0.0.1:${port}/test"

#remote with wrong GPG key
port=$(cat httpd-port-main)
flatpak remote-add ${U} --gpg-import=${FL_GPG_HOMEDIR2}/pubring.gpg test-wrong-gpg-repo "http://127.0.0.1:${port}/test"

install_repo test-no-gpg
echo "ok install without gpg key"

${FLATPAK} ${U} uninstall org.test.Platform org.test.Hello

install_repo test-gpg2
echo "ok with alternative gpg key"

${FLATPAK} ${U} uninstall org.test.Platform org.test.Hello

if ${FLATPAK} ${U} install test-missing-gpg-repo org.test.Platform 2> install-error-log; then
    assert_not_reached "Should not be able to install with missing gpg key"
fi
assert_file_has_content install-error-log "GPG signatures found, but none are in trusted keyring"


if ${FLATPAK} ${U} install test-missing-gpg-repo org.test.Hello 2> install-error-log; then
    assert_not_reached "Should not be able to install with missing gpg key"
fi
assert_file_has_content install-error-log "GPG signatures found, but none are in trusted keyring"

echo "ok fail with missing gpg key"

if ${FLATPAK} ${U} install test-wrong-gpg-repo org.test.Platform 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong gpg key"
fi
assert_file_has_content install-error-log "GPG signatures found, but none are in trusted keyring"

if ${FLATPAK} ${U} install test-wrong-gpg-repo org.test.Hello 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong gpg key"
fi
assert_file_has_content install-error-log "GPG signatures found, but none are in trusted keyring"

echo "ok fail with wrong gpg key"
