#!/bin/bash
#
# Copyright (C) 2025 Red Hat, Inc
# Copyright (C) 2026 Collabora Ltd.
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

# shellcheck source=tests/libtest.sh
. "$(dirname "$0")/libtest.sh"

skip_without_bwrap

REPONAME="test"
BRANCH="master"
COLLECTION_ID="org.test.Collection.${REPONAME}"

setup_repo ${REPONAME} ${COLLECTION_ID}

# create the extra data
EXTRA_DATA_FILE="extra-data-test"
EXTRA_DATA_DIR="${TEST_DATA_DIR}/extra-data-server/"
mkdir -p "${EXTRA_DATA_DIR}"
echo "extra-data-test-content" > "${EXTRA_DATA_DIR}/${EXTRA_DATA_FILE}"

# serve the extra data
httpd web-server.py "${EXTRA_DATA_DIR}"
EXTRA_DATA_URL="http://127.0.0.1:$(cat httpd-port)/${EXTRA_DATA_FILE}"

# download to get the size and sha256 sum
DOWNLOADED_EXTRA_DATA="${TEST_DATA_DIR}/downloaded-extra-data"
curl "${EXTRA_DATA_URL}" -o "${DOWNLOADED_EXTRA_DATA}"
EXTRA_DATA_SIZE=$(stat --printf="%s" "${DOWNLOADED_EXTRA_DATA}")
EXTRA_DATA_SHA256=$(sha256sum "${DOWNLOADED_EXTRA_DATA}" | cut -f1 -d' ')

flatpak_maybe_user() {
    # shellcheck disable=SC2086
    ${FLATPAK} ${U} "$@"
}

build_extra_data_noruntime_app() {
    local repo="$1"
    local branch="$2"

    DIR="$(mktemp -d)"
    ARCH="$(flatpak --default-arch)"

    mkdir -p "${DIR}/files/bin"

    cp "${G_TEST_BUILDDIR}/apply-extra-deeply-nested-setuid" "${DIR}/files/bin/apply_extra"
    chmod +x "${DIR}/files/bin/apply_extra"

    cat > "${DIR}/metadata" <<EOF
[Application]
name=org.test.ExtraDataNoRuntime
runtime=org.test.Platform/${ARCH}/${branch}

[Extra Data]
NoRuntime=true
uri=${EXTRA_DATA_URL}
size=${EXTRA_DATA_SIZE}
checksum=${EXTRA_DATA_SHA256}
EOF

    flatpak build-finish "${DIR}" >&2
    # shellcheck disable=SC2086
    flatpak build-export ${FL_GPGARGS} "${repo}" "${DIR}" "${branch}" >&2
    rm -rf "${DIR}"
}

if test -f "${G_TEST_BUILDDIR}/apply-extra-deeply-nested-setuid"; then
    build_extra_data_noruntime_app repos/test "${BRANCH}"
    update_repo "${REPONAME}" "${COLLECTION_ID}"

    if flatpak_maybe_user install --or-update -y "${REPONAME}-repo" org.test.ExtraDataNoRuntime "${BRANCH}" >&2; then
        DEPLOY_DIR="$(flatpak_maybe_user info --show-location org.test.ExtraDataNoRuntime)"
        ( find "${DEPLOY_DIR}/files/extra" -not -type d -ls || true ) | sed -e 's/^/# /' >&2

        # There must not be any setuid or setgid files in the deploy dir
        find "${DEPLOY_DIR}/files/extra" -type f -perm -4000 > setuid-files
        assert_not_file_has_content setuid-files .
        find "${DEPLOY_DIR}/files/extra" -type f -perm -2000 > setgid-files
        assert_not_file_has_content setgid-files .

        # `flatpak uninstall` won't necessarily be able to recover by
        # deleting the directory that was so deeply nested, because the
        # directory is also so deeply nested that we run out of fd limit
        # while removing it
        rm -fr "${DEPLOY_DIR}/files/extra"

        flatpak_maybe_user uninstall -y org.test.ExtraDataNoRuntime >&2

        ok "extra-data app with deeply nested setuid binary was disarmed"
    else
        app_dir="${FL_DIR}/app/org.test.ExtraDataNoRuntime"
        find "${app_dir}" -not -type d -ls | sed -e 's/^/# /' >&2

        # This filename pattern needs to be kept in sync with the
        # choice of deploy directory mkdtempat template in
        # flatpak_dir_deploy()
        for d in "${app_dir}/${ARCH}/${BRANCH}"/.*-*/files/extra; do
            assert_file_has_mode "$d" 700

            # All files for which it was feasible had their permissions reset
            if [ -e "$d/exe" ]; then
                assert_file_has_mode "$d/exe" 755
            fi
            if [ -e "$d/extra-data-test" ]; then
                assert_file_has_mode "$d/extra-data-test" 644
            fi
        done

        rm -fr "${app_dir}"

        ok "extra-data app with deeply nested setuid binary refused to install"
    fi
else
    ok "# SKIP install+run extra data app with NoRuntime apply-extra-deeply-nested-setuid not found"
fi

done_testing
