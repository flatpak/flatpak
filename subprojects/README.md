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

To update the version Flatpak gets compiled with, edit libglnx.wrap.

variant-schema-compiler
-----------------------

Upstream: <https://gitlab.gnome.org/alexl/variant-schema-compiler>

This is a "copylib" like libglnx.

To update the version Flatpak gets compiled with, edit
variant-schema-compiler.wrap.
