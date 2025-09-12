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

httpd http-utils-test-server.py .
port=$(cat httpd-port)

assert_result() {
    test_string=$1
    compressed=
    if [ "$2" = "--compressed" ] ; then
	compressed="--compressed"
	shift
    fi
    remote=$2
    local=$3

    out=`${test_builddir}/httpcache $compressed "http://localhost:$port$remote" $local || :`

    case "$out" in
	$test_string*)
	    return
	    ;;
	*)
	    echo "For $remote => $local, expected '$test_string', got '$out'"
	    exit 1
	    ;;
    esac
}

assert_cached() {
    assert_result "Reusing cached value" $@
}

assert_304() {
    assert_result "Server returned status 304" $@
}

assert_ok() {
    assert_result "Server returned status 200" $@
}


have_xattrs() {
    touch $1/test-xattrs
    setfattr -n user.testvalue -v somevalue $1/test-xattrs > /dev/null 2>&1
}

echo "1..6"

# Without anything else, cached for 30 minutes
assert_ok "/" $test_tmpdir/output
assert_cached "/" $test_tmpdir/output
rm -f $test_tmpdir/output*

# An explicit cache-lifetime
assert_ok "/?max-age=3600" $test_tmpdir/output
assert_cached "/?max-age=3600" $test_tmpdir/output
rm -f $test_tmpdir/output*

# Turn off caching
assert_ok "/?max-age=0" $test_tmpdir/output
assert_ok "/?max-age=0" $test_tmpdir/output
rm -f $test_tmpdir/output*

# Turn off caching a different way
assert_ok "/?no-cache" $test_tmpdir/output
assert_ok "/?no-cache" $test_tmpdir/output
rm -f $test_tmpdir/output*

# Expires support
assert_ok "/?expires-future" $test_tmpdir/output
assert_cached "/?expires-future" $test_tmpdir/output
rm -f $test_tmpdir/output*

assert_ok "/?expires-past" $test_tmpdir/output
assert_ok "/?expires-past" $test_tmpdir/output
rm -f $test_tmpdir/output*

ok 'http cache lifetimes'

# Revalidation with an etag
assert_ok "/?etag&no-cache" $test_tmpdir/output
assert_304 "/?etag&no-cache" $test_tmpdir/output
rm -f $test_tmpdir/output*

# Revalidation with a modified time
assert_ok "/?modified-time&no-cache" $test_tmpdir/output
assert_304 "/?modified-time&no-cache" $test_tmpdir/output
rm -f $test_tmpdir/output*

ok 'http revalidation'

# Test compressed downloading and storage
assert_ok --compressed "/compress" $test_tmpdir/output
contents=$(gunzip -c < $test_tmpdir/output)
assert_streq $contents path=/compress
rm -f $test_tmpdir/output*

ok 'compressed download'

# Test uncompressed downloading with compressed storage
assert_ok --compressed "/compress?ignore-accept-encoding" $test_tmpdir/output
contents=$(gunzip -c < $test_tmpdir/output)
assert_streq $contents path=/compress?ignore-accept-encoding
rm -f $test_tmpdir/output*

ok 'compress after download'

# Testing that things work without xattr support

if command -v setfattr >/dev/null &&
   ! have_xattrs $test_tmpdir ; then
    assert_ok "/?etag&no-cache" $test_tmpdir/output
    assert_has_file $test_tmpdir/output.flatpak.http
    assert_304 "/?etag&no-cache" $test_tmpdir/output
    rm -f $test_tmpdir/output*
    ok "no-xattrs"
else
    ok "no-xattrs # skip No setfattr or /tmp doesn't have user xattr support"
fi

# Testing with xattr support

xattrs_tempdir=`mktemp -d /var/tmp/test-flatpak-XXXXXX`
xattrs_cleanup () {
    rm -rf xattrs_tempdir
    cleanup
}
trap xattrs_cleanup EXIT

if command -v setfattr >/dev/null &&
   have_xattrs $xattrs_tempdir ; then
    assert_ok "/?etag&no-cache" $xattrs_tempdir/output
    assert_not_has_file $xattrs_tempdir/output.flatpak.http
    assert_304 "/?etag&no-cache" $xattrs_tempdir/output
    rm -f $xattrs_tempdir/output*
    ok "xattrs"
else
    ok "xattrs # skip No setfattr or /var/tmp has user no xattr support"
fi
