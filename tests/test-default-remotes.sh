#!/bin/bash
#
# Copyright (C) 2019 Alexander Larsson <alexl@redhat.com>
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

USE_SYSTEMDIR=yes

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_revokefs_without_fuse

setup_repo

cat << EOF > added-default.flatpakrepo
[Flatpak Repo]
Url=http://127.0.0.1/test
Title=The Title
Comment=The Comment
Description=The Description
Homepage=https://the.homepage/
Icon=https://the.icon/
EOF

echo "1..5"

mkdir -p $FLATPAK_CONFIG_DIR/remotes.d

${FLATPAK} -vv --system  remotes > remotes
assert_not_file_has_content remotes "added-default"

cp added-default.flatpakrepo $FLATPAK_CONFIG_DIR/remotes.d/

${FLATPAK}  --system remotes > remotes
assert_file_has_content remotes "added-default"

assert_remote_has_config added-default url "http://127.0.0.1/test"
assert_remote_has_config added-default gpg-verify "false"
assert_remote_has_config added-default xa.title "The Title"
assert_remote_has_no_config added-default xa.title-is-set
assert_remote_has_config added-default xa.comment "The Comment"
assert_remote_has_no_config added-default xa.comment-is-set
assert_remote_has_config added-default xa.description "The Description"
assert_remote_has_no_config added-default xa.description-is-set
assert_remote_has_config added-default xa.homepage "https://the.homepage/"
assert_remote_has_no_config added-default xa.homepage-is-set
assert_remote_has_config added-default xa.icon "https://the.icon/"
assert_remote_has_no_config added-default xa.icon-is-set
assert_remote_has_no_config added-default xa.noenumerate
assert_remote_has_no_config added-default xa.filter

ok "pre-existing installation"

rm -rf $FL_DIR

${FLATPAK}  --system remotes > remotes
assert_file_has_content remotes "added-default"

ok "non-existing installation"

${FLATPAK} --system remotes > remotes
assert_file_has_content remotes "added-default"

${FLATPAK}  --system remote-delete added-default

# Doesn't come back once removed
${FLATPAK} --system remotes > remotes
assert_not_file_has_content remotes "added-default"

ok "allow remove"

rm -rf $FL_DIR
rm -rf $FLATPAK_CONFIG_DIR/remotes.d/*

${FLATPAK}  --system remote-add  --title "Title2" added-default http://127.0.0.1/other-url

${FLATPAK}  --system remotes > remotes
assert_file_has_content remotes "added-default"

assert_remote_has_config added-default url "http://127.0.0.1/other-url"

cp added-default.flatpakrepo $FLATPAK_CONFIG_DIR/remotes.d/

${FLATPAK}  --system remotes > remotes
assert_file_has_content remotes "added-default"

# Should keep the old value
assert_remote_has_config added-default url "http://127.0.0.1/other-url"

# And none of the fields from the file
assert_remote_has_config added-default xa.title "Title2"
assert_remote_has_no_config added-default xa.comment
assert_remote_has_no_config added-default xa.description
assert_remote_has_no_config added-default xa.homepage

ok "pre-existing remote"

rm -rf $FL_DIR
rm -rf $FLATPAK_CONFIG_DIR/remotes.d/*

cp added-default.flatpakrepo $FLATPAK_CONFIG_DIR/remotes.d/
echo "Filter=${test_builddir}/test.filter" >> $FLATPAK_CONFIG_DIR/remotes.d/added-default.flatpakrepo

${FLATPAK} --system remotes > remotes
assert_file_has_content remotes "added-default"

assert_remote_has_config added-default xa.filter "${test_builddir}/test.filter"

# --if-not-exists will still magically override the filter
${FLATPAK}  --system remote-add --if-not-exists --from added-default added-default.flatpakrepo

${FLATPAK} --system remotes > remotes
assert_file_has_content remotes "added-default"

assert_remote_has_no_config added-default xa.filter

ok "override default filter"
