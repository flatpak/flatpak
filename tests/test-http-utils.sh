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

$(dirname $0)/test-webserver.sh "" "python $test_srcdir/http-utils-test-server.py 0"
FLATPAK_HTTP_PID=$(cat httpd-pid)
mv httpd-port httpd-port-main
port=$(cat httpd-port-main)

assert_result() {
    test_string=$1
    compressed=
    if [ "$2" = "--compressed" ] ; then
	compressed="--compressed"
	shift
    fi
    remote=$2
    local=$3

    out=`httpcache $compressed "http://localhost:$port$remote" $local || :`

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
    assert_result "Server returned status 304:" $@
}

assert_ok() {
    assert_result "Server returned status 200:" $@
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

echo 'ok http cache lifetimes'

# Revalation with an etag
assert_ok "/?etag&no-cache" $test_tmpdir/output
assert_304 "/?etag&no-cache" $test_tmpdir/output
rm -f $test_tmpdir/output*

# Revalation with an modified time
assert_ok "/?modified-time&no-cache" $test_tmpdir/output
assert_304 "/?modified-time&no-cache" $test_tmpdir/output
rm -f $test_tmpdir/output*

echo 'ok http revalidation'

# Test compressd downloading and storage
assert_ok --compressed "/compress" $test_tmpdir/output
contents=$(gunzip -c < $test_tmpdir/output)
assert_streq $contents path=/compress
rm -f $test_tmpdir/output*

echo 'ok compressed download'

# Test uncompressed downloading with compressed storage
assert_ok --compressed "/compress?ignore-accept-encoding" $test_tmpdir/output
contents=$(gunzip -c < $test_tmpdir/output)
assert_streq $contents path=/compress?ignore-accept-encoding
rm -f $test_tmpdir/output*

echo 'ok compress after download'

# Testing that things work with without xattr support

if have_xattrs $test_tmpdir ; then
    assert_ok "/?etag&no-cache" $test_tmpdir/output
    assert_not_has_file $test_tmpdir/output.flatpak.http
    assert_304 "/?etag&no-cache" $test_tmpdir/output
    rm -f $test_tmpdir/output*
    echo "ok with-xattrs"
else
    echo "ok with-xattrs # skip /var/tmp doesn't have user xattr support"
fi

# Testing fallback without xattr support

no_xattrs_tempdir=`mktemp -d /tmp/test-flatpak-XXXXXX`
no_xattrs_cleanup () {
    rm -rf test_tmpdir
    cleanup
}
trap no_xattrs_cleanup EXIT

if have_xattrs $no_xattrs_tempdir ; then
    echo "ok no-xattrs # skip /tmp has user xattr support"
else
    assert_ok "/?etag&no-cache" $no_xattrs_tempdir/output
    assert_has_file $no_xattrs_tempdir/output.flatpak.http
    assert_304 "/?etag&no-cache" $no_xattrs_tempdir/output
    rm -f $no_xattrs_tempdir/output*
    echo "ok no-xattrs"
fi
