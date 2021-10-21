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
skip_revokefs_without_fuse

echo "1..20"

# Use stable rather than master as the branch so we can test that the run
# command automatically finds the branch correctly
setup_repo "" "" stable
install_repo "" stable

# Verify that app is correctly installed

assert_has_dir $FL_DIR/app/org.test.Hello
assert_has_symlink $FL_DIR/app/org.test.Hello/current
assert_symlink_has_content $FL_DIR/app/org.test.Hello/current ^$ARCH/stable$
assert_has_dir $FL_DIR/app/org.test.Hello/$ARCH/stable
assert_has_symlink $FL_DIR/app/org.test.Hello/$ARCH/stable/active
ID=`readlink $FL_DIR/app/org.test.Hello/$ARCH/stable/active`
assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/stable/active/deploy
assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/stable/active/metadata
assert_has_dir $FL_DIR/app/org.test.Hello/$ARCH/stable/active/files
assert_has_dir $FL_DIR/app/org.test.Hello/$ARCH/stable/active/export
assert_has_file $FL_DIR/exports/share/applications/org.test.Hello.desktop
# Ensure Exec key is rewritten
assert_file_has_content $FL_DIR/exports/share/applications/org.test.Hello.desktop "^Exec=.*flatpak run --branch=stable --arch=$ARCH --command=hello\.sh org\.test\.Hello$"
assert_has_file $FL_DIR/exports/share/gnome-shell/search-providers/org.test.Hello.search-provider.ini
assert_file_has_content $FL_DIR/exports/share/gnome-shell/search-providers/org.test.Hello.search-provider.ini "^DefaultDisabled=true$"
assert_has_file $FL_DIR/exports/share/icons/hicolor/64x64/apps/org.test.Hello.png
assert_not_has_file $FL_DIR/exports/share/icons/hicolor/64x64/apps/dont-export.png
assert_has_file $FL_DIR/exports/share/icons/HighContrast/64x64/apps/org.test.Hello.png

# Ensure triggers ran
assert_has_file $FL_DIR/exports/share/applications/mimeinfo.cache
assert_file_has_content $FL_DIR/exports/share/applications/mimeinfo.cache x-test/Hello
assert_has_file $FL_DIR/exports/share/icons/hicolor/icon-theme.cache
assert_has_file $FL_DIR/exports/share/icons/hicolor/index.theme

$FLATPAK list ${U} | grep org.test.Hello > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep test-repo > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep current > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep ${ID:0:12} > /dev/null

$FLATPAK info ${U} org.test.Hello > /dev/null
$FLATPAK info ${U} org.test.Hello | grep test-repo > /dev/null
$FLATPAK info ${U} org.test.Hello | grep $ID > /dev/null

ok "install"

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

ok "hello"

# XDG_RUNTIME_DIR is set to <temp directory>/runtime by libtest.sh,
# so we always have the necessary setup to reproduce #4372
assert_not_streq "$XDG_RUNTIME_DIR" "/run/user/$(id -u)"
run_sh org.test.Platform 'echo $XDG_RUNTIME_DIR' > value-in-sandbox
head value-in-sandbox >&2
assert_file_has_content value-in-sandbox "^/run/user/$(id -u)\$"

ok "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR not inherited"

assert_streq "$XDG_CACHE_HOME" "${TEST_DATA_DIR}/home/cache"
run_sh org.test.Hello 'echo "$XDG_CACHE_HOME"' > value-in-sandbox
head value-in-sandbox >&2
assert_file_has_content value-in-sandbox "^${TEST_DATA_DIR}/home/\\.var/app/org\\.test\\.Hello/cache\$"
test -d "${TEST_DATA_DIR}/home/.var/app/org.test.Hello/cache"
run_sh org.test.Hello 'echo "$HOST_XDG_CACHE_HOME"' > host-value-in-sandbox
head host-value-in-sandbox >&2
assert_file_has_content host-value-in-sandbox "^${TEST_DATA_DIR}/home/cache\$"

assert_streq "$XDG_CONFIG_HOME" "${TEST_DATA_DIR}/home/config"
run_sh org.test.Hello 'echo "$XDG_CONFIG_HOME"' > value-in-sandbox
head value-in-sandbox >&2
assert_file_has_content value-in-sandbox "^${TEST_DATA_DIR}/home/\\.var/app/org\\.test\\.Hello/config\$"
test -d "${TEST_DATA_DIR}/home/.var/app/org.test.Hello/config"
run_sh org.test.Hello 'echo "$HOST_XDG_CONFIG_HOME"' > host-value-in-sandbox
head host-value-in-sandbox >&2
assert_file_has_content host-value-in-sandbox "^${TEST_DATA_DIR}/home/config\$"

