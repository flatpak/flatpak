#!/bin/bash
#
# Copyright 2026 Philip Withnall <philip@tecnocode.co.uk>
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
skip_without_ostree_version 2026 5  # need https://github.com/ostreedev/ostree/pull/3633

mkdir -p repos
httpd

# Test: Can’t update from a repo which has an expired key.
push_gpg_homedir
export REPONAME=test-expired-gpg
export COLLECTION_ID=org.test.Collection.ExpiredGpg
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2

# Update the repo metadata then expire the key by setting its expiry date to a
# known past-date, and then manually propagate the updated key to the checkout
update_repo $REPONAME $COLLECTION_ID
gpg --homedir "${FL_GPG_HOMEDIR}" --quick-set-expire "${FL_GPG_FINGERPRINT}" "20260101T000000"
${FLATPAK} ${U} remote-modify --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo >&2

if ${FLATPAK} ${U} update --appstream $REPONAME-repo >&2; then
    assert_not_reached "Should fail metadata-update due to expired GPG key"
fi

# Cleanup
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "no updates if key has expired"


# Test: Set up repo as normal; then update and find it’s acquired a gpg-keys-url=;
# update from that, get new subkeys, update successfully
push_gpg_homedir
export REPONAME=test-new-subkeys
export COLLECTION_ID=org.test.Collection.NewSubkeys
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_SUBKEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"

