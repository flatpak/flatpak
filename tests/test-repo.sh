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
[ x${USE_SYSTEMDIR-} != xyes ] || skip_without_user_xattrs

if [ x${USE_COLLECTIONS_IN_CLIENT-} == xyes ] || [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
    skip_without_p2p
fi

echo "1..6"

#Regular repo
setup_repo

# Unsigned repo (not supported with collections; client-side use of collections requires GPG)
if [ x${USE_COLLECTIONS_IN_CLIENT-} == xyes ] ; then
    if GPGPUBKEY=" " GPGARGS=" " setup_repo test-no-gpg org.test.Collection.NoGpg; then
        assert_not_reached "Should fail remote-add due to missing GPG key"
    fi
elif [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
    # Set a collection ID and GPG on the server, but not in the client configuration
    setup_repo_no_add test-no-gpg org.test.Collection.NoGpg
    port=$(cat httpd-port-main)
    flatpak remote-add ${U} --no-gpg-verify test-no-gpg-repo "http://127.0.0.1:${port}/test-no-gpg"
else
    GPGPUBKEY="" GPGARGS="" setup_repo test-no-gpg
fi

#alternative gpg key repo
GPGPUBKEY="${FL_GPG_HOMEDIR2}/pubring.gpg" GPGARGS="${FL_GPGARGS2}" setup_repo test-gpg2 org.test.Collection.Gpg2

#remote with missing GPG key
# Don’t use --collection-id= here, or the collections code will grab the appropriate
# GPG key from one of the previously-configured remotes with the same collection ID.
port=$(cat httpd-port-main)
if flatpak remote-add ${U} test-missing-gpg-repo "http://127.0.0.1:${port}/test"; then
    assert_not_reached "Should fail metadata-update due to missing gpg key"
fi

#remote with wrong GPG key
port=$(cat httpd-port-main)
if flatpak remote-add ${U} --gpg-import=${FL_GPG_HOMEDIR2}/pubring.gpg test-wrong-gpg-repo "http://127.0.0.1:${port}/test"; then
    assert_not_reached "Should fail metadata-update due to wrong gpg key"
fi

if [ x${USE_COLLECTIONS_IN_CLIENT-} != xyes ] ; then
    install_repo test-no-gpg
    echo "ok install without gpg key"

    ${FLATPAK} ${U} uninstall org.test.Platform org.test.Hello
else
    echo "ok install without gpg key # skip not supported for collections"
fi

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

${FLATPAK} ${U} remotes -d | grep ^test-repo > repo-info
assert_not_file_has_content repo-info "new-title"
UPDATE_REPO_ARGS=--title=new-title update_repo
assert_file_has_content repos/test/config new-title

# This should make us automatically pick up the new metadata
${FLATPAK} ${U} install test-repo org.test.Platform
${FLATPAK} ${U} remotes -d | grep ^test-repo > repo-info
assert_file_has_content repo-info "new-title"

echo "ok update metadata"

port=$(cat httpd-port-main)
UPDATE_REPO_ARGS="--redirect-url=http://127.0.0.1:${port}/test-gpg3 --gpg-import=${FL_GPG_HOMEDIR2}/pubring.gpg" update_repo
GPGPUBKEY="${FL_GPG_HOMEDIR2}/pubring.gpg" GPGARGS="${FL_GPGARGS2}" setup_repo_no_add test-gpg3 org.test.Collection.test

${FLATPAK} ${U} update org.test.Platform
# Ensure we have the new uri
${FLATPAK} ${U} remotes -d | grep ^test-repo > repo-info
assert_file_has_content repo-info "/test-gpg3"

# Make sure we also get new installs from the new repo
GPGARGS="${FL_GPGARGS2}" make_updated_app test-gpg3 org.test.Collection.test
update_repo test-gpg3 org.test.Collection.test

${FLATPAK} ${U} install test-repo org.test.Hello
assert_file_has_content $FL_DIR/app/org.test.Hello/$ARCH/master/active/files/bin/hello.sh UPDATED

echo "ok redirect url and gpg key"
