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
    # Wrap this to avoid set -x showing the echo commands
    {
        echo "ok $@";
        echo "================ $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]} ================";
    } 2> /dev/null
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
export FLATPAK_RUN_DIR=${TEST_DATA_DIR}/run
export FLATPAK_FANCY_OUTPUT=0

export HOME=${TEST_DATA_DIR}/home
export XDG_CACHE_HOME=${TEST_DATA_DIR}/home/cache
export XDG_CONFIG_HOME=${TEST_DATA_DIR}/home/config
export XDG_DATA_HOME=${TEST_DATA_DIR}/home/share
export XDG_RUNTIME_DIR=${TEST_DATA_DIR}/runtime

export XDG_DESKTOP_PORTAL_DIR=${test_builddir}/share/xdg-desktop-portal/portals
export XDG_CURRENT_DESKTOP=test

export USERDIR=${TEST_DATA_DIR}/home/share/flatpak
export SYSTEMDIR=${TEST_DATA_DIR}/system
export ARCH=`flatpak --default-arch`

if [ x${USE_SYSTEMDIR-} == xyes ] ; then
    export FL_DIR=${SYSTEMDIR}
    export U=
    export INVERT_U=--user
else
    export FL_DIR=${USERDIR}
    export U="--user"
    export INVERT_U=--system
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
    if ostree config --repo=$FL_DIR/repo get --group 'remote "'"$1"'"' "$2" > /dev/null; then
        echo 1>&2 "Remote '$1' unexpectedly has key '$2' at $(basename ${BASH_SOURCE[1]}):${BASH_LINENO[0]}"
        exit 1
    fi
    } 3> /dev/null
}

