# Source library for shell script tests
#
# Copyright (C) 2016 Alexander Larsson <alexl@redhat.com>
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

# Under Autotools, redirect stderr to stdout, otherwise the log will have
# command output out of order with xtrace output.
# Under Meson, we need stdout to be in strict TAP format, so we'll
# consistently send everything except TAP to stderr instead.
if [ -z "${FLATPAK_TESTS_STRICT_TAP-}" ]; then
    exec 2>&1
fi

if [ -n "${G_TEST_SRCDIR:-}" ]; then
    test_srcdir="${G_TEST_SRCDIR}"
else
    test_srcdir=$(dirname $0)
fi

if [ -n "${G_TEST_BUILDDIR:-}" ]; then
    test_builddir="${G_TEST_BUILDDIR}"
else
    test_builddir=$(dirname $0)
fi

if [ -e "$test_srcdir/installed-tests.sh" ]; then
    . "$test_srcdir/installed-tests.sh"
fi

test_number=0

done_testing () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    echo "1..$test_number"
    echo "# Done testing"
    } 3> /dev/null
}

# All the asserts and ok functions below are wrapped such that they
# don't output any set -x traces of their internals (but still echo
# errors to stderr). This way the log output focuses on tracing what
# is essential to the test (the asserts being run and errors from them)

assert_not_reached () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    echo $@ 1>&2; exit 1
    } 3> /dev/null
}

ok () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
        test_number=$(( test_number + 1 ))
        echo "ok $test_number - $*";
        echo "================ $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]} - $@ ================" >&2;
    } 3> /dev/null
}

test_tmpdir=$(pwd)

# Sanity check that we're in a tmpdir that has
# just .testtmp (created by tap-driver for `make check`,
# or nothing at all (as ginstest-runner does)
if ! test -f .testtmp; then
    files=$(ls)
    if test -n "${files}"; then
        ls -l
        assert_not_reached "test tmpdir=${test_tmpdir} is not empty; run this test via \`make check TESTS=\`, not directly"
    fi
    # Remember that this is an acceptable test $(pwd), for the benefit of
    # C and JS tests which may source this file again
    touch .testtmp
fi

export G_DEBUG=fatal-warnings

# Also, unbreak `tar` inside `make check`...Automake will inject
# TAR_OPTIONS: --owner=0 --group=0 --numeric-owner presumably so that
# tarballs are predictable, except we don't want this in our tests.
unset TAR_OPTIONS

if test -n "${FLATPAK_TESTS_VALGRIND:-}"; then
    CMD_PREFIX="env G_SLICE=always-malloc valgrind -q --leak-check=no --error-exitcode=1 --gen-suppressions=all --num-callers=30 --suppressions=${test_srcdir}/flatpak.supp --suppressions=${test_srcdir}/glib.supp"
elif test -n "${FLATPAK_TESTS_VALGRIND_LEAKS:-}"; then
    CMD_PREFIX="env G_SLICE=always-malloc valgrind -q --leak-check=full  --errors-for-leak-kinds=definite --error-exitcode=1 --gen-suppressions=all --num-callers=30 --suppressions=${test_srcdir}/flatpak.supp --suppressions=${test_srcdir}/glib.supp"
else
    CMD_PREFIX=""
fi
unset OSTREE_DEBUG_HTTP

export MALLOC_CHECK_=3
export MALLOC_PERTURB_=$(($RANDOM % 255 + 1))

TEST_DATA_DIR=`mktemp -d /tmp/test-flatpak-XXXXXX`
mkdir -p ${TEST_DATA_DIR}/home
mkdir -p ${TEST_DATA_DIR}/runtime
mkdir -p ${TEST_DATA_DIR}/system
mkdir -p ${TEST_DATA_DIR}/config
mkdir -p ${TEST_DATA_DIR}/run
export FLATPAK_SYSTEM_DIR=${TEST_DATA_DIR}/system
export FLATPAK_SYSTEM_CACHE_DIR=${TEST_DATA_DIR}/system-cache
export FLATPAK_SYSTEM_HELPER_ON_SESSION=1
export FLATPAK_CONFIG_DIR=${TEST_DATA_DIR}/config
export FLATPAK_DATA_DIR=${TEST_DATA_DIR}/datadir
export FLATPAK_RUN_DIR=${TEST_DATA_DIR}/run
export FLATPAK_FANCY_OUTPUT=0
export FLATPAK_FORCE_ALLOW_FUZZY_MATCHING=1

