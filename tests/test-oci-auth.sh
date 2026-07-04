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

echo "1..1"

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

# Test: install from an auth-protected OCI registry
#
# The registry requires a bearer token for all /v2/ requests.  The client
# authenticates via the mock authenticator and completes the installation
# successfully.

echo -n "token-1" > "${XDG_RUNTIME_DIR}/required-token"
$client configure-auth --token token-1

${FLATPAK} ${U} install -y oci-auth-registry org.test.Hello >&2

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

ok "install from auth-protected OCI registry"
