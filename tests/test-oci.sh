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
skip_without_user_xattrs

echo "1..6"

setup_repo

${FLATPAK} ${U} install test-repo org.test.Platform master

mkdir -p oci
${FLATPAK} build-bundle --oci $FL_GPGARGS repos/test oci/registry org.test.Hello

assert_has_file oci/registry/oci-layout
assert_has_dir oci/registry/blobs/sha256
assert_has_file oci/registry/index.json

for i in oci/registry/blobs/sha256/*; do
     echo $(basename $i) $i >> sums
done
sha256sum -c sums

echo "ok export oci"

ostree --repo=repo2 init --mode=archive-z2

$FLATPAK build-import-bundle --oci repo2 oci/registry

ostree checkout -U --repo=repo2 app/org.test.Hello/$ARCH/master checked-out

assert_has_dir checked-out/files
assert_has_file checked-out/files/bin/hello.sh
assert_has_file checked-out/metadata

echo "ok commit oci"

${FLATPAK} remote-add ${U} --oci --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg oci-remote oci/registry
${FLATPAK} install ${U} -v oci-remote org.test.Hello

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

echo "ok install oci"

sleep 1 # Make sure the index.json mtime is changed
make_updated_app
${FLATPAK} build-bundle -v --oci $FL_GPGARGS repos/test oci/registry org.test.Hello

${FLATPAK} update ${U} -v org.test.Hello
run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATED$'

echo "ok update oci"

flatpak uninstall  ${U} org.test.Hello

make_updated_app HTTP
${FLATPAK} build-bundle --oci $FL_GPGARGS repos/test oci/registry org.test.Hello

$(dirname $0)/test-webserver.sh `pwd`/oci
ociport=$(cat httpd-port)
FLATPAK_HTTP_PID="${FLATPAK_HTTP_PID} $(cat httpd-pid)"

${FLATPAK} remote-add ${U} --oci --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg oci-remote-http http://127.0.0.1:${ociport}/registry
${FLATPAK} install -v ${U} oci-remote-http org.test.Hello

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxHTTP$'

echo "ok install oci http"

make_updated_app UPDATEDHTTP
${FLATPAK} build-bundle --oci $FL_GPGARGS repos/test oci/registry org.test.Hello

${FLATPAK} update ${U} org.test.Hello
run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATEDHTTP$'

echo "ok update oci http"
