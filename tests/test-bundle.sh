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

echo "1..6"

setup_repo

${FLATPAK} build-bundle repo --repo-url=file://`pwd`/repo --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg hello.flatpak org.test.Hello
assert_has_file hello.flatpak

${FLATPAK} build-bundle repo --runtime --repo-url=file://`pwd`/repo --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg platform.flatpak org.test.Platform
assert_has_file platform.flatpak

echo "ok create bundles"

${FLATPAK} install ${U} --bundle hello.flatpak

assert_has_dir $FL_DIR/app/org.test.Hello
assert_has_symlink $FL_DIR/app/org.test.Hello/current
assert_symlink_has_content $FL_DIR/app/org.test.Hello/current ^$ARCH/master$
assert_has_dir $FL_DIR/app/org.test.Hello/$ARCH/master
assert_has_symlink $FL_DIR/app/org.test.Hello/$ARCH/master/active
ID=`readlink $FL_DIR/app/org.test.Hello/$ARCH/master/active`
assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/master/active/deploy
assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/master/active/metadata
assert_has_dir $FL_DIR/app/org.test.Hello/$ARCH/master/active/files
assert_has_dir $FL_DIR/app/org.test.Hello/$ARCH/master/active/export
assert_has_file $FL_DIR/exports/share/applications/org.test.Hello.desktop
# Ensure Exec key is rewritten
assert_file_has_content $FL_DIR/exports/share/applications/org.test.Hello.desktop "^Exec=.*/flatpak run --branch=master --arch=$ARCH --command=hello.sh org.test.Hello$"
assert_has_file $FL_DIR/exports/share/icons/hicolor/64x64/apps/org.test.Hello.png

# Ensure triggers ran
assert_has_file $FL_DIR/exports/share/applications/mimeinfo.cache
assert_file_has_content $FL_DIR/exports/share/applications/mimeinfo.cache x-test/Hello
assert_has_file $FL_DIR/exports/share/icons/hicolor/icon-theme.cache
assert_has_file $FL_DIR/exports/share/icons/hicolor/index.theme

$FLATPAK list ${U} | grep org.test.Hello > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep org.test.Hello-origin > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep current > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep ${ID:0:12} > /dev/null

$FLATPAK info ${U} org.test.Hello > /dev/null
$FLATPAK info ${U} org.test.Hello | grep org.test.Hello-origin > /dev/null
$FLATPAK info ${U} org.test.Hello | grep $ID > /dev/null

$FLATPAK remote-list ${U} -d | grep org.test.Hello-origin > /dev/null
$FLATPAK remote-list ${U} -d | grep org.test.Hello-origin | grep no-enumerate > /dev/null
assert_has_file $FL_DIR/repo/org.test.Hello-origin.trustedkeys.gpg

echo "ok install app bundle"

${FLATPAK} install ${U} --bundle platform.flatpak

assert_has_dir $FL_DIR/runtime/org.test.Platform
assert_has_dir $FL_DIR/runtime/org.test.Platform/$ARCH/master
assert_has_symlink $FL_DIR/runtime/org.test.Platform/$ARCH/master/active
ID=`readlink $FL_DIR/runtime/org.test.Platform/$ARCH/master/active`
assert_has_file $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/deploy
assert_has_file $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/metadata
assert_has_dir $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/files

$FLATPAK list ${U} --runtime | grep org.test.Platform > /dev/null
$FLATPAK list ${U} -d --runtime | grep org.test.Platform | grep org.test.Platform-origin > /dev/null
$FLATPAK list ${U} -d --runtime | grep org.test.Platform | grep ${ID:0:12} > /dev/null

$FLATPAK info ${U} --runtime org.test.Platform > /dev/null
$FLATPAK info ${U} --runtime org.test.Platform | grep org.test.Platform-origin > /dev/null
$FLATPAK info ${U} --runtime org.test.Platform | grep $ID > /dev/null

$FLATPAK remote-list ${U} -d | grep org.test.Platform-origin > /dev/null
$FLATPAK remote-list ${U} -d | grep org.test.Platform-origin | grep no-enumerate > /dev/null
assert_has_file $FL_DIR/repo/org.test.Platform-origin.trustedkeys.gpg

echo "ok install runtime bundle"

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

echo "ok run"


OLD_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

# TODO: For weird reasons this breaks in the system case. Needs debugging
if [ x${USE_SYSTEMDIR-} != xyes ] ; then
    ${FLATPAK} ${U} update -v org.test.Hello master
    ALSO_OLD_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`
    assert_streq "$OLD_COMMIT" "$ALSO_OLD_COMMIT"
fi

echo "ok null update"

make_updated_app

${FLATPAK} ${U} update org.test.Hello

NEW_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

assert_not_streq "$OLD_COMMIT" "$NEW_COMMIT"

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATED$'

echo "ok update"
