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

#FLATPAK=flatpak
. $(dirname $0)/libtest.sh

skip_revokefs_without_fuse

# This test looks for specific localized strings.
export LC_ALL=C

echo "1..17"

setup_repo
install_repo

${FLATPAK} complete "flatpak a" 9 "a" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
EOF

ok "complete a commands"

${FLATPAK} complete "flatpak b" 9 "b" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
build 
build-bundle 
build-commit-from 
build-export 
build-finish 
build-import-bundle 
build-init 
build-sign 
build-update-repo 
EOF

ok "complete b commands"

${FLATPAK} complete "flatpak i" 9 "i" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
info 
install 
EOF

ok "complete i commands"

${FLATPAK} complete "flatpak --" 10 "--" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
--default-arch 
--gl-drivers 
--help 
--installation=
--installations 
--ostree-verbose 
--print-system-only 
--print-updated-env 
--supported-arches 
--system 
--user 
--verbose 
--version 
EOF

ok "complete global options"

${FLATPAK} complete "flatpak list --" 15 "--" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
--all 
--app 
--app-runtime=
--arch=
--columns=
--help 
--installation=
--json 
--ostree-verbose 
--runtime 
--show-details 
--system 
--user 
--verbose 
EOF

ok "complete list options"

${FLATPAK} complete "flatpak create-usb /" 20 "/" | sort > complete_out
#(diff -u complete_out - || echo "fail") <<EOF
#__FLATPAK_DIR
#EOF

ok "complete create-usb"

${FLATPAK} complete "flatpak list --arch=" 20 "--arch=" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
aarch64 
arm 
i386 
x86_64 
EOF

ok "complete --arch"

${FLATPAK} complete "flatpak override --allow=" 25 "--allow=" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
bluetooth 
canbus 
devel 
multiarch 
per-app-dev-shm 
EOF

ok "complete --allow"

${FLATPAK} complete "flatpak config --set l" 23 "l" > complete_out
(diff -u complete_out - || exit 1) <<EOF
languages
EOF

ok "complete config"

${FLATPAK} complete "flatpak info o" 16 "o" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
org.test.Hello 
org.test.Hello.Locale 
org.test.Platform 
EOF

ok "complete ref"

${FLATPAK} complete "flatpak permission-reset o" 26 "o" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
org.test.Hello
EOF

ok "complete partial ref"

for cmd in build-bundle build-commit-from build-export build-finish \
           build-import-bundle build-init build-sign build-update-repo \
           build document-export document-info document-list document-unexport \
           enter kill permission-list permission-remove permission-reset \
           permission-show ps repo; do
  len=$(awk '{ print length($0) }' <<< "flatpak $cmd --")
  ${FLATPAK} complete "flatpak $cmd --" $len "--"  > complete_out
  assert_not_file_has_content complete_out "^--system "
  assert_not_file_has_content complete_out "^--user "
  assert_not_file_has_content complete_out "^--installation="
done

ok "complete NO_DIR commands"

for cmd in history info list run update mask \
           config install make-current override remote-add repair \
           create-usb remote-delete remote-info remote-list remote-ls \
           remote-modify search uninstall update; do
  len=$(awk '{ print length($0) }' <<< "flatpak $cmd --")
  ${FLATPAK} complete "flatpak $cmd --" $len "--"  > complete_out
  assert_file_has_content complete_out "^--system "
  assert_file_has_content complete_out "^--user "
  assert_file_has_content complete_out "^--installation="
done

ok "complete non-NO_DIR commands"

${FLATPAK} complete "flatpak list --columns=" 24 "--columns=" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
active
all
application
arch
branch
description
help
installation
latest
name
options
origin
ref
runtime
size
version
EOF

ok "complete list --columns="

${FLATPAK} complete "flatpak list --columns=all" 27 "--columns=all" | sort > complete_out
assert_file_empty complete-out

ok "complete list --columns=all"

${FLATPAK} complete "flatpak list --columns=hel" 27 "--columns=hel" | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
help
EOF

ok "complete list --columns=hel"

${FLATPAK} complete "flatpak list --columns=arch," 29 "--columns=arch," | sort > complete_out
(diff -u complete_out - || exit 1) <<EOF
arch,active
arch,application
arch,branch
arch,description
arch,installation
arch,latest
arch,name
arch,options
arch,origin
arch,ref
arch,runtime
arch,size
arch,version
EOF

ok "complete list --columns=arch,"

