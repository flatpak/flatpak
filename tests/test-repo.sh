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
skip_revokefs_without_fuse

echo "1..40"

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
assert_file_has_content appdata.xml "<id>org\.test\.Hello\.desktop</id>"

ostree cat --repo=repos/test appstream2/${ARCH} /appstream.xml > appdata2.xml
assert_file_has_content appdata2.xml "<id>org\.test\.Hello\.desktop</id>"

# Unsigned repo (not supported with collections; client-side use of collections requires GPG)
if [ x${USE_COLLECTIONS_IN_CLIENT-} == xyes ] ; then
    if GPGPUBKEY=" " GPGARGS=" " setup_repo test-no-gpg org.test.Collection.NoGpg; then
        assert_not_reached "Should fail remote-add due to missing GPG key"
    fi
elif [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
    # Set a collection ID and GPG on the server, but not in the client configuration
    setup_repo_no_add test-no-gpg org.test.Collection.NoGpg
    port=$(cat httpd-port)
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
port=$(cat httpd-port)
if flatpak remote-add ${U} test-missing-gpg-repo "http://127.0.0.1:${port}/test"; then
    assert_not_reached "Should fail metadata-update due to missing gpg key"
fi

#remote with wrong GPG key
port=$(cat httpd-port)
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
assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml
assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml.gz

ok "update compat appstream"

# Then regenerate new appstream branch and verify that we update to it
update_repo

flatpak ${U} --appstream update test-repo

assert_has_file $FL_DIR/repo/refs/remotes/test-repo/appstream2/$ARCH

assert_has_file $FL_DIR/appstream/test-repo/$ARCH/.timestamp
assert_has_symlink $FL_DIR/appstream/test-repo/$ARCH/active
assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml
assert_has_file $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml.gz

ok "update appstream"

if [ x${USE_COLLECTIONS_IN_CLIENT-} != xyes ] ; then
    install_repo test-no-gpg
    ok "install without gpg key"

    ${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello
else
    ok "install without gpg key # skip not supported for collections"
fi

install_repo local-test-no-gpg
${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello
${FLATPAK} ${U} update --appstream local-test-no-gpg-repo

ok "local without gpg key"

install_repo test-gpg2
ok "with alternative gpg key"

if ${FLATPAK} ${U} install -y test-repo org.test.Platform 2> install-error-log; then
    assert_not_reached "Should not be able to install again from different remote without reinstall"
fi
ok "failed to install again from different remote"

${FLATPAK} ${U} install -y --reinstall test-repo org.test.Platform
ok "re-install"

${FLATPAK} ${U} uninstall -y org.test.Hello

# Note: This typo is only auto-corrected without user interaction because we're using -y
${FLATPAK} ${U} install -y test-repo org.test.Hllo >install-log
assert_file_has_content install-log "org\.test\.Hello"

${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "org\.test\.Hello"

ok "typo correction works for install"

${FLATPAK} ${U} uninstall -y org.test.Hello

# Temporarily disable some remotes so that org.test.Hello only exists in one
${FLATPAK} ${U} remote-modify --disable test-missing-gpg-repo
${FLATPAK} ${U} remote-modify --disable test-wrong-gpg-repo
${FLATPAK} ${U} remote-modify --disable test-gpg2-repo
${FLATPAK} ${U} remote-modify --disable local-test-no-gpg-repo
if [ x${USE_COLLECTIONS_IN_CLIENT-} != xyes ] ; then
    ${FLATPAK} ${U} remote-modify --disable test-no-gpg-repo
fi

# Note: The missing remote is only auto-corrected without user interaction because we're using -y
${FLATPAK} ${U} install -y org.test.Hello |& tee install-log
assert_file_has_content install-log "org\.test\.Hello"

${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "org\.test\.Hello"

${FLATPAK} ${U} remote-modify --enable test-missing-gpg-repo
${FLATPAK} ${U} remote-modify --enable test-wrong-gpg-repo
${FLATPAK} ${U} remote-modify --enable test-gpg2-repo
${FLATPAK} ${U} remote-modify --enable local-test-no-gpg-repo
if [ x${USE_COLLECTIONS_IN_CLIENT-} != xyes ] ; then
    ${FLATPAK} ${U} remote-modify --enable test-no-gpg-repo
fi

ok "missing remote name auto-corrects for install"

port=$(cat httpd-port)
if ${FLATPAK} ${U} install -y http://127.0.0.1:${port}/nonexistent.flatpakref 2> install-error-log; then
    assert_not_reached "Should not be able to install a nonexistent flatpakref"
fi
assert_file_has_content install-error-log "Server returned status 404: Not Found"

ok "install fails gracefully for 404 URLs"

# Use a new remote so we can be sure it doesn't match any existing one's URL
setup_repo_no_add flatpakref org.test.Collection.Flatpakref

cat << EOF > repos/flatpakref/flatpakref-repo.flatpakrepo
[Flatpak Repo]
Version=1
Url=http://127.0.0.1:$(cat httpd-port)/flatpakref/
Title=The Title
GPGKey=${FL_GPG_BASE64}
EOF

if [ x${USE_COLLECTIONS_IN_CLIENT-} == xyes ]; then
    echo "DeployCollectionID=org.test.Collection.Flatpakref" >> repos/flatpakref/flatpakref-repo.flatpakrepo
fi

cat << EOF > org.test.Hello.flatpakref
[Flatpak Ref]
Name=org.test.Hello
Branch=master
Url=http://127.0.0.1:$(cat httpd-port)/flatpakref
GPGKey=${FL_GPG_BASE64}
RuntimeRepo=http://127.0.0.1:$(cat httpd-port)/flatpakref/flatpakref-repo.flatpakrepo
EOF

${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello

# Ensure that only one remote is added even though the URL in the flatpakref
# does not have a trailing slash and the URL in the flatpakrepo file does
NUM_REMOTES_BEFORE=$(flatpak remotes | wc -l)
${FLATPAK} ${U} install -y org.test.Hello.flatpakref
NUM_REMOTES_AFTER=$(flatpak remotes | wc -l)

if [ $NUM_REMOTES_AFTER -ne $((NUM_REMOTES_BEFORE + 1)) ]; then
    assert_not_reached "install of flatpakref should only add one remote"
fi

ok "install flatpakref normalizes remote URL trailing slash"

${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello

if ${FLATPAK} ${U} install -y test-missing-gpg-repo org.test.Platform 2> install-error-log; then
    assert_not_reached "Should not be able to install with missing gpg key"
fi
assert_log_has_gpg_signature_error install-error-log

if ${FLATPAK} ${U} install test-missing-gpg-repo org.test.Hello 2> install-error-log; then
    assert_not_reached "Should not be able to install with missing gpg key"
fi
assert_log_has_gpg_signature_error install-error-log

ok "fail with missing gpg key"

if ${FLATPAK} ${U} install test-wrong-gpg-repo org.test.Platform 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong gpg key"
fi
assert_log_has_gpg_signature_error install-error-log

if ${FLATPAK} ${U} install test-wrong-gpg-repo org.test.Hello 2> install-error-log; then
    assert_not_reached "Should not be able to install with wrong gpg key"
fi
assert_log_has_gpg_signature_error install-error-log

ok "fail with wrong gpg key"

make_required_version_app () {
    APP_ID=${1}
    VERSION=${2}
    if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
        CID=org.test.Collection.test
    else
        CID=""
    fi

    REQUIRED_VERSION="${VERSION}" GPGARGS="${FL_GPGARGS}" $(dirname $0)/make-test-app.sh repos/test ${APP_ID} master "${CID}" > /dev/null
}

CURRENT_VERSION=`cat "$test_builddir/package_version.txt"`
V=( ${CURRENT_VERSION//./ } )  # Split parts to array

make_required_version_app org.test.SameVersion "${V[0]}.${V[1]}.${V[2]}"
make_required_version_app org.test.NeedNewerMicro "${V[0]}.${V[1]}.$(expr ${V[2]} + 1)"
make_required_version_app org.test.NeedNewerMinor "${V[0]}.$(expr ${V[1]} + 1).${V[2]}"
make_required_version_app org.test.NeedNewerMajor "$(expr ${V[0]} + 1).${V[1]}.${V[2]}"
make_required_version_app org.test.NeedOlderMinor "${V[0]}.$(expr ${V[1]} - 1).${V[2]}"
make_required_version_app org.test.MultiVersionFallback "${V[0]}.${V[1]}.${V[2]};1.0.0;"
make_required_version_app org.test.MultiVersionFallbackFail "${V[0]}.$(expr ${V[1]} + 1).${V[2]};1.0.0;"
make_required_version_app org.test.MultiVersionOk "${V[0]}.$(expr ${V[1]} + 1).0;${V[0]}.${V[1]}.${V[2]};"
make_required_version_app org.test.MultiVersionNotOk "${V[0]}.$(expr ${V[1]} + 1).0;${V[0]}.${V[1]}.$(expr ${V[2]} + 1);"

update_repo

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
    assert_not_reached "Should not be able to install with wrong major version"
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

ok "handles version requirements"

${FLATPAK} ${U} remotes -d | grep ^test-repo > repo-info
assert_not_file_has_content repo-info "new-title"
UPDATE_REPO_ARGS=--title=new-title update_repo
assert_file_has_content repos/test/config new-title

# This should make us automatically pick up the new metadata
${FLATPAK} ${U} install -y test-repo org.test.Platform
${FLATPAK} ${U} remotes -d | grep ^test-repo > repo-info
assert_file_has_content repo-info "new-title"

ok "update metadata"

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
assert_not_file_has_content appdata.xml "org\.test\.Hello\.desktop"

${FLATPAK} repo --branches repos/test-copy > branches-log
assert_file_has_content branches-log "^app/org\.test\.Hello/.*eol=Reason1"

ok "eol build-commit-from"

${FLATPAK} ${U} install -y test-repo org.test.Hello

EXPORT_ARGS="--end-of-life=Reason2" make_updated_app

# Ensure we have no eol app in appdata
if ! ostree show --repo=repos/test appstream/${ARCH} > /dev/null; then
    assert_not_reached "No appstream branch"
fi
ostree cat --repo=repos/test appstream/${ARCH} /appstream.xml.gz | gunzip -d > appdata.xml
assert_not_file_has_content appdata.xml "org\.test\.Hello\.desktop"

${FLATPAK} repo --branches repos/test > branches-log
assert_file_has_content branches-log "^app/org\.test\.Hello/.*eol=Reason2"

# eol only visible in remote-ls if -a:
${FLATPAK} ${U} remote-ls -d test-repo > remote-ls-log
assert_not_file_has_content remote-ls-log "app/org\.test\.Hello"

${FLATPAK} ${U} remote-ls -d -a test-repo > remote-ls-log
assert_file_has_content remote-ls-log "app/org\.test\.Hello/.*eol=Reason2"

${FLATPAK} ${U} update -y org.test.Hello > update-log
assert_file_has_content update-log "org\.test\.Hello.*Reason2"

${FLATPAK} ${U} info org.test.Hello > info-log
assert_file_has_content info-log "End-of-life: Reason2"

${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "org\.test\.Hello/.*eol=Reason2"

${FLATPAK} ${U} uninstall -y org.test.Hello

# Remove eol for future tests
EXPORT_ARGS="" make_updated_app

ok "eol build-export"

if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
    REBASE_COLLECTION_ID=org.test.Collection.rebase
    rebase_collection_args=--collection-id=${REBASE_COLLECTION_ID}
else
    REBASE_COLLECTION_ID=
    rebase_collection_args=
fi

ostree init --repo=repos/test-rebase --mode=archive-z2 ${rebase_collection_args}
${FLATPAK} build-commit-from --src-repo=repos/test ${FL_GPGARGS} repos/test-rebase app/org.test.Hello/$ARCH/master runtime/org.test.Hello.Locale/$ARCH/master
update_repo test-rebase ${REBASE_COLLECTION_ID}

flatpak remote-add ${U} --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg test-rebase "http://127.0.0.1:${port}/test-rebase"

${FLATPAK} ${U} install -y test-rebase org.test.Hello

assert_not_has_dir $HOME/.var/app/org.test.Hello
${CMD_PREFIX} flatpak run --command=bash org.test.Hello -c 'echo foo > $XDG_DATA_HOME/a-file'
assert_has_dir $HOME/.var/app/org.test.Hello
assert_has_file $HOME/.var/app/org.test.Hello/data/a-file

${FLATPAK} build-commit-from --end-of-life-rebase=org.test.Hello=org.test.NewHello --src-repo=repos/test ${FL_GPGARGS} repos/test-rebase app/org.test.Hello/$ARCH/master runtime/org.test.Hello.Locale/$ARCH/master
GPGARGS="${FL_GPGARGS}" $(dirname $0)/make-test-app.sh repos/test-rebase org.test.NewHello master "${REBASE_COLLECTION_ID}" "NEW" > /dev/null

${FLATPAK} ${U} update -y org.test.Hello

# Make sure we got the new version installed
assert_has_dir $FL_DIR/app/org.test.NewHello/$ARCH/master/active/files
assert_not_has_file $FL_DIR/app/org.test.NewHello/$ARCH/master/active/files

${CMD_PREFIX} flatpak run --command=bash org.test.NewHello -c 'echo foo > $XDG_DATA_HOME/another-file'

# Ensure we migrated the app data
assert_has_dir $HOME/.var/app/org.test.NewHello
assert_has_file $HOME/.var/app/org.test.NewHello/data/a-file
assert_has_file $HOME/.var/app/org.test.NewHello/data/another-file

# And that the old is symlinked
assert_has_symlink $HOME/.var/app/org.test.Hello
assert_has_file $HOME/.var/app/org.test.Hello/data/a-file
assert_has_file $HOME/.var/app/org.test.Hello/data/another-file

${FLATPAK} ${U} uninstall -y org.test.NewHello org.test.Platform

ok "eol-rebase"

# Remove any pin of the runtime from an earlier test
${FLATPAK} ${U} pin --remove runtime/org.test.Platform/$ARCH/master 2>/dev/null || true

EXPORT_ARGS="--end-of-life=Reason3" make_updated_runtime

${FLATPAK} ${U} install -y test-repo org.test.Hello
${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "org\.test\.Hello"
assert_file_has_content list-log "org\.test\.Platform"

${FLATPAK} ${U} uninstall -y org.test.Hello

${FLATPAK} ${U} list -d -a > list-log
assert_not_file_has_content list-log "org\.test\.Hello"
assert_not_file_has_content list-log "org\.test\.Platform"
assert_not_file_has_content list-log "org\.test\.Platform.Locale"

ok "eol runtime uninstalled with app"

${FLATPAK} ${U} install -y test-repo org.test.Hello

${FLATPAK} ${U} info org.test.Platform > info-log
assert_file_has_content info-log "End-of-life: Reason3"

assert_has_dir $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/files

# Update the app to a different runtime branch
make_updated_runtime "" "" "mainline" ""
make_updated_app "" "" "" "UPDATED99" "" "mainline"

${FLATPAK} ${U} update -y org.test.Hello

# The previous runtime should have been removed during the update
assert_has_dir $FL_DIR/runtime/org.test.Platform/$ARCH/mainline/active/files
assert_not_has_dir $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/files

# Revert things for future tests
EXPORT_ARGS="" make_updated_runtime
make_updated_app "" "" "" "UPDATED100" "" "master"
${FLATPAK} ${U} uninstall -y --all
ostree refs --repo=repos/test --delete runtime/org.test.Platform/$ARCH/mainline
ostree refs --repo=repos/test --delete runtime/org.test.Platform.Locale/$ARCH/mainline
update_repo

ok "eol runtime uninstalled on app update to different runtime"

${FLATPAK} ${U} install -y test-repo org.test.Platform

port=$(cat httpd-port)
UPDATE_REPO_ARGS="--redirect-url=http://127.0.0.1:${port}/test-gpg3 --gpg-import=${FL_GPG_HOMEDIR2}/pubring.gpg" update_repo
SRC_RUNTIME_REPO="test" GPGPUBKEY="${FL_GPG_HOMEDIR2}/pubring.gpg" GPGARGS="${FL_GPGARGS2}" setup_repo_no_add test-gpg3 org.test.Collection.test master

${FLATPAK} ${U} update -y org.test.Platform
# Ensure we have the new uri
${FLATPAK} ${U} remotes -d | grep ^test-repo > repo-info
assert_file_has_content repo-info "/test-gpg3"

# Make sure we also get new installs from the new repo
GPGARGS="${FL_GPGARGS2}" make_updated_app test-gpg3 org.test.Collection.test master
update_repo test-gpg3 org.test.Collection.test

${FLATPAK} ${U} install -y test-repo org.test.Hello
assert_file_has_content $FL_DIR/app/org.test.Hello/$ARCH/master/active/files/bin/hello.sh UPDATED

ok "redirect url and gpg key"

# Test https://github.com/flatpak/flatpak/issues/3222
mkdir -p $FL_DIR/repo/refs/mirrors/org.test.Collection.test/app/org.test.Hello/$ARCH/
cp $FL_DIR/repo/refs/remotes/test-repo/app/org.test.Hello/$ARCH/master $FL_DIR/repo/refs/mirrors/org.test.Collection.test/app/org.test.Hello/$ARCH/
make_updated_app test-gpg3 org.test.Collection.test master UPDATE2
${FLATPAK} ${U} update -y org.test.Hello
assert_not_has_file $FL_DIR/repo/refs/mirrors/org.test.Collection.test/app/org.test.Hello/$ARCH/master
assert_has_file $FL_DIR/repo/refs/remotes/test-repo/app/org.test.Hello/$ARCH/master

ok "mirror ref deletion on update"

${FLATPAK} ${U} list --arch=$ARCH --columns=ref > list-log
assert_file_has_content list-log "org\.test\.Hello"
assert_file_has_content list-log "org\.test\.Platform"

ok "flatpak list --arch --columns works"

if ${FLATPAK} ${INVERT_U} uninstall -y org.test.Hello; then
    assert_not_reached "Should not be able to uninstall ${INVERT_U} when installed ${U}"
fi

# Test that unspecified --user/--system finds the right one, so no ${U}
${FLATPAK} uninstall -y org.test.Platform org.test.Hello

${FLATPAK} ${U} list -d > list-log
assert_not_file_has_content list-log "org\.test\.Hello"
assert_not_file_has_content list-log "org\.test\.Platform"

ok "uninstall vs installations"

${FLATPAK} ${U} install -y test-repo org.test.Hello

${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "org\.test\.Hello"
assert_file_has_content list-log "org\.test\.Platform"

if ${FLATPAK} ${U} uninstall -y org.test.Platform; then
    assert_not_reached "Should not be able to uninstall ${U} when there is a dependency installed"
fi

${FLATPAK} ${U} uninstall -y org.test.Hello
${FLATPAK} ${U} uninstall -y org.test.Platform

${FLATPAK} ${U} list -d > list-log
assert_not_file_has_content list-log "org\.test\.Hello"
assert_not_file_has_content list-log "org\.test\.Platform"

ok "uninstall dependencies"

${FLATPAK} ${U} install -y test-repo org.test.Hello

# Note: This typo is only auto-corrected without user interaction because we're using -y
${FLATPAK} ${U} uninstall -y hello
${FLATPAK} ${U} uninstall -y platform

ok "typo correction works for uninstall"

${FLATPAK} ${U} install -y test-repo org.test.Hello master

${FLATPAK} ${U} uninstall -y org.test.Hello master
${FLATPAK} ${U} uninstall -y org.test.Platform master

ok "install and uninstall support 'NAME BRANCH' syntax"

${FLATPAK} ${U} install -y --no-deploy test-repo org.test.Hello

${FLATPAK} ${U} list -d > list-log
assert_not_file_has_content list-log "org\.test\.Hello"
assert_not_file_has_content list-log "org\.test\.Platform"

# Disable the remote to make sure we don't do i/o
port=$(cat httpd-port)
${FLATPAK} ${U} remote-modify --url="http://127.0.0.1:${port}/disable-test" test-repo

${FLATPAK} ${U} install -y --no-pull test-repo org.test.Hello

# re-enable remote
${FLATPAK} ${U} remote-modify --url="http://127.0.0.1:${port}/test" test-repo

${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "org\.test\.Hello"
assert_file_has_content list-log "org\.test\.Platform"

ok "install with --no-deploy and then --no-pull"

${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform

${FLATPAK} ${U} install -y --no-deploy test-repo hello

${FLATPAK} ${U} list -d > list-log
assert_not_file_has_content list-log "org\.test\.Hello"
assert_not_file_has_content list-log "org\.test\.Platform"

# Disable the remote to make sure we don't do i/o
port=$(cat httpd-port)
${FLATPAK} ${U} remote-modify --url="http://127.0.0.1:${port}/disable-test" test-repo

# Note: The partial ref is only auto-corrected without user interaction because we're using -y
${FLATPAK} ${U} install -y --no-pull test-repo hello

# re-enable remote
${FLATPAK} ${U} remote-modify --url="http://127.0.0.1:${port}/test" test-repo

${FLATPAK} ${U} list -d > list-log
assert_file_has_content list-log "org\.test\.Hello"
assert_file_has_content list-log "org\.test\.Platform"

ok "install with --no-deploy and then --no-pull works with typo correction"

${FLATPAK} uninstall -y --all

${FLATPAK} ${U} list -d > list-log
assert_not_file_has_content list-log "org\.test\.Hello"
assert_not_file_has_content list-log "org\.test\.Platform"

ok "uninstall --all"

${FLATPAK} ${U} install -y test-repo org.test.Hello

${FLATPAK} ${U} list -a --columns=application > list-log
assert_file_has_content list-log "org\.test\.Hello"
assert_file_has_content list-log "org\.test\.Hello\.Locale"

${FLATPAK} ${U} remote-delete --force test-repo
${FLATPAK} ${U} uninstall -y org.test.Hello

${FLATPAK} ${U} list -a --columns=application > list-log
assert_not_file_has_content list-log "org\.test\.Hello"
assert_not_file_has_content list-log "org\.test\.Hello\.Locale"

setup_repo

ok "uninstall with missing remote"

# Remove any pin of the runtime from an earlier test
${FLATPAK} ${U} pin --remove runtime/org.test.Platform/$ARCH/master 2>/dev/null || true

${FLATPAK} ${U} list -a --columns=application > list-log
assert_file_has_content list-log "org\.test\.Platform"

${FLATPAK} ${U} uninstall -y --unused

${FLATPAK} ${U} list -a --columns=application > list-log
assert_not_file_has_content list-log "org\.test\.Platform"

ok "uninstall --unused"

${FLATPAK} ${U} install -y test-repo org.test.Platform

${FLATPAK} ${U} list -a --columns=application > list-log
assert_file_has_content list-log "org\.test\.Platform"

# Check that the runtime won't be removed if it's pinned
# (which happens during the install above)
${FLATPAK} ${U} pin > pins
assert_file_has_content pins "runtime/org\.test\.Platform/$ARCH/master"
NUM_PINS=$(cat pins | wc -l)
if [ $NUM_PINS -ne 1 ]; then
    assert_not_reached "There should only be one pinned runtime"
fi
rm pins

${FLATPAK} ${U} uninstall -y --unused

${FLATPAK} ${U} list -a --columns=application > list-log
assert_file_has_content list-log "org\.test\.Platform"

# Remove the pin and try again
${FLATPAK} ${U} pin --remove "runtime/org.test.Platform/$ARCH/master"
${FLATPAK} ${U} uninstall -y --unused
${FLATPAK} ${U} list -a --columns=application > list-log
assert_not_file_has_content list-log "org\.test\.Platform"

ok "uninstall --unused ignores pinned runtimes"

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
    assert_file_has_content repo-list "org\.test\.Hello"

    ${FLATPAK} remote-ls test-repo > repo-list
    assert_file_has_content repo-list "org\.test\.Hello"

    if ${FLATPAK} --user remote-ls test-repo 2> remote-ls-error-log; then
        assert_not_reached "flatpak --user remote-ls should not work for system remotes"
    fi
    assert_file_has_content remote-ls-error-log "Remote \"test-repo\" not found"
else
    ${FLATPAK} --user remote-ls test-repo > repo-list
    assert_file_has_content repo-list "org\.test\.Hello"

    ${FLATPAK} remote-ls test-repo > repo-list
    assert_file_has_content repo-list "org\.test\.Hello"

    if ${FLATPAK} --system remote-ls test-repo 2> remote-ls-error-log; then
        assert_not_reached "flatpak --system remote-ls should not work for user remotes"
    fi
    assert_file_has_content remote-ls-error-log "Remote \"test-repo\" not found"
fi

ok "remote-ls"

# Test that remote-ls can take a file:// URI
ostree --repo=repos/test summary -u
${FLATPAK} remote-ls file://`pwd`/repos/test > repo-list
assert_file_has_content repo-list "org\.test\.Hello"

ok "remote-ls URI"

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

ok "remote-modify"

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

ok "remote-delete"

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
    assert_file_has_content remote-ref-info "ID: org\.test\.Hello"

    ${FLATPAK} remote-info test-repo org.test.Hello > remote-ref-info
    assert_file_has_content remote-ref-info "ID: org\.test\.Hello"

    if ${FLATPAK} --user remote-info test-repo org.test.Hello 2> remote-info-error-log; then
        assert_not_reached "flatpak --user remote-info should not work for system remotes"
    fi
    assert_file_has_content remote-info-error-log "Remote \"test-repo\" not found"
else
    ${FLATPAK} --user remote-info test-repo org.test.Hello > remote-ref-info
    assert_file_has_content remote-ref-info "ID: org\.test\.Hello"

    ${FLATPAK} remote-info test-repo org.test.Hello > remote-ref-info
    assert_file_has_content remote-ref-info "ID: org\.test\.Hello"

    if ${FLATPAK} --system remote-info test-repo org.test.Hello 2> remote-info-error-log; then
        assert_not_reached "flatpak --system remote-info should not work for user remotes"
    fi
    assert_file_has_content remote-info-error-log "Remote \"test-repo\" not found"
fi

ok "remote-info"

${FLATPAK} ${U} remote-ls -d -a test-repo > remote-ls-log
assert_file_has_content remote-ls-log "app/org\.test\.Hello"
assert_file_has_content remote-ls-log "runtime/org\.test\.Hello\.Locale"
assert_file_has_content remote-ls-log "runtime/org\.test\.Platform"

${FLATPAK}  ${U} remote-info test-repo org.test.Hello > remote-ref-info
assert_file_has_content remote-ref-info "ID: org\.test\.Hello"

${FLATPAK} ${U} update --appstream test-repo
assert_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml "app/org\.test\.Hello"

# Make a copy so we can remove it later
cp ${test_srcdir}/test.filter test.filter
${FLATPAK} ${U} remote-modify test-repo --filter $(pwd)/test.filter

${FLATPAK} ${U} remote-ls -d -a test-repo > remote-ls-log

assert_not_file_has_content remote-ls-log "app/org\.test\.Hello"
assert_not_file_has_content remote-ls-log "runtime/org\.test\.Hello\.Locale"
assert_file_has_content remote-ls-log "runtime/org\.test\.Platform"

if ${FLATPAK}  ${U} remote-info test-repo org.test.Hello > remote-ref-info 2> /dev/null; then
    assert_not_reached "flatpak remote-info test-repo org.test.Hello should fail due to filter"
fi

if ${FLATPAK} ${U} install -y test-repo org.test.Hello 2> /dev/null; then
    assert_not_reached "should not be able to install org.test.Hello should fail due to filter"
fi

${FLATPAK} ${U} update --appstream test-repo
assert_not_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml "app/org\.test\.Hello"

# Ensure that filter works even when the filter file is removed (uses the backup)
rm -f test.filter
${FLATPAK} ${U} remote-ls -d -a test-repo > remote-ls-log
assert_not_file_has_content remote-ls-log "app/org\.test\.Hello"
assert_not_file_has_content remote-ls-log "runtime/org\.test\.Hello\.Locale"
assert_file_has_content remote-ls-log "runtime/org\.test\.Platform"
if ${FLATPAK}  ${U} remote-info test-repo org.test.Hello > remote-ref-info; then
    assert_not_reached "flatpak remote-info test-repo org.test.Hello should fail due to filter"
fi
if ${FLATPAK} ${U} install -y test-repo org.test.Hello; then
    assert_not_reached "should not be able to install org.test.Hello should fail due to filter"
fi

${FLATPAK} ${U} update --appstream test-repo
assert_not_file_has_content $FL_DIR/appstream/test-repo/$ARCH/active/appstream.xml "app/org\.test\.Hello"

# Remove filter

${FLATPAK} ${U} remote-modify test-repo --no-filter

${FLATPAK} ${U} remote-ls -d -a test-repo > remote-ls-log
assert_file_has_content remote-ls-log "app/org\.test\.Hello"
assert_file_has_content remote-ls-log "runtime/org\.test\.Hello\.Locale"
assert_file_has_content remote-ls-log "runtime/org\.test\.Platform"

ok "filter"

# Try installing it from a flatpakrepo file. Don’t uninstall afterwards because
# we need it for the next test.
cat << EOF > test.flatpakrepo
[Flatpak Repo]
Url=http://127.0.0.1:${port}/test-no-gpg
Title=The Title
Comment=The Comment
Description=The Description
Homepage=https://the.homepage/
Icon=https://the.icon/
DefaultBranch=default-branch
NoDeps=true
EOF

if ${FLATPAK} ${U} remote-add test-repo test.flatpakrepo; then
    assert_not_reached "should not be able to add pre-existing remote"
fi

# No-op
${FLATPAK} ${U} remote-add --if-not-exists test-repo test.flatpakrepo


${FLATPAK} ${U} remote-add new-repo test.flatpakrepo

assert_remote_has_config new-repo url "http://127.0.0.1:${port}/test-no-gpg"
assert_remote_has_config new-repo gpg-verify "false"
assert_remote_has_config new-repo xa.title "The Title"
assert_remote_has_no_config new-repo xa.title-is-set
assert_remote_has_config new-repo xa.comment "The Comment"
assert_remote_has_no_config new-repo xa.comment-is-set
assert_remote_has_config new-repo xa.description "The Description"
assert_remote_has_no_config new-repo xa.description-is-set
assert_remote_has_config new-repo xa.homepage "https://the.homepage/"
assert_remote_has_no_config new-repo xa.homepage-is-set
assert_remote_has_config new-repo xa.icon "https://the.icon/"
assert_remote_has_no_config new-repo xa.icon-is-set
assert_remote_has_config new-repo xa.default-branch "default-branch"
assert_remote_has_no_config new-repo xa.default-branch-is-set
assert_remote_has_config new-repo xa.nodeps "true"
assert_remote_has_no_config new-repo xa.noenumerate
assert_remote_has_no_config new-repo xa.filter

${FLATPAK} ${U} remote-delete new-repo
${FLATPAK} ${U} remote-add  --title=Title2 --comment=Comment2 --default-branch=branch2 new-repo test.flatpakrepo

assert_remote_has_config new-repo url "http://127.0.0.1:${port}/test-no-gpg"
assert_remote_has_config new-repo gpg-verify "false"
assert_remote_has_config new-repo xa.title "Title2"
assert_remote_has_config new-repo xa.title-is-set true
assert_remote_has_config new-repo xa.comment "Comment2"
assert_remote_has_config new-repo xa.comment-is-set true
assert_remote_has_config new-repo xa.description "The Description"
assert_remote_has_no_config new-repo xa.description-is-set
assert_remote_has_config new-repo xa.homepage "https://the.homepage/"
assert_remote_has_no_config new-repo xa.homepage-is-set
assert_remote_has_config new-repo xa.icon "https://the.icon/"
assert_remote_has_no_config new-repo xa.icon-is-set
assert_remote_has_config new-repo xa.default-branch "branch2"
assert_remote_has_config new-repo xa.default-branch-is-set true
assert_remote_has_config new-repo xa.nodeps "true"
assert_remote_has_no_config new-repo xa.noenumerate
assert_remote_has_no_config new-repo xa.filter

${FLATPAK} ${U} remote-delete new-repo
${FLATPAK} ${U} remote-add  --filter="${test_srcdir}/test.filter" new-repo test.flatpakrepo

assert_remote_has_config new-repo xa.filter "${test_srcdir}/test.filter"

# This should unset the filter:
${FLATPAK} ${U} remote-add --if-not-exists new-repo test.flatpakrepo
assert_remote_has_no_config new-repo xa.filter

ok "flatpakrepo"