assert_streq "$XDG_DATA_HOME" "${TEST_DATA_DIR}/home/share"
run_sh org.test.Hello 'echo "$XDG_DATA_HOME"' > value-in-sandbox
head value-in-sandbox >&2
assert_file_has_content value-in-sandbox "^${TEST_DATA_DIR}/home/\\.var/app/org\\.test\\.Hello/data\$"
test -d "${TEST_DATA_DIR}/home/.var/app/org.test.Hello/data"
run_sh org.test.Hello 'echo "$HOST_XDG_DATA_HOME"' > host-value-in-sandbox
head host-value-in-sandbox >&2
assert_file_has_content host-value-in-sandbox "^${TEST_DATA_DIR}/home/share\$"

assert_streq "$XDG_STATE_HOME" "${TEST_DATA_DIR}/home/state"
run_sh org.test.Hello 'echo "$XDG_STATE_HOME"' > value-in-sandbox
head value-in-sandbox >&2
assert_file_has_content value-in-sandbox "^${TEST_DATA_DIR}/home/\\.var/app/org\\.test\\.Hello/\\.local/state\$"
test -d "${TEST_DATA_DIR}/home/.var/app/org.test.Hello/.local/state"
run_sh org.test.Hello 'echo "$HOST_XDG_STATE_HOME"' > host-value-in-sandbox
head host-value-in-sandbox >&2
assert_file_has_content host-value-in-sandbox "^${TEST_DATA_DIR}/home/state\$"

ok "XDG_foo_HOME work as expected"

run_sh org.test.Platform cat /.flatpak-info >runtime-fpi
assert_file_has_content runtime-fpi "[Runtime]"
assert_file_has_content runtime-fpi "^runtime=runtime/org\.test\.Platform/$ARCH/stable$"

ok "run a runtime"

if [ -f /etc/os-release ]; then
    run_sh org.test.Platform cat /run/host/os-release >os-release
    (cd /etc; md5sum os-release) | md5sum -c

    ARGS="--filesystem=host-etc" run_sh org.test.Platform cat /run/host/os-release >os-release
    (cd /etc; md5sum os-release) | md5sum -c

    if run_sh org.test.Platform "echo test >> /run/host/os-release"; then exit 1; fi
    if run_sh org.test.Platform "echo test >> /run/host/os-release"; then exit 1; fi
elif [ -f /usr/lib/os-release ]; then
    run_sh org.test.Platform cat /run/host/os-release >os-release
    (cd /usr/lib; md5sum os-release) | md5sum -c

    ARGS="--filesystem=host-os" run_sh org.test.Platform cat /run/host/os-release >os-release
    (cd /usr/lib; md5sum os-release) | md5sum -c

    if run_sh org.test.Platform "echo test >> /run/host/os-release"; then exit 1; fi
    if run_sh org.test.Platform "echo test >> /run/host/os-release"; then exit 1; fi
fi

ok "host os-release"

run_sh org.test.Platform 'cat /run/host/container-manager' > container-manager
echo flatpak > expected
diff -u expected container-manager
run_sh org.test.Platform 'echo "${container}"' > container-manager
diff -u expected container-manager

ok "host container-manager"

if run org.test.Nonexistent 2> run-error-log; then
    assert_not_reached "Unexpectedly able to run non-existent runtime"
fi
assert_file_has_content run-error-log "error: app/org\.test\.Nonexistent/$ARCH/master not installed"

if ${FLATPAK} run --commit=abc runtime/org.test.Platform 2> run-error-log; then
    assert_not_reached "Unexpectedly able to run non-existent commit"
fi
assert_file_has_content run-error-log "error: runtime/org\.test\.Platform/$ARCH/stable (commit abc) not installed"

if run runtime/org.test.Nonexistent 2> run-error-log; then
    assert_not_reached "Unexpectedly able to run non-existent runtime"
fi
assert_file_has_content run-error-log "error: runtime/org\.test\.Nonexistent/\*unspecified\*/\*unspecified\* not installed"

ok "error handling for invalid refs"

run_sh org.test.Hello cat /run/user/`id -u`/flatpak-info > fpi
assert_file_has_content fpi '^name=org\.test\.Hello$'

ok "flatpak-info"

run_sh org.test.Hello readlink /proc/self/ns/net > unshared_net_ns
ARGS="--share=network" run_sh org.test.Hello readlink /proc/self/ns/net > shared_net_ns
assert_not_streq `cat unshared_net_ns` `readlink /proc/self/ns/net`
assert_streq `cat shared_net_ns` `readlink /proc/self/ns/net`

