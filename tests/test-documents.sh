#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

. "$(dirname "$0")/libtest.sh"

setup_document_portal

mkdir -p "$TEST_DATA_DIR/documents/exported directory"
existing="$TEST_DATA_DIR/documents/existing file.txt"
missing="$TEST_DATA_DIR/documents/not created yet.txt"
directory="$TEST_DATA_DIR/documents/exported directory"
printf 'existing content\n' > "$existing"
printf 'directory content\n' > "$directory/child.txt"

# Exercise Add and AddFull as controls for the AddNamed reply parser below.
document=$($FLATPAK document-export --app=org.test.Hello "$existing")
assert_streq "$(cat "$document")" "existing content"
ok "export an existing file"

document=$($FLATPAK document-export --app=org.test.Hello "$directory")
assert_streq "$(cat "$document/child.txt")" "directory content"
ok "export an existing directory"

assert_fail $FLATPAK document-export --app=org.test.Hello "$missing" > missing.out 2> missing.err
assert_not_has_file "$missing"
ok "ordinary export rejects a missing file"

# --noexist passes an fd for the parent directory, but calls AddNamed, whose
# reply contains one document ID rather than AddFull's array of document IDs.
document=$($FLATPAK document-export --noexist --app=org.test.Hello "$missing")
assert_streq "$(dirname "$(dirname "$document")")" "$XDG_RUNTIME_DIR/doc"
assert_streq "$(basename "$document")" "not created yet.txt"
assert_not_has_file "$missing"
printf 'created after export\n' > "$missing"
assert_streq "$(cat "$document")" "created after export"
$FLATPAK document-info "$missing" > document-info.out
assert_file_has_content document-info.out 'org\.test\.Hello.*read'
ok "export a nonexistent file and read it after creation"

done_testing
