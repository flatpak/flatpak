#!/bin/bash
#
# Copyright (C) 2020 Alexander Larsson <alexl@redhat.com>
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

echo "1..2"

setup_repo

sha256() {
    sha256sum -b | awk "{ print \$1 }"
}

gunzip_sha256() {
    gunzip -c $1 | sha256
}

make_app() {
    APP_ID=$1
    APPARCH=$2
    REPO=$3

    DIR=`mktemp -d`
    cat > ${DIR}/metadata <<EOF
[Application]
name=$APP_ID
runtime=org.test.Platform/$APPARCH/master
EOF

    mkdir -p ${DIR}/files/bin
    cat > ${DIR}/files/bin/hello.sh <<EOF
#!/bin/sh
echo "Hello world, from a sandbox"
EOF
    chmod a+x ${DIR}/files/bin/hello.sh

    mkdir -p ${DIR}/files/share/app-info/xmls
    mkdir -p ${DIR}/files/share/app-info/icons/flatpak/64x64
    gzip -c > ${DIR}/files/share/app-info/xmls/${APP_ID}.xml.gz <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<components version="0.8">
  <component type="desktop">
    <id>$APP_ID.desktop</id>
    <name>Hello world test app: $APP_ID</name>
    <summary>Print a greeting</summary>
    <description><p>This is a test app.</p></description>
    <releases>
      <release timestamp="1525132800" version="0.0.1"/>
    </releases>
  </component>
</components>
EOF
    cp $(dirname $0)/org.test.Hello.png ${DIR}/files/share/app-info/icons/flatpak/64x64/${APP_ID}.png

    $FLATPAK build-finish --command=hello.sh ${DIR} &> /dev/null
    $FLATPAK build-export ${GPGARGS} --arch=$APPARCH --disable-sandbox  ${REPO} ${DIR} &> /dev/null
    rm -rf ${DIR}
}

active_subsets() {
    REPO=$1
    $FLATPAK repo --subsets $REPO | awk "{ print \$2 }"
}

active_subset_for_arch() {
    REPO=$1
    THE_ARCH=$2
    $FLATPAK repo --subsets $REPO --subset $THE_ARCH | awk "{ print \$2 }"
}

active_subset() {
    active_subset_for_arch $1 $ARCH
}

n_histories() {
    REPO=$1
    $FLATPAK repo --subsets $REPO | awk "{ sum +=\$3 } END {print sum}"
}

