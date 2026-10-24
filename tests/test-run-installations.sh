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

# Install different commits of the same runtime ref in each installation.
system_commit=$($FLATPAK info --system --show-commit org.test.Platform//master)
make_updated_runtime test "" master USER
$FLATPAK update --user -y org.test.Platform//master >&2
user_commit=$($FLATPAK info --user --show-commit org.test.Platform//master)
make_updated_runtime test "" master ALT
$FLATPAK install --installation=alt -y test-repo org.test.Platform//master >&2
alt_commit=$($FLATPAK info --installation=alt --show-commit org.test.Platform//master)

runtime_output='Hello world, from a runtime'
assert_streq "$(run --system --command=runtime_hello.sh org.test.Platform//master)" "$runtime_output"
assert_streq "$(run --user --command=runtime_hello.sh org.test.Platform//master)" "${runtime_output}USER"
assert_streq "$(run --system --commit="$system_commit" --command=runtime_hello.sh org.test.Platform//master)" "$runtime_output"
assert_streq "$(run --user --commit="$user_commit" --command=runtime_hello.sh org.test.Platform//master)" "${runtime_output}USER"
assert_streq "$(run --installation=alt --command=runtime_hello.sh org.test.Platform//master)" "${runtime_output}ALT"
assert_streq "$(run --installation=alt --commit="$alt_commit" --command=runtime_hello.sh org.test.Platform//master)" "${runtime_output}ALT"
assert_streq "$(run --system --runtime-commit="$system_commit" --command=runtime_hello.sh org.test.Platform//master)" "$runtime_output"
ok "runtime launches use the selected installation and commit"

# An application's runtime still uses the normal global lookup.
assert_streq "$(run --system --command=runtime_hello.sh org.test.Hello)" "${runtime_output}USER"
ok "application launches can use a runtime from another installation"

if run --system --commit="$user_commit" --command=runtime_hello.sh org.test.Platform//master >run-output 2>run-error; then
    assert_not_reached "runtime launch accepted a commit from another installation"
fi
ok "runtime launches reject commits from other installations"

done_testing
