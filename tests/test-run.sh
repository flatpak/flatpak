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

echo "1..3"

setup_repo
install_repo

# Verify that app is correctly installed

assert_has_dir $USERDIR/app/org.test.Hello
assert_has_symlink $USERDIR/app/org.test.Hello/current
assert_symlink_has_content $USERDIR/app/org.test.Hello/current ^$ARCH/master$
assert_has_dir $USERDIR/app/org.test.Hello/$ARCH/master
assert_has_symlink $USERDIR/app/org.test.Hello/$ARCH/master/active
assert_has_file $USERDIR/app/org.test.Hello/$ARCH/master/active/deploy
assert_has_file $USERDIR/app/org.test.Hello/$ARCH/master/active/metadata
assert_has_dir $USERDIR/app/org.test.Hello/$ARCH/master/active/files
assert_has_dir $USERDIR/app/org.test.Hello/$ARCH/master/active/export
assert_has_file $USERDIR/exports/share/applications/org.test.Hello.desktop
# Ensure Exec key is rewritten
assert_file_has_content $USERDIR/exports/share/applications/org.test.Hello.desktop "^Exec=.*/xdg-app run --branch=master --arch=$ARCH --command=hello.sh org.test.Hello$"
assert_has_file $USERDIR/exports/share/icons/hicolor/64x64/apps/org.test.Hello.png

# Ensure triggers ran
assert_has_file $USERDIR/exports/share/applications/mimeinfo.cache
assert_file_has_content $USERDIR/exports/share/applications/mimeinfo.cache x-test/Hello
assert_has_file $USERDIR/exports/share/icons/hicolor/icon-theme.cache
assert_has_file $USERDIR/exports/share/icons/hicolor/index.theme

echo "ok install"

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

echo "ok hello"

run_sh cat /run/user/`id -u`/xdg-app-info > xai
assert_file_has_content xai '^name=org.test.Hello$'

echo "ok xdg-app-info"
