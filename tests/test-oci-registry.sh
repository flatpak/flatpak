#!/bin/bash
#
# Copyright (C) 2018 Red Hat, Inc.
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

echo "1..13"

# Start the fake registry server

$(dirname $0)/test-webserver.sh "" "python3 $test_srcdir/oci-registry-server.py 0"
FLATPAK_HTTP_PID=$(cat httpd-pid)
mv httpd-port httpd-port-main
port=$(cat httpd-port-main)
client="python3 $test_srcdir/oci-registry-client.py 127.0.0.1:$port"

setup_repo_no_add oci

# Add OCI bundles to it

${FLATPAK} build-bundle --runtime --oci $FL_GPGARGS repos/oci oci/platform-image org.test.Platform
$client add platform latest $(pwd)/oci/platform-image

${FLATPAK} build-bundle --oci $FL_GPGARGS repos/oci oci/app-image org.test.Hello
$client add hello latest $(pwd)/oci/app-image

# Add an OCI remote

${FLATPAK} remote-add ${U} oci-registry "oci+http://127.0.0.1:${port}"

# Check that the images we expect are listed

images=$(${FLATPAK} remote-ls ${U} --columns=app oci-registry | sort | tr '\n' ' ' | sed 's/ $//')
assert_streq "$images" "org.test.Hello org.test.Platform"
echo "ok list remote"

# Pull appstream data

${FLATPAK} update ${U} --appstream oci-registry

# Check that the appstream and icons exist

if [ x${USE_SYSTEMDIR-} == xyes ] ; then
    appstream=$SYSTEMDIR/appstream/oci-registry/$ARCH/appstream.xml.gz
    icondir=$SYSTEMDIR/appstream/oci-registry/$ARCH/icons
else
    appstream=$USERDIR/appstream/oci-registry/$ARCH/appstream.xml.gz
    icondir=$USERDIR/appstream/oci-registry/$ARCH/icons
fi

gunzip -c $appstream > appstream-uncompressed
assert_file_has_content appstream-uncompressed '<id>org.test.Hello.desktop</id>'
assert_has_file $icondir/64x64/org.test.Hello.png

echo "ok appstream"

# Test that 'flatpak search' works
${FLATPAK} search org.test.Hello > search-results
assert_file_has_content search-results "Print a greeting"

echo "ok search"

# Replace with the app image with detached icons, check that the icons work

old_icon_hash=(md5sum $icondir/64x64/org.test.Hello.png)
rm $icondir/64x64/org.test.Hello.png
$client delete hello latest
$client  add --detach-icons hello latest $(pwd)/oci/app-image
${FLATPAK} update ${U} --appstream oci-registry
assert_has_file $icondir/64x64/org.test.Hello.png
new_icon_hash=(md5sum $icondir/64x64/org.test.Hello.png)
assert_streq $old_icon_hash $new_icon_hash

echo "ok detached icons"

# Try installing from the remote

${FLATPAK} ${U} install -y oci-registry org.test.Platform
echo "ok install"

# Remove the app from the registry, check that things were removed properly

$client delete hello latest

images=$(${FLATPAK} remote-ls ${U} --columns=app oci-registry | sort | tr '\n' ' ' | sed 's/ $//')
assert_streq "$images" "org.test.Platform"

${FLATPAK} update ${U} --appstream oci-registry

assert_not_file_has_content $appstream '<id>org.test.Hello.desktop</id>'
assert_not_has_file $icondir/64x64/org.test.Hello.png
assert_not_has_file $icondir/64x64

echo "ok appstream change"

# Change the remote to a non-OCI remote, check that we cleaned up

if [ x${USE_SYSTEMDIR-} == xyes ] ; then
    base=$SYSTEMDIR
else
    base=$USERDIR
fi

assert_has_file $base/oci/oci-registry.index.gz
assert_has_file $base/oci/oci-registry.summary
assert_has_dir $base/appstream/oci-registry
${FLATPAK} remote-modify ${U} --url=http://127.0.0.1:${port} oci-registry
assert_not_has_file $base/oci/oci-registry.index.gz
assert_not_has_file $base/oci/oci-registry.summary
assert_not_has_dir $base/appstream/oci-registry

echo "ok change remote to non-OCI"

# Change it back and refetch

${FLATPAK} remote-modify ${U} --url=oci+http://127.0.0.1:${port} oci-registry
${FLATPAK} update ${U} --appstream oci-registry

# Delete the remote, check that everything was removed

assert_has_file $base/oci/oci-registry.index.gz
assert_has_file $base/oci/oci-registry.summary
assert_has_dir $base/appstream/oci-registry
${FLATPAK} ${U} -y uninstall org.test.Platform
${FLATPAK} ${U} remote-delete oci-registry
assert_not_has_file $base/oci/oci-registry.index.gz
assert_not_has_file $base/oci/oci-registry.summary
assert_not_has_dir $base/appstream/oci-registry

echo "ok delete remote"

# Try installing the platform via a flatpakref file.

cat << EOF > org.test.Platform.flatpakref
[Flatpak Ref]
Title=Test Platform
Name=org.test.Platform
Branch=master
Url=oci+http://127.0.0.1:${port}
IsRuntime=true
EOF

${FLATPAK} ${U} install -y --from ./org.test.Platform.flatpakref

${FLATPAK} remotes > remotes-list
assert_file_has_content remotes-list '^platform-origin'

assert_has_file $base/oci/platform-origin.index.gz

echo "ok install via flatpakref"

# Uninstall, check that the origin remote was pruned, and files were
# cleaned up properly

${FLATPAK} ${U} -y uninstall org.test.Platform

${FLATPAK} remotes > remotes-list
assert_not_file_has_content remotes '^platform-origin'

assert_not_has_file $base/oci/platform-origin.index.gz

echo "ok prune origin remote"

# Install from a (non-OCI) bundle, check that the repo-url is respected

${FLATPAK} build-bundle --runtime --repo-url "oci+http://127.0.0.1:${port}" $FL_GPGARGS repos/oci org.test.Platform.flatpak org.test.Platform

${FLATPAK} ${U} install -y --bundle org.test.Platform.flatpak

${FLATPAK} remotes -d > remotes-list
assert_file_has_content remotes-list "^platform-origin.*[ 	]oci+http://127.0.0.1:${port}"

assert_has_file $base/oci/platform-origin.index.gz

echo "ok install via bundle"

# Install an app from a bundle

${FLATPAK} build-bundle --repo-url "oci+http://127.0.0.1:${port}" $FL_GPGARGS repos/oci org.test.Hello.flatpak org.test.Hello

${FLATPAK} ${U} install -y --bundle org.test.Hello.flatpak

${FLATPAK} remotes -d > remotes-list
assert_file_has_content remotes-list "^hello-origin.*[ 	]oci+http://127.0.0.1:${port}"

assert_has_file $base/oci/hello-origin.index.gz

echo "ok app install via bundle"

# Install an updated app bundle with a different origin

make_updated_app oci
${FLATPAK} build-bundle --repo-url "http://127.0.0.1:${port}" $FL_GPGARGS repos/oci org.test.Hello.flatpak org.test.Hello

${FLATPAK} ${U} install -y --bundle org.test.Hello.flatpak

${FLATPAK} remotes -d > remotes-list
assert_file_has_content remotes-list "^hello-origin.*[ 	]http://127.0.0.1:${port}"

assert_not_has_file $base/oci/hello-origin.index.gz

echo "ok change remote origin via bundle"
