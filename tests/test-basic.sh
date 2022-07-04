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

echo "1..11"

${FLATPAK} --version > version_out

VERSION=`cat "$test_builddir/package_version.txt"`
assert_file_has_content version_out "^Flatpak $VERSION$"

ok "version"

${FLATPAK} --help > help_out

assert_file_has_content help_out "^Usage:$"

ok "help"

${FLATPAK} --default-arch > arch

${FLATPAK} --supported-arches > arches

assert_streq `head -1 arches` `cat arch`

ok "default arch"

${FLATPAK} --print-updated-env > updated_env
${FLATPAK} --print-updated-env --print-system-only > updated_env_system

assert_file_has_content updated_env "exports/share"
assert_file_has_content updated_env "^XDG_DATA_DIRS="
assert_file_has_content updated_env_system "exports/share"
assert_file_has_content updated_env_system "^XDG_DATA_DIRS="

ok "print updated env"

${FLATPAK} --gl-drivers > drivers

assert_file_has_content drivers "^default$";
assert_file_has_content drivers "^host$";

ok "gl drivers"

for cmd in install update uninstall list info config repair create-usb \
           search run override make-current enter ps document-export \
           document-unexport document-info documents permission-remove \
           permissions permission-show permission-reset remotes remote-add \
           remote-modify remote-delete remote-ls remote-info build-init \
           build build-finish build-export build-bundle build-import-bundle \
           build-sign build-update-repo build-commit-from repo kill history \
           mask alias;
do
  ${FLATPAK} $cmd --help > help_out
  head -2 help_out > help_out2

  assert_file_has_content help_out2 "^Usage:$"
  assert_file_has_content help_out2 "flatpak $cmd"
done

ok "command help"

for cmd in list ps remote-ls remotes documents history;
do
  ${FLATPAK} $cmd --columns=help > help_out

  assert_file_has_content help_out "^Available columns:$"
  assert_file_has_content help_out "^  all"
  assert_file_has_content help_out "^  help"
done

ok "columns help"

${FLATPAK} >out 2>&1 || true

assert_file_has_content out "^error: No command specified$"
assert_file_has_content out "flatpak --help"

ok "missing command"

${FLATPAK} indo >out 2>&1 || true

assert_file_has_content out "^error: .* 'info'"
assert_file_has_content out "flatpak --help"

ok "misspelt command"

${FLATPAK} info >out 2>&1 || true

assert_file_has_content out "^error: NAME must be specified$"
assert_file_has_content out "flatpak info --help"

ok "info missing NAME"

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

ok "ONE_DIR commands"
