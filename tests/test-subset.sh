#!/bin/bash
#
# Copyright (C) 2020 Alexander Larsson <alexl@redhat.com>
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
skip_revokefs_without_fuse

echo "1..3"

EXPORT_ARGS="--subset=subset1 --subset=subset2" setup_repo

$FLATPAK repo repos/test > repo-info.txt
assert_file_has_content repo-info.txt "Subsummaries: .*subset1-$ARCH.*"
assert_file_has_content repo-info.txt "Subsummaries: .*subset2-$ARCH.*"

$FLATPAK repo --branches repos/test > repo-all.txt
assert_file_has_content repo-all.txt "app/org.test.Hello/$ARCH/master"
assert_file_has_content repo-all.txt "runtime/org.test.Platform/$ARCH/master"

EXPORT_ARGS="--subset=subset1 " GPGARGS="${FL_GPGARGS}" SIGNARGS="${FL_SIGNARGS}" $(dirname $0)/make-test-app.sh repos/test org.test.SubsetOne master ""
EXPORT_ARGS="--subset=subset2 " GPGARGS="${FL_GPGARGS}" SIGNARGS="${FL_SIGNARGS}" $(dirname $0)/make-test-app.sh repos/test org.test.SubsetTwo master ""
EXPORT_ARGS="" GPGARGS="${FL_GPGARGS}" SIGNARGS="${FL_SIGNARGS}" $(dirname $0)/make-test-app.sh repos/test org.test.NoSubset master ""
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} ${FL_SIGNARGS} repos/test

$FLATPAK repo repos/test > repo-info.txt
assert_file_has_content repo-info.txt "Subsummaries: .*subset1-$ARCH.*"
assert_file_has_content repo-info.txt "Subsummaries: .*subset2-$ARCH.*"

$FLATPAK repo --branches repos/test > repo-all.txt
assert_file_has_content repo-all.txt "app/org.test.Hello/$ARCH/master"
assert_file_has_content repo-all.txt "app/org.test.SubsetOne/$ARCH/master"
assert_file_has_content repo-all.txt "app/org.test.SubsetTwo/$ARCH/master"
assert_file_has_content repo-all.txt "app/org.test.NoSubset/$ARCH/master"
assert_file_has_content repo-all.txt "runtime/org.test.Platform/$ARCH/master"

$FLATPAK repo --branches repos/test --subset=subset1 > repo-subset1.txt
assert_file_has_content repo-subset1.txt "app/org.test.Hello/$ARCH/master"
assert_file_has_content repo-subset1.txt "app/org.test.SubsetOne/$ARCH/master"
assert_not_file_has_content repo-subset1.txt "app/org.test.SubsetTwo/$ARCH/master"
assert_not_file_has_content repo-subset1.txt "app/org.test.NoSubset/$ARCH/master"
assert_file_has_content repo-subset1.txt "runtime/org.test.Platform/$ARCH/master"

$FLATPAK repo --branches repos/test --subset=subset2 > repo-subset2.txt
assert_file_has_content repo-subset2.txt "app/org.test.Hello/$ARCH/master"
assert_not_file_has_content repo-subset2.txt "app/org.test.SubsetOne/$ARCH/master"
assert_file_has_content repo-subset2.txt "app/org.test.SubsetTwo/$ARCH/master"
assert_not_file_has_content repo-subset2.txt "app/org.test.NoSubset/$ARCH/master"
assert_file_has_content repo-subset2.txt "runtime/org.test.Platform/$ARCH/master"

ok "repo has right refs in right subset"

${FLATPAK} ${U} remote-modify --subset=subset1 test-repo

${FLATPAK} ${U} remote-ls test-repo > remote-subset1.txt
assert_file_has_content remote-subset1.txt "org.test.Hello"
assert_file_has_content remote-subset1.txt "org.test.SubsetOne"
assert_not_file_has_content remote-subset1.txt "org.test.SubsetTwo"
assert_not_file_has_content remote-subset1.txt "org.test.NoSubset"
assert_file_has_content remote-subset1.txt "org.test.Platform"

${FLATPAK} ${U} install -y org.test.Hello &> /dev/null
${FLATPAK} ${U} install -y org.test.SubsetOne &> /dev/null

if ${FLATPAK} ${U} install -y org.test.SubsetTwo &> /dev/null; then
    assert_not_reached "Subset2 should not be visible"
fi

${FLATPAK} ${U} update --appstream
assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml
assert_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml org.test.Hello.desktop
assert_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml org.test.SubsetOne.desktop
assert_not_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml org.test.SubsetTwo.desktop
assert_not_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml org.test.NoSubset.desktop

ok "remote subset handling works"

${FLATPAK} ${U} remote-modify --subset=subset2 test-repo

${FLATPAK} ${U} remote-ls test-repo > remote-subset2.txt
assert_file_has_content remote-subset2.txt "org.test.Hello"
assert_not_file_has_content remote-subset2.txt "org.test.SubsetOne"
assert_file_has_content remote-subset2.txt "org.test.SubsetTwo"
assert_not_file_has_content remote-subset1.txt "org.test.NoSubset"
assert_file_has_content remote-subset2.txt "org.test.Platform"

${FLATPAK} ${U} install -y org.test.SubsetTwo &> /dev/null

${FLATPAK} ${U} update --appstream
assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml
assert_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml org.test.Hello.desktop
assert_not_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml org.test.SubsetOne.desktop
assert_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml org.test.SubsetTwo.desktop
assert_not_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml org.test.NoSubset.desktop

ok "remote subset switching works"
