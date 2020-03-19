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

echo "1..7"

# Configure a repository and set up a collection ID for it. Check that setting
# the collection ID in the remote config disables summary signature checking.
setup_repo
install_repo

echo -e "[core]\ncollection-id=org.test.Collection" >> repos/test/config
${FLATPAK} remote-modify ${U} test-repo --collection-id org.test.Collection

assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=false$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify-summary=true$'
assert_file_has_content ${FL_DIR}/repo/config '^collection-id=org\.test\.Collection$'
assert_file_has_content ${FL_DIR}/repo/config '^gpg-verify=true$'
assert_not_file_has_content ${FL_DIR}/repo/config '^gpg-verify=false$'

ok "1 repo config with collections"

# Test that building an app with a collection ID set produces the right
# metadata in the resulting repository.
DIR=$(mktemp -d)
${FLATPAK} build-init ${DIR} org.test.App org.test.Platform org.test.Platform
mkdir -p ${DIR}/files/a
echo "a" > ${DIR}/files/a/data
${FLATPAK} build-finish ${DIR} --socket=x11 --share=network --command=true
${FLATPAK} build-export ${FL_GPGARGS} --update-appstream repos/test --collection-id org.test.Collection ${DIR} master
update_repo

ostree --repo=repos/test refs > refs
assert_file_has_content refs "^app/org\.test\.App/$ARCH/master$"
assert_file_has_content refs '^ostree-metadata$'
assert_file_has_content refs "^appstream/${ARCH}$"
assert_file_has_content refs "^appstream2/${ARCH}$"
ostree --repo=repos/test refs --collections > refs-collections
assert_file_has_content refs-collections "^(org\.test\.Collection, app/org\.test\.App/$ARCH/master)$"
assert_file_has_content refs-collections '^(org\.test\.Collection, ostree-metadata)$'
assert_file_has_content refs-collections "^(org\.test\.Collection, appstream/${ARCH})$"
assert_file_has_content refs-collections "^(org\.test\.Collection, appstream2/${ARCH})$"
assert_has_file repos/test/summary.sig
ostree --repo=repos/test summary --view > summary
assert_file_has_content summary '^Collection ID (ostree\.summary\.collection-id): org\.test\.Collection$'
assert_file_has_content summary '^xa\.cache: '
ostree --repo=repos/test show --raw ostree-metadata > metadata
assert_file_has_content metadata "'xa\.cache': "
assert_file_has_content metadata "'ostree\.collection-binding': <'org\.test\.Collection'>"
assert_file_has_content metadata "'ostree\.ref-binding': <\['ostree-metadata'\]>"

ok "2 create app with collections"

# Try installing the app.
${FLATPAK} ${U} install -y test-repo org.test.App master
${FLATPAK} ${U} uninstall -y org.test.App

ok "3 install app with collections"

# Regenerate the summary so it doesn’t contain xa.cache and is unsigned; try installing again.
ostree --repo=repos/test summary --update
assert_not_has_file repos/test/summary.sig
ostree --repo=repos/test summary --view > summary
assert_file_has_content summary '^Collection ID (ostree\.summary\.collection-id): org\.test\.Collection$'
assert_not_file_has_content summary '^xa\.cache: '

${FLATPAK} ${U} install -y test-repo org.test.App master
${FLATPAK} ${U} uninstall -y org.test.App

ok "4 install app with collections from unsigned summary"

# Try installing it from a flatpakref file. Don’t uninstall afterwards because
# we need it for the next test.
cat << EOF > org.test.App.flatpakref
[Flatpak Ref]
Title=Test App
Name=org.test.App
Branch=master
Url=http://127.0.0.1:$(cat httpd-port)/test
IsRuntime=False
GPGKey=${FL_GPG_BASE64}
#RuntimeRepo=http://127.0.0.1:$(cat httpd-port)/test
DeployCollectionID=org.test.Collection
EOF

${FLATPAK} ${U} install -y --from ./org.test.App.flatpakref
${FLATPAK} ${U} uninstall -y org.test.App

ok "5 install app with collections from flatpakref"

# Update the repo metadata and check that it changes in the ostree-metadata branch
# and the summary file.
${FLATPAK} build-update-repo ${FL_GPGARGS} --title "New title" repos/test

assert_has_file repos/test/summary.sig
ostree --repo=repos/test summary --view > summary
assert_file_has_content summary '^Collection ID (ostree\.summary\.collection-id): org\.test\.Collection$'
assert_file_has_content summary '^xa\.title: '
ostree --repo=repos/test show --raw ostree-metadata > metadata
assert_file_has_content metadata "'xa\.title': "
assert_file_has_content metadata "'ostree\.collection-binding': <'org\.test\.Collection'>"
assert_file_has_content metadata "'ostree\.ref-binding': <\['ostree-metadata'\]>"

ok "6 update repo metadata"

# Try to install the app again, which should pull the updated repository
# metadata as a side effect.
${FLATPAK} ${U} install -y test-repo org.test.App master
assert_file_has_content ${FL_DIR}/repo/config '^xa\.title=New title$'

ok "7 pull updated repo metadata"
