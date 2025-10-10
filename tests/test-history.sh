#!/bin/bash
# Copyright (C) 2021 Matthew Leeds <mwleeds@protonmail.com>
# SPDX-License-Identifier: LGPL-2.0-or-later

set -euo pipefail

USE_SYSTEMDIR=yes

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_revokefs_without_fuse
skip_without_libsystemd

HISTORY_START_TIME=$(date +"%Y-%m-%d %H:%M:%S")
sleep 1

if ! logger "Checking whether Flatpak can use the journal..."; then
    skip "Cannot write to Journal with logger"
fi

messages="$(journalctl --user --since="${HISTORY_START_TIME}" || true)"

if [ -z "$messages" ]; then
    skip "Cannot read back from Journal with journalctl"
fi

echo "1..1"

mkdir -p ${TEST_DATA_DIR}/system-history-installation
mkdir -p ${FLATPAK_CONFIG_DIR}/installations.d
cat << EOF > ${FLATPAK_CONFIG_DIR}/installations.d/history-installation.conf
[Installation "history-installation"]
Path=${TEST_DATA_DIR}/system-history-installation
EOF

# setup repo and install from it
setup_repo_no_add
port=$(cat httpd-port)
${FLATPAK} --installation=history-installation remote-add \
    --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg test-repo "http://127.0.0.1:${port}/test" >&2
${FLATPAK} --installation=history-installation install -y test-repo org.test.Hello master >&2

# appstream update shouldn't show up in history
${FLATPAK} ${U} --appstream update test-repo >&2

# update, uninstall, and remote-delete should show up
EXPORT_ARGS="" make_updated_app
${FLATPAK} --installation=history-installation update -y org.test.Hello >&2
${FLATPAK} --installation=history-installation uninstall -y org.test.Platform org.test.Hello >&2
${FLATPAK} --installation=history-installation remote-delete test-repo >&2

# need --since and --columns here to make the test idempotent
if ! ${FLATPAK} --installation=history-installation history --since="${HISTORY_START_TIME}" \
    --columns=change,application,branch,installation,remote > history-log 2>&1; then
    cat history-log >&2
    echo "Bail out! 'flatpak history' failed"
    exit 1
fi

diff history-log - >&2 << EOF
add remote			system (history-installation)	test-repo
deploy install	org.test.Hello.Locale	master	system (history-installation)	test-repo
deploy install	org.test.Platform	master	system (history-installation)	test-repo
deploy install	org.test.Hello	master	system (history-installation)	test-repo
deploy update	org.test.Hello.Locale	master	system (history-installation)	test-repo
deploy update	org.test.Hello	master	system (history-installation)	test-repo
uninstall	org.test.Hello	master	system (history-installation)
uninstall	org.test.Platform	master	system (history-installation)
uninstall	org.test.Hello.Locale	master	system (history-installation)
remove remote			system (history-installation)	test-repo
EOF

if ! ${FLATPAK} --installation=history-installation history --since="${HISTORY_START_TIME}" \
    --columns=change,application,branch,installation,remote --json > history-log 2>&1; then
    cat history-log >&2
    echo "Bail out! 'flatpak history' failed"
    exit 1
fi

diff history-log - >&2 << EOF
[
  {
    "Change" : "add remote",
    "Application" : "",
    "Branch" : "",
    "Installation" : "system (history-installation)",
    "Remote" : "test-repo"
  },
  {
    "Change" : "deploy install",
    "Application" : "org.test.Hello.Locale",
    "Branch" : "master",
    "Installation" : "system (history-installation)",
    "Remote" : "test-repo"
  },
  {
    "Change" : "deploy install",
    "Application" : "org.test.Platform",
    "Branch" : "master",
    "Installation" : "system (history-installation)",
    "Remote" : "test-repo"
  },
  {
    "Change" : "deploy install",
    "Application" : "org.test.Hello",
    "Branch" : "master",
    "Installation" : "system (history-installation)",
    "Remote" : "test-repo"
  },
  {
    "Change" : "deploy update",
    "Application" : "org.test.Hello.Locale",
    "Branch" : "master",
    "Installation" : "system (history-installation)",
    "Remote" : "test-repo"
  },
  {
    "Change" : "deploy update",
    "Application" : "org.test.Hello",
    "Branch" : "master",
    "Installation" : "system (history-installation)",
    "Remote" : "test-repo"
  },
  {
    "Change" : "uninstall",
    "Application" : "org.test.Hello",
    "Branch" : "master",
    "Installation" : "system (history-installation)",
    "Remote" : ""
  },
  {
    "Change" : "uninstall",
    "Application" : "org.test.Platform",
    "Branch" : "master",
    "Installation" : "system (history-installation)",
    "Remote" : ""
  },
  {
    "Change" : "uninstall",
    "Application" : "org.test.Hello.Locale",
    "Branch" : "master",
    "Installation" : "system (history-installation)",
    "Remote" : ""
  },
  {
    "Change" : "remove remote",
    "Application" : "",
    "Branch" : "",
    "Installation" : "system (history-installation)",
    "Remote" : "test-repo"
  }
]
EOF

rm -f ${FLATPAK_CONFIG_DIR}/installations.d/history-inst.conf
rm -rf ${TEST_DATA_DIR}/system-history-installation

ok "history looks correct"