# Add a new signing subkey to the keyring and publish it on the web server
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-add-key "${FL_GPG_FINGERPRINT}" default sign
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${FL_GPG_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"

# Grab the fingerprint of the new subkey; this assumes that subkeys are listed in creation order, so the new one will be listed last
NEW_SUBKEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"

# Build an app and sign it with both subkeys, re-sign the repo with both subkeys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_SUBKEY_FINGERPRINT}! --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new subkey; installation should
# now fail.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_SUBKEY_FINGERPRINT}! --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" update_repo "${REPONAME}" "${COLLECTION_ID}"
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new subkey which the client doesn’t yet have should not work"
fi

# Now publish the updated keyring: add a gpg-keys-url= to the repo
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Update the app again as previously, then try to install it again. It should
# work this time because the new subkey has been published so the client can
# grab it.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_SUBKEY_FINGERPRINT}! --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" update_repo "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} update -y org.test.Hello >&2

# Check that the new subkey has been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_file_has_content updated-local-keyring "^fpr:::::::::${NEW_SUBKEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_SUBKEY_FINGERPRINT
unset NEW_SUBKEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new subkey propagation using gpg-keys-url="


# Test: Set up repo as normal; then update and find it’s acquired a gpg-keys-url=;
# update from that, get new subkeys, update successfully
push_gpg_homedir
export REPONAME=test-gpg-verify-false
export COLLECTION_ID=org.test.Collection.GpgVerifyFalse
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --no-gpg-verify --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_SUBKEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"

# Add a new signing subkey to the keyring and publish it on the web server
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-add-key "${FL_GPG_FINGERPRINT}" default sign
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${FL_GPG_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"

# Grab the fingerprint of the new subkey; this assumes that subkeys are listed in creation order, so the new one will be listed last
NEW_SUBKEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"

# Build an app and sign it with both subkeys, re-sign the repo with both subkeys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_SUBKEY_FINGERPRINT}! --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new subkey. Installation should
# still succeed because the remote has --no-gpg-verify.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_SUBKEY_FINGERPRINT}! --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" update_repo "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} update -y org.test.Hello >&2

# Now publish the updated keyring: add a gpg-keys-url= to the repo
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Update the app again as previously, then try to install it again. It should
# still work this time because the repo still has --no-gpg-verify.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_SUBKEY_FINGERPRINT}! --gpg-sign=${NEW_SUBKEY_FINGERPRINT}!" update_repo "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} update -y org.test.Hello >&2

# Check that the new subkey has *not* been imported locally because of --no-gpg-verify
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_SUBKEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_SUBKEY_FINGERPRINT
unset NEW_SUBKEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new subkey propagation disabled by --no-gpg-verify"


# Test: Set up repo with a gpg-keys-url=; update from that, get new subkey
# but which has already been revoked, cannot update apps from that
push_gpg_homedir
export REPONAME=test-new-subkey-revoked
export COLLECTION_ID=org.test.Collection.NewSubkeyRevoked
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_SUBKEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new signing subkey to the keyring, and grab its fingerprint (this assumes
# keys are listed in creation order).
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-add-key "${FL_GPG_FINGERPRINT}" default sign
NEW_SUBKEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"

# Build an app and sign it with just the new subkey
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_SUBKEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"

# Now that we’ve signed everything we need to with new subkey, revoke it
# and publish everything on the repo and the web server. Unfortunately subkeys
# don’t get their own revocation certificates, so have to be revoked using the
# --edit-key command, which is an exercise in suffering.
echo -e "uid 1\nkey 2\nrevkey\ny\n0\nTest subkey revocation\n\ny\nsave\n" | gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --command-fd 0 --edit-key "${NEW_SUBKEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_SUBKEY_FINGERPRINT}" "${NEW_SUBKEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Try to install the app; this should fail due to only being signed with the revoked subkey.
if ${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new revoked subkey should not work"
fi

# Check that the new key has been imported locally as revoked
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_file_has_content updated-local-keyring "^sub:r:255:22:${NEW_SUBKEY_FINGERPRINT: -16}:[^:]*::::::s:::::ed25519::$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Platform >&2  # the runtime gets installed because it’s built with the original key in setup_repo_no_add()
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_SUBKEY_FINGERPRINT
unset NEW_SUBKEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new subkey ignored if revoked"


# Test: Install a bundle and check that its repo has gpg-keys-url= configured
# correctly.
push_gpg_homedir
export REPONAME=test-bundle
export COLLECTION_ID=org.test.Collection.Bundle
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)

# Configure the repo with a public keyring advertised via gpg-keys-url=
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${FL_GPG_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Build a bundle from the repo; use a runtime bundle since then we don’t run
# into dependency problems
mkdir bundles
${FLATPAK} build-bundle repos/${REPONAME} --runtime --repo-url=file://$(pwd)/repos/${REPONAME} --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg bundles/platform.flatpak org.test.Platform >&2

# Install the bundle and check its gpg-keys-url got configured
${FLATPAK} ${U} install -y --bundle bundles/platform.flatpak >&2
assert_remote_has_config platform-origin xa.gpg-keys-url "${GPG_KEYS_URL}"

# Cleanup
rm -rf bundles
${FLATPAK} ${U} uninstall -y org.test.Platform >&2
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "installing a bundle to set gpg-keys-url="


# Test: Install a flatpakrepo and check that its repo has gpg-keys-url=
# configured correctly
push_gpg_homedir
export REPONAME=test-flatpakrepo
export COLLECTION_ID=org.test.Collection.Flatpakrepo
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)

# Configure the repo with a public keyring advertised via gpg-keys-url=
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${FL_GPG_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Build a flatpakrepo file
GPG_KEY="$(gpg --homedir "${FL_GPG_HOMEDIR}" --export "${FL_GPG_FINGERPRINT}" | base64 --wrap 0)"

cat << EOF > ${REPONAME}.flatpakrepo
[Flatpak Repo]
Version=1
Url=http://127.0.0.1:${port}/${REPONAME}/
Title=The Remote Title
GPGKey=${GPG_KEY}
GPGKeysUrl=${GPG_KEYS_URL}
EOF

# Add the .flatpakrepo and check its gpg-keys-url got configured
${FLATPAK} ${U} remote-add ${REPONAME}-repo ${REPONAME}.flatpakrepo >&2
assert_remote_has_config ${REPONAME}-repo xa.gpg-keys-url "${GPG_KEYS_URL}"

# Cleanup
rm ${REPONAME}.flatpakrepo
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset GPG_KEY
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "installing a .flatpakrepo to set gpg-keys-url="


# Test: Install a flatpakref and check that its repo has gpg-keys-url=
# configured correctly
push_gpg_homedir
export REPONAME=test-flatpakref
export COLLECTION_ID=org.test.Collection.Flatpakref
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)

# Configure the repo with a public keyring advertised via gpg-keys-url=
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${FL_GPG_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Build a .flatpakref file; we also need to build a .flatpakrepo file as that’s
# always required, but don’t set the GPGKeysUrl= in that one, since we want to
# test .flatpakref.
GPG_KEY="$(gpg --homedir "${FL_GPG_HOMEDIR}" --export "${FL_GPG_FINGERPRINT}" | base64 --wrap 0)"

cat << EOF > ${REPONAME}.flatpakrepo
[Flatpak Repo]
Version=1
Url=http://127.0.0.1:${port}/${REPONAME}/
Title=The Remote Title
GPGKey=${GPG_KEY}
EOF

cat << EOF > org.test.Hello.flatpakref
[Flatpak Ref]
Name=org.test.Hello
Branch=master
Url=http://127.0.0.1:${port}/${REPONAME}
SuggestRemoteName=${REPONAME}-repo
GPGKey=${GPG_KEY}
GPGKeysUrl=${GPG_KEYS_URL}
RuntimeRepo=file://$(pwd)/${REPONAME}.flatpakrepo
EOF

# Add the .flatpakref and check its gpg-keys-url got configured
${FLATPAK} ${U} install -y --no-deps org.test.Hello.flatpakref >&2
assert_remote_has_config ${REPONAME}-repo xa.gpg-keys-url "${GPG_KEYS_URL}"

# Cleanup
rm org.test.Hello.flatpakref ${REPONAME}.flatpakrepo
${FLATPAK} ${U} uninstall -y org.test.Hello >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset GPG_KEY
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "installing a .flatpakref to set gpg-keys-url="


# Test: Adding a remote with `flatpak remote-add --gpg-keys-url=blah` works
push_gpg_homedir
export REPONAME=test-remote-add
export COLLECTION_ID=org.test.Collection.RemoteAdd
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)

# Publish the keys, but don’t update the repo to add gpg-keys-url= to its
# summary yet.
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${FL_GPG_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"

# Add the repo, but explicitly set its gpg-keys-url= configuration. Updating
# metadata should work after this.
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg --gpg-keys-url="${GPG_KEYS_URL}" $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
assert_remote_has_config ${REPONAME}-repo xa.gpg-keys-url "${GPG_KEYS_URL}"
${FLATPAK} ${U} update --appstream $REPONAME-repo >&2

# Now publish the gpg-keys-url= in the repo metadata, and update again to check
# the server-provided metadata doesn’t conflict with the local metadata.
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2
${FLATPAK} ${U} update --appstream $REPONAME-repo >&2
assert_remote_has_config ${REPONAME}-repo xa.gpg-keys-url "${GPG_KEYS_URL}"

# Cleanup
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "configuring gpg-keys-url= using remote-add"


# Test: Modifying a remote with `flatpak remote-modify --gpg-keys-url=blah` works
push_gpg_homedir
export REPONAME=test-remote-modify
export COLLECTION_ID=org.test.Collection.RemoteModify
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)

# Publish the keys, but don’t update the repo to add gpg-keys-url= to its
# summary yet.
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${FL_GPG_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"

# Add the repo, but don’t yet set its gpg-keys-url= configuration. Updating
# metadata should work after this, but should not set up the gpg-keys-url= config.
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
assert_remote_has_no_config ${REPONAME}-repo xa.gpg-keys-url
${FLATPAK} ${U} update --appstream $REPONAME-repo >&2
assert_remote_has_no_config ${REPONAME}-repo xa.gpg-keys-url

# Modify the remote, the config should now be set
${FLATPAK} ${U} remote-modify --gpg-keys-url="${GPG_KEYS_URL}" $REPONAME-repo >&2
assert_remote_has_config ${REPONAME}-repo xa.gpg-keys-url "${GPG_KEYS_URL}"

${FLATPAK} ${U} remotes --columns=name,gpg-keys-url | grep ^$REPONAME-repo > repo-info
assert_file_has_content repo-info "${GPG_KEYS_URL}"

# Now publish the gpg-keys-url= in the repo metadata, and update again to check
# the server-provided metadata doesn’t conflict with the local metadata.
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2
${FLATPAK} ${U} update --appstream $REPONAME-repo >&2
assert_remote_has_config ${REPONAME}-repo xa.gpg-keys-url "${GPG_KEYS_URL}"

# Cleanup
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "configuring gpg-keys-url= using remote-modify"


# Test: Adding a remote with `flatpak remote-add --gpg-keys-url=blah` works when
# the repo already has gpg-keys-url in its summary
push_gpg_homedir
export REPONAME=test-remote-add-immediate
export COLLECTION_ID=org.test.Collection.RemoteAddImmediate
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)

# Publish the keys and update the repo to add gpg-keys-url= to its summary.
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${FL_GPG_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Add the repo but don’t set its gpg-keys-url= configuration. Updating
# metadata should work after this, and the gpg-keys-url= should have been
# grabbed from the summary.
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
assert_remote_has_config ${REPONAME}-repo xa.gpg-keys-url "${GPG_KEYS_URL}"
${FLATPAK} ${U} update --appstream $REPONAME-repo >&2

# Now try installing an app
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${FL_GPG_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "configuring gpg-keys-url= using remote-add with gpg-keys-url in summary already"


# Test: Set up repo as normal; then update and find it’s acquired a gpg-keys-url=;
# update from that, get new primary key, update successfully
push_gpg_homedir
export REPONAME=test-new-primary
export COLLECTION_ID=org.test.Collection.NewPrimary
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), cross-sign it from the original key, and publish it
# on the web server
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"

# Build an app and sign it with both keys, re-sign the repo with both keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new key; installation should
# now fail.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key which the client doesn’t yet have should not work"
fi

# Now publish the updated keyring: add a gpg-keys-url= to the repo
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Update the app again as previously, then try to install it again. It should
# work this time because the new key has been published so the client can
# grab it.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} update -y org.test.Hello >&2

# Check that the new primary key has been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key propagation using gpg-keys-url="


# Test: Set up repo as normal; then update and find it’s acquired a gpg-keys-url=;
# update from that, but find that it’s not a valid keyring; fail to update
push_gpg_homedir
export REPONAME=test-new-primary-invalid-keyring
export COLLECTION_ID=org.test.Collection.NewPrimaryInvalidKeyring
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), cross-sign it from the original key, but then
# publish a duff file on the web server
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
echo "oh no" > repos/${REPONAME}.published.gpg
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"

# Build an app and sign it with both keys, re-sign the repo with both keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new key; installation should
# now fail.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key which the client doesn’t yet have should not work"
fi

# Now publish the updated keyring: add a gpg-keys-url= to the repo
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Update the app again as previously, then try to install it again. It should
# still fail this time, because the new keyring is invalid.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key which the client doesn’t yet have (due to keyring being invalid) should not work"
fi

# Check that the new primary key has not been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Re-export and it should work this time.
rm "repos/${REPONAME}.published.gpg"
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
${FLATPAK} ${U} update -y org.test.Hello >&2

# Check that the new primary key has been imported locally this time
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key propagation ignored if gpg-keys-url= is invalid"


# Test: Set up repo with a gpg-keys-url=; update from that, get new primary key
# but *without* any cross-signatures from the old key, cannot update apps from that
push_gpg_homedir
export REPONAME=test-new-primary-unsigned
export COLLECTION_ID=org.test.Collection.NewPrimaryUnsigned
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), do *not* cross-sign it from the original key, but
# do sign the original key with the new one in an attempt to confuse; then
# publish it on the repo and the web server
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Build an app and sign it with both keys, re-sign the repo with both keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new key; installation should
# now fail.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key which is not cross-signed by the old key should not work"
fi

# Check that the new primary key has *not* been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key ignored if not cross-signed"


# Test: Set up repo with a gpg-keys-url=; update from that, get a chain of new
# primary keys which cross-sign each other but without any cross-signatures from
# the *original* key, cannot update apps from that
push_gpg_homedir
export REPONAME=test-new-primary-unsigned-chain
export COLLECTION_ID=org.test.Collection.NewPrimaryUnsignedChain
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add three new primaries to the keyring, grab their fingerprints (this assumes keys are
# listed in creation order), do *not* cross-sign them from the original key, but
# do sign the original key with the new ones, and cross-sign all the new ones
# with each other in an attempt to confuse; then publish it on the repo and the web server
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary 1" default sign >&2
NEW_KEY_FINGERPRINT1="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary 2" default sign >&2
NEW_KEY_FINGERPRINT2="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary 3" default sign >&2
NEW_KEY_FINGERPRINT3="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"

gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT1}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT2}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT3}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2

gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT1}" --quick-sign-key "${NEW_KEY_FINGERPRINT2}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT1}" --quick-sign-key "${NEW_KEY_FINGERPRINT3}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT2}" --quick-sign-key "${NEW_KEY_FINGERPRINT1}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT2}" --quick-sign-key "${NEW_KEY_FINGERPRINT3}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT3}" --quick-sign-key "${NEW_KEY_FINGERPRINT1}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT3}" --quick-sign-key "${NEW_KEY_FINGERPRINT2}" >&2

gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT1}" "${NEW_KEY_FINGERPRINT2}" "${NEW_KEY_FINGERPRINT3}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Build an app and sign it with all keys, re-sign the repo with all keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT1} --gpg-sign=${NEW_KEY_FINGERPRINT2} --gpg-sign=${NEW_KEY_FINGERPRINT3}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new keys; installation should
# now fail.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT1} --gpg-sign=${NEW_KEY_FINGERPRINT2} --gpg-sign=${NEW_KEY_FINGERPRINT3}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT1} --gpg-sign=${NEW_KEY_FINGERPRINT2} --gpg-sign=${NEW_KEY_FINGERPRINT3}" update_repo "${REPONAME}" "${COLLECTION_ID}"
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with new primary keys which are not cross-signed by the old key should not work"
fi

