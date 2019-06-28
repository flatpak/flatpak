#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

pkg_install sudo which attr fuse bison dbus-daemon \
    libubsan libasan libtsan clang python2 \
    elfutils git gettext-devel libappstream-glib-devel hicolor-icon-theme \
    dconf-devel fuse-devel meson dbus-devel \
    /usr/bin/{update-mime-database,update-desktop-database,gtk-update-icon-cache}
pkg_install_testing ostree-devel ostree
pkg_install gdk-pixbuf2-modules # needed to make icon validation work
pkg_install_builddeps flatpak

# malcontent isn’t packaged for our CI distributions yet
git clone https://gitlab.freedesktop.org/pwithnall/malcontent.git /tmp/malcontent
pushd /tmp/malcontent
git checkout tags/0.4.0
meson setup --prefix=/usr _build
ninja -C _build
ninja -C _build install
popd

build --enable-gtk-doc ${CONFIGOPTS:-}
