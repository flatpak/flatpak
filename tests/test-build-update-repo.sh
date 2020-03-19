#!/bin/bash
#
# Copyright © 2018 Endless Mobile, Inc.
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

echo "1..5"

# Configure a repository, then set a collection ID on it and check that the ID
# is saved in the config file.
setup_repo
install_repo

${FLATPAK} build-update-repo --collection-id=org.test.Collection repos/test

assert_file_has_content repos/test/config '^collection-id=org\.test\.Collection$'

ok "1 update repo to add collection ID"

# Test that you’re not allowed to change the collection ID once it’s already set.
if ${FLATPAK} build-update-repo --collection-id=org.test.Collection2 repos/test 2> build-update-repo-error-log; then
    assert_not_reached "flatpak build-update-repo should not set a collection ID when one is already set"
fi

assert_file_has_content repos/test/config '^collection-id=org\.test\.Collection$'
assert_not_file_has_content repos/test/config '^collection-id=org\.test\.Collection2$'

ok "2 collection ID cannot be changed"

${FLATPAK} build-update-repo --title="My little repo" repos/test

assert_file_has_content repos/test/config '^title=My little repo$'

ok "can update repo title"

${FLATPAK} build-update-repo --redirect-url=http://no.where/ repos/test

assert_file_has_content repos/test/config '^redirect-url=http://no\.where/$'

ok "can update redirect url"

${FLATPAK} build-update-repo --default-branch=no-such-branch repos/test

assert_file_has_content repos/test/config '^default-branch=no-such-branch$'

ok "can update default branch"
