#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

pkg_install sudo which attr fuse \
    libubsan libasan libtsan elfutils-libelf-devel libdwarf-devel \
    elfutils git gettext-devel ostree-devel ostree \
    /usr/bin/{update-mime-database,update-desktop-database,gtk-update-icon-cache}
pkg_install_if_os fedora gjs parallel clang
pkg_install_builddeps flatpak

build --enable-gtk-doc ${CONFIGOPTS:-}
