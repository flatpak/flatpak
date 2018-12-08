#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

pkg_install sudo which attr fuse bison \
    libubsan libasan libtsan clang python2 \
    elfutils git gettext-devel libappstream-glib-devel hicolor-icon-theme \
    dconf-devel \
    /usr/bin/{update-mime-database,update-desktop-database,gtk-update-icon-cache}
pkg_install_testing ostree-devel ostree
pkg_install_builddeps flatpak

build --enable-gtk-doc ${CONFIGOPTS:-}
