#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail
. "$(dirname "$0")/libtest.sh"

skip_without_bwrap
USE_SYSTEMDIR=yes skip_revokefs_without_fuse
setup_repo_no_add
make_updated_app test "" stable STABLE "" master

mkdir -p "$FLATPAK_CONFIG_DIR/installations.d" "$TEST_DATA_DIR/alt"
cat > "$FLATPAK_CONFIG_DIR/installations.d/alt.conf" <<EOF
[Installation "alt"]
Path=$TEST_DATA_DIR/alt
Priority=100
EOF

port=$(cat httpd-port)
for selector in --user --system --installation=alt; do
    $FLATPAK remote-add "$selector" --gpg-import="$FL_GPG_HOMEDIR/pubring.gpg" \
        test-repo "http://127.0.0.1:$port/test" >&2
    $FLATPAK install "$selector" -y --no-related test-repo \
        org.test.Hello//master org.test.Hello//stable >&2
    $FLATPAK make-current "$selector" org.test.Hello stable >&2
done
$FLATPAK make-current --user org.test.Hello master >&2

master_output='Hello world, from a sandbox'
stable_output='Hello world, from a sandboxSTABLE'
assert_streq "$(run org.test.Hello)" "$master_output"
assert_streq "$(run --user org.test.Hello)" "$master_output"
ok "default launch prefers the user's current branch"

for selector in --system --installation=alt; do
    assert_streq "$(run "$selector" org.test.Hello)" "$stable_output"
    assert_streq "$(run "$selector" --arch="$ARCH" org.test.Hello)" "$stable_output"
    assert_streq "$(run "$selector" --branch=master org.test.Hello)" "$master_output"
    assert_streq "$(run "$selector" --arch="$ARCH" --branch=master org.test.Hello)" "$master_output"
    ok "$selector uses its current branch unless a branch is specified"
done

# Remove the user app so the default lookup uses system installation priority.
# Give the system installations different current branches.
$FLATPAK uninstall --user -y org.test.Hello//master org.test.Hello//stable >&2
$FLATPAK make-current --system org.test.Hello master >&2
assert_streq "$(run org.test.Hello)" "$stable_output"
assert_streq "$(run --system org.test.Hello)" "$master_output"
assert_streq "$(run --installation=alt org.test.Hello)" "$stable_output"
assert_streq "$(run --system --installation=alt org.test.Hello)" "$stable_output"
assert_streq "$(run --user --system org.test.Hello)" "$master_output"
ok "app launches respect system installation priority and explicit selectors"

done_testing
