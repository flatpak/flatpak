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

echo "1..2"

setup_repo_no_add oci

mkdir -p oci
${FLATPAK} build-bundle --oci $FL_GPGARGS repos/oci oci/image org.test.Hello

assert_has_file oci/image/oci-layout
assert_has_dir oci/image/blobs/sha256
assert_has_file oci/image/index.json

for i in oci/image/blobs/sha256/*; do
     echo $(basename $i) $i >> sums
done
sha256sum -c sums

digest=$(grep sha256: oci/image/index.json | sed s'@.*sha256:\([a-fA-F0-9]\+\).*@\1@')
manifest=oci/image/blobs/sha256/$digest

assert_has_file $manifest

DIGEST=$(grep -C2 application/vnd.oci.image.config.v1+json $manifest | grep digest  | sed s/.*\"sha256:\\\(.*\\\)\".*/\\1/)
echo DIGEST: $DIGEST
image=oci/image/blobs/sha256/$DIGEST

assert_has_file $image
assert_file_has_content $image "org\.freedesktop\.appstream\.appdata.*<summary>Print a greeting</summary>"
assert_file_has_content $image "org\.freedesktop\.appstream\.icon-64"
assert_file_has_content $image org.flatpak.ref.*app/org.test.Hello/x86_64/master

ok "export oci"

ostree --repo=repo2 init --mode=archive-z2

$FLATPAK build-import-bundle --oci repo2 oci/image

ostree checkout -U --repo=repo2 app/org.test.Hello/$ARCH/master checked-out

assert_has_dir checked-out/files
assert_has_file checked-out/files/bin/hello.sh
assert_has_file checked-out/metadata

ok "import oci"
