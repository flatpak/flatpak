#!/bin/bash
#
# Copyright (C) 2021 Matthew Leeds <mwleeds@protonmail.com>
#
# SPDX-License-Identifier: LGPL-2.0-or-later

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..7"

setup_repo

COUNTER=1

create_app () {
    local OPTIONS="$1"
    local DIR=`mktemp -d`

    sleep 1

    mkdir ${DIR}/files
    echo $COUNTER > ${DIR}/files/counter
    let COUNTER=COUNTER+1

    local INVALID=""
    if [[ $OPTIONS =~ "invalid" ]]; then
        INVALID=invalidkeyfileline
    fi
    cat > ${DIR}/metadata <<EOF
[Application]
name=org.test.Malicious
runtime=org.test.Platform/${ARCH}/master
$INVALID

[Context]
EOF
    if [[ $OPTIONS =~ "mismatch" ]]; then
        echo -e "filesystems=host;" >> ${DIR}/metadata
    fi
    if [[ $OPTIONS =~ "hidden" ]]; then
        echo -ne "\0" >> ${DIR}/metadata
        echo -e "\nfilesystems=home;" >> ${DIR}/metadata
    fi
    local XA_METADATA=--add-metadata-string=xa.metadata="$(head -n6 ${DIR}/metadata)"$'\n'
    if [[ $OPTIONS =~ "no-xametadata" ]]; then
        XA_METADATA="--add-metadata-string=xa.nometadata=1"
    fi
    ostree commit --repo=repos/test --branch=app/org.test.Malicious/${ARCH}/master ${FL_GPGARGS} "$XA_METADATA" ${DIR}/
    if [[ $OPTIONS =~ "no-cache-in-summary" ]]; then
        ostree --repo=repos/test ${FL_GPGARGS} summary -u
        # force use of legacy summary format
        rm -rf repos/test/summary.idx repos/test/summaries
    else
        update_repo
    fi
    rm -rf ${DIR}
}

cleanup_repo () {
    ostree refs --repo=repos/test --delete app/org.test.Malicious/${ARCH}/master
    update_repo
}

create_app "hidden"

if ${FLATPAK} ${U} install -y test-repo org.test.Malicious 2>install-error-log; then
    assert_not_reached "Should not be able to install app with hidden permissions"
fi

assert_file_has_content install-error-log "not matching expected metadata"

assert_not_has_dir $FL_DIR/app/org.test.Malicious/current/active

cleanup_repo

ok "app with hidden permissions can't be installed (CVE-2021-43860)"

create_app no-xametadata

# The install will fail because the metadata in the summary doesn't match the metadata on the commit
# The missing xa.metadata in the commit got turned into "" in the xa.cache
if ${FLATPAK} ${U} install -y test-repo org.test.Malicious 2>install-error-log; then
    assert_not_reached "Should not be able to install app with missing xa.metadata"
fi

assert_file_has_content install-error-log "not matching expected metadata"

assert_not_has_dir $FL_DIR/app/org.test.Malicious/current/active

cleanup_repo

ok "app with no xa.metadata can't be installed"

create_app "no-xametadata no-cache-in-summary"

# The install will fail because there's no metadata in the summary or on the commit
if ${FLATPAK} ${U} install -y test-repo org.test.Malicious 2>install-error-log; then
    assert_not_reached "Should not be able to install app with missing metadata"
fi
sed -e 's/^/## /' < install-error-log >&2
# TODO: In versions >= 1.10.x this was "No xa.metadata in local commit".
# Is it OK that this is not the case here?
assert_file_has_content install-error-log "not matching expected metadata"

assert_not_has_dir $FL_DIR/app/org.test.Malicious/current/active

cleanup_repo

ok "app with no xa.metadata and no metadata in summary can't be installed"

create_app "invalid"

if ${FLATPAK} ${U} install -y test-repo org.test.Malicious 2>install-error-log; then
    assert_not_reached "Should not be able to install app with invalid metadata"
fi
assert_file_has_content install-error-log "Metadata for .* is invalid"

assert_not_has_dir $FL_DIR/app/org.test.Malicious/current/active

cleanup_repo

ok "app with invalid metadata (in summary) can't be installed"

create_app "invalid no-cache-in-summary"

if ${FLATPAK} ${U} install -y test-repo org.test.Malicious 2>install-error-log; then
    assert_not_reached "Should not be able to install app with invalid metadata"
fi
# In versions that didn't support sideloaded repositories (1.7.0 or older),
# missing metadata in the cache was tolerated, but then we'd load the
# invalid metadata and find that it was invalid.
sed -e 's/^/## /' < install-error-log >&2
assert_file_has_content install-error-log "Can't find .* metadata for dependencies"
assert_file_has_content install-error-log "not a key-value pair, group or comment"

assert_not_has_dir $FL_DIR/app/org.test.Malicious/current/active

cleanup_repo

ok "app with invalid metadata (in commit) can't be installed"

create_app "mismatch no-cache-in-summary"

if ${FLATPAK} ${U} install -y test-repo org.test.Malicious 2>install-error-log; then
    assert_not_reached "Should not be able to install app with non-matching metadata"
fi
assert_file_has_content install-error-log "Commit metadata for .* not matching expected metadata"

assert_not_has_dir $FL_DIR/app/org.test.Malicious/current/active

cleanup_repo

ok "app with mismatched metadata (in commit) can't be installed"

create_app "mismatch"

if ${FLATPAK} ${U} install -y test-repo org.test.Malicious 2>install-error-log; then
    assert_not_reached "Should not be able to install app with non-matching metadata"
fi
assert_file_has_content install-error-log "Commit metadata for .* not matching expected metadata"

assert_not_has_dir $FL_DIR/app/org.test.Malicious/current/active

cleanup_repo

ok "app with mismatched metadata (in summary) can't be installed"