export FL_GPG_HOMEDIR=${TEST_DATA_DIR}/gpghome
export FL_GPG_HOMEDIR2=${TEST_DATA_DIR}/gpghome2
mkdir -p ${FL_GPG_HOMEDIR}
mkdir -p ${FL_GPG_HOMEDIR2}
# This need to be writable, so copy the keys
cp $(dirname $0)/test-keyring/*.gpg ${FL_GPG_HOMEDIR}/
cp $(dirname $0)/test-keyring2/*.gpg ${FL_GPG_HOMEDIR2}/

export FL_GPG_ID=7B0961FD
export FL_GPG_ID2=B2314EFC
export FL_GPGARGS="--gpg-homedir=${FL_GPG_HOMEDIR} --gpg-sign=${FL_GPG_ID}"
export FL_GPGARGS2="--gpg-homedir=${FL_GPG_HOMEDIR2} --gpg-sign=${FL_GPG_ID2}"
export FL_GPG_BASE64="mQENBFbPBvoBCADWbz5O+XzuyN+dDExK81pci+gIzBNWB+7SsN0EgoJppKgwBCX+Bd6ERe9Yz0nJbJB/tjazRp7MnnoPnh6fXnhIbHA766/Eciy4sL5X8laqDmWmROCqCe79QZH/w6vYTKsDmoLQrw9eKRP1ilCvECNGcVdhIyfTDlNrU//uy5U4h2PVUz1/Al87lvaJnrj5423m5GnX+qpEG8mmpmcw52lvXNPuC95ykylPQJjI0WnOuaTcxzRhm5eHPkqKQ+nPIS+66iw1SFdobYuye/vg/rDiyp8uyQkh7FWXnzHxz4J8ovesnrCM7pKI4VEHCnZ4/sj2v9E3l0wJlqZxLTULaV3lABEBAAG0D1hkZy1hcHAgdGVzdGluZ4kBOAQTAQIAIgUCVs8G+gIbAwYLCQgHAwIGFQgCCQoLBBYCAwECHgECF4AACgkQE4sx4HsJYf2DiAf7BQ8anU3CgYpJjuO2rT8jQPO0jGRCNaPyaeAcBx8IjFkjf8daKMPCAt6gQioEpC8OhDig86Bl5piYOB7L7JSB53mgUrADJXhgC/dG4soCt7/U4wW30MseXdlXSOqHGApblF/bIs4B30OBGReBj3DcWIqyb48GraSKlPlaCpkZFySNEAcGUCeCqbbygxCQAM8MDq9FgVRk5oVrE/nAUm6oScEBhseoB7+CaHaRTmLoe/SBs0z2AJ7alIH1Sv4X3mQXpfsAIcWf3Zu2MZydF/Vuh8vTMROwPYtOVEtGxZvEBN3h5uc88dHSk928maqsop9T6oEwM43mBKCOu1gdAOw4OLkBDQRWzwb6AQgAx/XuEaQvdI3J2YYmOE6RY0jJZXLauXH46cJR4q70mlDev/OqYKTSLlo4q06D4ozCwzTYflppDak7kmjWMN224/u1koqFOtF76LsglAeLaQmweWmX0ecbPrzFYaX30kaQAqQ9Wk0PRe0+arRzWDWfUv3qX3y1decKUrBCuEC6WvVVwooWs+zX0cUBS8CROhazTjvXFAz36mhK0u+B3WCBlK+T2tIPOjLjlYgzYARw+X7/J6B3C798r2Hw/yXqCDcKLrq7WWUB33kv3buuG2G6LUamctdD8IsTBxi+nIjAvQITFqq4cPbbXAJGaAnWGuLOddQ9e/GhCOI4JjopRnnjOwARAQABiQEfBBgBAgAJBQJWzwb6AhsMAAoJEBOLMeB7CWH9TC8H/A6oreCxeiL8DPOWN29OaQ5sEw7Dg7bnLSZLu8aREgwfCiFSv0numOABjn/G89Y5M6NiEXFZZhUa+SXOALiBLUy98O84lyp9hlP9qGbWRgBXwe5vOAJERqtoUwR5bygpAw5Nc4y3wddPC4vH7upJ8ftU/eEFtPdI0cKrrAZDFdhXFp3RxdCC6fD62wbofE0mo1Ea1iD3xqVh2t7jfWN1RhMV308htHRGkkmWcEbbvHqugwL6dWZEvQmLYi6/7tQyA1KdG4AZksBP/MBi3t2hthRqQx1v52JwdCaZNuItuEe5rWXhfvoGxPoqYZt9ZPjna6yJfcfJwPbMfjNwX2LR4p4="
export FL_GPG_BASE642="mQENBFkSyx4BCACq/8XFcF+NTpJKfoo8F6YyR8RQXww6kCV47zN78Dt7aCh43WSYLRUBRt1tW5MRT8R60pwCsGvKnFiNS2Vqe4T1IW4mDnFMZIZJXdNVwKUqVBPL/jzkIDnQ9NXtuPNH0qET6VhYnb9aykLo/MiBmx6q+4MvYd/qwiN8kstRifRIxjjZx6wsg+muY6yx9fZKxlgvhc3nsrl3oyDo7/+V+b3heYLtMCQFwlHRKz3Yf2X9H0aUSbDYcgTy6w3q94HVNCpJSqeiR+kBG175BQYKR2l7WYdaVPFf5LMEvAJh0SGnqu77X+8TYYRQiiBB5fYjGOeHfOh6uH5GAJRQymVIJwy/ABEBAAG0KkZsYXRwYWsgKFRlc3Qga2V5IDIpIDxmbGF0cGFrQGZsYXRwYWsub3JnPokBOAQTAQIAIgUCWRLLHgIbAwYLCQgHAwIGFQgCCQoLBBYCAwECHgECF4AACgkQdZ9f0LIxTvyeUQf/euAZpipXBkGWxeW4G10r1QRi2tZAWNeLpy8SB17eo9E6yB61SdH80jALborVs/plnZzKcFf+nLvjCn51FLMh6QPL3S+079WHsed//qtUWfbJ85hLevfCMTZMLktUmqwwUh238WW/gKtbUjYOqr1IZSMBoMiQtc0iOVBP7HUdhYigxTKvs/MBEGHANeQkY07ZnX9oFXElOo+EIPAHScwEOSwEVrXUVHpQODzIfjOoPUHWAZtM1yJT+iWmVHe4HtU8CyBnPyUcnTmTWKr92QmgfWkb1T7ugT5gXt/6ZlYAaZGnr9yNuSk3MMhDMOyldtJBM5Zl8eScE9KBf7pRJoxnMLkBDQRZEsseAQgAvA29IyiJpB+jUHj3MOyVyTBOmvLme+0Ndhpt/mTh+swchJUvzb0IzQS9Le5yVAvn+ppAtDCMb+bV4Xh5zrbiH0Hu0qwK4Qk+KcIKRE8ImDiUM8NFE2SZoomZSsgZ1NBWbAdEyVpkBfrt3Dd8FssMrwPF6kqo02TZr7Pxng+BEHUZT6jPCxueqyXyv2cLbQMe1H0U7klsxPmnnIYUqdwOmPxUspVEYP9oJb5y123mx0yj5JuYdZMjWbP3cRLox1RKIlFWgQqOn2yJiEoWzpqdbtb7sE3ggnbZKJED0ZxUZIakjnyMhX+GAEA8ZMZ6+HfDt1iHV8qHcYiLW5A3AQTxZwARAQABiQEfBBgBAgAJBQJZEsseAhsMAAoJEHWfX9CyMU78Ns4IAJRQ5UJ9KkeZClHm1EjYlgsAq1UJr9wgbyBFKTEkGZ/CAvVmgg+BUXcN/SPAkELbEAOJZTyv8C5cuJC49iFHOxUbRZXZ5eN2SvhZzl+5gep2uHwVLdqRIxFDTHbLWnmtHxPeU7IRA9u86q3wV1N0pD7kreNN7BWKY3/tI33hY2/XVVFy0MN5sutPn+lVK66MqAHqtode5xqqz9Z8LmS7LlqokQkAytcGd6Xqsx99NTk8kk3bnk9HWsAvDO8tRZroeseKeRNmbhGvCNUxPSB6bpYBJLvQtjA9ZVv6sNm0E+SuiXKizZkBGO5AH50pDoy0+MCGoOhwwXeY5+1kZAOzkMI="

make_runtime () {
    REPONAME="$1"
    COLLECTION_ID="$2"
    BRANCH="$3"
    GPGARGS="$4"

    RUNTIME_REF="runtime/org.test.Platform/$(flatpak --default-arch)/${BRANCH}"
    if [ ! -z "${SRC_RUNTIME_REPO:-}" ]; then
        RUNTIME_REPO=repos/${SRC_RUNTIME_REPO}
    elif [ -f ${test_builddir}/runtime-repo/${RUNTIME_REF} ]; then
        RUNTIME_REPO=${test_builddir}/runtime-repo
    else
        RUNTIME_REPO=${TEST_DATA_DIR}/runtime-repo
        (
            flock -s 200
            if [ ! -d ${RUNTIME_REPO} ]; then
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
        ostree --repo=repos/${REPONAME} init --mode=archive-z2 ${collection_args}
    fi

    flatpak build-commit-from --disable-fsync --src-repo=${RUNTIME_REPO} --force ${GPGARGS} repos/${REPONAME}  ${RUNTIME_REF}
}

httpd () {
    COMMAND=${1:-web-server.py}
    DIR=${2:-repos}

    rm -f httpd-pipe
    mkfifo httpd-pipe
    PYTHONUNBUFFERED=1 $(dirname $0)/$COMMAND "$DIR" 3> httpd-pipe 2>&1 | tee --append httpd-log &
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

    flatpak remote-add ${U} ${collection_args} ${import_args} ${REPONAME}-repo "http://127.0.0.1:${port}/$REPONAME"
}

update_repo () {
    REPONAME=${1:-test}
    COLLECTION_ID=${2:-org.test.Collection.${REPONAME}}

    if [ x${USE_COLLECTIONS_IN_SERVER-} == xyes ] ; then
        collection_args=--collection-id=${COLLECTION_ID}
    else
        collection_args=
    fi

    ${FLATPAK} build-update-repo ${collection_args} ${GPGARGS:-${FL_GPGARGS}} ${UPDATE_REPO_ARGS-} repos/${REPONAME}
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
    ${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Platform ${BRANCH}
    ${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Hello ${BRANCH}
}

install_sdk_repo () {
    REPONAME=${1:-test}
    BRANCH=${2:-master}
    ${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Sdk ${BRANCH}
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
        echo "ok $* # SKIP Cannot run bwrap"
        return 0
    fi
}

skip_without_fuse () {
    fusermount --version >/dev/null 2>&1 || skip "no fusermount"

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

sed s#@testdir@#${test_builddir}# ${test_srcdir}/session.conf.in > session.conf
dbus-daemon --fork --config-file=session.conf --print-address=3 --print-pid=4 \
    3> dbus-session-bus-address 4> dbus-session-bus-pid
export DBUS_SESSION_BUS_ADDRESS="$(cat dbus-session-bus-address)"
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
    gpg-connect-agent --homedir "${FL_GPG_HOMEDIR}" killagent /bye || true
    fusermount -u $XDG_RUNTIME_DIR/doc || :
    kill $(jobs -p) &> /dev/null || true
    if test -n "${TEST_SKIP_CLEANUP:-}"; then
        echo "Skipping cleanup of ${TEST_DATA_DIR}"
    else
        rm -rf $TEST_DATA_DIR
    fi
}
trap cleanup EXIT

if test -n "${FLATPAK_TESTS_DEBUG:-}"; then
    set -x
fi
