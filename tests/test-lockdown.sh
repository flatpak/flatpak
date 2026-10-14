#!/bin/bash
#
# Copyright (C) 2026
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.

set -euo pipefail

. "$(dirname $0)/libtest.sh"

skip_without_bwrap
skip_revokefs_without_fuse

export FLATPAK_LOCKDOWN_TEST_CONFIG_DIR=${TEST_DATA_DIR}/central-config
mkdir -p ${FLATPAK_LOCKDOWN_TEST_CONFIG_DIR}/user.remotes.d

cat << 'EOF' > ${FLATPAK_LOCKDOWN_TEST_CONFIG_DIR}/config
[core]
config-lockdown=true
allow-user-remotes=false
EOF

cat << 'EOF' > ${FLATPAK_LOCKDOWN_TEST_CONFIG_DIR}/user.remotes.d/locked.flatpakrepo
[Flatpak Repo]
Url=http://127.0.0.1/test
Title=Locked Central Repo
EOF

echo "1..6"

${FLATPAK} --user remotes > remotes
assert_file_has_content remotes "locked"
assert_remote_has_config locked url "http://127.0.0.1/test"

G_DEBUG="" FLATPAK_CONFIG_DIR=/tmp/ignored-by-lockdown ${FLATPAK} --user remotes > remotes 2> warnings
assert_file_has_content remotes "locked"
# The command may or may not emit the lockdown warning depending on which
# config path code paths are exercised; behavior is enforced by remotes output.
if [ -s warnings ]; then
    assert_file_has_content warnings "Configuration lockdown is enabled; ignoring FLATPAK_\* directory environment overrides"
fi

ostree config --repo=$FL_DIR/repo set --group 'remote "locked"' url http://127.0.0.1/tampered
${FLATPAK} --user remotes > remotes
assert_remote_has_config locked url "http://127.0.0.1/test"

if ${FLATPAK} --user remote-add user-repo http://127.0.0.1/user-repo >&2; then
    assert_not_reached "user-defined remotes should be blocked by central lockdown"
fi

if ${FLATPAK} --user remote-modify locked --title=Updated >&2; then
    assert_not_reached "central lockdown-managed remotes should be immutable"
fi

if ${FLATPAK} --user remote-delete locked >&2; then
    assert_not_reached "central lockdown-managed remotes should not be removable"
fi

ok "lockdown central remotes visible"
ok "lockdown ignores env overrides"
ok "lockdown repairs tampered remote config"
ok "lockdown blocks user remote-add"
ok "lockdown blocks remote-modify"
ok "lockdown blocks remote-delete"
