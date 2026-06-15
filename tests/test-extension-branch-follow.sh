#!/bin/bash

set -euo pipefail

# shellcheck source=tests/libtest.sh
# shellcheck disable=SC1091
. "$(dirname "$0")/libtest.sh"

skip_without_bwrap

echo "1..4"

setup_repo () {
    mkdir -p repos
    ostree init --repo=repos/test --mode=archive-z2
}

make_extension () {
    local ID=$1
    local VERSION=$2
    local CONTENT=${3:-$ID:$VERSION}

    local DIR
    DIR=$(mktemp -d)

    cat > "${DIR}/metadata" <<EOF
[Runtime]
name=${ID}
EOF
    mkdir -p "${DIR}/usr"
    mkdir -p "${DIR}/files"
    printf "%s\n" "${CONTENT}" > "${DIR}/usr/extension-$ID:$VERSION"

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

# Create app version 1 with a versions list containing the installed branch.
mkdir -p versions-follow
cat > versions-follow/metadata <<EOF
[Application]
name=org.test.VersionsFollow
runtime=org.test.Platform/$(uname -m)/master
sdk=org.test.Platform/$(uname -m)/master

[Extension org.test.VersionsFollow.Extension]
directory=files/ext
versions=1.0;
no-autodownload=true
EOF
mkdir -p versions-follow/files/ext
${FLATPAK} build-finish --no-inherit-permissions versions-follow >&2
${FLATPAK} build-export --no-update-summary --disable-sandbox repos/test versions-follow master >&2
update_repo

make_extension org.test.VersionsFollow.Extension 1.0

${FLATPAK} ${U} install -y test-repo org.test.VersionsFollow master >&2
${FLATPAK} ${U} install -y test-repo org.test.VersionsFollow.Extension 1.0 >&2

# Create app version 2 with an added extension branch in versions=.
mkdir -p versions-follow2
cat > versions-follow2/metadata <<EOF
[Application]
name=org.test.VersionsFollow
runtime=org.test.Platform/$(uname -m)/master
sdk=org.test.Platform/$(uname -m)/master

[Extension org.test.VersionsFollow.Extension]
directory=files/ext
versions=1.0;2.0;
no-autodownload=true
EOF
mkdir -p versions-follow2/files/ext
touch versions-follow2/files/update-marker
${FLATPAK} build-finish --no-inherit-permissions versions-follow2 >&2
${FLATPAK} build-export --no-update-summary --disable-sandbox repos/test versions-follow2 master >&2
update_repo

make_extension org.test.VersionsFollow.Extension 2.0

${FLATPAK} ${U} update -y --verbose org.test.VersionsFollow master >&2
${FLATPAK} ${U} list --runtime >&2

if ! ${FLATPAK} ${U} list --runtime | grep -q "org.test.VersionsFollow.Extension.*2.0"; then
    assert_not_reached "Extension 2.0 NOT installed automatically from versions list"
fi

ok "extension branch follow from versions list"

# Create app version 1 with multiple allowed extension versions
mkdir -p multi-version
cat > multi-version/metadata <<EOF
[Application]
name=org.test.MultiVersion
runtime=org.test.Platform/$(uname -m)/master
sdk=org.test.Platform/$(uname -m)/master

[Extension org.test.MultiVersion.Extension]
directory=files/ext
versions=1.0;2.0;
no-autodownload=true
EOF
mkdir -p multi-version/files/ext
${FLATPAK} build-finish --no-inherit-permissions multi-version >&2
${FLATPAK} build-export --no-update-summary --disable-sandbox repos/test multi-version master >&2
update_repo

# Create both extension branches, but only install the newer branch.
make_extension org.test.MultiVersion.Extension 1.0
make_extension org.test.MultiVersion.Extension 2.0

${FLATPAK} ${U} install -y test-repo org.test.MultiVersion master >&2
${FLATPAK} ${U} install -y test-repo org.test.MultiVersion.Extension 2.0 >&2

if ${FLATPAK} ${U} list --runtime | grep -q "org.test.MultiVersion.Extension.*1.0"; then
    ${FLATPAK} ${U} list --runtime >&2
    assert_not_reached "Extension 1.0 installed before update"
fi

# Create app version 2 with the same extension metadata, so updating the app
# checks related refs while branch 2.0 is already installed.
mkdir -p multi-version2
cat > multi-version2/metadata <<EOF
[Application]
name=org.test.MultiVersion
runtime=org.test.Platform/$(uname -m)/master
sdk=org.test.Platform/$(uname -m)/master

[Extension org.test.MultiVersion.Extension]
directory=files/ext
versions=1.0;2.0;
no-autodownload=true
EOF
mkdir -p multi-version2/files/ext
touch multi-version2/files/update-marker
${FLATPAK} build-finish --no-inherit-permissions multi-version2 >&2
${FLATPAK} build-export --no-update-summary --disable-sandbox repos/test multi-version2 master >&2
update_repo

${FLATPAK} ${U} update -y --verbose org.test.MultiVersion master >&2
${FLATPAK} ${U} list --runtime >&2

if ${FLATPAK} ${U} list --runtime | grep -q "org.test.MultiVersion.Extension.*1.0"; then
    assert_not_reached "Extension 1.0 should not be installed when only 2.0 was selected"
fi

ok "extension branch follow does not install unselected branch"

# Updating an already-installed selected branch should not install another
# branch from the same versions list.
mkdir -p selected-branch-update
cat > selected-branch-update/metadata <<EOF
[Application]
name=org.test.SelectedBranchUpdate
runtime=org.test.Platform/$(uname -m)/master
sdk=org.test.Platform/$(uname -m)/master

[Extension org.test.SelectedBranchUpdate.Extension]
directory=files/ext
versions=1.0;2.0;
no-autodownload=true
EOF
mkdir -p selected-branch-update/files/ext
${FLATPAK} build-finish --no-inherit-permissions selected-branch-update >&2
${FLATPAK} build-export --no-update-summary --disable-sandbox repos/test selected-branch-update master >&2
update_repo

make_extension org.test.SelectedBranchUpdate.Extension 1.0
make_extension org.test.SelectedBranchUpdate.Extension 2.0 old

${FLATPAK} ${U} install -y test-repo org.test.SelectedBranchUpdate master >&2
${FLATPAK} ${U} install -y test-repo org.test.SelectedBranchUpdate.Extension 2.0 >&2

old_commit=$(${FLATPAK} ${U} info --show-commit runtime/org.test.SelectedBranchUpdate.Extension/${ARCH}/2.0)

make_extension org.test.SelectedBranchUpdate.Extension 2.0 new

mkdir -p selected-branch-update2
cat > selected-branch-update2/metadata <<EOF
[Application]
name=org.test.SelectedBranchUpdate
runtime=org.test.Platform/$(uname -m)/master
sdk=org.test.Platform/$(uname -m)/master

[Extension org.test.SelectedBranchUpdate.Extension]
directory=files/ext
versions=1.0;2.0;
no-autodownload=true
EOF
mkdir -p selected-branch-update2/files/ext
touch selected-branch-update2/files/update-marker
${FLATPAK} build-finish --no-inherit-permissions selected-branch-update2 >&2
${FLATPAK} build-export --no-update-summary --disable-sandbox repos/test selected-branch-update2 master >&2
update_repo

${FLATPAK} ${U} update -y --verbose org.test.SelectedBranchUpdate master >&2
${FLATPAK} ${U} list --runtime >&2

new_commit=$(${FLATPAK} ${U} info --show-commit runtime/org.test.SelectedBranchUpdate.Extension/${ARCH}/2.0)
assert_not_streq "${old_commit}" "${new_commit}"

if ${FLATPAK} ${U} list --runtime | grep -q "org.test.SelectedBranchUpdate.Extension.*1.0"; then
    assert_not_reached "Extension 1.0 should not be installed while updating selected 2.0 branch"
fi

ok "extension branch follow updates selected branch only"
