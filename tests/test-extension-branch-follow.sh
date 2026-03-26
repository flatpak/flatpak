#!/bin/bash

set -euo pipefail

# shellcheck source=tests/libtest.sh
# shellcheck disable=SC1091
. "$(dirname "$0")/libtest.sh"

skip_without_bwrap

echo "1..1"

setup_repo () {
    mkdir -p repos
    ostree init --repo=repos/test --mode=archive-z2
}

make_extension () {
    local ID=$1
    local VERSION=$2

    local DIR
    DIR=$(mktemp -d)

    cat > "${DIR}/metadata" <<EOF
[Runtime]
name=${ID}
EOF
    mkdir -p "${DIR}/usr"
    mkdir -p "${DIR}/files"
    touch "${DIR}/usr/extension-$ID:$VERSION"

    # shellcheck disable=SC2086
    ${FLATPAK} build-export --no-update-summary --runtime ${GPGARGS-} repos/test "${DIR}" "${VERSION}" >&2
    update_repo
    rm -rf "${DIR}"
}

setup_repo

"$(dirname "$0")/make-test-runtime.sh" repos/test org.test.Platform master "" "" bash ls cat echo readlink > /dev/null

# Create app version 1 with extension version 1.0
mkdir -p hello
cat > hello/metadata <<EOF
[Application]
name=org.test.Hello
runtime=org.test.Platform/$(uname -m)/master
sdk=org.test.Platform/$(uname -m)/master

[Extension org.test.Extension]
directory=files/ext
version=1.0
no-autodownload=true
EOF
mkdir -p hello/files/ext
${FLATPAK} build-finish --no-inherit-permissions hello >&2
${FLATPAK} build-export --no-update-summary --disable-sandbox repos/test hello master >&2
update_repo

# Create extension branch 1.0
make_extension org.test.Extension 1.0

# Install app and extension 1.0
${FLATPAK} remote-add --user --no-gpg-verify test-repo repos/test >&2
${FLATPAK} --user install -y test-repo org.test.Hello master >&2
${FLATPAK} --user install -y test-repo org.test.Extension 1.0 >&2

if ! ${FLATPAK} --user list --runtime | grep -q "org.test.Extension.*1.0"; then
    ${FLATPAK} --user list --runtime >&2
    assert_not_reached "Extension 1.0 not installed"
fi
echo "# Extension 1.0 installed successfully" >&2

# Create app version 2 with extension version 2.0
mkdir -p hello2
cat > hello2/metadata <<EOF
[Application]
name=org.test.Hello
runtime=org.test.Platform/$(uname -m)/master
sdk=org.test.Platform/$(uname -m)/master

[Extension org.test.Extension]
directory=files/ext
version=2.0
no-autodownload=true
EOF
mkdir -p hello2/files/ext
${FLATPAK} build-finish --no-inherit-permissions hello2 >&2
${FLATPAK} build-export --no-update-summary --disable-sandbox repos/test hello2 master >&2
update_repo

# Create extension branch 2.0
make_extension org.test.Extension 2.0

# Update the app
${FLATPAK} --user update -y --verbose org.test.Hello master >&2

# Check if extension 2.0 was installed
${FLATPAK} --user list --runtime >&2
if ${FLATPAK} --user list --runtime | grep -q "org.test.Extension.*2.0"; then
    echo "# Success: Extension 2.0 installed automatically after branch follow"
else
    assert_not_reached "Extension 2.0 NOT installed automatically"
fi

ok "extension branch follow"
