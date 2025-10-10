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
skip_revokefs_without_fuse

echo "1..9"

mkdir bundles

setup_repo

${FLATPAK} build-bundle repos/test --repo-url=file://`pwd`/repos/test --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg bundles/hello.flatpak org.test.Hello >&2
assert_has_file bundles/hello.flatpak

${FLATPAK} build-bundle repos/test --runtime --repo-url=file://`pwd`/repos/test --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg bundles/platform.flatpak org.test.Platform >&2
assert_has_file bundles/platform.flatpak

ok "create bundles server-side"

rm bundles/hello.flatpak
${FLATPAK} ${U} install -y test-repo org.test.Hello >&2
${FLATPAK} build-bundle $FL_DIR/repo --repo-url=file://`pwd`/repos/test --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg bundles/hello.flatpak org.test.Hello >&2
assert_has_file bundles/hello.flatpak

ok "create bundles client-side"

${FLATPAK} uninstall ${U} -y org.test.Hello >&2
${FLATPAK} install ${U} -y --bundle bundles/hello.flatpak >&2

# Installing again without reinstall option should fail...
! ${FLATPAK} install ${U} -y --bundle bundles/hello.flatpak >&2
# Now with reinstall option it should pass...
${FLATPAK} install ${U} -y --bundle bundles/hello.flatpak --reinstall >&2

# This should have installed the runtime dependency too
assert_has_file $FL_DIR/repo/refs/remotes/test-repo/runtime/org.test.Platform/$ARCH/master

assert_has_file $FL_DIR/repo/refs/remotes/hello-origin/app/org.test.Hello/$ARCH/master
APP_COMMIT=`cat $FL_DIR/repo/refs/remotes/hello-origin/app/org.test.Hello/$ARCH/master`
assert_has_file $FL_DIR/repo/objects/$(echo $APP_COMMIT | cut -b 1-2)/$(echo $APP_COMMIT | cut -b 3-).commit
assert_has_file $FL_DIR/repo/objects/$(echo $APP_COMMIT | cut -b 1-2)/$(echo $APP_COMMIT | cut -b 3-).commitmeta

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
assert_file_has_content $FL_DIR/exports/share/applications/org.test.Hello.desktop "^Exec=.*flatpak run --branch=master --arch=$ARCH --command=hello\.sh org\.test\.Hello$"
assert_has_file $FL_DIR/exports/share/icons/hicolor/64x64/apps/org.test.Hello.png
assert_has_file $FL_DIR/exports/share/icons/HighContrast/64x64/apps/org.test.Hello.png

$FLATPAK list ${U} | grep org.test.Hello > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep hello-origin > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep current > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep ${ID:0:12} > /dev/null

$FLATPAK info ${U} org.test.Hello > /dev/null
$FLATPAK info ${U} org.test.Hello | grep hello-origin > /dev/null
$FLATPAK info ${U} org.test.Hello | grep $ID > /dev/null

$FLATPAK remote-list ${U} -d | grep hello-origin > /dev/null
$FLATPAK remote-list ${U} -d | grep hello-origin | grep no-enumerate > /dev/null
assert_has_file $FL_DIR/repo/hello-origin.trustedkeys.gpg

ok "install app bundle"

if command -v update-desktop-database >/dev/null &&
   command -v update-mime-database >/dev/null &&
   command -v gtk-update-icon-cache >/dev/null &&
   test -f /usr/share/icons/hicolor/index.theme; then
    # Ensure triggers ran
    assert_has_file $FL_DIR/exports/share/applications/mimeinfo.cache
    assert_file_has_content $FL_DIR/exports/share/applications/mimeinfo.cache x-test/Hello
    assert_has_file $FL_DIR/exports/share/icons/hicolor/icon-theme.cache
    assert_has_file $FL_DIR/exports/share/icons/hicolor/index.theme

    ok "install app bundle triggers"
else
    ok "install app bundle triggers triggers # skip  Dependencies not available"
fi

${FLATPAK} uninstall -y --force-remove ${U} org.test.Platform >&2

assert_not_has_file $FL_DIR/repo/refs/remotes/platform-origin/runtime/org.test.Platform/$ARCH/master

${FLATPAK} install -y ${U} --bundle bundles/platform.flatpak >&2

assert_has_file $FL_DIR/repo/refs/remotes/platform-origin/runtime/org.test.Platform/$ARCH/master
RUNTIME_COMMIT=`cat $FL_DIR/repo/refs/remotes/platform-origin/runtime/org.test.Platform/$ARCH/master`
assert_has_file $FL_DIR/repo/objects/$(echo $RUNTIME_COMMIT | cut -b 1-2)/$(echo $RUNTIME_COMMIT | cut -b 3-).commit
assert_has_file $FL_DIR/repo/objects/$(echo $RUNTIME_COMMIT | cut -b 1-2)/$(echo $RUNTIME_COMMIT | cut -b 3-).commitmeta

assert_has_dir $FL_DIR/runtime/org.test.Platform
assert_has_dir $FL_DIR/runtime/org.test.Platform/$ARCH/master
assert_has_symlink $FL_DIR/runtime/org.test.Platform/$ARCH/master/active
ID=`readlink $FL_DIR/runtime/org.test.Platform/$ARCH/master/active`
assert_has_file $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/deploy
assert_has_file $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/metadata
assert_has_dir $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/files

$FLATPAK list ${U} --runtime | grep org.test.Platform > /dev/null
$FLATPAK list ${U} -d --runtime | grep org.test.Platform | grep platform-origin > /dev/null
$FLATPAK list ${U} -d --runtime | grep org.test.Platform | grep ${ID:0:12} > /dev/null

$FLATPAK info ${U} org.test.Platform > /dev/null
$FLATPAK info ${U} org.test.Platform | grep platform-origin > /dev/null
$FLATPAK info ${U} org.test.Platform | grep $ID > /dev/null

$FLATPAK remote-list ${U} -d | grep platform-origin > /dev/null
$FLATPAK remote-list ${U} -d | grep platform-origin | grep no-enumerate > /dev/null
assert_has_file $FL_DIR/repo/platform-origin.trustedkeys.gpg

ok "install runtime bundle"

run org.test.Hello &> hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

ok "run"


OLD_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

# TODO: For weird reasons this breaks in the system case. Needs debugging
if [ x${USE_SYSTEMDIR-} != xyes ] ; then
    ${FLATPAK} ${U} update -y -v org.test.Hello master >&2
    ALSO_OLD_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`
    assert_streq "$OLD_COMMIT" "$ALSO_OLD_COMMIT"
fi

ok "null update"

make_updated_app

${FLATPAK} ${U} update -y org.test.Hello >&2

NEW_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

assert_not_streq "$OLD_COMMIT" "$NEW_COMMIT"

run org.test.Hello &> hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATED$'

ok "update"

make_updated_app test org.test.Collection.test master UPDATED2

${FLATPAK} build-bundle repos/test --repo-url=file://`pwd`/repos/test --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg bundles/hello2.flatpak org.test.Hello >&2
assert_has_file bundles/hello2.flatpak

${FLATPAK} install ${U} -y --bundle bundles/hello2.flatpak >&2

NEW2_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

assert_not_streq "$NEW_COMMIT" "$NEW2_COMMIT"

run org.test.Hello &> hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATED2$'

ok "update as bundle"
