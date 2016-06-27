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

# This test looks for specific localized strings.
export LC_ALL=C

echo "1..3"

${FLATPAK} --version > version_out

VERSION=`cat "$test_builddir/package_version.txt"`
assert_file_has_content version_out "^Flatpak $VERSION$"

echo "ok version"

${FLATPAK} --help > help_out

assert_file_has_content help_out "^Usage:$"

echo "ok help"

${FLATPAK} --default-arch > arch

${FLATPAK} --supported-arches > arches

assert_streq `head -1 arches` `cat arch`

echo "ok default arch"
