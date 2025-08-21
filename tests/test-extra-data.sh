#!/bin/bash
#
# Copyright (C) 2025 Red Hat, Inc
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

. "$(dirname $0)/libtest.sh"

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

echo "1..2"

# build the app with the extra data
EXTRA_DATA="--extra-data=test:${EXTRA_DATA_SHA256}:${EXTRA_DATA_SIZE}:${EXTRA_DATA_SIZE}:${EXTRA_DATA_URL}"
BUILD_FINISH_ARGS=${EXTRA_DATA} make_updated_app ${REPONAME} ${COLLECTION_ID} ${BRANCH} UPDATE1

# ensure it installs correctly
install_repo ${REPONAME} ${BRANCH}

# ensure the right extra-data got downloaded
${FLATPAK} run --command=sh org.test.Hello -c "cat /app/extra/test" > out
assert_file_has_content out "extra-data-test-content"

${FLATPAK} ${U} uninstall -y org.test.Hello >&2

ok "install extra data app with ostree"

# Start the fake registry server

httpd oci-registry-server.py --dir=.
port=$(cat httpd-port)
scheme=http

client="python3 $test_srcdir/oci-registry-client.py --url=${scheme}://127.0.0.1:${port}"

# Add OCI bundles to it

${FLATPAK} build-bundle --runtime --oci $FL_GPGARGS repos/test oci/platform-image org.test.Platform >&2
$client add platform latest "$(pwd)/oci/platform-image"

${FLATPAK} build-bundle --oci $FL_GPGARGS repos/test oci/app-image org.test.Hello >&2
$client add hello latest "$(pwd)/oci/app-image"

# Add an OCI remote

${FLATPAK} remote-add ${U} oci-registry "oci+${scheme}://127.0.0.1:${port}" >&2

# Check that the images we expect are listed

images=$(${FLATPAK} remote-ls ${U} --columns=app oci-registry | sort | tr '\n' ' ' | sed 's/ $//')
assert_streq "$images" "org.test.Hello org.test.Platform"

${FLATPAK} ${U} install -y oci-registry org.test.Hello >&2

# ensure the right extra-data got downloaded
${FLATPAK} run --command=sh org.test.Hello -c "cat /app/extra/test" > out
assert_file_has_content out "extra-data-test-content"

${FLATPAK} ${U} uninstall -y org.test.Hello >&2

ok "install extra data app with oci"