#!/bin/bash
#
# Copyright © 2017 Endless Mobile, Inc.
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
#
# Authors:
#  - Philip Withnall <withnall@endlessm.com>

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_revokefs_without_fuse

echo "1..3"

# Configure a repository without a collection ID and pull it locally.
setup_repo
install_repo

DIR=$(mktemp -d)
${FLATPAK} build-init ${DIR} org.test.App org.test.Platform org.test.Platform >&2
mkdir -p ${DIR}/files/a
echo "a" > ${DIR}/files/a/data
${FLATPAK} build-finish ${DIR} --socket=x11 --share=network --command=true >&2
${FLATPAK} build-export --no-update-summary ${FL_GPGARGS} ${FL_SIGNARGS} --update-appstream repos/test ${DIR} master >&2
update_repo

${FLATPAK} ${U} install -y test-repo org.test.App master >&2

assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=true$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=false$'
assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify=true$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify=false$'

if [ x${FL_SIGN_ENABLED} == xyes ]; then
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=true$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=false$'
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify=ed25519$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify=false$'
else
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=false$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=true$'
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify=false$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify=ed25519$'
fi

assert_not_file_has_content ${FL_DIR}/repo/config '^collection-id='

# Change its configuration to include a collection ID, update the repository,
# but don’t mark the collection ID as to be deployed yet. Ensure it doesn’t
# appear in the client’s configuration.
echo -e "[core]\ncollection-id=org.test.Collection" >> repos/test/config
${FLATPAK} build-export --no-update-summary  ${FL_GPGARGS} ${FL_SIGNARGS} --update-appstream repos/test --collection-id org.test.Collection ${DIR} master >&2
UPDATE_REPO_ARGS="--collection-id=org.test.Collection" update_repo

${FLATPAK} ${U} update -y org.test.App master >&2

assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=true$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=false$'
assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify=true$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify=false$'

if [ x${FL_SIGN_ENABLED} == xyes ]; then
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=true$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=false$'
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify=ed25519$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify=false$'
else
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=false$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=true$'
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify=false$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify=ed25519$'
fi

assert_not_file_has_content ${FL_DIR}/repo/config '^collection-id='

ok "1 update repo config without deploying collection ID"

# Now mark the collection ID as to be deployed. The client configuration should
# be updated.
UPDATE_REPO_ARGS="--collection-id=org.test.Collection --deploy-collection-id" update_repo
assert_file_has_content repos/test/config '^deploy-collection-id=true$'

${FLATPAK} ${U} update -y org.test.App master >&2

assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=true$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=false$'
assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify=true$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify=false$'

if [ x${FL_SIGN_ENABLED} == xyes ]; then
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=true$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=false$'
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify=ed25519$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify=false$'
else
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=false$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify-summary=true$'
    assert_file_has_content ${FL_DIR}/repo/config '^sign-verify=false$'
    assert_not_file_has_content ${FL_DIR}/repo/config '^sign-verify=ed25519$'
fi

assert_file_has_content ${FL_DIR}/repo/config '^collection-id=org\.test\.Collection$'

# Try the deploy for sideload only method
sed -i "s/deploy-collection-id=true//" repos/test/config
assert_not_file_has_content repos/test/config '^deploy-collection-id=true$'

${FLATPAK} remote-modify --collection-id= test-repo >&2
assert_not_file_has_content ${FL_DIR}/repo/config '^collection-id=org\.test\.Collection$'

UPDATE_REPO_ARGS="--collection-id=org.test.Collection --deploy-sideload-collection-id" update_repo
assert_file_has_content repos/test/config '^deploy-sideload-collection-id=true$'

${FLATPAK} ${U} update -y org.test.App master >&2

assert_file_has_content ${FL_DIR}/repo/config '^collection-id=org\.test\.Collection$'

ok "2 update repo config to deploy collection ID"

# Try updating the collection ID to some other non-empty value on the server.
# The client should ignore the update (otherwise we have a security vulnerability).
# We have to manually add refs under the old collection ID so the client can pull
# using its old collection ID.
#UPDATE_REPO_ARGS="--collection-id=net.malicious.NewCollection --deploy-collection-id" update_repo
#for ref in app/org.test.App/$(flatpak --default-arch)/master app/org.test.Hello/$(flatpak --default-arch)/master appstream/$(flatpak --default-arch) ostree-metadata runtime/org.test.Platform/$(flatpak --default-arch)/master; do
#  ostree --repo=repos/test refs --collections --create=org.test.Collection:$ref $ref
#done
ostree --repo=repos/test summary --update --add-metadata="ostree.deploy-collection-id='net.malicious.NewCollection'" >&2
${FLATPAK} ${U} update org.test.App master >&2

assert_file_has_content ${FL_DIR}/repo/config '^collection-id=org\.test\.Collection$'
assert_not_file_has_content ${FL_DIR}/repo/config '^collection-id=net\.malicious\.NewCollection$'

ok "3 update repo config with different collection ID"