run_sh org.test.Hello readlink /proc/self/ns/ipc > unshared_ipc_ns
ARGS="--share=ipc" run_sh org.test.Hello readlink /proc/self/ns/ipc > shared_ipc_ns
assert_not_streq `cat unshared_ipc_ns` `readlink /proc/self/ns/ipc`
assert_streq `cat shared_ipc_ns` `readlink /proc/self/ns/ipc`

# We try the filesystem namespace tests several times with different
# shared-or-not directories, because:
# - --filesystem=/foo doesn't work if /foo is read-only in the container
#   (notably, --filesystem=/usr/... won't work)
# - --filesystem=host doesn't expose either /usr or /var/... or /var/tmp
#   from the host because they're on the list of things we expect to be
#   supplied by the container

test_filesystem_binding () {
    local dir="$1"

    if run_sh org.test.Hello cat "$dir/package_version.txt" &> /dev/null; then
        assert_not_reached "Unexpectedly allowed to access file"
    fi

    case "$dir" in
        (/home/*|/opt/*|/var/tmp/*)
            if ! ARGS="--filesystem=$dir" run_sh org.test.Hello cat "$dir/package_version.txt" > /dev/null; then
                assert_not_reached "Failed to share --filesystem=$dir"
            fi
            ;;
        (*)
            echo "Not testing --filesystem=$dir, it won't necessarily work" >&2
            ;;
    esac

    case "$dir" in
        (/home/*|/opt/*)
            if ! ARGS="--filesystem=host" run_sh org.test.Hello cat "$dir/package_version.txt" > /dev/null; then
                assert_not_reached "Failed to share $dir as part of host filesystem"
            fi
            ;;
        (*)
            echo "Not testing --filesystem=host with $dir, it won't necessarily work" >&2
            ;;
    esac
}

test_filesystem_binding "${test_builddir}"

mkdir "${TEST_DATA_DIR}/shareable"
cp "${test_builddir}/package_version.txt" "${TEST_DATA_DIR}/shareable/"
test_filesystem_binding "${TEST_DATA_DIR}/shareable"

# We don't want to pollute the home directory unprompted, but the user
# can opt-in by creating this directory.
if [ -e "${HOME}/.flatpak-tests" ]; then
    cp "${test_builddir}/package_version.txt" "${HOME}/.flatpak-tests/"
    test_filesystem_binding "${HOME}/.flatpak-tests"
else
    echo "not testing \$HOME binding, \$HOME/.flatpak-tests/ does not exist" >&2
fi

ok "namespaces"

test_overrides () {
    local dir="$1"

    if run_sh org.test.Hello cat "$dir/package_version.txt" &> /dev/null; then
        assert_not_reached "Unexpectedly allowed to access file"
    fi

    $FLATPAK override ${U} --filesystem=host org.test.Hello

    case "$dir" in
        (/home/*|/opt/*)
            if ! run_sh org.test.Hello cat "$dir/package_version.txt" > /dev/null; then
                assert_not_reached "Failed to share $dir as part of host filesystem"
            fi
            ;;
        (*)
            echo "Not testing --filesystem=host with $dir, it won't necessarily work" >&2
            ;;
    esac

    if ARGS="--nofilesystem=host" run_sh org.test.Hello cat "${dir}/package_version.txt" &> /dev/null; then
        assert_not_reached "Unexpectedly allowed to access --nofilesystem=host file"
    fi

    $FLATPAK override ${U} --nofilesystem=host org.test.Hello

    if run_sh org.test.Hello cat "${dir}/package_version.txt" &> /dev/null; then
        assert_not_reached "Unexpectedly allowed to access file"
    fi
}

test_overrides "${test_builddir}"

if [ -e "${HOME}/.flatpak-tests" ]; then
    cp "${test_builddir}/package_version.txt" "${HOME}/.flatpak-tests/"
    test_overrides "${HOME}/.flatpak-tests"
else
    echo "not testing \$HOME binding overrides, \$HOME/.flatpak-tests/ does not exist" >&2
fi

ok "overrides"

OLD_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

# TODO: For weird reasons this breaks in the system case. Needs debugging
if [ x${USE_SYSTEMDIR-} != xyes ] ; then
    ${FLATPAK} ${U} update -y -v org.test.Hello stable
    ALSO_OLD_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`
    assert_streq "$OLD_COMMIT" "$ALSO_OLD_COMMIT"
fi

ok "null update"

make_updated_app "" "" stable

${FLATPAK} ${U} update -y org.test.Hello

NEW_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

assert_not_streq "$OLD_COMMIT" "$NEW_COMMIT"

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATED$'

ok "update"

ostree --repo=repos/test reset app/org.test.Hello/$ARCH/stable "$OLD_COMMIT"
update_repo

if ${FLATPAK} ${U} update -y org.test.Hello; then
    assert_not_reached "Should not be able to update to older commit"
fi

NEW_NEW_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

assert_streq "$NEW_COMMIT" "$NEW_NEW_COMMIT"

ok "backwards update"

make_updated_app "" "" stable UPDATED2

OLD_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

# We should ignore the update (and warn) by default
${FLATPAK} ${U} install -y test-repo org.test.Hello >& install_stderr

NEW_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

assert_streq "$OLD_COMMIT" "$NEW_COMMIT"
assert_file_has_content install_stderr 'org.test.Hello/.* is already installed'

# But --or-update should do the update
${FLATPAK} ${U} install -y --or-update test-repo org.test.Hello

NEW_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

assert_not_streq "$OLD_COMMIT" "$NEW_COMMIT"

ok "install --or-update"

DIR=`mktemp -d`
${FLATPAK} build-init ${DIR} org.test.Split org.test.Platform org.test.Platform stable

mkdir -p ${DIR}/files/a
echo "a" > ${DIR}/files/a/data
mkdir -p ${DIR}/files/b
echo "b" > ${DIR}/files/b/data
mkdir -p ${DIR}/files/c
echo "c" > ${DIR}/files/c/data
mkdir -p ${DIR}/files/d
echo "d" > ${DIR}/files/d/data
echo "nope" > ${DIR}/files/nope

${FLATPAK} build-finish --command=hello.sh ${DIR}
${FLATPAK} build-export --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable
update_repo

${FLATPAK} ${U} install -y test-repo org.test.Split --subpath=/a --subpath=/b --subpath=/nosuchdir stable

COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Split`
if [ x${USE_SYSTEMDIR-} != xyes ] ; then
    # Work around bug in ostree: local pulls don't do commitpartials
    assert_has_file $FL_DIR/repo/state/${COMMIT}.commitpartial
fi

assert_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/a/data
assert_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/b/data
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/c
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/d
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/nope
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/nosuchdir

echo "aa" > ${DIR}/files/a/data2
rm  ${DIR}/files/a/data
mkdir -p ${DIR}/files/e
echo "e" > ${DIR}/files/e/data
mkdir -p ${DIR}/files/f
echo "f" > ${DIR}/files/f/data
rm -rf  ${DIR}/files/b

${FLATPAK} build-export --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable
update_repo

${FLATPAK} ${U} update -y --subpath=/a --subpath=/b --subpath=/e --subpath=/nosuchdir org.test.Split

COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Split`
if [ x${USE_SYSTEMDIR-} != xyes ] ; then
    # Work around bug in ostree: local pulls don't do commitpartials
    assert_has_file $FL_DIR/repo/state/${COMMIT}.commitpartial
fi

assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/a/data
assert_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/a/data2
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/b
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/c
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/d
assert_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/e/data
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/f
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/nope

${FLATPAK} build-export --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable
update_repo

# Test reusing the old subpath list
${FLATPAK} ${U} update -y org.test.Split

COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Split`
if [ x${USE_SYSTEMDIR-} != xyes ] ; then
    # Work around bug in ostree: local pulls don't do commitpartials
    assert_has_file $FL_DIR/repo/state/${COMMIT}.commitpartial
fi

assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/a/data
assert_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/a/data2
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/b
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/c
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/d
assert_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/e/data
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/f
assert_not_has_file $FL_DIR/app/org.test.Split/$ARCH/stable/active/files/nope

ok "subpaths"

VERSION=`cat "$test_builddir/package_version.txt"`

DIR=`mktemp -d`
${FLATPAK} build-init ${DIR} org.test.CurrentVersion org.test.Platform org.test.Platform stable
${FLATPAK} build-finish --require-version=${VERSION} --command=hello.sh ${DIR}
${FLATPAK} build-export --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable
DIR=`mktemp -d`
${FLATPAK} build-init ${DIR} org.test.OldVersion org.test.Platform org.test.Platform stable
${FLATPAK} build-finish --require-version=0.6.10 --command=hello.sh ${DIR}
${FLATPAK} build-export --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable
DIR=`mktemp -d`
${FLATPAK} build-init ${DIR} org.test.NewVersion org.test.Platform org.test.Platform stable
${FLATPAK} build-finish --require-version=1${VERSION} --command=hello.sh ${DIR}
${FLATPAK} build-export --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable

update_repo

${FLATPAK} ${U} install -y test-repo org.test.OldVersion stable
${FLATPAK} ${U} install -y test-repo org.test.CurrentVersion stable
(! ${FLATPAK} ${U} install -y test-repo org.test.NewVersion stable)

DIR=`mktemp -d`
${FLATPAK} build-init ${DIR} org.test.OldVersion org.test.Platform org.test.Platform stable
${FLATPAK} build-finish --require-version=99.0.0 --command=hello.sh ${DIR}
${FLATPAK} build-export  --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable
update_repo

(! ${FLATPAK} ${U} update -y org.test.OldVersion)

DIR=`mktemp -d`
${FLATPAK} build-init ${DIR} org.test.OldVersion org.test.Platform org.test.Platform stable
${FLATPAK} build-finish --require-version=0.1.1 --command=hello.sh ${DIR}
${FLATPAK} build-export  --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable
update_repo

${FLATPAK} ${U} update -y org.test.OldVersion

# Make sure a multi-ref update succeeds even if some update requires a newer version
# Note that updates are in alphabetical order, so CurrentVersion will be pulled first
# and should not block a successful install of OldVersion later

DIR=`mktemp -d`
${FLATPAK} build-init ${DIR} org.test.CurrentVersion org.test.Platform org.test.Platform stable
touch ${DIR}/files/updated
${FLATPAK} build-finish --require-version=99.0.0 --command=hello.sh ${DIR}
${FLATPAK} build-export --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable
DIR=`mktemp -d`
${FLATPAK} build-init ${DIR} org.test.OldVersion org.test.Platform org.test.Platform stable
touch ${DIR}/files/updated
${FLATPAK} build-finish --require-version=${VERSION} --command=hello.sh ${DIR}
${FLATPAK} build-export --no-update-summary ${FL_GPGARGS} repos/test ${DIR} stable
update_repo

if ${FLATPAK} ${U} update -y &> err_version.txt; then
    assert_not_reached "Should not have been able to update due to version"
fi
assert_file_has_content err_version.txt "needs a later flatpak version"

assert_not_has_file $FL_DIR/app/org.test.CurrentVersion/$ARCH/stable/active/files/updated
assert_has_file $FL_DIR/app/org.test.OldVersion/$ARCH/stable/active/files/updated

ok "version checks"

rm -rf app
flatpak build-init app org.test.Writable org.test.Platform org.test.Platform stable
mkdir -p app/files/a-dir
chmod a+rwx app/files/a-dir
flatpak build-finish --command=hello.sh app
# Note: not --canonical-permissions
${FLATPAK} build-export -vv  --no-update-summary --disable-sandbox --files=files repos/test app stable
ostree --repo=repos/test commit  --keep-metadata=xa.metadata --owner-uid=0 --owner-gid=0  --no-xattrs  ${FL_GPGARGS} --branch=app/org.test.Writable/$ARCH/stable app
update_repo

# In the system-helper case this fails to install due to the permission canonicalization happening in the
# child-repo making objects get the wrong checksum, whereas in the user case we successfully import it, but
# it will have canonicalized permissions.
if ${FLATPAK} ${U} install -y test-repo org.test.Writable; then
    assert_file_has_mode $FL_DIR/app/org.test.Writable/$ARCH/stable/active/files/a-dir 775
fi

ok "no world writable dir"

rm -rf app
flatpak build-init app org.test.Setuid org.test.Platform org.test.Platform stable
mkdir -p app/files/
touch app/files/exe
chmod u+s app/files/exe
flatpak build-finish --command=hello.sh app
# Note: not --canonical-permissions
${FLATPAK} build-export -vv  --no-update-summary --disable-sandbox --files=files repos/test app stable
ostree -v --repo=repos/test commit --keep-metadata=xa.metadata --owner-uid=0 --owner-gid=0 --no-xattrs  ${FL_GPGARGS} --branch=app/org.test.Setuid/$ARCH/stable app
update_repo

if ${FLATPAK} ${U} install -y test-repo org.test.Setuid &> err2.txt; then
    assert_not_reached "Should not be able to install with setuid file"
fi
assert_file_has_content err2.txt [Ii]nvalid

ok "no setuid"

rm -rf app
flatpak build-init app org.test.App org.test.Platform org.test.Platform stable
mkdir -p app/files/
touch app/files/exe
flatpak build-finish --command=hello.sh --sdk=org.test.Sdk app
${FLATPAK} build-export  --no-update-summary ${FL_GPGARGS} repos/test app stable
update_repo

${FLATPAK} ${U} install -y test-repo org.test.App
${FLATPAK} ${U} info -m org.test.App > out

assert_file_has_content out "^sdk=org\.test\.Sdk/$(flatpak --default-arch)/stable$"

ok "--sdk option"
