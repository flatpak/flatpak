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

echo "1..10"

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

${FLATPAK} --gl-drivers > drivers

assert_file_has_content drivers "^default$";
assert_file_has_content drivers "^host$";

echo "ok gl drivers"

for cmd in install update uninstall list info config repair create-usb \
           search run override make-current enter ps document-export \
           document-unexport document-info documents permission-remove \
           permissions permission-show permission-reset remotes remote-add \
           remote-modify remote-delete remote-ls remote-info build-init \
           build build-finish build-export build-bundle build-import-bundle \
           build-sign build-update-repo build-commit-from repo kill history \
           mask;
do
  ${FLATPAK} $cmd --help | head -2 > help_out

  assert_file_has_content help_out "^Usage:$"
  assert_file_has_content help_out "flatpak $cmd"
done

echo "ok command help"

for cmd in list ps remote-ls remotes documents history;
do
  ${FLATPAK} $cmd --columns=help > help_out

  assert_file_has_content help_out "^Available columns:$"
  assert_file_has_content help_out "^  all"
  assert_file_has_content help_out "^  help"
done

echo "ok columns help"

${FLATPAK} >out 2>&1 || true

assert_file_has_content out "^error: No command specified$"
assert_file_has_content out "flatpak --help"

echo "ok missing command"

${FLATPAK} indo >out 2>&1 || true

assert_file_has_content out "^error: .* 'info'"
assert_file_has_content out "flatpak --help"

echo "ok misspelt command"

${FLATPAK} info >out 2>&1 || true

assert_file_has_content out "^error: NAME must be specified$"
assert_file_has_content out "flatpak info --help"

echo "ok info missing NAME"

for cmd in config make-current override remote-add repair; do
  ${FLATPAK} $cmd --system --user >out 2>&1 || true
  assert_file_has_content out "^error: Multiple installations specified"
  ${FLATPAK} $cmd --system --installation=foo >out 2>&1 || true
  assert_file_has_content out "^error: Multiple installations specified"
  ${FLATPAK} $cmd --user --installation=foo >out 2>&1 || true
  assert_file_has_content out "^error: Multiple installations specified"
  ${FLATPAK} $cmd --installation=foo --installation=bar >out 2>&1 || true
  assert_file_has_content out "^error: Multiple installations specified"
done

echo "ok ONE_DIR commands"
