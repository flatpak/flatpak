Subprojects built as part of Flatpak
====================================

<!-- This document:
Copyright 2023-2024 Collabora Ltd.
SPDX-License-Identifier: MIT
-->

bubblewrap
----------

Upstream: <https://github.com/containers/bubblewrap>

To use a system copy instead, configure with `-Dsystem_bubblewrap=bwrap`
or similar.

To update the suggested version, edit bubblewrap.wrap.

dbus-proxy
----------

Upstream: <https://github.com/flatpak/xdg-dbus-proxy>

To use a system copy instead, configure with
`-Dsystem_dbus_proxy=xdg-dbus-proxy` or similar.

To update the suggested version, edit dbus-proxy.wrap.

libglnx
-------

Upstream: <https://gitlab.gnome.org/GNOME/libglnx/>

This is a "copylib", similar to gnulib, which only supports being
integrated as a subproject and does not guarantee a stable API.
A suitable version is vendored into Flatpak using `git subtree`, to make
our source releases self-contained (if system copies of bubblewrap and
dbus-proxy are used).

To compare with upstream:

    git remote add --no-tags libglnx https://gitlab.gnome.org/GNOME/libglnx.git
    git fetch libglnx
    git diff HEAD:subprojects/libglnx libglnx/master

To merge from upstream:

    git fetch libglnx
    git subtree merge -P subprojects/libglnx libglnx/master
    git commit --amend -s

variant-schema-compiler
-----------------------

Upstream: <https://gitlab.gnome.org/alexl/variant-schema-compiler>

This is a "copylib" like libglnx.

To compare with upstream or merge from upstream, the procedure is similar
to libglnx (see above).
