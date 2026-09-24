#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

. "$(dirname "$0")/libtest.sh"

skip_without_bwrap
setup_document_portal
setup_repo
install_repo

# Exercise different host and sandbox runtime paths.
assert_not_streq "$XDG_RUNTIME_DIR" "/run/user/$(id -u)"
mkdir -p "$TEST_DATA_DIR/forwarded/directory with spaces"
file="$TEST_DATA_DIR/forwarded/file with spaces.txt"
directory="$TEST_DATA_DIR/forwarded/directory with spaces"
printf 'original file content\n' > "$file"
printf 'directory child content\n' > "$directory/child.txt"

assert_fail run --command=cat org.test.Hello "$file" > direct.out 2> direct.err
ok "the original host file is not directly accessible"

run --file-forwarding --command=sh org.test.Hello -c '
    cat "$1" && printf "written in sandbox\n" > "$1"
' sh @@ "$file" @@ > forwarded.out
assert_file_has_content forwarded.out '^original file content$'
assert_file_has_content "$file" '^written in sandbox$'
ok "forwarded filename with spaces can be read and written"

# Only spaces need URI escaping in this fixture.
uri="file://${file// /%20}"
run --file-forwarding --command=bash org.test.Hello -c '
    case "$1" in file://*) path=${1#file://} ;; *) exit 1 ;; esac
    path=${path//%20/ }
    cat "$path"
' bash @@u "$uri" @@ > forwarded-uri.out
assert_file_has_content forwarded-uri.out '^written in sandbox$'
ok "forwarded file URI with escaped spaces is readable"

if (( DOCUMENT_PORTAL_VERSION >= 4 )); then
    run --file-forwarding --command=sh org.test.Hello -c 'cat "$1/child.txt"' \
        sh @@ "$directory" @@ > forwarded-directory.out
    assert_file_has_content forwarded-directory.out '^directory child content$'
    ok "forwarded directory exposes its child"
else
    ok "forwarded directory exposes its child # SKIP document portal interface older than 4"
fi

run --file-forwarding --command=sh org.test.Hello -c 'printf "%s\n" "$1"' \
    sh @@u https://example.com/document @@ > remote-uri.out
assert_file_has_content remote-uri.out '^https://example.com/document$'
ok "remote URI is passed through unchanged"

run --file-forwarding --filesystem="$TEST_DATA_DIR/forwarded:ro" \
    --command=sh org.test.Hello -c '
        test "$1" = "$2" && cat "$1"
    ' sh @@ "$file" @@ "$file" > visible.out
assert_file_has_content visible.out '^written in sandbox$'
ok "already visible file keeps its original path"

done_testing
