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

# We default to use systemdir, so we can also test shadowing from the user dir
export USE_SYSTEMDIR=yes

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_revokefs_without_fuse

echo "1..2"

setup_empty_repo &> /dev/null > /dev/null

# Manually add the user remote too
$FLATPAK remote-add --user --gpg-import=${FL_GPG_HOMEDIR}/pubring.gpg test-repo "http://127.0.0.1:${port}/test"


# This tests the detection of unused refs. Used refs are any that have
# some kind of dependency from an installed application to the ref.
#
# Possible reasons a ref are used:
# Direct use:
# * App is installed
# * Runtime is marked as pinned and installed
#
# Dependent ref (of a used ref)
# * App $USED uses runtime $DEP
# * App $USED uses runtime $DEP as sdk (so that if you're developing the app we don't remove the sdk)
# * Runtime $USED has extra-data and not NoRuntime, and lists $DEP as "ExtensionOf"
# * Runtime $USED uses runtime $DEP as sdk
#
# Related ref (of a used ref):
# * App $USED uses extension $REL, which is not marked as autoprune-unless
# * Runtime $USED uses extension $REL, which is not marked as autoprune-unless
#
# There are options to affect this:
# * Specific refs can be excluded (never considered used), used to emulate them being uninstalled in a transaction
# * Specify custom new metadata for an installed ref, used to emulate it being updated in a transaction
#
# Additionally, for the system installation, the dependencies are considered from the user installation, such that any dependencies
# in the user installation that are resolved from the system installation (i.e. not in the user installation) are considered used.
# I.E. if an app and its runtime both are installed in the user dir, it is not considered used in the system installation, but if
# only the app is, then it is considered used.

create_runtime () {
    local ID="${1}"
    local SDK="${2}"

    local BRANCH=stable
    local DIR=`mktemp -d`

    mkdir ${DIR}/files
    mkdir ${DIR}/usr
    cat > ${DIR}/metadata <<EOF
[Runtime]
name=${ID}
runtime=${ID}/$ARCH/$BRANCH
EOF

    if test x$SDK != "x"; then
       cat >> ${DIR}/metadata <<EOF
sdk=${SDK}/$ARCH/$BRANCH
EOF
    fi

    $FLATPAK build-finish $DIR ${finish_args[$ID]:-} &> /dev/null > /dev/null
    $FLATPAK build-export -v ${FL_GPGARGS} --disable-sandbox --runtime repos/test ${DIR} ${BRANCH} &> /dev/null > /dev/null
    rm -rf ${DIR}
}

create_app () {
    local ID="${1}"
    local RUNTIME="${2}"
    local SDK="${3}"

    local BRANCH=stable
    local DIR=`mktemp -d`

    mkdir ${DIR}/files
    cat > ${DIR}/metadata <<EOF
[Application]
name=${ID}
runtime=${RUNTIME}/$ARCH/$BRANCH
EOF

    if test x$SDK != "x"; then
       cat >> ${DIR}/metadata <<EOF
sdk=${SDK}/$ARCH/$BRANCH
EOF
    fi

    set -x
    $FLATPAK build-finish ${DIR}  ${finish_args[$ID]:-} &> /dev/null > /dev/null

    $FLATPAK build-export ${FL_GPGARGS} --disable-sandbox repos/test ${DIR} ${BRANCH} &> /dev/null > /dev/null
    rm -rf ${DIR}
}

declare -a runtimes
declare -A runtimes_sdk
declare -A runtimes_pinned
declare -a apps
declare -A apps_runtime
declare -A apps_sdk

declare -A finish_args

declare -A installs_in

make_runtime() {
    { set +x; } 2>/dev/null
    local ID="org.runtime.${1}"
    local SDK="${2:-}"
    if test x$SDK != x; then
        SDK=org.runtime.$SDK
    fi
    runtimes+=( "$ID" )
    runtimes_sdk["$ID"]="$SDK"
    set -x
}

pin_runtime() {
    { set +x; } 2>/dev/null
    local ID="org.runtime.${1}"
    runtimes_pinned["$ID"]="yes"
    set -x
}

install_runtime_in() {
    { set +x; } 2>/dev/null
    local ID="org.runtime.${1}"
    local WHERE="${2}"
    installs_in["$ID"]=$WHERE
    set -x
}

make_extension() {
    { set +x; } 2>/dev/null
    local ID="org.extension.${1}"
    runtimes+=( "$ID" )
    set -x
}

pin_extension() {
    { set +x; } 2>/dev/null
    local ID="org.extension.${1}"
    runtimes_pinned["$ID"]="yes"
    set -x
}

install_extension_in() {
    { set +x; } 2>/dev/null
    local ID="org.extension.${1}"
    local WHERE="${2}"
    installs_in["$ID"]=$WHERE
    set -x
}