# Check that none of the new primary keys have been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT1}:$"
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT2}:$"
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT3}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT1
unset NEW_KEY_FINGERPRINT2
unset NEW_KEY_FINGERPRINT3
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key chain ignored if not cross-signed"


# Test: Set up repo with a gpg-keys-url=; update from that, get a chain of new
# primary keys which cross-sign each other and with cross-signatures from the
# *original* key, can update apps from that
push_gpg_homedir
export REPONAME=test-new-primary-chain
export COLLECTION_ID=org.test.Collection.NewPrimaryChain
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add three new primaries to the keyring, grab their fingerprints (this assumes keys are
# listed in creation order), and cross-sign them in a chain from the primary;
# then publish it on the repo and the web server
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary 1" default sign >&2
NEW_KEY_FINGERPRINT1="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary 2" default sign >&2
NEW_KEY_FINGERPRINT2="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary 3" default sign >&2
NEW_KEY_FINGERPRINT3="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"

gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT1}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT1}" --quick-sign-key "${NEW_KEY_FINGERPRINT2}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT2}" --quick-sign-key "${NEW_KEY_FINGERPRINT3}" >&2

gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT1}" "${NEW_KEY_FINGERPRINT2}" "${NEW_KEY_FINGERPRINT3}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Build an app and sign it with the final new key, and re-sign the repo with
# that key and the original (so the summary can be loaded), then try to install
# the new app. This should succeed.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT3}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT3}" update_repo "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Check that at least the final new key has been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT3}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT1
unset NEW_KEY_FINGERPRINT2
unset NEW_KEY_FINGERPRINT3
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new complex primary key chain used if cross-signed"


