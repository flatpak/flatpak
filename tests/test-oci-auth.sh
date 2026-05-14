#!/bin/bash
#
# Copyright (C) 2026 Red Hat, Inc.
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

# shellcheck source=libtest.sh
. "$(dirname "$0")/libtest.sh"

skip_without_bwrap

echo "1..6"

# Start the fake OCI registry server

httpd oci-registry-server.py --dir=.
port=$(cat httpd-port)

# shellcheck disable=SC2154
client="python3 ${test_srcdir}/oci-registry-client.py --url=http://127.0.0.1:${port}"

setup_repo_no_add oci

# Build OCI bundles and upload them to the registry

${FLATPAK} build-bundle --runtime --oci $FL_GPGARGS repos/oci oci/platform-image org.test.Platform >&2
$client add platform latest "$(pwd)/oci/platform-image"

${FLATPAK} build-bundle --oci $FL_GPGARGS repos/oci oci/app-image org.test.Hello >&2
$client add hello latest "$(pwd)/oci/app-image"

# Add OCI remote configured to use the mock test authenticator.
# The default authenticator for OCI remotes is org.flatpak.Authenticator.Oci;
# we override it to use the mock authenticator that reads its token from
# $XDG_RUNTIME_DIR/required-token on every call.

${FLATPAK} remote-add ${U} oci-auth-registry "oci+http://127.0.0.1:${port}" \
    --authenticator-name org.flatpak.Authenticator.test >&2

# Test: install from an auth-protected registry

echo -n "token-1" > "${XDG_RUNTIME_DIR}/required-token"
$client set-auth --token token-1

${FLATPAK} ${U} install -y oci-auth-registry org.test.Hello >&2

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

ok "install"

# Test: token expires during install on manifest fetch

$client reset-auth
${FLATPAK} ${U} uninstall -y org.test.Hello >&2

echo -n "token-1" > "${XDG_RUNTIME_DIR}/required-token"
$client set-auth --token token-1 \
    --expire-on-path '/v2/*/manifests/*' \
    --next-token token-2 \
    --token-update-file "${XDG_RUNTIME_DIR}/required-token"

${FLATPAK} ${U} install -y oci-auth-registry org.test.Hello >&2

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

ok "install with token expiry on manifest fetch"

# Test: token expires during install on blob fetch

$client reset-auth
${FLATPAK} ${U} uninstall -y org.test.Hello >&2

echo -n "token-1" > "${XDG_RUNTIME_DIR}/required-token"
$client set-auth --token token-1 \
    --expire-on-path '/v2/*/blobs/*' \
    --next-token token-2 \
    --token-update-file "${XDG_RUNTIME_DIR}/required-token"

${FLATPAK} ${U} install -y oci-auth-registry org.test.Hello >&2

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

ok "install with token expiry on blob fetch"

# Test: update from an auth-protected registry

$client reset-auth
make_updated_app oci "" "" UPDATE

${FLATPAK} build-bundle --oci $FL_GPGARGS repos/oci oci/app-image org.test.Hello >&2
$client add hello latest "$(pwd)/oci/app-image"

echo -n "token-1" > "${XDG_RUNTIME_DIR}/required-token"
$client set-auth --token token-1

${FLATPAK} ${U} update -y org.test.Hello >&2

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATE$'

ok "update"

# Test: token expires during update on manifest fetch

$client reset-auth
make_updated_app oci "" "" UPDATE1

${FLATPAK} build-bundle --oci $FL_GPGARGS repos/oci oci/app-image org.test.Hello >&2
$client add hello latest "$(pwd)/oci/app-image"

echo -n "token-1" > "${XDG_RUNTIME_DIR}/required-token"
$client set-auth --token token-1 \
    --expire-on-path '/v2/*/manifests/*' \
    --next-token token-2 \
    --token-update-file "${XDG_RUNTIME_DIR}/required-token"

${FLATPAK} ${U} update -y org.test.Hello >&2

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATE1$'

ok "update with token expiry on manifest fetch"

# Test: token expires during update on blob fetch

$client reset-auth
make_updated_app oci "" "" UPDATE2

${FLATPAK} build-bundle --oci $FL_GPGARGS repos/oci oci/app-image org.test.Hello >&2
$client add hello latest "$(pwd)/oci/app-image"

echo -n "token-1" > "${XDG_RUNTIME_DIR}/required-token"
$client set-auth --token token-1 \
    --expire-on-path '/v2/*/blobs/*' \
    --next-token token-2 \
    --token-update-file "${XDG_RUNTIME_DIR}/required-token"

${FLATPAK} ${U} update -y org.test.Hello >&2

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATE2$'

ok "update with token expiry on blob fetch"