runtime_add_extension() {
    { set +x; } 2>/dev/null
    local RUNTIME="org.runtime.${1}"
    local EXT="org.extension.${2}"
    finish_args["$RUNTIME"]="${finish_args[$RUNTIME]:-} --extension=${EXT}=directory=/ext"
    set -x
}

runtime_add_autoprune_extension() {
    { set +x; } 2>/dev/null
    local RUNTIME="org.runtime.${1}"
    local EXT="org.extension.${2}"
    finish_args["$RUNTIME"]="${finish_args[$RUNTIME]:-} --extension=${EXT}=directory=/ext --extension=${EXT}=autoprune-unless=active-gl-driver"
    set -x
}

make_runtime_full() {
    make_runtime "$1" "${2:-}"
    make_extension "$1.Locale"
    runtime_add_extension "$1" $1.Locale
    make_extension "$1.Debug"
    runtime_add_extension "$1" $1.Debug
}

app_add_extension() {
    { set +x; } 2>/dev/null
    local APP="org.app.${1}"
    local EXT="org.extension.${2}"
    finish_args["$APP"]="${finish_args[$APP]:-} --extension=${EXT}=directory=/ext"
    set -x
}

install_app_in() {
    { set +x; } 2>/dev/null
    local ID="org.app.${1}"
    local WHERE="${2}"
    installs_in["$ID"]=$WHERE
    set -x
}

app_add_autoprune_extension() {
    { set +x; } 2>/dev/null
    local APP="org.app.${1}"
    local EXT="org.extension.${2}"
    finish_args["$RUNTIME"]="${finish_args[$APP]:-} --extension=${EXT}=directory=/ext --extension=${EXT}=autoprune-unless=active-gl-driver"
    set -x
}

make_app() {
    { set +x; } 2>/dev/null
    local ID="org.app.${1}"
    local RUNTIME="org.runtime.${2}"
    local SDK="${3:-}"
    if test x$SDK != x; then
        SDK=org.runtime.$SDK
    fi

    apps+=( $ID )
    apps_runtime[$ID]="$RUNTIME"
    apps_sdk[$ID]="$SDK"
    set -x
}

make_app_full() {
    make_app "$1" "$2" "${3:-}"
    make_extension "$1.Locale"
    app_add_extension "$1" $1.Locale
    make_extension "$1.Debug"
    app_add_extension "$1" $1.Debug
}

make_it_happen() {
    { set +x; } 2>/dev/null
    for runtime in "${runtimes[@]}"; do
        create_runtime "$runtime" "${runtimes_sdk[$runtime]:-}"
    done
    for app in "${apps[@]}"; do
        create_app "$app" "${apps_runtime[$app]}" "${apps_sdk[$app]}"
    done
    update_repo  &> /dev/null > /dev/null

    for runtime in "${runtimes[@]}"; do
        local in=${installs_in[${runtime}]:-system}
        if test $in = system -o $in = both; then
            $FLATPAK install -y --system --no-deps --no-related --no-auto-pin test-repo runtime/$runtime/$ARCH/stable  &> /dev/null > /dev/null
        fi
        if test $in = user -o $in = both; then
            $FLATPAK install -y --user --no-deps --no-related --no-auto-pin test-repo runtime/$runtime/$ARCH/stable  &> /dev/null > /dev/null
        fi
        if test "x${runtimes_pinned[$runtime]:-}" == "xyes"; then
            $FLATPAK pin $runtime//stable
        fi
    done
    for app in "${apps[@]}"; do
        local in=${installs_in[$app]:-system}
        if test $in = system -o $in = both; then
            $FLATPAK install -y --system --no-deps --no-related --no-auto-pin test-repo app/$app/$ARCH/stable  &> /dev/null > /dev/null
        fi
        if test $in = user -o $in = both; then
            $FLATPAK install -y --user --no-deps --no-related --no-auto-pin test-repo app/$app/$ARCH/stable  &> /dev/null > /dev/null
        fi
    done

    $FLATPAK list --system -a --columns=ref | sort > installed.txt
    $FLATPAK list --user -a --columns=ref | sort > user-installed.txt

    # No USER_ should be in system dir
    assert_not_file_has_content installed.txt USER_

    set -x
}

verify_unused() {
    comm -23 installed.txt unused.txt > used.txt
    comm -12 used.txt unused.txt > used-and-unused.txt
    assert_file_empty used-and-unused.txt

    #echo ======= UNUSED ==============
    #cat unused.txt
    #echo ======= USED ==============
    #cat used.txt
    #echo =============================

    assert_not_file_has_content unused.txt "\.USED"
    assert_not_file_has_content used.txt "\.UNUSED"
    # Also, no app or app extensions are unused
    assert_not_file_has_content unused.txt "\.APP"
}

