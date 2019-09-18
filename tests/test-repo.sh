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

. $(dirname $0)/libtest.sh

skip_without_bwrap

echo "1..23"

#Regular repo
setup_repo

# Ensure we have appdata
if ! ostree show --repo=repos/test appstream/${ARCH} > /dev/null; then
    assert_not_reached "No appstream branch"
fi
if ! ostree show --repo=repos/test appstream2/${ARCH} > /dev/null; then
    assert_not_reached "No appstream2 branch"
fi
ostree cat --repo=repos/test appstream/${ARCH} /appstream.xml.gz | gunzip -d > appdata.xml
assert_file_has_content appdata.xml "<id>org.test.Hello.desktop</id>"

ostree cat --repo=repos/test appstream2/${ARCH} /appstream.xml > appdata2.xml
assert_file_has_content appdata2.xml "<id>org.test.Hello.desktop</id>"

# Unsigned repo (not supported with collections; client-side use of collections requires GPG)
if [ x${USE_COLLECTIONS_IN_CLIENT-} == xyes ] ; then
    if GPGPUBKEY=" " GPGARGS=" " setup_repo test-no-gpg org.test.Collection.NoGpg; then
        assert_not_reached "Should fail remote-add due to missing GPG key"
    fi
elif [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
    # Set a collection ID and GPG on the server, but not in the client configuration
    setup_repo_no_add test-no-gpg org.test.Collection.NoGpg
    port=$(cat httpd-port-main)
    flatpak remote-add ${U} --no-gpg-verify test-no-gpg-repo "http://127.0.0.1:${port}/test-no-gpg"
else
    GPGPUBKEY="" GPGARGS="" setup_repo test-no-gpg
fi

flatpak remote-add ${U} --no-gpg-verify local-test-no-gpg-repo `pwd`/repos/test-no-gpg

#alternative gpg key repo
GPGPUBKEY="${FL_GPG_HOMEDIR2}/pubring.gpg" GPGARGS="${FL_GPGARGS2}" setup_repo test-gpg2 org.test.Collection.Gpg2

#remote with missing GPG key
# Don’t use --collection-id= here, or the collections code will grab the appropriate
# GPG key from one of the previously-configured remotes with the same collection ID.
port=$(cat httpd-port-main)
if flatpak remote-add ${U} test-missing-gpg-repo "http://127.0.0.1:${port}/test"; then
    assert_not_reached "Should fail metadata-update due to missing gpg key"
fi

#remote with wrong GPG key
port=$(cat httpd-port-main)
if flatpak remote-add ${U} --gpg-import=${FL_GPG_HOMEDIR2}/pubring.gpg test-wrong-gpg-repo "http://127.0.0.1:${port}/test"; then
    assert_not_reached "Should fail metadata-update due to wrong gpg key"
fi

# Remove new appstream branch so we can test deploying the old one
rm -rf repos/test/refs/heads/appstream2
ostree summary -u --repo=repos/test ${FL_GPGARGS}

flatpak ${U} --appstream update test-repo

assert_has_file $FL_DIR/repo/refs/remotes/test-repo/appstream/$ARCH
assert_not_has_file $FL_DIR/repo/refs/remotes/test-repo/appstream2/$ARCH

assert_has_file $FL_DIR/appstream/test-repo/$ARCH/.timestamp
assert_has_symlink $FL_DIR/appstream/test-repo/$ARCH/active
assert_not_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml
assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml.gz

echo "ok update compat appstream"

# Then regenerate new appstream branch and verify that we update to it
update_repo

flatpak ${U} --appstream update test-repo

assert_has_file $FL_DIR/repo/refs/remotes/test-repo/appstream2/$ARCH

assert_has_file $FL_DIR/appstream/test-repo/$ARCH/.timestamp
assert_has_symlink $FL_DIR/appstream/test-repo/$ARCH/active
assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml
assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml.gz

echo "ok update appstream"

if [ x${USE_COLLECTIONS_IN_CLIENT-} != xyes ] ; then
    install_repo test-no-gpg
    echo "ok install without gpg key"

    ${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello
else
    echo "ok install without gpg key # skip not supported for collections"
fi

install_repo local-test-no-gpg
${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello
${FLATPAK} ${U} update --appstream local-test-no-gpg-repo

echo "ok local without gpg key"

install_repo test-gpg2
echo "ok with alternative gpg key"

if ${FLATPAK} ${U} install -y test-repo org.test.Platform 2> install-error-log; then
    assert_not_reached "Should not be able to install again from different remote without reinstall"
fi
echo "ok failed to install again from different remote"

${FLATPAK} ${U} install -y --reinstall test-repo org.test.Platform
echo "ok re-install"

${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello

if ${FLATPAK} ${U} install -y test-missing-gpg-repo org.test.Platform 2> install-error-log; then
    assert_not_reached "Should not be able to install with missing gpg key"
fi
assert_file_has_content install-error-log "GPG signatures found, but none are in trusted keyring"


if ${FLATPAK} ${U} install test-missing-gpg-repo org.test.Hello 2> install-error-log; then
    assert_not_reached "Should not be able to install with missing gpg key"
fi
assert_file_has_content install-error-log "GPG signatures found, but none are in trusted keyring"

echo "ok fail with missing gpg key"

if ${FLATPAK} ${U} install test-wrong-gpg-repo org.test.Platform 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong gpg key"
fi
assert_file_has_content install-error-log "GPG signatures found, but none are in trusted keyring"

if ${FLATPAK} ${U} install test-wrong-gpg-repo org.test.Hello 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong gpg key"
fi
assert_file_has_content install-error-log "GPG signatures found, but none are in trusted keyring"

echo "ok fail with wrong gpg key"

if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
    CID=org.test.Collection.test
else
    CID=""
fi

make_required_version_app () {
    APP_ID=${1}
    VERSION=${2}

    REQUIRED_VERSION="${VERSION}" GPGARGS="${FL_GPGARGS}" $(dirname $0)/make-test-app.sh test ${APP_ID} "${CID}" > /dev/null
}

CURRENT_VERSION=`cat "$test_builddir/package_version.txt"`
V=( ${CURRENT_VERSION//./ } )  # Split parts to array

make_required_version_app org.test.SameVersion "${V[0]}.${V[1]}.${V[2]}"
make_required_version_app org.test.NeedNewerMicro "${V[0]}.${V[1]}.$(expr ${V[2]} + 1)"
make_required_version_app org.test.NeedNewerMinor "${V[0]}.$(expr ${V[1]} + 1).${V[2]}"
make_required_version_app org.test.NeedNewerMajor "$(expr ${V[0]} + 1).${V[1]}.${V[2]}"
make_required_version_app org.test.NeedOlderMinor "${V[0]}.$(expr ${V[1]} - 1).${V[2]}"
make_required_version_app org.test.MultiVersionFallback "${V[0]}.${V[1]}.${V[2]};0.9.0;"
make_required_version_app org.test.MultiVersionFallbackFail "${V[0]}.$(expr ${V[1]} + 1).${V[2]};0.9.0;"
make_required_version_app org.test.MultiVersionOk "${V[0]}.$(expr ${V[1]} + 1).0;${V[0]}.${V[1]}.${V[2]};"
make_required_version_app org.test.MultiVersionNotOk "${V[0]}.$(expr ${V[1]} + 1).0;${V[0]}.${V[1]}.$(expr ${V[2]} + 1);"

update_repo test "${CID}"

${FLATPAK} ${U} install -y test-repo org.test.SameVersion
${FLATPAK} ${U} install -y test-repo org.test.NeedOlderMinor

if ${FLATPAK} ${U} install -y test-repo org.test.NeedNewerMicro 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong micro version"
fi
assert_file_has_content install-error-log "needs a later flatpak version"

if ${FLATPAK} ${U} install -y test-repo org.test.NeedNewerMinor 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong minor version"
fi
assert_file_has_content install-error-log "needs a later flatpak version"

if ${FLATPAK} ${U} install -y test-repo org.test.NeedNewerMajor 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong micro version"
fi
assert_file_has_content install-error-log "needs a later flatpak version"

${FLATPAK} ${U} install -y test-repo org.test.MultiVersionFallback

if ${FLATPAK} ${U} install -y test-repo org.test.MultiVersionFallbackFail 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong fallback version"
fi
assert_file_has_content install-error-log "needs a later flatpak version"

${FLATPAK} ${U} install -y test-repo org.test.MultiVersionOk

if ${FLATPAK} ${U} install -y test-repo org.test.MultiVersionNotOk 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong multi version"
fi
assert_file_has_content install-error-log "needs a later flatpak version"

${FLATPAK} ${U} uninstall -y --all

echo "ok handles version requirements"

${FLATPAK} ${U} remotes -d | grep ^test-repo > repo-info
assert_not_file_has_content repo-info "new-title"
UPDATE_REPO_ARGS=--title=new-title update_repo
assert_file_has_content repos/test/config new-title

# This should make us automatically pick up the new metadata
${FLATPAK} ${U} install test-repo org.test.Platform
${FLATPAK} ${U} remotes -d | grep ^test-repo > repo-info
assert_file_has_content repo-info "new-title"

echo "ok update metadata"

if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
    COPY_COLLECTION_ID=org.test.Collection.test
    copy_collection_args=--collection-id=${COLLECTION_ID}
else
    COPY_COLLECTION_ID=
    copy_collection_args=
fi

ostree init --repo=repos/test-copy --mode=archive-z2 ${copy_collection_args}
${FLATPAK} build-commit-from --end-of-life=Reason1 --src-repo=repos/test repos/test-copy app/org.test.Hello/$ARCH/master
update_repo test-copy ${COPY_COLLECTION_ID}

# Ensure we have no eol app in appdata
if ! ostree show --repo=repos/test-copy appstream/${ARCH} > /dev/null; then
    assert_not_reached "No appstream branch"
fi
ostree cat --repo=repos/test-copy appstream/${ARCH} /appstream.xml.gz | gunzip -d > appdata.xml
assert_not_file_has_content appdata.xml "org.test.Hello.desktop"

${FLATPAK} repo --branches repos/test-copy > branches-log
assert_file_has_content branches-log "^app/org.test.Hello/.*eol=Reason1"

echo "ok eol build-commit-from"

${FLATPAK} ${U} install -y test-repo org.test.Hello

EXPORT_ARGS="--end-of-life=Reason2" make_updated_app

# Ensure we have no eol app in appdata
if ! ostree show --repo=repos/test appstream/${ARCH} > /dev/null; then
    assert_not_reached "No appstream branch"
fi
ostree cat --repo=repos/test appstream/${ARCH} /appstream.xml.gz | gunzip -d > appdata.xml
assert_not_file_has_content appdata.xml "org.test.Hello.desktop"

${FLATPAK} repo --branches repos/test > branches-log
assert_file_has_content branches-log "^app/org.test.Hello/.*eol=Reason2"

${FLATPAK} ${U} remote-ls -d test-repo > remote-ls-log
assert_file_has_content remote-ls-log "^app/org.test.Hello/.*eol=Reason2"

${FLATPAK} ${U} update -y org.test.Hello 2> update-log
assert_file_has_content update-log "app/org.test.Hello/.*Reason2"

${FLATPAK} ${U} info org.test.Hello > info-log
assert_file_has_content info-log "end-of-life: Reason2"

${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "^org.test.Hello/.*eol=Reason2"

${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform

echo "ok eol build-export"

${FLATPAK} ${U} install -y test-repo org.test.Platform

port=$(cat httpd-port-main)
UPDATE_REPO_ARGS="--redirect-url=http://127.0.0.1:${port}/test-gpg3 --gpg-import=${FL_GPG_HOMEDIR2}/pubring.gpg" update_repo
GPGPUBKEY="${FL_GPG_HOMEDIR2}/pubring.gpg" GPGARGS="${FL_GPGARGS2}" setup_repo_no_add test-gpg3 org.test.Collection.test

${FLATPAK} ${U} update -y org.test.Platform
# Ensure we have the new uri
${FLATPAK} ${U} remotes -d | grep ^test-repo > repo-info
assert_file_has_content repo-info "/test-gpg3"

# Make sure we also get new installs from the new repo
GPGARGS="${FL_GPGARGS2}" make_updated_app test-gpg3 org.test.Collection.test
update_repo test-gpg3 org.test.Collection.test

${FLATPAK} ${U} install -y test-repo org.test.Hello
assert_file_has_content $FL_DIR/app/org.test.Hello/$ARCH/master/active/files/bin/hello.sh UPDATED

echo "ok redirect url and gpg key"

if ${FLATPAK} ${INVERT_U} uninstall -y org.test.Platform org.test.Hello; then
    assert_not_reached "Should not be able to uninstall ${INVERT_U} when installed ${U}"
fi

# Test that unspecified --user/--system finds the right one, so no ${U}
${FLATPAK} uninstall -y org.test.Platform org.test.Hello

${FLATPAK} ${U} list -d > list-log
assert_not_file_has_content list-log "^org.test.Hello"
assert_not_file_has_content list-log "^org.test.Platform"

echo "ok uninstall vs installations"

${FLATPAK} ${U} install -y test-repo org.test.Hello

${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "^org.test.Hello"
assert_file_has_content list-log "^org.test.Platform"

if ${FLATPAK} ${U} uninstall -y org.test.Platform; then
    assert_not_reached "Should not be able to uninstall ${U} when there is a dependency installed"
fi

${FLATPAK} ${U} uninstall -y org.test.Hello
${FLATPAK} ${U} uninstall -y org.test.Platform

${FLATPAK} ${U} list -d > list-log
assert_not_file_has_content list-log "^org.test.Hello"
assert_not_file_has_content list-log "^org.test.Platform"

echo "ok uninstall dependencies"

${FLATPAK} ${U} install -y --no-deploy test-repo org.test.Hello

${FLATPAK} ${U} list -d > list-log
assert_not_file_has_content list-log "^org.test.Hello"
assert_not_file_has_content list-log "^org.test.Platform"

# Disable the remote to make sure we don't do i/o
port=$(cat httpd-port-main)
${FLATPAK}  ${U} remote-modify --url="http://127.0.0.1:${port}/disable-test" test-repo

${FLATPAK} ${U} install -y --no-pull test-repo org.test.Hello

# re-enable remote
${FLATPAK}  ${U} remote-modify --url="http://127.0.0.1:${port}/test" test-repo

${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "^org.test.Hello"
assert_file_has_content list-log "^org.test.Platform"

echo "ok install with --no-deploy and then --no-pull"

${FLATPAK} uninstall -y --all

${FLATPAK} ${U} list -d > list-log
assert_not_file_has_content list-log "^org.test.Hello"
assert_not_file_has_content list-log "^org.test.Platform"

echo "ok uninstall --all"

# Test that remote-ls works in all of the following cases:
# * system remote, and --system is used
# * system remote, and --system is omitted
# * user remote, and --user is used
# * user remote, and --user is omitted
# and fails in the following cases:
# * system remote, and --user is used
# * user remote, and --system is used
if [ x${USE_SYSTEMDIR-} == xyes ]; then
    ${FLATPAK} --system remote-ls test-repo > repo-list
    assert_file_has_content repo-list "org.test.Hello"

    ${FLATPAK} remote-ls test-repo > repo-list
    assert_file_has_content repo-list "org.test.Hello"

    if ${FLATPAK} --user remote-ls test-repo 2> remote-ls-error-log; then
        assert_not_reached "flatpak --user remote-ls should not work for system remotes"
    fi
    assert_file_has_content remote-ls-error-log "Remote \"test-repo\" not found"
else
    ${FLATPAK} --user remote-ls test-repo > repo-list
    assert_file_has_content repo-list "org.test.Hello"

    ${FLATPAK} remote-ls test-repo > repo-list
    assert_file_has_content repo-list "org.test.Hello"

    if ${FLATPAK} --system remote-ls test-repo 2> remote-ls-error-log; then
        assert_not_reached "flatpak --system remote-ls should not work for user remotes"
    fi
    assert_file_has_content remote-ls-error-log "Remote \"test-repo\" not found"
fi

echo "ok remote-ls"

# Test that remote-ls can take a file:// URI
ostree --repo=repos/test summary -u
${FLATPAK} remote-ls file://`pwd`/repos/test > repo-list
assert_file_has_content repo-list "org.test.Hello"

echo "ok remote-ls URI"

# Test that remote-modify works in all of the following cases:
# * system remote, and --system is used
# * system remote, and --system is omitted
# * user remote, and --user is used
# * user remote, and --user is omitted
# and fails in the following cases:
# * system remote, and --user is used
# * user remote, and --system is used
if [ x${USE_SYSTEMDIR-} == xyes ]; then
    ${FLATPAK} --system remote-modify --title=NewTitle test-repo
    ${FLATPAK} remotes -d | grep ^test-repo > repo-info
    assert_file_has_content repo-info "NewTitle"
    ${FLATPAK} --system remote-modify --title=OldTitle test-repo

    ${FLATPAK} remote-modify --title=NewTitle test-repo
    ${FLATPAK} remotes -d | grep ^test-repo > repo-info
    assert_file_has_content repo-info "NewTitle"
    ${FLATPAK} --system remote-modify --title=OldTitle test-repo

    if ${FLATPAK} --user remote-modify --title=NewTitle test-repo 2> remote-modify-error-log; then
        assert_not_reached "flatpak --user remote-modify should not work for system remotes"
    fi
    assert_file_has_content remote-modify-error-log "Remote \"test-repo\" not found"
else
    ${FLATPAK} --user remote-modify --title=NewTitle test-repo
    ${FLATPAK} remotes -d | grep ^test-repo > repo-info
    assert_file_has_content repo-info "NewTitle"
    ${FLATPAK} --user remote-modify --title=OldTitle test-repo

    ${FLATPAK} remote-modify --title=NewTitle test-repo
    ${FLATPAK} remotes -d | grep ^test-repo > repo-info
    assert_file_has_content repo-info "NewTitle"
    ${FLATPAK} remote-modify --title=OldTitle test-repo

    if ${FLATPAK} --system remote-modify --title=NewTitle test-repo 2> remote-modify-error-log; then
        assert_not_reached "flatpak --system remote-modify should not work for user remotes"
    fi
    assert_file_has_content remote-modify-error-log "Remote \"test-repo\" not found"
fi

echo "ok remote-modify"

# Test that remote-delete works in all of the following cases:
# * system remote, and --system is used
# * system remote, and --system is omitted
# * user remote, and --user is used
# * user remote, and --user is omitted
# and fails in the following cases:
# * system remote, and --user is used
# * user remote, and --system is used
if [ x${USE_SYSTEMDIR-} == xyes ]; then
    ${FLATPAK} --system remote-delete test-repo
    ${FLATPAK} remotes > repo-info
    assert_not_file_has_content repo-info "test-repo"
    setup_repo

    ${FLATPAK} remote-delete test-repo
    ${FLATPAK} remotes > repo-list
    assert_not_file_has_content repo-info "test-repo"
    setup_repo

    if ${FLATPAK} --user remote-delete test-repo 2> remote-delete-error-log; then
        assert_not_reached "flatpak --user remote-delete should not work for system remotes"
    fi
    assert_file_has_content remote-delete-error-log "Remote \"test-repo\" not found"
else
    ${FLATPAK} --user remote-delete test-repo
    ${FLATPAK} remotes > repo-info
    assert_not_file_has_content repo-info "test-repo"
    setup_repo

    ${FLATPAK} remote-delete test-repo
    ${FLATPAK} remotes > repo-info
    assert_not_file_has_content repo-info "test-repo"
    setup_repo

    if ${FLATPAK} --system remote-delete test-repo 2> remote-delete-error-log; then
        assert_not_reached "flatpak --system remote-delete should not work for user remotes"
    fi
    assert_file_has_content remote-delete-error-log "Remote \"test-repo\" not found"
fi

echo "ok remote-delete"

# Test that remote-info works in all of the following cases:
# * system remote, and --system is used
# * system remote, and --system is omitted
# * user remote, and --user is used
# * user remote, and --user is omitted
# and fails in the following cases:
# * system remote, and --user is used
# * user remote, and --system is used
if [ x${USE_SYSTEMDIR-} == xyes ]; then
    ${FLATPAK} --system remote-info test-repo org.test.Hello > remote-ref-info
    assert_file_has_content remote-ref-info "ID: org.test.Hello"

    ${FLATPAK} remote-info test-repo org.test.Hello > remote-ref-info
    assert_file_has_content remote-ref-info "ID: org.test.Hello"

    if ${FLATPAK} --user remote-info test-repo org.test.Hello 2> remote-info-error-log; then
        assert_not_reached "flatpak --user remote-info should not work for system remotes"
    fi
    assert_file_has_content remote-info-error-log "Remote \"test-repo\" not found"
else
    ${FLATPAK} --user remote-info test-repo org.test.Hello > remote-ref-info
    assert_file_has_content remote-ref-info "ID: org.test.Hello"

    ${FLATPAK} remote-info test-repo org.test.Hello > remote-ref-info
    assert_file_has_content remote-ref-info "ID: org.test.Hello"

    if ${FLATPAK} --system remote-info test-repo org.test.Hello 2> remote-info-error-log; then
        assert_not_reached "flatpak --system remote-info should not work for user remotes"
    fi
    assert_file_has_content remote-info-error-log "Remote \"test-repo\" not found"
fi

echo "ok remote-info"
