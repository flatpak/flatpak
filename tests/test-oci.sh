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

${FLATPAK} build-bundle --oci repos/test oci-dir org.test.Hello

assert_has_file oci-dir/oci-layout
assert_has_dir oci-dir/blobs/sha256
assert_has_dir oci-dir/refs
assert_file_has_content oci-dir/refs/latest "application/vnd.oci.image.manifest.v1+json"

for i in oci-dir/blobs/sha256/*; do
     echo $(basename $i) $i >> sums
done
sha256sum -c sums

echo "ok export oci"

ostree --repo=repo2 init --mode=archive-z2

$FLATPAK build-import-bundle --oci repo2 oci-dir

ostree checkout -U --repo=repo2 app/org.test.Hello/$ARCH/master checked-out

assert_has_dir checked-out/files
assert_has_file checked-out/files/bin/hello.sh
assert_has_file checked-out/metadata

echo "ok commit oci"

${FLATPAK} install ${U} --oci oci-dir latest

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

echo "ok install oci"

make_updated_app
${FLATPAK} build-bundle --oci repos/test oci-dir org.test.Hello

${FLATPAK} update ${U} org.test.Hello
run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATED$'

echo "ok update oci"

flatpak uninstall  ${U} org.test.Hello

make_updated_app HTTP
${FLATPAK} build-bundle --oci repos/test oci-dir org.test.Hello

ostree trivial-httpd --autoexit --daemonize -p oci-port `pwd`/oci-dir
ociport=$(cat oci-port)

${FLATPAK} install -v ${U} --oci http://127.0.0.1:${ociport} latest

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxHTTP$'

echo "ok install oci http"

make_updated_app UPDATEDHTTP
${FLATPAK} build-bundle --oci repos/test oci-dir org.test.Hello

${FLATPAK} update ${U} org.test.Hello
run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATEDHTTP$'

echo "ok update oci http"