export HOME=${TEST_DATA_DIR}/home
export XDG_CACHE_HOME=${TEST_DATA_DIR}/home/cache
export XDG_CONFIG_HOME=${TEST_DATA_DIR}/home/config
export XDG_DATA_HOME=${TEST_DATA_DIR}/home/share
export XDG_STATE_HOME=${TEST_DATA_DIR}/home/state
export XDG_RUNTIME_DIR=${TEST_DATA_DIR}/runtime

export XDG_DESKTOP_PORTAL_DIR=${test_builddir}/share/xdg-desktop-portal/portals
export XDG_CURRENT_DESKTOP=test

# On Debian derivatives, /usr/sbin and /sbin aren't in ordinary users'
# PATHs, but ldconfig and capsh are kept in /sbin
PATH="$PATH:/usr/sbin:/sbin"

export USERDIR=${TEST_DATA_DIR}/home/share/flatpak
export SYSTEMDIR=${TEST_DATA_DIR}/system
export ARCH=`flatpak --default-arch`

if [ x${SUMMARY_FORMAT-} == xold ] ; then
    export BUILD_UPDATE_REPO_FLAGS="--no-summary-index"
fi

if [ x${USE_SYSTEMDIR-} == xyes ] ; then
    export FL_DIR=${SYSTEMDIR}
    export U=
    export INVERT_U=--user
    if [ x${HAVE_SYSTEM_HELPER-} == x1 ] && [ x${UID} != x0 ] ; then
        # If system helper is compiled during build and will actually be
        # used at runtime eg. UID != 0, Flatpak stores summary caches
        # in the user's XDG cache directory
        export FL_CACHE_DIR=${XDG_CACHE_HOME}/flatpak/system-cache
    else
        # If the system helper is unavailable at build time, or it
        # cannot be used eg. UID == 0 Flatpak uses the repo-local cache
        # directory.
        export FL_CACHE_DIR=$FL_DIR/repo/tmp/cache
    fi
else
    export FL_DIR=${USERDIR}
    export U="--user"
    export INVERT_U=--system
    export FL_CACHE_DIR=$FL_DIR/repo/tmp/cache
fi

if [ x${USE_DELTAS-} == xyes ] ; then
    export UPDATE_REPO_ARGS="--generate-static-deltas"
fi

export FLATPAK="${CMD_PREFIX} flatpak"

assert_streq () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    test "$1" = "$2" || (echo 1>&2 "$1 != $2 at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"; exit 1)
    } 3> /dev/null
}

assert_not_streq () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    (! test "$1" = "$2") || (echo 1>&2 "$1 == $2 at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"; exit 1)
    } 3> /dev/null
}

assert_has_file () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    test -f "$1" || (echo 1>&2 "Couldn't find '$1' at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"; exit 1)
    } 3> /dev/null
}

assert_has_symlink () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    test -L "$1" || (echo 1>&2 "Couldn't find '$1' at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"; exit 1)
    } 3> /dev/null
}

assert_has_dir () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    test -d "$1" || (echo 1>&2 "Couldn't find '$1' at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"; exit 1)
    } 3> /dev/null
}

assert_not_has_file () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    if test -f "$1"; then
        sed -e 's/^/# /' < "$1" >&2
        echo 1>&2 "File '$1' exists at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"
        exit 1
    fi
    } 3> /dev/null
}

assert_not_file_has_content () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    if grep -q -e "$2" "$1"; then
        sed -e 's/^/# /' < "$1" >&2
        echo 1>&2 "File '$1' incorrectly matches regexp '$2' at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"
        exit 1
    fi
    } 3> /dev/null
}

