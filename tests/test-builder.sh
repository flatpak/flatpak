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

echo "1..4"

setup_repo
install_repo
setup_sdk_repo
install_sdk_repo

# Need /var/tmp cwd for xattrs
REPO=`pwd`/repos/test
cd $TEST_DATA_DIR/

cp -a $(dirname $0)/test-configure .
echo "version1" > app-data
cp $(dirname $0)/test.json .
cp $(dirname $0)/test-runtime.json .
cp $(dirname $0)/0001-Add-test-logo.patch .
mkdir include1
cp $(dirname $0)/module1.json include1/
cp $(dirname $0)/data1 include1/
cp $(dirname $0)/data1.patch include1/
mkdir include1/include2
cp $(dirname $0)/module2.json include1/include2/
cp $(dirname $0)/data2 include1/include2/
cp $(dirname $0)/data2.patch include1/include2/
${FLATPAK_BUILDER} --repo=$REPO $FL_GPGARGS --force-clean appdir test.json

assert_file_has_content appdir/files/share/app-data version1
assert_file_has_content appdir/metadata shared=network;
assert_file_has_content appdir/metadata tags=test;
assert_file_has_content appdir/files/ran_module1 module1
assert_file_has_content appdir/files/ran_module2 module2

assert_not_has_file appdir/files/cleanup/a_filee
assert_not_has_file appdir/files/bin/file.cleanup

assert_has_file appdir/files/cleaned_up > out
assert_has_file appdir/files/share/icons/org.test.Hello.png

assert_file_has_content appdir/files/out '^foo$'
assert_file_has_content appdir/files/out2 '^foo2$'

${FLATPAK} build appdir /app/bin/hello2.sh > hello_out2
assert_file_has_content hello_out2 '^Hello world2, from a sandbox$'

echo "ok build"

${FLATPAK} ${U} install test-repo org.test.Hello2 master
run org.test.Hello2 > hello_out3
assert_file_has_content hello_out3 '^Hello world2, from a sandbox$'

run --command=cat org.test.Hello2 /app/share/app-data > app_data_1
assert_file_has_content app_data_1 version1

echo "ok install+run"

echo "version2" > app-data
${FLATPAK_BUILDER} $FL_GPGARGS --repo=$REPO --force-clean appdir test.json
assert_file_has_content appdir/files/share/app-data version2

${FLATPAK} ${U} update org.test.Hello2 master

run --command=cat org.test.Hello2 /app/share/app-data > app_data_2
assert_file_has_content app_data_2 version2

echo "ok update"

# The build-args of --help should prevent the faulty cleanup and
# platform-cleanup commands from executing
${FLATPAK_BUILDER} $FL_GPGARGS --repo=$REPO --force-clean runtimedir \
    test-runtime.json

echo "ok runtime build cleanup with build-args"