verify_subsummaries() {
    REPO=$1

    ACTIVE_SUBSETS=$(active_subsets $REPO)
    for SUBSET in $ACTIVE_SUBSETS; do
        assert_has_file $REPO/summaries/$SUBSET.gz
    done

    N_HISTORIES=$(n_histories $REPO)

    N_DELTAS=0
    for DELTA_FILE in $REPO/summaries/*.delta; do
        DELTA=$(basename $DELTA_FILE .delta)
        FROM=$(echo $DELTA | cut -d "-" -f 1)
        TO=$(echo $DELTA | cut -d "-" -f 2)

        # All TO should be in an active SUBSET
        if [[ "$ACTIVE_SUBSETS" != *"$TO"* ]]; then
            assert_not_reached
        fi
        N_DELTAS=$(($N_DELTAS+1))
    done
    assert_streq "$N_DELTAS" "$N_HISTORIES"

    N_OLD=0
    for SUMMARY_FILE in $REPO/summaries/*.gz; do
        DIGEST=$(basename $SUMMARY_FILE .gz)
        COMPUTED_DIGEST=$(gunzip_sha256 $SUMMARY_FILE)
        assert_streq "$DIGEST" "$COMPUTED_DIGEST"

        if [[ "$ACTIVE_SUBSETS" != *"$DIGEST"* ]]; then
            N_OLD=$(($N_OLD+1))
        fi
    done
    assert_streq "$N_OLD" "$N_HISTORIES"

}

set +x

# A non-default arch (that isn't compatible)
if [ $ARCH == x86_64 -o $ARCH == i386 ]; then
    OTHER_ARCH=aarch64
else
    OTHER_ARCH=x86_64
fi

# Set up some arches, including the current one
declare -A arches
for A in aarch64 arm $($FLATPAK --supported-arches); do
    arches[$A]=1
done
ARCHES=${!arches[@]}

for A in $ARCHES; do
    # Create runtimes for all arches (based on $ARCH version)
    if [ $A != $ARCH ]; then
        $FLATPAK build-commit-from  ${GPGARGS} --src-ref=runtime/org.test.Platform/$ARCH/master repos/test runtime/org.test.Platform/$A/master
    fi

    # Create a bunch of apps (for all arches)
    for I in $(seq 10); do
        make_app org.app.App$I $A repos/test
    done

    # Make sure we have no superfluous summary files
    verify_subsummaries repos/test
done

set -x

ok subsummary update generations

ACTIVE_SUBSET=$(active_subset repos/test)
ACTIVE_SUBSET_OTHER=$(active_subset_for_arch repos/test $OTHER_ARCH)

assert_not_has_file $FL_CACHE_DIR/summaries/test-repo-${ACTIVE_SUBSET}.sub
assert_not_has_file $FL_CACHE_DIR/summaries/test-repo-${ACTIVE_SUBSET_OTHER}.sub

httpd_clear_log
$FLATPAK $U remote-ls test-repo  > /dev/null

assert_has_file $FL_CACHE_DIR/summaries/test-repo-${ARCH}-${ACTIVE_SUBSET}.sub
assert_not_has_file $FL_CACHE_DIR/summaries/test-repo-${OTHER_ARCH}-${ACTIVE_SUBSET_OTHER}.sub
# We downloaded the full summary (not delta)
assert_file_has_content httpd-log summaries/${ACTIVE_SUBSET}.gz

httpd_clear_log
$FLATPAK $U remote-ls test-repo --arch=$OTHER_ARCH  > /dev/null

assert_has_file $FL_CACHE_DIR/summaries/test-repo-${ARCH}-${ACTIVE_SUBSET}.sub
assert_has_file $FL_CACHE_DIR/summaries/test-repo-${OTHER_ARCH}-${ACTIVE_SUBSET_OTHER}.sub
# We downloaded the full summary (not delta)
assert_file_has_content httpd-log summaries/${ACTIVE_SUBSET_OTHER}.gz

# Modify the ARCH subset
$FLATPAK build-commit-from ${GPGARGS} --src-ref=app/org.app.App1/$ARCH/master repos/test app/org.app.App1.NEW/$ARCH/master

OLD_ACTIVE_SUBSET=$ACTIVE_SUBSET
OLD_ACTIVE_SUBSET_OTHER=$ACTIVE_SUBSET_OTHER
ACTIVE_SUBSET=$(active_subset repos/test)
ACTIVE_SUBSET_OTHER=$(active_subset_for_arch repos/test $OTHER_ARCH)

assert_not_streq "$OLD_ACTIVE_SUBSET" "$ACTIVE_SUBSET"
assert_streq "$OLD_ACTIVE_SUBSET_OTHER" "$ACTIVE_SUBSET_OTHER"

sleep 1 # Ensure mtime differs for cached summary files (so they are removed)
httpd_clear_log
$FLATPAK $U remote-ls test-repo > /dev/null

assert_has_file $FL_CACHE_DIR/summaries/test-repo-${ARCH}-${ACTIVE_SUBSET}.sub
assert_not_has_file $FL_CACHE_DIR/summaries/test-repo-${ARCH}-${OLD_ACTIVE_SUBSET}.sub
assert_has_file $FL_CACHE_DIR/summaries/test-repo-${OTHER_ARCH}-${ACTIVE_SUBSET_OTHER}.sub # This is the same as before
# We should have uses the delta
assert_not_file_has_content httpd-log summaries/${ACTIVE_SUBSET}.gz
assert_file_has_content httpd-log summaries/${OLD_ACTIVE_SUBSET}-${ACTIVE_SUBSET}.delta

# Modify the ARCH *and* OTHER_ARCH subset
$FLATPAK build-commit-from ${GPGARGS} --src-ref=app/org.app.App1/$ARCH/master repos/test app/org.app.App1.NEW2/$ARCH/master
$FLATPAK build-commit-from ${GPGARGS} --src-ref=app/org.app.App1/$OTHER_ARCH/master repos/test app/org.app.App1.NEW2/$OTHER_ARCH/master

OLD_OLD_ACTIVE_SUBSET=$OLD_ACTIVE_SUBSET
OLD_OLD_ACTIVE_SUBSET_OTHER=$OLD_ACTIVE_SUBSET_OTHER
OLD_ACTIVE_SUBSET=$ACTIVE_SUBSET
OLD_ACTIVE_SUBSET_OTHER=$ACTIVE_SUBSET_OTHER
ACTIVE_SUBSET=$(active_subset repos/test)
ACTIVE_SUBSET_OTHER=$(active_subset_for_arch repos/test $OTHER_ARCH)

assert_not_streq "$OLD_ACTIVE_SUBSET" "$ACTIVE_SUBSET"
assert_not_streq "$OLD_ACTIVE_SUBSET_OTHER" "$ACTIVE_SUBSET_OTHER"

sleep 1 # Ensure mtime differs for cached summary files (so they are removed)
httpd_clear_log
$FLATPAK $U remote-ls test-repo > /dev/null # Only update for $ARCH

assert_has_file $FL_CACHE_DIR/summaries/test-repo-${ARCH}-${ACTIVE_SUBSET}.sub
assert_not_has_file $FL_CACHE_DIR/summaries/test-repo-${ARCH}-${OLD_ACTIVE_SUBSET}.sub
# We didn't get OTHER_ARCH summary
assert_not_has_file $FL_CACHE_DIR/summaries/test-repo-${OTHER_ARCH}-${ACTIVE_SUBSET_OTHER}.sub
assert_has_file $FL_CACHE_DIR/summaries/test-repo-${OTHER_ARCH}-${OLD_ACTIVE_SUBSET_OTHER}.sub
# We should have used the delta
assert_not_file_has_content httpd-log summaries/${ACTIVE_SUBSET}.gz
assert_file_has_content httpd-log summaries/${OLD_ACTIVE_SUBSET}-${ACTIVE_SUBSET}.delta

sleep 1 # Ensure mtime differs for cached summary files (so they are removed)
httpd_clear_log
$FLATPAK $U remote-ls --arch=* test-repo > /dev/null # update for all arches

assert_has_file $FL_CACHE_DIR/summaries/test-repo-${ARCH}-${ACTIVE_SUBSET}.sub
assert_not_has_file $FL_CACHE_DIR/summaries/test-repo-${ARCH}-${OLD_ACTIVE_SUBSET}.sub
assert_has_file $FL_CACHE_DIR/summaries/test-repo-${OTHER_ARCH}-${ACTIVE_SUBSET_OTHER}.sub
assert_not_has_file $FL_CACHE_DIR/summaries/test-repo-${OTHER_ARCH}-${OLD_ACTIVE_SUBSET_OTHER}.sub
# We should have used the delta
assert_not_file_has_content httpd-log summaries/${ACTIVE_SUBSET_OTHER}.gz
assert_file_has_content httpd-log summaries/${OLD_ACTIVE_SUBSET_OTHER}-${ACTIVE_SUBSET_OTHER}.delta
# We should have used the $ARCH one from the cache
assert_not_file_has_content httpd-log summaries/${ACTIVE_SUBSET}.gz
assert_not_file_has_content httpd-log summaries/${OLD_ACTIVE_SUBSET}-${ACTIVE_SUBSET}.delta

ok subsummary fetching and caching
