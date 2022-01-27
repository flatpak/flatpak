#!/bin/bash
#
# Copyright (C) 2019 Matthias Clasen
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

echo "1..5"

${FLATPAK} config --list > list_out
assert_file_has_content list_out "^languages:"

ok "config list"

${FLATPAK} config --set languages "de;fr"
${FLATPAK} config --get languages > get_out
assert_file_has_content get_out "^de;fr"

ok "config set"

${FLATPAK} config --set languages "*"
${FLATPAK} config --get languages > get_out
assert_file_has_content get_out "*"

ok "config languages *"

${FLATPAK} config --set languages "all"
${FLATPAK} config --get languages > get_out
assert_file_has_content get_out "all"

ok "config languages *"

${FLATPAK} config --unset languages
${FLATPAK} config --get languages > get_out
assert_file_has_content get_out "^[*]unset[*]"

ok "config unset"
