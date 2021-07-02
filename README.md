libglnx is the successor to [libgsystem](https://gitlab.gnome.org/Archive/libgsystem).

It is for modules which depend on both GLib and Linux, intended to be
used as a git submodule.

Features:

 - File APIs which use `openat()` like APIs, but also take a `GCancellable`
   to support dynamic cancellation
 - APIs also have a `GError` parameter
 - High level "shutil", somewhat inspired by Python's
 - A "console" API for tty output
 - A backport of the GLib cleanup macros for projects which can't yet take
   a dependency on 2.40.

Why?
----

There are multiple projects which have a hard dependency on Linux and
GLib, such as NetworkManager, ostree, flatpak, etc.  It makes sense
for them to be able to share Linux-specific APIs.

This module also contains some code taken from systemd, which has very
high quality LGPLv2+ shared library code, but most of the internal
shared library is private, and not namespaced.

One could also compare this project to gnulib; the salient differences
there are that at least some of this module is eventually destined for
inclusion in GLib.

Adding this to your project
---------------------------

## Meson

First, set up a Git submodule:

```
git submodule add https://gitlab.gnome.org/GNOME/libglnx subprojects/libglnx
```

Or a Git [subtree](https://github.com/git/git/blob/master/contrib/subtree/git-subtree.txt):

```
git remote add libglnx https://gitlab.gnome.org/GNOME/libglnx.git
git fetch libglnx
git subtree add -P subprojects/libglnx libglnx/master
```

Then, in your top-level `meson.build`:

```
libglnx_dep = subproject('libglnx').get_variable('libglnx_dep')
# now use libglnx_dep in your dependencies
```

Porting from libgsystem
-----------------------

For all of the filesystem access code, libglnx exposes only
fd-relative API, not `GFile*`.  It does use `GCancellable` where
applicable.

For local allocation macros, you should start using the `g_auto`
macros from GLib.  A backport is included in libglnx.  There are a few
APIs not defined in GLib yet, such as `glnx_autofd`.

`gs_transfer_out_value` is replaced by `g_steal_pointer`.

Contributing
------------

Development happens in GNOME Gitlab: https://gitlab.gnome.org/GNOME/libglnx

(If you're seeing this on the Github mirror, we used to do development
 on Github but that was before GNOME deployed Gitlab.)

<!--
Copyright 2015-2018 Colin Walters
Copyright 2019 Endless OS Foundation LLC
SPDX-License-Identifier: LGPL-2.1-or-later
-->