assert_file_has_mode () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    mode=$(stat -c '%a' $1)
    if [ "$mode" != "$2" ]; then
        echo 1>&2 "File '$1' has wrong mode: expected $2, but got $mode at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"
        exit 1
    fi
    } 3> /dev/null
}

assert_not_has_dir () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    if test -d "$1"; then
        echo 1>&2 "Directory '$1' exists at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"; exit 1
    fi
    } 3> /dev/null
}

assert_file_has_content () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    if ! grep -q -e "$2" "$1"; then
        sed -e 's/^/# /' < "$1" >&2
        echo 1>&2 "File '$1' doesn't match regexp '$2' at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"
        exit 1
    fi
    } 3> /dev/null
}

assert_log_has_gpg_signature_error () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    if ! grep -q -e "GPG signatures found, but none are in trusted keyring" "$1"; then
        if ! grep -q -e "Can't check signature: public key not found" "$1"; then
            sed -e 's/^/# /' < "$1" >&2
            echo 1>&2 "File '$1' doesn't have gpg signature error at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"
            exit 1
        fi
    fi
    } 3> /dev/null
}

assert_symlink_has_content () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    if ! readlink "$1" | grep -q -e "$2"; then
        readlink "$1" |sed -e 's/^/# /' >&2
        echo 1>&2 "Symlink '$1' doesn't match regexp '$2' at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"
        exit 1
    fi
    } 3> /dev/null
}

assert_file_empty() {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    if test -s "$1"; then
        sed -e 's/^/# /' < "$1" >&2
        echo 1>&2 "File '$1' is not empty at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"
        exit 1
    fi
    } 3> /dev/null
}

assert_remote_has_config () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    ostree config --repo=$FL_DIR/repo get --group 'remote "'"$1"'"' "$2" > key-output
    assert_file_has_content key-output "$3"
    } 3> /dev/null
}

assert_remote_has_no_config () {
    { { local BASH_XTRACEFD=3; } 2> /dev/null
    if ostree config --repo=$FL_DIR/repo get --group 'remote "'"$1"'"' "$2" &> /dev/null; then
        echo 1>&2 "Remote '$1' unexpectedly has key '$2' at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"
        exit 1
    fi
    } 3> /dev/null
}

assert_fail () {
    if "$@"; then
        { { local BASH_XTRACEFD=3; } 2> /dev/null
            echo "Command '$*' should not have succeeded at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}" >&2
            exit 1
        } 3> /dev/null
    fi
}

# Try and make GPG use more stable from scripts. --status-fd is also recommended
# for this, but we don’t have a need for parsing status messages at the moment.
export GPG="gpg --batch --with-colons"