# This is used for the autoprune check
export FLATPAK_GL_DRIVERS=ACTIVEGL

# Runtime for app A, and it has its own sdk
make_runtime_full USED_A USED_A_SDK
make_runtime_full USED_A_SDK
# Sdk for app A, and it has its own sdk
make_runtime USED_A_SDK2 USED_A_SDK3
make_runtime USED_A_SDK3
# App A
make_app_full APP_A USED_A USED_A_SDK2

# Plain unused runtime
make_runtime_full UNUSED_B

# unused runtime with sdk
make_runtime_full UNUSED_C UNUSED_B_SDK
make_runtime_full UNUSED_C_SDK

# Unused extension and dependency
make_extension UNUSED_D

# Pinned runtime
make_runtime_full USED_E
pin_runtime USED_E

# Pinned extension
make_extension USED_F
pin_extension USED_F

# app with runtime with autopruned extension
make_app APP_G USED_G
make_runtime USED_G
make_extension USED_G.ACTIVEGL
runtime_add_autoprune_extension USED_G USED_G.ACTIVEGL
make_extension UNUSED_G.NONACTIVEGL
runtime_add_autoprune_extension USED_G UNUSED_G.NONACTIVEGL

# unused runtime with autopruned extension
make_runtime UNUSED_H
make_extension UNUSED_H.ACTIVEGL
runtime_add_autoprune_extension UNUSED_H UNUSED_H.ACTIVEGL
make_extension UNUSED_H.NONACTIVEGL
runtime_add_autoprune_extension UNUSED_H UNUSED_H.NONACTIVEGL

# pinned runtime with autopruned extension
make_runtime USED_I
pin_runtime USED_I
make_extension USED_I.ACTIVEGL
runtime_add_autoprune_extension USED_I USED_I.ACTIVEGL
make_extension UNUSED_I.NONACTIVEGL
runtime_add_autoprune_extension USED_I UNUSED_I.NONACTIVEGL

# System runtime used by user app
make_runtime USED_J
make_app USER_APP_J USED_J
install_app_in USER_APP_J user

# System runtime shadowed by user app and runtime, but with and extension that isn't shadowed and one that is
make_extension USED_EXT_K
make_extension UNUSED_EXT2_K
install_extension_in UNUSED_EXT2_K both
make_runtime UNUSED_K
install_runtime_in UNUSED_K both
runtime_add_extension UNUSED_K USED_EXT_K
runtime_add_extension UNUSED_K UNUSED_EXT2_K
make_app USER_APP_K UNUSED_K
install_app_in USER_APP_K user

make_it_happen

# Verify that the right thing got user-installed
assert_file_has_content user-installed.txt USER_APP_J
assert_file_has_content user-installed.txt USER_APP_K
assert_file_has_content user-installed.txt UNUSED_K
assert_file_has_content user-installed.txt UNUSED_EXT2_K
assert_not_file_has_content user-installed.txt USED_EXT_K

${test_builddir}/list-unused | sed s@^app/@@g | sed s@^runtime/@@g | sort > unused.txt

verify_unused

ok "list unused regular"

mv unused.txt old-unused.txt

${test_builddir}/list-unused --exclude app/org.app.APP_A/x86_64/stable | sed s@^app/@@g | sed s@^runtime/@@g | sort > unused.txt

# We don't report the excluded ref itself as unused. It's as if it wasn't even installed
assert_not_file_has_content unused.txt "org.app.APP_A/"

# Excluding a ref should not use more refs
comm -23 old-unused.txt unused.txt > newly-used.txt
assert_file_empty newly-used.txt

# We should add all dependencies from the app, but no more
assert_file_has_content unused.txt "org.extension.APP_A.Debug/"
assert_file_has_content unused.txt "org.extension.APP_A.Locale/"
assert_file_has_content unused.txt "org.runtime.USED_A/"
assert_file_has_content unused.txt "org.extension.USED_A.Debug/"
assert_file_has_content unused.txt "org.extension.USED_A.Locale/"
assert_file_has_content unused.txt "org.runtime.USED_A_SDK/"
assert_file_has_content unused.txt "org.extension.USED_A_SDK.Debug/"
assert_file_has_content unused.txt "org.extension.USED_A_SDK.Locale/"
assert_file_has_content unused.txt "org.runtime.USED_A_SDK2/"
assert_file_has_content unused.txt "org.runtime.USED_A_SDK3/"

comm -13 old-unused.txt unused.txt > newly-unused.txt
if [ $(cat newly-unused.txt | wc -l) -ne 10 ]; then
    assert_not_reached "Unexpected unused ref"
fi

ok "list unused exclude"