# Test: Set up repo with a gpg-keys-url=; update from that, get a new primary
# key which is cross-signed from the old key, and also get a revocation packet
# for the old key, can update apps from that, but can no longer install apps
# signed only with the old key
push_gpg_homedir
export REPONAME=test-new-primary-and-revocation
export COLLECTION_ID=org.test.Collection.NewPrimaryAndRevocation
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), and cross-sign it with the old primary. Then build
# the apps and update the repo as we can’t do this once the old primary is revoked.
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2

# Build an app three times:
#  - First, signed with the original key;
#  - second, signed with the original and new keys; and
#  - third, signed with only the new key.
# Then re-sign the repo with the original and new keys (so the summary can be
# loaded). Having three versions of the app means we can try a downgrade against
# the old key later on, once it’s been revoked.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
OLD_COMMIT1=$(cat repos/${REPONAME}/refs/heads/app/org.test.Hello/${ARCH}/master)
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
OLD_COMMIT2=$(cat repos/${REPONAME}/refs/heads/app/org.test.Hello/${ARCH}/master)
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"

# Install the platform first, as make_updated_app doesn’t re-sign that, so it’s
# only signed with the original key and hence will cause problems if we try and
# install it as a dependency after revoking the key.
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Platform >&2

# Now that we’ve signed everything we need to with the original key, revoke it
# and publish everything on the repo and the web server. We need to edit the
# .rev file first to remove a safety switch and allow it to be used.
rev_file="${FL_GPG_HOMEDIR}/openpgp-revocs.d/${ORIGINAL_KEY_FINGERPRINT}.rev"
sed -i 's/^:-----BEGIN PGP PUBLIC KEY BLOCK-----/-----BEGIN PGP PUBLIC KEY BLOCK-----/' "${rev_file}"
gpg --homedir "${FL_GPG_HOMEDIR}" --import "${rev_file}"
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Try to install the app; this should succeed.
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Try and downgrade to the previous version. This should work, as it’s signed
# with the new key (valid) as well as the old key (revoked at this point).
${FLATPAK} ${U} update -y --commit "${OLD_COMMIT2}" org.test.Hello >&2

