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

# shellcheck source=tests/libtest.sh
. "$(dirname "$0")/libtest.sh"

# This test looks for specific localized strings.
export LC_ALL=C

${FLATPAK} --version > version_out

VERSION=$(cat "$test_builddir/package_version.txt")
assert_file_has_content version_out "^Flatpak $VERSION$"

ok "version"

${FLATPAK} --help > help_out

assert_file_has_content help_out "^Usage:$"

ok "help"

${FLATPAK} --default-arch > arch

${FLATPAK} --supported-arches > arches

assert_streq "$(head -1 arches)" "$(cat arch)"

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
           mask preinstall;
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

mkdir -p "$HOME/.var/app/not-an-app/data"
mkdir -p "$HOME/.var/app/org.test.Orphan/data"

${FLATPAK} uninstall --delete-data -y

assert_has_dir "$HOME/.var/app/not-an-app"
assert_not_has_dir "$HOME/.var/app/org.test.Orphan"

ok "skip invalid orphan app data"

mkdir -p runtime/usr runtime/files
printf '[Runtime]\nname=org.test.Choices\n' > runtime/metadata
for branch in 9 11 10; do
    ${FLATPAK} build-export --disable-sandbox --runtime --no-update-summary repo runtime "$branch" >&2
done
printf '[Runtime]\nname=org.test.ChoicesExtra\n' > runtime/metadata
${FLATPAK} build-export --disable-sandbox --runtime --no-update-summary repo runtime 11 >&2
printf '[Runtime]\nname=org.test.Channels\n' > runtime/metadata
for branch in beta nightly stable; do
    ${FLATPAK} build-export --disable-sandbox --runtime --no-update-summary repo runtime "$branch" >&2
done
${FLATPAK} build-update-repo --no-update-appstream repo >&2
${FLATPAK} remote-add --user --no-gpg-verify test-choices "$(pwd)/repo"

printf '2\n' | socat -t 30 - EXEC:"${FLATPAK} install --user -y test-choices Choices",pty,raw,echo=0 > choices
assert_file_has_content choices " 1) runtime/org.test.Choices/$ARCH/11$"
assert_file_has_content choices " 2) runtime/org.test.Choices/$ARCH/10$"
assert_file_has_content choices " 3) runtime/org.test.Choices/$ARCH/9$"
assert_file_has_content choices " 4) runtime/org.test.ChoicesExtra/$ARCH/11$"
assert_streq "$(${FLATPAK} info --user --show-ref org.test.Choices)" "runtime/org.test.Choices/$ARCH/10"

ok "install numbers branches in descending numeric order"

printf '2\n' | socat -t 30 - EXEC:"${FLATPAK} install --user -y test-choices org.test.Channels",pty,raw,echo=0 > channels
assert_file_has_content channels " 1) runtime/org.test.Channels/$ARCH/stable$"
assert_file_has_content channels " 2) runtime/org.test.Channels/$ARCH/nightly$"
assert_file_has_content channels " 3) runtime/org.test.Channels/$ARCH/beta$"
assert_streq "$(${FLATPAK} info --user --show-ref org.test.Channels)" "runtime/org.test.Channels/$ARCH/nightly"

ok "install lists named branches in descending alphabetical order"

done_testing
