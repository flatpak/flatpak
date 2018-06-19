#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

pkg_install sudo which attr fuse bison \
    libubsan libasan libtsan \
    elfutils git gettext-devel libappstream-glib-devel \
    /usr/bin/{update-mime-database,update-desktop-database,gtk-update-icon-cache}
pkg_install_testing ostree-devel ostree
pkg_install_if_os fedora gjs parallel clang python2
pkg_install_builddeps flatpak

pkg_install_builddeps ostree
(git clone --depth=1 https://github.com/ostreedev/ostree.git
 cd ostree
 unset CFLAGS # the sanitizers require calling apps be linked too
 build --disable-introspection
 make install
)

build --enable-gtk-doc ${CONFIGOPTS:-}