# Try and downgrade to the previous previous version. This should fail, as it’s
# only signed with the old key (revoked at this point).
if ${FLATPAK} ${U} update -y --commit "${OLD_COMMIT1}" org.test.Hello >&2; then
    assert_not_reached "Installing a version of an app signed with an old revoked primary key should not work"
fi

# Check that the new key has been imported locally, and that the old key has
# been revoked locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
assert_file_has_content updated-local-keyring "^pub:r:2048:1:${ORIGINAL_KEY_FINGERPRINT: -16}:[^:]*:::-:::c::::::23::0:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset rev_file
unset OLD_COMMIT1
unset OLD_COMMIT2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key used and old one revoked if cross-signed"


# Test: Set up repo with a gpg-keys-url=; update from that, get new primary key
# with cross-signatures from the old key, but the new one’s already expired,
# cannot update apps from that. We should, however, still import the expired
# key, as it could be used as part of a chain of trust which we encounter in
# future, if that chain of trust was created before the key expired.
push_gpg_homedir
export REPONAME=test-new-primary-expired
export COLLECTION_ID=org.test.Collection.NewPrimaryExpired
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), cross-sign it from the original key; then use it to
# sign everything else we need before expiring it and publishing it on the repo
# and the web server. We have to expire it last as gpg won’t let us sign stuff
# with an expired key.
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2

# Build an app and sign it with both keys, re-sign the repo with both keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new key (before we expire it).
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"

# Expire the key and publish it
gpg --homedir "${FL_GPG_HOMEDIR}" --quick-set-expire "${NEW_KEY_FINGERPRINT}" "20260101T000000"
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Installation of the updated app should now fail.
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key which is expired should not work"
fi

# Check that the expired primary key *has* been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key can’t be used for app installation but is imported if expired"


# Test: Set up repo with a gpg-keys-url=; update from that, get new primary key
# which has cross-signatures from the old key but which has since been revoked,
# cannot update apps from that
push_gpg_homedir
export REPONAME=test-new-primary-revoked
export COLLECTION_ID=org.test.Collection.NewPrimaryRevoked
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), cross-sign it from the original key; then use it to
# sign everything else we need before revoking it and publishing it on the repo
# and the web server. We have to revoke it last as gpg won’t let us sign stuff
# with a revoked key.
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2

# Build an app and sign it with both keys, re-sign the repo with both keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new key (before we revoke it).
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"

# Revoke the key and publish it. We need to edit the .rev file first to remove a
# safety switch and allow it to be used.
rev_file="${FL_GPG_HOMEDIR}/openpgp-revocs.d/${NEW_KEY_FINGERPRINT}.rev"
sed -i 's/^:-----BEGIN PGP PUBLIC KEY BLOCK-----/-----BEGIN PGP PUBLIC KEY BLOCK-----/' "${rev_file}"
gpg --homedir "${FL_GPG_HOMEDIR}" --import "${rev_file}"
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Installation of the updated app should now fail.
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key which is revoked should not work"
fi

# Check that the revoked primary key has *not* been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset rev_file
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key ignored if revoked"


# Test: Set up repo with a gpg-keys-url=; update from that, get new primary key
# which has cross-signatures from the old key but which has since had them
# revoked, cannot update apps from that
push_gpg_homedir
export REPONAME=test-new-primary-revoked-sig
export COLLECTION_ID=org.test.Collection.NewPrimaryRevokedSig
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), cross-sign it from the original key; then use it to
# sign everything else we need before revoking its sigs and publishing it on the
# repo and the web server. We have to revoke it last as gpg won’t let us sign
# stuff with a revoked key.
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2

