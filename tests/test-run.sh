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

echo "1..5"

setup_repo
install_repo

# Verify that app is correctly installed

assert_has_dir $USERDIR/app/org.test.Hello
assert_has_symlink $USERDIR/app/org.test.Hello/current
assert_symlink_has_content $USERDIR/app/org.test.Hello/current ^$ARCH/master$
assert_has_dir $USERDIR/app/org.test.Hello/$ARCH/master
assert_has_symlink $USERDIR/app/org.test.Hello/$ARCH/master/active
ID=`readlink $USERDIR/app/org.test.Hello/$ARCH/master/active`
assert_has_file $USERDIR/app/org.test.Hello/$ARCH/master/active/deploy
assert_has_file $USERDIR/app/org.test.Hello/$ARCH/master/active/metadata
assert_has_dir $USERDIR/app/org.test.Hello/$ARCH/master/active/files
assert_has_dir $USERDIR/app/org.test.Hello/$ARCH/master/active/export
assert_has_file $USERDIR/exports/share/applications/org.test.Hello.desktop
# Ensure Exec key is rewritten
assert_file_has_content $USERDIR/exports/share/applications/org.test.Hello.desktop "^Exec=.*/flatpak run --branch=master --arch=$ARCH --command=hello.sh org.test.Hello$"
assert_has_file $USERDIR/exports/share/icons/hicolor/64x64/apps/org.test.Hello.png

# Ensure triggers ran
assert_has_file $USERDIR/exports/share/applications/mimeinfo.cache
assert_file_has_content $USERDIR/exports/share/applications/mimeinfo.cache x-test/Hello
assert_has_file $USERDIR/exports/share/icons/hicolor/icon-theme.cache
assert_has_file $USERDIR/exports/share/icons/hicolor/index.theme

$FLATPAK list --user | grep org.test.Hello > /dev/null
$FLATPAK list --user -d | grep org.test.Hello | grep test-repo > /dev/null
$FLATPAK list --user -d | grep org.test.Hello | grep current > /dev/null
$FLATPAK list --user -d | grep org.test.Hello | grep ${ID:0:12} > /dev/null

$FLATPAK info --user org.test.Hello > /dev/null
$FLATPAK info --user org.test.Hello | grep test-repo > /dev/null
$FLATPAK info --user org.test.Hello | grep $ID > /dev/null

echo "ok install"

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

echo "ok hello"

run_sh cat /run/user/`id -u`/flatpak-info > fpi
assert_file_has_content fpi '^name=org.test.Hello$'

echo "ok flatpak-info"

run_sh readlink /proc/self/ns/net > unshared_net_ns
ARGS="--share=network" run_sh readlink /proc/self/ns/net > shared_net_ns
assert_not_streq `cat unshared_net_ns` `readlink /proc/self/ns/net`
assert_streq `cat shared_net_ns` `readlink /proc/self/ns/net`

run_sh readlink /proc/self/ns/ipc > unshared_ipc_ns
ARGS="--share=ipc" run_sh readlink /proc/self/ns/ipc > shared_ipc_ns
assert_not_streq `cat unshared_ipc_ns` `readlink /proc/self/ns/ipc`
assert_streq `cat shared_ipc_ns` `readlink /proc/self/ns/ipc`

if run_sh cat $(dirname $0)/package_version.txt &> /dev/null; then
    assert_not_reached "Unexpectedly allowed to access file"
fi

ARGS="--filesystem=$(dirname $0)" run_sh cat $(dirname $0)/package_version.txt > /dev/null
ARGS="--filesystem=host" run_sh cat $(dirname $0)/package_version.txt > /dev/null

echo "ok namespaces"

$FLATPAK override --user --filesystem=host org.test.Hello
run_sh cat $(dirname $0)/package_version.txt &> /dev/null
if ARGS="--nofilesystem=host" run_sh cat $(dirname $0)/package_version.txt &> /dev/null; then
    assert_not_reached "Unexpectedly allowed to access --nofilesystem=host file"
fi
$FLATPAK override --user --nofilesystem=host org.test.Hello

if run_sh cat $(dirname $0)/package_version.txt &> /dev/null; then
    assert_not_reached "Unexpectedly allowed to access file"
fi

echo "ok overrides"
