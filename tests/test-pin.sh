#!/bin/bash

set -euo pipefail

. "$(dirname "$0")/libtest.sh"

skip_revokefs_without_fuse

export LC_ALL=C
export FLATPAK_DISABLE_REVOKEFS=yes

echo "1..6"

setup_repo
install_repo
setup_sdk_repo
install_sdk_repo

${FLATPAK} ${U} pin --remove "runtime/org.test.Platform/$ARCH/master" || true
${FLATPAK} ${U} pin --remove "runtime/org.test.Sdk/$ARCH/master" || true
${FLATPAK} ${U} pin org.test.DoesNotExist org.test.Platform 'org.test.*//master'

socat EXEC:"env -u FLATPAK_FANCY_OUTPUT flatpak ${U} pin",pty,raw,echo=0 - > pin-output
assert_file_has_content pin-output '^Pinned patterns:$'
assert_file_has_content pin-output '^  org\.test\.DoesNotExist  0 matches$'
assert_file_has_content pin-output '^  org\.test\.Platform      1 match$'
assert_file_has_content pin-output '^  org\.test\.\*//master     3 matches$'

ok "interactive pin counts are independent"

${FLATPAK} ${U} pin > pin-output
(diff -u pin-output - || exit 1) <<EOF
  org.test.*//master
  org.test.DoesNotExist
  org.test.Platform
EOF

ok "plain pin output is unchanged"

${FLATPAK} ${U} pin --remove org.test.Platform
${FLATPAK} ${U} pin > pin-output
assert_not_file_has_content pin-output '^  org\.test\.Platform$'
assert_file_has_content pin-output '^  org\.test\.\*//master$'

ok "pin removal remains exact"

${FLATPAK} ${INVERT_U} pin org.test.OtherInstallation
${FLATPAK} ${U} pin > selected-output
${FLATPAK} ${INVERT_U} pin > other-output
assert_not_file_has_content selected-output 'org\.test\.OtherInstallation'
assert_file_has_content other-output 'org\.test\.OtherInstallation'

ok "pin listings are installation-specific"

mkdir -p ${TEST_DATA_DIR}/named-installation
mkdir -p ${FLATPAK_CONFIG_DIR}/installations.d
cat <<EOF > ${FLATPAK_CONFIG_DIR}/installations.d/pin-test.conf
[Installation "pin-test"]
Path=${TEST_DATA_DIR}/named-installation
EOF
port=$(cat httpd-port)
${FLATPAK} --installation=pin-test remote-add \
    --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg test-repo "http://127.0.0.1:${port}/test" >&2
${FLATPAK} --installation=pin-test install -y test-repo org.test.Platform >&2
${FLATPAK} --installation=pin-test pin --remove "runtime/org.test.Platform/$ARCH/master"
${FLATPAK} --installation=pin-test pin org.test.Platform
socat EXEC:"env -u FLATPAK_FANCY_OUTPUT flatpak --installation=pin-test pin",pty,raw,echo=0 - > named-output
assert_file_has_content named-output '^  org\.test\.Platform  1 match$'
${FLATPAK} complete "flatpak --installation=pin-test pin --remove " 46 "" > named-completion
assert_file_has_content named-completion '^org\.test\.Platform $'
assert_not_file_has_content named-completion 'org\.test\.OtherInstallation'

ok "named installation pin state is isolated"

${FLATPAK} ${U} pin --remove org.test.DoesNotExist 'org.test.*//master'
${FLATPAK} ${INVERT_U} pin --remove org.test.OtherInstallation
socat EXEC:"env -u FLATPAK_FANCY_OUTPUT flatpak ${U} pin",pty,raw,echo=0 - > fancy-empty
${FLATPAK} ${U} pin > plain-empty
assert_file_has_content fancy-empty '^No pinned patterns$'
assert_file_empty plain-empty

ok "empty pin output"