# Build an app and sign it with both keys, re-sign the repo with both keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new key (before we revoke it).
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"

# Revoke the key’s signature and publish it.
gpg --homedir "${FL_GPG_HOMEDIR}" --quick-revoke-sig "${NEW_KEY_FINGERPRINT}" "${ORIGINAL_KEY_FINGERPRINT}"
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Installation of the updated app should now fail.
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key which has revoked signatures should not work"
fi

# Check that the primary key with revoked signatures has *not* been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key ignored if it has revoked signatures"


# Test: Set up repo with a gpg-keys-url=; update from that, get new primary key
# which has cross-signatures from the old key but which have since expired,
# cannot update apps from that
push_gpg_homedir
export REPONAME=test-new-primary-expired-sig
export COLLECTION_ID=org.test.Collection.NewPrimaryExpiredSig
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), and cross-sign it from the original key. We fake
# the system time so that we can set an expiry date which ends up in the past.
# We need to create the key with the same faked system time, otherwise GPG
# complains that the key comes from the future and refuses to sign it. The faked
# time needs to tick between the commands just in case the actual system clock
# has too (which could also lead to the key appearing from the future).
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --faked-system-time 20260531T000000 --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --faked-system-time 20260531T000001 --default-cert-expire 2026-06-01 --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --faked-system-time 20260531T000001 --default-cert-expire 2026-06-01 --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2

# Build an app and sign it with both keys, re-sign the repo with both keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new key.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"

# Publish the key.
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Installation of the updated app should now fail.
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key which has expired signatures should not work"
fi

# Check that the primary key with expired signatures has *not* been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key ignored if it has expired signatures"


# Test: Set up repo with a gpg-keys-url=; update from that, get new primary key
# which has cross-signatures from the old key but that old key has since expired
# whereas the new key is still valid; we *should* be able to update apps from
# that (we trust signatures from expired-but-not-revoked keys)
push_gpg_homedir
export REPONAME=test-new-primary-expired-key-sig
export COLLECTION_ID=org.test.Collection.NewPrimaryExpiredKeySig
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), and cross-sign it from the original key.
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2

# Build an app and sign it with both keys, re-sign the repo with both keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new key.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"

# Now we can expire the original key and publish the keyring.
gpg --homedir "${FL_GPG_HOMEDIR}" --quick-set-expire "${FL_GPG_FINGERPRINT}" "20260101T000000"
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Installation of the updated app should succeed.
${FLATPAK} ${U} update -y org.test.Hello >&2

# Check that both primary keys have been imported locally, but the original key
# is marked as expired.
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_file_has_content updated-local-keyring "^pub:e:2048:1:${ORIGINAL_KEY_FINGERPRINT: -16}:[^:]*:[^:]*::-:::c::::::23::0:$"
assert_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key used if it has signatures from an expired primary key"


# Test: Set up repo with a gpg-keys-url=; update from that, get new primary key
# which has cross-signatures from the old key on one UID but that UID has since
# been revoked, and it has no cross-signatures on a second (non-revoked) UID,
# cannot update apps from that
push_gpg_homedir
export REPONAME=test-new-primary-revoked-uid
export COLLECTION_ID=org.test.Collection.NewPrimaryRevokedUid
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"

