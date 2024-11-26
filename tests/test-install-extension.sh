#!/bin/bash
#
# Copyright (C) 2024 Eitan Isaacson <eitan@monotonous.org>
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

. "$(dirname $0)/libtest.sh"

skip_without_bwrap
skip_revokefs_without_fuse

echo "1..3"

setup_repo "" "" master

$FLATPAK list --app ${U} > app_list
assert_not_file_has_content app_list "Hello world"

ok "App is not installed"

${FLATPAK} ${U} install -y test-repo org.test.Hello.Plugin.fun v1 >&2

$FLATPAK list ${U} | grep org.test.Hello.Plugin.fun > /dev/null
$FLATPAK list --app ${U} > app_list
assert_not_file_has_content app_list "Hello world"

ok "fun extension does not pull in app"

${FLATPAK} ${U} install -y test-repo org.test.Hello.Plugin.joy >&2

$FLATPAK list --app ${U} > app_list
assert_file_has_content app_list "Hello world"

ok "Pulled in extension's app"
