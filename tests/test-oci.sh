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

export FLATPAK_ENABLE_EXPERIMENTAL_OCI=1

skip_without_bwrap
[ x${USE_SYSTEMDIR-} != xyes ] || skip_without_user_xattrs

echo "1..2"

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

digest=$(grep sha256: oci/registry/index.json | sed s'@.*sha256:\([a-fA-F0-9]\+\).*@\1@')
manifest=oci/registry/blobs/sha256/$digest

assert_has_file $manifest
assert_file_has_content $manifest "org.freedesktop.appstream.appdata.*<summary>Print a greeting</summary>"
assert_file_has_content $manifest "org.freedesktop.appstream.icon-64"

echo "ok export oci"

ostree --repo=repo2 init --mode=archive-z2

$FLATPAK build-import-bundle --oci repo2 oci/registry

ostree checkout -U --repo=repo2 app/org.test.Hello/$ARCH/master checked-out

assert_has_dir checked-out/files
assert_has_file checked-out/files/bin/hello.sh
assert_has_file checked-out/metadata

echo "ok import oci"