# Add a new primary to the keyring, grab its fingerprint (this assumes keys are
# listed in creation order), cross-sign it from the original key; then use it to
# sign everything else we need before revoking one of its UIDs and publishing it
# on the repo and the web server. We have to revoke it last as gpg won’t let us
# sign stuff with a revoked key.
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary" default sign >&2
NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${NEW_KEY_FINGERPRINT}" --quick-sign-key "${ORIGINAL_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2
gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-add-uid "${NEW_KEY_FINGERPRINT}" "New primary 2"

# Build an app and sign it with both keys, re-sign the repo with both keys,
# then try to install the new app (but without publishing the new keyring yet).
# This should succeed due to being signed by the old key too.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
${FLATPAK} ${U} install -y "${REPONAME}-repo" org.test.Hello >&2

# Now update the app but only sign it with the new key (before we revoke it).
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"

# Revoke the UID which was used to sign everything, leaving the other UID (since
# a key requires at least one valid UID) but with no signatures on it. Publish it.
gpg --homedir "${FL_GPG_HOMEDIR}" --quick-revoke-uid "${NEW_KEY_FINGERPRINT}" "New primary"
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" "${NEW_KEY_FINGERPRINT}"
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Installation of the updated app should now fail.
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key where the signing UID has been revoked should not work"
fi

# Check that the primary key with a revoked UID has *not* been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Hello org.test.Platform >&2
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new primary key ignored if it has a revoked UID"


# Test: Set up repo with a gpg-keys-url=; update from that, but find it contains
# too many new keys and bail out; local keyring is not updated
push_gpg_homedir
export REPONAME=test-too-many-new-primaries
export COLLECTION_ID=org.test.Collection.TooManyNewPrimaries
export MAX_N_KEYS_TO_PROCESS=20  # keep this in sync with flatpak-gpg-keys.c
setup_repo_no_add $REPONAME $COLLECTION_ID
port=$(cat httpd-port)
${FLATPAK} ${U} remote-add --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg $REPONAME-repo "http://127.0.0.1:${port}/${REPONAME}" >&2
ORIGINAL_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | head -1 | cut -d : -f 10)"
NEW_KEY_FINGERPRINTS=""

# Add too many new primaries to the keyring, and grab an arbitrary one of their
# fingerprints for later use (this assumes keys are listed in creation order).
for ((i=0; i <= MAX_N_KEYS_TO_PROCESS; i++)); do
    gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --quick-gen-key "New primary ${i}" default sign >&2
    NEW_KEY_FINGERPRINT="$(gpg --homedir "${FL_GPG_HOMEDIR}" --list-keys --with-colons --fingerprint | grep fpr | tail -1 | cut -d : -f 10)"
    NEW_KEY_FINGERPRINTS="${NEW_KEY_FINGERPRINTS} ${NEW_KEY_FINGERPRINT}"
    gpg --homedir "${FL_GPG_HOMEDIR}" --batch --passphrase "" --local-user "${ORIGINAL_KEY_FINGERPRINT}" --quick-sign-key "${NEW_KEY_FINGERPRINT}" >&2
done

# Build an app and sign it with the new key, re-sign the repo with both keys.
# We don’t need to suffix keys with `!` in this test, as we’re working with
# separate primary keys rather than subkeys.
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${NEW_KEY_FINGERPRINT}" make_updated_app "${REPONAME}" "${COLLECTION_ID}"
GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${ORIGINAL_KEY_FINGERPRINT} --gpg-sign=${NEW_KEY_FINGERPRINT}" update_repo "${REPONAME}" "${COLLECTION_ID}"

# Publish the keyring.
gpg --homedir "${FL_GPG_HOMEDIR}" --output repos/${REPONAME}.published.gpg --armor --export "${ORIGINAL_KEY_FINGERPRINT}" ${NEW_KEY_FINGERPRINTS}
GPG_KEYS_URL="http://127.0.0.1:${port}/${REPONAME}.published.gpg"
${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${FL_GPGARGS} --gpg-keys-url="${GPG_KEYS_URL}" repos/${REPONAME} >&2

# Installation of the new app should fail.
if ${FLATPAK} ${U} install -y org.test.Hello >&2; then
    assert_not_reached "Installing an app signed with a new primary key which has not been imported should not work"
fi

# Check that the new primary keys have *not* been imported locally
temp_homedir="$(mktemp -d "${test_tmpdir}/gnupgXXXXXX")"
gpg --homedir "${temp_homedir}" --with-colons --import-options show-only --import "$FL_DIR/repo/${REPONAME}-repo.trustedkeys.gpg" > updated-local-keyring 2>&1
assert_not_file_has_content updated-local-keyring "^fpr:::::::::${NEW_KEY_FINGERPRINT}:$"
rm -rf "${temp_homedir}"

# Cleanup
${FLATPAK} ${U} uninstall -y org.test.Platform >&2  # the runtime gets installed because it’s built with the original key in setup_repo_no_add()
${FLATPAK} ${U} remote-delete ${REPONAME}-repo >&2
unset MAX_N_KEYS_TO_PROCESS
unset ORIGINAL_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINT
unset NEW_KEY_FINGERPRINTS
unset GPG_KEYS_URL
unset COLLECTION_ID
unset REPONAME
pop_gpg_homedir
ok "new keyring not processed if it’s too large"

done_testing