export FL_GPG_HOMEDIR=${TEST_DATA_DIR}/gpghome
export FL_GPG_HOMEDIR2=${TEST_DATA_DIR}/gpghome2
mkdir -p ${FL_GPG_HOMEDIR}
mkdir -p ${FL_GPG_HOMEDIR2}
# This need to be writable, so copy the keys
cp $(dirname $0)/test-keyring/*.gpg ${FL_GPG_HOMEDIR}/
cp $(dirname $0)/test-keyring2/*.gpg ${FL_GPG_HOMEDIR2}/

export FL_GPG_ID=7B0961FD
export FL_GPG_FINGERPRINT=3718EEBEB5740A7AB3D651B7138B31E07B0961FD
export FL_GPG_ID2=B2314EFC
export FL_GPG_FINGERPRINT2=E47567C56629E90B6791C5EE759F5FD0B2314EFC
export FL_GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${FL_GPG_ID}"
export FL_GPGARGS2="--gpg-homedir=${FL_GPG_HOMEDIR2} --gpg-sign=${FL_GPG_ID2}"
export FL_GPGCMDARGS="--homedir ${FL_GPG_HOMEDIR} -u ${FL_GPG_ID}"
export FL_GPGCMDARGS2="--homedir ${FL_GPG_HOMEDIR2} -u ${FL_GPG_ID2}"
FL_GPG_BASE64="$(${GPG} --homedir "${FL_GPG_HOMEDIR}" --export "${FL_GPG_ID}" | base64 --wrap 0)"
FL_GPG_BASE642="$(${GPG} --homedir "${FL_GPG_HOMEDIR2}" --export "${FL_GPG_ID2}" | base64 --wrap 0)"
export FL_GPG_BASE64 FL_GPG_BASE642

make_oci_signature () {
    DIGEST="$1"
    REFERENCE="$2"
    GPGARGS="${3:-${FL_GPGCMDARGS}}"

    ${GPG} ${GPGARGS} --sign - <<EOF
{
    "critical": {
        "type": "atomic container signature",
        "image": {
            "docker-manifest-digest": "$DIGEST"
        },
        "identity": {
            "docker-reference": "$REFERENCE"
        }
    },
    "optional": {}
}
EOF
}

make_runtime () {
    REPONAME="$1"
    COLLECTION_ID="$2"
    BRANCH="$3"
    GPGARGS="$4"

    RUNTIME_REF="runtime/org.test.Platform/$(flatpak --default-arch)/${BRANCH}"
    if [ ! -z "${SRC_RUNTIME_REPO:-}" ]; then
        RUNTIME_REPO=repos/${SRC_RUNTIME_REPO}
    elif [ -f ${test_builddir}/runtime-repo/refs/heads/${RUNTIME_REF} ]; then
        RUNTIME_REPO=${test_builddir}/runtime-repo
    else
        RUNTIME_REPO=${TEST_DATA_DIR}/runtime-repo
        (
            flock -s 200
            if [ ! -f "${RUNTIME_REPO}/refs/heads/${RUNTIME_REF}" ]; then
                $(dirname $0)/make-test-runtime.sh ${RUNTIME_REPO} org.test.Platform ${BRANCH} "" "" > /dev/null
            fi
        ) 200>${TEST_DATA_DIR}/runtime-repo-lock
    fi

    if [ ! -d repos/${REPONAME} ]; then
        if [ "x${COLLECTION_ID}" != "x" ]; then
            collection_args=--collection-id=${COLLECTION_ID}
        else
            collection_args=
        fi
        mkdir -p repos
        ostree --repo=repos/${REPONAME} init --mode=archive-z2 ${collection_args} >&2
    fi

    flatpak build-commit-from --disable-fsync --no-update-summary --src-repo=${RUNTIME_REPO} --force ${GPGARGS} ${EXPORT_ARGS-}  repos/${REPONAME}  ${RUNTIME_REF} >&2
}

httpd () {
    if [ $# -eq 0 ] ; then
        set web-server.py repos
    fi

    COMMAND=$1
    shift

    rm -f httpd-pipe
    mkfifo httpd-pipe
    PYTHONUNBUFFERED=1 $(dirname $0)/$COMMAND "$@" 3> httpd-pipe 2>&1 | tee -a httpd-log >&2 &
    read < httpd-pipe
}

httpd_clear_log () {
    truncate -s 0 httpd-log
}

setup_repo_no_add () {
    REPONAME=${1:-test}
    if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
        COLLECTION_ID=${2:-org.test.Collection.${REPONAME}}
    else
        COLLECTION_ID=
    fi
    BRANCH=${3:-master}

    make_runtime "${REPONAME}" "${COLLECTION_ID}" "${BRANCH}" "${GPGARGS:-${FL_GPGARGS}}"
    GPGARGS="${GPGARGS:-${FL_GPGARGS}}" $(dirname $0)/make-test-app.sh repos/${REPONAME} "" "${BRANCH}" "${COLLECTION_ID}" > /dev/null
    update_repo $REPONAME "${COLLECTION_ID}"
    if [ $REPONAME == "test" ]; then
        httpd
    fi
}

setup_repo () {
    REPONAME=${1:-test}
    COLLECTION_ID=${2:-org.test.Collection.${REPONAME}}

    setup_repo_no_add "$@"

    port=$(cat httpd-port)
    if [ x${GPGPUBKEY:-${FL_GPG_HOMEDIR}/pubring.gpg} != x ]; then
        import_args=--gpg-import=${GPGPUBKEY:-${FL_GPG_HOMEDIR}/pubring.gpg}
    else
        import_args=
    fi
    if [ x${USE_COLLECTIONS_IN_CLIENT-} == xyes ] ; then
        collection_args=--collection-id=${COLLECTION_ID}
    else
        collection_args=
    fi

    flatpak remote-add ${U} ${collection_args} ${import_args} ${REPONAME}-repo "http://127.0.0.1:${port}/$REPONAME" >&2
}

setup_empty_repo () {
    REPONAME=${1:-test}
    COLLECTION_ID=${2:-org.test.Collection.${REPONAME}}

    if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
        COLLECTION_ID=${2:-org.test.Collection.${REPONAME}}
    else
        COLLECTION_ID=
    fi

    mkdir -p repos
    ostree --repo=repos/${REPONAME} init --mode=archive-z2 >&2
    update_repo $REPONAME "${COLLECTION_ID}"
    if [ $REPONAME == "test" ]; then
        httpd
    fi

    port=$(cat httpd-port)
    if [ x${GPGPUBKEY:-${FL_GPG_HOMEDIR}/pubring.gpg} != x ]; then
        import_args=--gpg-import=${GPGPUBKEY:-${FL_GPG_HOMEDIR}/pubring.gpg}
    else
        import_args=
    fi
    if [ x${USE_COLLECTIONS_IN_CLIENT-} == xyes ] ; then
        collection_args=--collection-id=${COLLECTION_ID}
    else
        collection_args=
    fi

    flatpak remote-add ${U} ${collection_args} ${import_args} ${REPONAME}-repo "http://127.0.0.1:${port}/$REPONAME" >&2
}

update_repo () {
    REPONAME=${1:-test}
    COLLECTION_ID=${2:-org.test.Collection.${REPONAME}}

    if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
        collection_args=--collection-id=${COLLECTION_ID}
    else
        collection_args=
    fi

    ${FLATPAK} build-update-repo ${BUILD_UPDATE_REPO_FLAGS-} ${collection_args} ${GPGARGS:-${FL_GPGARGS}} ${UPDATE_REPO_ARGS-} repos/${REPONAME} >&2
    if [ x${SUMMARY_FORMAT-} == xold ] ; then
        assert_not_has_file repos/${REPONAME}/summary.idx
    else
        assert_has_file repos/${REPONAME}/summary.idx
    fi
}

make_updated_app () {
    REPONAME=${1:-test}
    if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
        COLLECTION_ID=${2:-org.test.Collection.${REPONAME}}
    else
        COLLECTION_ID=""
    fi
    BRANCH=${3:-master}
    TEXT=${4:-UPDATED}
    APP_ID=${5:-""}
    RUNTIME_BRANCH=${6:-$BRANCH}

    RUNTIME_BRANCH=$RUNTIME_BRANCH GPGARGS="${GPGARGS:-${FL_GPGARGS}}" $(dirname $0)/make-test-app.sh repos/${REPONAME} "${APP_ID}" "${BRANCH}" "${COLLECTION_ID}" "${TEXT}" > /dev/null
    update_repo $REPONAME "${COLLECTION_ID}"
}

make_updated_runtime () {
    REPONAME=${1:-test}
    if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
        COLLECTION_ID=${2:-org.test.Collection.${REPONAME}}
    else
        COLLECTION_ID=""
    fi
    BRANCH=${3:-master}
    TEXT=${4:-UPDATED}

    GPGARGS="${GPGARGS:-${FL_GPGARGS}}" $(dirname $0)/make-test-runtime.sh repos/${REPONAME} org.test.Platform "${BRANCH}" "${COLLECTION_ID}" "${TEXT}" > /dev/null
    update_repo $REPONAME "${COLLECTION_ID}"
}

setup_sdk_repo () {
    REPONAME=${1:-test}
    if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
        COLLECTION_ID=${2:-org.test.Collection.${REPONAME}}
    else
        COLLECTION_ID=""
    fi
    BRANCH=${3:-master}

    GPGARGS="${GPGARGS:-${FL_GPGARGS}}" . $(dirname $0)/make-test-runtime.sh repos/${REPONAME} org.test.Sdk "${BRANCH}" "${COLLECTION_ID}" "" make mkdir cp touch > /dev/null
    update_repo $REPONAME "${COLLECTION_ID}"
}

install_repo () {
    REPONAME=${1:-test}
    BRANCH=${2:-master}
    ${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Platform ${BRANCH} >&2
    ${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Hello ${BRANCH} >&2
}

install_sdk_repo () {
    REPONAME=${1:-test}
    BRANCH=${2:-master}
    ${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Sdk ${BRANCH} >&2
}

run () {
    ${CMD_PREFIX} flatpak run "$@"

}

run_with_sandboxed_bus () {
    BUSSOCK=$(mktemp ${test_tmpdir}/bus.XXXXXX)
    rm -rf ${BUSSOCK}
    run --command=socat --filesystem=${test_tmpdir} org.test.Hello unix-listen:${BUSSOCK} unix-connect:/run/user/`id -u`/bus &
    while [ ! -e ${BUSSOCK} ]; do sleep 1; done
    DBUS_SESSION_BUS_ADDRESS="unix:path=${BUSSOCK}" "$@"
}

run_sh () {
    ID=${1:-org.test.Hello}
    shift
    ${CMD_PREFIX} flatpak run --command=bash ${ARGS-} ${ID} -c "$*"
}

# true, false, or empty for indeterminate
_flatpak_bwrap_works=

if [ -z "${FLATPAK_BWRAP:-}" ]; then
    # running installed-tests: assume we know what we're doing
    _flatpak_bwrap_works=true
elif ! "$FLATPAK_BWRAP" --unshare-ipc --unshare-net --unshare-pid \
        --ro-bind / / /bin/true > bwrap-result 2>&1; then
    _flatpak_bwrap_works=false
else
    _flatpak_bwrap_works=true
fi

have_working_bwrap() {
    [[ "${_flatpak_bwrap_works}" == "true" ]]
    return $?
}

# Use to skip all of these tests
skip() {
    echo "1..0 # SKIP" "$@"
    exit 0
}

skip_without_bwrap () {
    if "${_flatpak_bwrap_works}"; then
        return 0
    else
        sed -e 's/^/# /' < bwrap-result
        skip "Cannot run bwrap"
    fi
}

skip_one_without_bwrap () {
    if "${_flatpak_bwrap_works}"; then
        return 1
    else
        test_number=$(( test_number + 1 ))
        echo "ok $* # SKIP Cannot run bwrap"
        return 0
    fi
}

skip_without_fuse () {
    "${FUSERMOUNT}" --version >/dev/null 2>&1 || skip "no fusermount"

    capsh --print | grep -q 'Bounding set.*[^a-z]cap_sys_admin' || \
        skip "No cap_sys_admin in bounding set, can't use FUSE"

    [ -w /dev/fuse ] || skip "no write access to /dev/fuse"
    [ -e /etc/mtab ] || skip "no /etc/mtab"
}

skip_revokefs_without_fuse () {
    if [ "x${USE_SYSTEMDIR-}" = xyes ] && [ "x${FLATPAK_DISABLE_REVOKEFS-}" != xyes ]; then
        skip_without_fuse
    fi
}

skip_without_p2p () {
    if [ x${USE_COLLECTIONS_IN_CLIENT-} == xyes ] ; then
        return 0
    else
        skip "No P2P support enabled"
    fi
}

# Usage: skip_without_ostree_version 2019 2
skip_without_ostree_version () {
    OSTREE_YEAR_VERSION=$(ostree --version | sed -n "s/^ Version: '\([0-9]\+\)\.[0-9]\+'$/\1/p")
    OSTREE_RELEASE_VERSION=$(ostree --version | sed -n "s/^ Version: '[0-9]\+\.\([0-9]\+\)'$/\1/p")
    if [ "$OSTREE_YEAR_VERSION" -gt "$1" ]; then
        return 0
    elif [ "$OSTREE_YEAR_VERSION" -eq "$1" ] && [ "$OSTREE_RELEASE_VERSION" -ge "$2" ]; then
        return 0
    else
        skip "OSTree version requirement $1.$2 not met"
    fi
}

skip_without_libsystemd () {
  ${FLATPAK} history > history-log 2>&1 || true
  if  grep -q 'history not available without libsystemd' history-log; then
      skip "no libsystemd available"
  fi
}

skip_without_seccomp () {
    if [ "${HAVE_SECCOMP:-0}" != "1" ]; then
        skip "seccomp support disabled"
    fi
}

FLATPAK_SYSTEM_CERTS_D=$(pwd)/certs.d
export FLATPAK_SYSTEM_CERTS_D

sed s#@testdir@#${test_builddir}# ${test_srcdir}/session.conf.in > session.conf
dbus-daemon --fork --config-file=session.conf --print-address=3 --print-pid=4 \
    3> dbus-session-bus-address 4> dbus-session-bus-pid

DBUS_SESSION_BUS_ADDRESS="$(cat dbus-session-bus-address)"
export DBUS_SESSION_BUS_ADDRESS
DBUS_SESSION_BUS_PID="$(cat dbus-session-bus-pid)"

if ! /bin/kill -0 "$DBUS_SESSION_BUS_PID"; then
    assert_not_reached "Failed to start dbus-daemon"
fi

gdb_bt () {
    gdb -batch -ex "run" -ex "thread apply all bt" -ex "quit 1"  --args "$@"
}

commit_to_path () {
    COMMIT=$1
    EXT=$2
    echo "objects/$(echo $COMMIT | cut -b 1-2)/$(echo $COMMIT | cut -b 3-)".${EXT}
}

cleanup () {
    /bin/kill -9 $DBUS_SESSION_BUS_PID
    gpg-connect-agent --homedir "${FL_GPG_HOMEDIR}" killagent /bye >&2 || true
    "${FUSERMOUNT}" -u $XDG_RUNTIME_DIR/doc >&2 || :
    kill $(jobs -p) &> /dev/null || true
    if test -n "${TEST_SKIP_CLEANUP:-}"; then
        echo "# Skipping cleanup of ${TEST_DATA_DIR}"
    else
        rm -rf $TEST_DATA_DIR
    fi
}
trap cleanup EXIT

if test -n "${FLATPAK_TESTS_DEBUG:-}"; then
    set -x
fi

assert_semicolon_list_contains () {
    list="$1"
    member="$2"

    case ";$list;" in
        (*";$member;"*)
            ;;
        (*)
            assert_not_reached "\"$list\" should contain \"$member\""
            ;;
    esac
}

assert_not_semicolon_list_contains () {
    local list="$1"
    local member="$2"

    case ";$list;" in
        (*";$member;"*)
            assert_not_reached "\"$list\" should not contain \"$member\""
            ;;
    esac
}

push_gpg_homedir () {
    if [ ! -z ${OLD_FL_GPG_HOMEDIR+x} ]; then
        assert_not_reached "push_gpg_homedir() doesn’t actually implement a stack (yet)"
    fi

    export OLD_FL_GPG_HOMEDIR="${FL_GPG_HOMEDIR}"
    export FL_GPG_HOMEDIR="${TEST_DATA_DIR}/gpghome-pushed"
    mkdir -p "${FL_GPG_HOMEDIR}"
    cp $(dirname $0)/test-keyring/*.gpg "${FL_GPG_HOMEDIR}"
    cp -r $(dirname $0)/test-keyring/openpgp-revocs.d/ "${FL_GPG_HOMEDIR}"

    # Force gpg to update the storage format for secret keys before we end up
    # doing it as part of a random command in a test
    ${GPG} --homedir="${FL_GPG_HOMEDIR}" --list-secret-keys >&2
}

pop_gpg_homedir () {
    if [ -z ${FL_GPG_HOMEDIR+x} ]; then
        assert_not_reached "pop_gpg_homedir() called too many times"
    fi

    rm -rf "${FL_GPG_HOMEDIR}"
    export FL_GPG_HOMEDIR="${OLD_FL_GPG_HOMEDIR}"
    unset OLD_FL_GPG_HOMEDIR
}
