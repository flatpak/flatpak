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

# shellcheck source=tests/libtest.sh
. $(dirname $0)/libtest.sh

httpd http-utils-test-server.py .
port=$(cat httpd-port)
httpcache=("${test_builddir}/httpcache")

assert_result() {
    test_string=$1
    compressed=
    if [ "$2" = "--compressed" ] ; then
	compressed="--compressed"
	shift
    fi
    remote=$2
    local=$3

    out=`"${httpcache[@]}" $compressed "http://localhost:$port$remote" $local || :`

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

# A dropped response must be retried without retaining its partial contents.
for path in /empty-reply /partial-reply; do
    assert_ok "$path" $test_tmpdir/output
    contents=$(cat $test_tmpdir/output)
    assert_streq "$contents" "path=$path"
    rm -f $test_tmpdir/output*
done

assert_ok --compressed "/partial-reply?compressed" $test_tmpdir/output
contents=$(gunzip -c < $test_tmpdir/output)
assert_streq "$contents" "path=/partial-reply?compressed"
rm -f $test_tmpdir/output*

ok 'retry dropped responses'

# Make httpcache reject xattrs even when the filesystem supports them.
httpcache=(env FLATPAK_TEST_NO_XATTRS=1 "${test_builddir}/httpcache")
assert_ok "/?etag&no-cache" $test_tmpdir/output
assert_has_file $test_tmpdir/output.flatpak.http
assert_304 "/?etag&no-cache" $test_tmpdir/output
rm -f $test_tmpdir/output*
httpcache=("${test_builddir}/httpcache")
ok "no-xattrs"

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

cat > openssl.config <<EOF
[req]
distinguished_name=default_dn

[v3_ca]
basicConstraints=critical,CA:TRUE,pathlen:0

[server_cert]
basicConstraints=CA:FALSE
subjectAltName=IP:127.0.0.1

[usr_cert]
basicConstraints=CA:FALSE
keyUsage=digitalSignature
extendedKeyUsage=clientAuth

[default_dn]
CN=Unused
EOF

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 \
    -nodes -keyout ca.key -out ca.crt -subj="/CN=Test CA" \
    -config openssl.config -extensions v3_ca 2>/dev/null
openssl req -newkey rsa:2048 -sha256 -nodes \
    -keyout server.key -out server.csr -subj="/CN=127.0.0.1" 2>/dev/null
openssl x509 -req -in server.csr -days 1 -CA ca.crt -CAkey ca.key \
    -CAcreateserial -extfile openssl.config -extensions server_cert \
    -out server.crt 2>/dev/null
openssl req -newkey rsa:2048 -sha256 -nodes \
    -keyout client.key -out client.csr -subj="/CN=Test Client" 2>/dev/null
openssl x509 -req -in client.csr -days 1 -CA ca.crt -CAkey ca.key \
    -CAcreateserial -extfile openssl.config -extensions usr_cert \
    -out client.cert 2>/dev/null

rm -f httpd-port
httpd http-session-test-server.py \
    --cert=server.crt --key=server.key --client-ca=ca.crt
first_port=$(cat httpd-port)
hostdir=$FLATPAK_SYSTEM_CERTS_D/127.0.0.1:${first_port}
mkdir -p "$hostdir"
cp ca.crt client.key client.cert "$hostdir"

rm -f httpd-port
httpd http-session-test-server.py \
    --cert=server.crt --key=server.key --client-ca=ca.crt
second_port=$(cat httpd-port)
hostdir=$FLATPAK_SYSTEM_CERTS_D/127.0.0.1:${second_port}
mkdir -p "$hostdir"
cp ca.crt "$hostdir"

first_url="https://127.0.0.1:${first_port}"
second_url="https://127.0.0.1:${second_port}"
http_session="${test_builddir}/http-session"
export no_proxy=127.0.0.1,localhost

"$http_session" client-cert "$first_url" "$second_url"
ok 'TLS options reset between requests'

"$http_session" same-origin "$first_url" "$second_url"
ok 'TLS identity changes on a keep-alive origin'

"$http_session" ca-reset "$first_url" "$second_url"
ok 'Custom CA trust does not leak to NULL certificates'

# libcurl does not necessarily use SSL_CERT_FILE or CURL_CA_BUNDLE. Supply
# default trust through its actual build-time path, privately for this process.
# Older libcurl or hosts without mount namespaces still run the tests above.
if ca_bundle=$("$http_session" --default-ca-path) &&
   "${FLATPAK_BWRAP:-bwrap}" --ro-bind / / true; then
    "${FLATPAK_BWRAP:-bwrap}" --die-with-parent --ro-bind / / \
        --bind "$test_tmpdir" "$test_tmpdir" \
        --setenv TMPDIR "$test_tmpdir" \
        --ro-bind "$test_tmpdir/ca.crt" "$ca_bundle" \
        "$http_session" default-ca "$first_url" "$second_url"
    ok 'Default CA bundle restored after custom TLS credentials'

    # Keep CAINFO identical across the identity change so a different CA
    # filename cannot mask incorrect connection-pool matching of client certs.
    hostdir="$test_tmpdir/client-only-certs.d/127.0.0.1:${first_port}"
    mkdir -p "$hostdir"
    cp client.key client.cert "$hostdir"
    "${FLATPAK_BWRAP:-bwrap}" --die-with-parent --ro-bind / / \
        --ro-bind "$test_tmpdir/ca.crt" "$ca_bundle" \
        --setenv FLATPAK_SYSTEM_CERTS_D "$test_tmpdir/client-only-certs.d" \
        "$http_session" same-origin "$first_url" "$second_url"
    ok 'TLS identity changes with unchanged CA settings'

    # The reverted NULL reset made HTTPS summary fetches fail, reporting an
    # installed Locale as missing and silently leaving it on the old commit.
    locale_ref="runtime/org.test.Http.Locale/$ARCH/master"
    mkdir -p locale/files/en
    cat > locale/metadata <<EOF
[Runtime]
name=org.test.Http.Locale

[ExtensionOf]
ref=app/org.test.Http/$ARCH/master
EOF
    echo first > locale/files/en/message
    $FLATPAK build-finish locale >&2
    $FLATPAK build-export --runtime --timestamp=2000-01-01 \
        locale-repo locale >&2
    $FLATPAK --user remote-add --no-gpg-verify tls-test \
        "file://$test_tmpdir/locale-repo" >&2
    $FLATPAK --user install --noninteractive --no-deps --no-related \
        tls-test "$locale_ref" >&2

    echo second > locale/files/en/message
    $FLATPAK build-export --runtime locale-repo locale >&2
    expected_commit=$(ostree --repo=locale-repo rev-parse "$locale_ref")
    rm -f httpd-port
    httpd http-session-test-server.py --cert=server.crt --key=server.key \
        --client-ca=ca.crt --directory=locale-repo
    $FLATPAK --user remote-modify --url="https://127.0.0.1:$(cat httpd-port)" \
        tls-test >&2

    "${FLATPAK_BWRAP:-bwrap}" --die-with-parent --ro-bind / / \
        --bind "$test_tmpdir" "$test_tmpdir" \
        --bind "$TEST_DATA_DIR" "$TEST_DATA_DIR" \
        --setenv TMPDIR "$test_tmpdir" \
        --ro-bind "$test_tmpdir/ca.crt" "$ca_bundle" \
        $FLATPAK --user update --noninteractive --no-deps --no-related \
        "$locale_ref" > locale-update.log 2>&1
    cat locale-update.log >&2
    assert_not_file_has_content locale-update.log 'Treating remote fetch error as non-fatal'
    assert_streq "$($FLATPAK --user info --show-commit "$locale_ref")" "$expected_commit"
    ok 'HTTPS Locale update with default CA trust'
else
    ok 'Default CA bundle restored # skip Requires libcurl CAINFO and mount namespaces'
    ok 'TLS identity changes with unchanged CA settings # skip Requires libcurl CAINFO and mount namespaces'
    ok 'HTTPS Locale update with default CA trust # skip Requires libcurl CAINFO and mount namespaces'
fi

done_testing
