#!/bin/bash
#
# Copyright (C) 2019 Alexander Larsson <alexl@redhat.com>
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


echo "1..3"

setup_repo

commit_to_obj () {
    echo objects/$(echo $1 | cut -b 1-2)/$(echo $1 | cut -b 3-).commit
}

mark_need_token () {
    REF=$1
    TOKEN=${2:-secret}
    REPO=${3:-test}

    COMMIT=$(cat repos/$REPO/refs/heads/$REF)
    echo -n $TOKEN > repos/$REPO/$(commit_to_obj $COMMIT).need_token
}

assert_failed_with_401 () {
    LOGFILE=${1:-install-error-log}
    assert_file_has_content $LOGFILE "401"
}

make_updated_app test "" autoinstall "" org.flatpak.Authenticator.test master

assert_not_has_dir $FL_DIR/app/org.flatpak.Authenticator.test/$ARCH/autoinstall/active/files

# Mark as need token, even though the app doesn't have token-type set
# We should not be able to install this because we will not present
# the token unnecessarily
mark_need_token app/org.test.Hello/$ARCH/master the-secret

if ${FLATPAK} ${U} install -y test-repo org.test.Hello master &> install-error-log; then
    assert_not_reached "Should not be able to install with no secret"
fi
assert_failed_with_401
assert_not_has_dir $FL_DIR/app/org.flatpak.Authenticator.test/$ARCH/autoinstall/active/files

# Propertly mark it with token-type
EXPORT_ARGS="--token-type=2" make_updated_app
mark_need_token app/org.test.Hello/$ARCH/master the-secret

# Install with no authenticator
if ${FLATPAK} ${U} install -y test-repo org.test.Hello master &> install-error-log; then
    assert_not_reached "Should not be able to install without authenticator"
fi
assert_file_has_content install-error-log "No authenticator configured for remote"
assert_not_has_dir $FL_DIR/app/org.flatpak.Authenticator.test/$ARCH/autoinstall/active/files

${FLATPAK} ${U} remote-modify test-repo --authenticator-name org.flatpak.Authenticator.test --authenticator-install >&2

flatpak remote-ls test-repo -a -d >&2
# Install with wrong token
echo -n not-the-secret > ${XDG_RUNTIME_DIR}/required-token
if ${FLATPAK} ${U} install -y test-repo org.test.Hello master &> install-error-log; then
    assert_not_reached "Should not be able to install with wrong secret"
fi
assert_failed_with_401
# Now we should have auto-installed the authenticator!
assert_has_dir $FL_DIR/app/org.flatpak.Authenticator.test/$ARCH/autoinstall/active/files

# Install with right token
echo -n the-secret > ${XDG_RUNTIME_DIR}/required-token
${FLATPAK} ${U} install -y test-repo org.test.Hello master >&2
assert_file_has_content ${XDG_RUNTIME_DIR}/request "^remote: test-repo$"
assert_file_has_content ${XDG_RUNTIME_DIR}/request "^uri: http://127.0.0.1:${port}/test$"
if [ x${USE_COLLECTIONS_IN_CLIENT-} == xyes ] ; then
    assert_file_has_content ${XDG_RUNTIME_DIR}/request "^options: .*'collection-id': <'org.test.Collection.test'>"
fi

EXPORT_ARGS="--token-type=2" make_updated_app test "" master UPDATE2
mark_need_token app/org.test.Hello/$ARCH/master the-secret

# Update with wrong token
echo -n not-the-secret > ${XDG_RUNTIME_DIR}/required-token
if ${FLATPAK} ${U} update -y org.test.Hello &> install-error-log; then
    assert_not_reached "Should not be able to install with wrong secret"
fi
assert_failed_with_401

# Update with right token
echo -n the-secret > ${XDG_RUNTIME_DIR}/required-token
${FLATPAK} ${U} update -y org.test.Hello >&2

ok "installed build-exported token-type app"

# Drop token-type on main version
make_updated_app test "" master UPDATE3
# And ensure its installable with no token
${FLATPAK} ${U} update -y org.test.Hello >&2

# Use build-commit-from to add it to a new version
$FLATPAK build-commit-from  --no-update-summary ${FL_GPGARGS} --token-type=2 --disable-fsync --src-ref=app/org.test.Hello/$ARCH/master repos/test app/org.test.Hello/$ARCH/copy >&2
update_repo
mark_need_token app/org.test.Hello/$ARCH/copy the-secret

# Install with wrong token
echo -n not-the-secret > ${XDG_RUNTIME_DIR}/required-token
if ${FLATPAK} ${U} install -y test-repo org.test.Hello//copy &> install-error-log; then
    assert_not_reached "Should not be able to install with wrong secret"
fi
assert_failed_with_401

# Install with right token
echo -n the-secret > ${XDG_RUNTIME_DIR}/required-token
${FLATPAK} ${U} install -y test-repo org.test.Hello//copy >&2

ok "installed build-commit-from token-type app"

EXPORT_ARGS="--token-type=2" make_updated_app test "" master UPDATE4
mark_need_token app/org.test.Hello/$ARCH/master the-secret

# In the below test, do a webflow
touch ${XDG_RUNTIME_DIR}/request-webflow
touch ${XDG_RUNTIME_DIR}/require-webflow
echo -n the-secret > ${XDG_RUNTIME_DIR}/required-token

# Broken browser, will not do webflow
export BROWSER=no-such-binary

# This should fail with no auth due to missing binary
if ${FLATPAK} ${U} update -y org.test.Hello >&2; then
    assert_not_reached "Should not be able to install with webflow"
fi

rm ${XDG_RUNTIME_DIR}/require-webflow

# This should be ok, falling back to silent no-auth case due to !require-webflow
${FLATPAK} ${U} update -y org.test.Hello >&2

# Try again with real webflow handler (curl)
touch ${XDG_RUNTIME_DIR}/require-webflow
export BROWSER=curl

EXPORT_ARGS="--token-type=2" make_updated_app test "" master UPDATE5
mark_need_token app/org.test.Hello/$ARCH/master the-secret

ok "update with webflow"
