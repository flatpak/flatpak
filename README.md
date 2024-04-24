# Flatpak 1.4.x: end-of-life branch

The Flatpak 1.4.x branch is no longer supported and does not receive
security fixes.
Please upgrade to a supported branch as documented in
[SECURITY.md](https://github.com/flatpak/flatpak/blob/main/SECURITY.md).

If maintainers of long-term-supported OS distributions are still
distributing Flatpak 1.4.x, those vendors will need to either take
responsibility for backporting security fixes, or upgrade their Flatpak
packages.

---

[![Language grade: Python](https://img.shields.io/lgtm/grade/python/g/flatpak/flatpak.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/flatpak/flatpak/context:python)

Flatpak is a system for building, distributing, and running sandboxed
desktop applications on Linux.

See https://flatpak.org/ for more information.

Community discussion happens in [#flatpak on Freenode](ircs://chat.freenode.net/flatpak) and on [the mailing list](https://lists.freedesktop.org/mailman/listinfo/flatpak).

Read documentation for the flatpak [commandline tools](http://docs.flatpak.org/en/latest/flatpak-command-reference.html) and for the libflatpak [library API](http://flatpak.github.io/flatpak/reference/html/index.html).

# Contributing

Flatpak welcomes contributions from anyone! Here are some ways you can help:
* Fix [one of the issues](https://github.com/flatpak/flatpak/issues/) and submit a PR
* Update flatpak's translations and submit a PR
* Improve flatpak's documentation, hosted at http://docs.flatpak.org and developed over in [flatpak-docs](https://github.com/flatpak/flatpak-docs)
* Find a bug and [submit a detailed report](https://github.com/flatpak/flatpak/issues/new) including your OS, flatpak version, and the steps to reproduce
* Add your favorite application to [Flathub](https://flathub.org) by writing a flatpak-builder manifest and [submitting it](https://github.com/flathub/flathub/wiki/App-Submission)
* Improve the [Flatpak support](https://github.com/flatpak/flatpak/wiki/Distribution) in your favorite Linux distribution

# Hacking
Flatpak uses a traditional autoconf-style build mechanism. To build just do
```
 ./autogen.sh
 ./configure [args]
 make
 make install
```

To automatically install dependencies on apt-based distributions you can try
running `apt build-dep flatpak` and on dnf ones try `dnf builddep flatpak`.
Dependencies you will need include: autoconf, automake, libtool, bison,
gettext, gtk-doc, gobject-introspection, libcap, libarchive, libxml2, libsoup,
gpgme, polkit, libXau, ostree, json-glib, appstream, libseccomp (or their devel
packages).

Most configure arguments are documented in `./configure --help`. However,
there are some options that are a bit more complicated.

Flatpak relies on a project called [Bubblewrap](https://github.com/projectatomic/bubblewrap) for the
low-level sandboxing.  By default, an in-tree copy of this is built
(distributed in the tarball or using git submodules in the git
tree). This will build a helper called flatpak-bwrap. If your system
has a recent enough version of Bubblewrap already, you can use
`--with-system-bubblewrap` to use that instead.

Bubblewrap can run in two modes, either using unprivileged user
namespaces or setuid mode. This requires that the kernel supports this,
which some distributions disable. For instance, Debian and Arch 
([linux](https://www.archlinux.org/packages/?name=linux) kernel v4.14.5
or later), support user namespaces with the `kernel.unprivileged_userns_clone`
sysctl enabled.

If unprivileged user namespaces are not available, then Bubblewrap must
be built as setuid root. This is believed to be safe, as it is
designed to do this. Any build of Bubblewrap supports both
unprivileged and setuid mode, you just need to set the setuid bit for
it to change mode.

However, this does complicate the installation a bit. If you pass
`--with-priv-mode=setuid` to configure (of Flatpak or Bubblewrap) then
`make install` will try to set the setuid bit. However that means you
have to run `make install` as root. Alternatively, you can pass
`--enable-sudo` to configure and it will call `sudo` when setting the
setuid bit. Alternatively you can enable setuid completely outside of
the installation, which is common for example when packaging Bubblewrap
in a .deb or .rpm.

There are some complications when building Flatpak to a different
prefix than the system-installed version. First of all, the newly
built Flatpak will look for system-installed flatpaks in
`$PREFIX/var/lib/flatpak`, which will not match existing installations.
You can use `--with-system-install-dir=/var/lib/flatpak` to make both
installations use the same location.

Secondly, Flatpak ships with a root-privileged PolicyKit helper for
system-wide installation, called `flatpak-system-helper`. It is D-Bus
activated (on the system bus) and if you install in a non-standard
location it is likely that D-Bus will not find it and PolicyKit
integration will not work. However, if the system installation is
synchronized, you can often use the system installed helper instead—
at least if the two versions are close in versions.

# This repository

The Flatpak project consists of multiple pieces, and it can be
a bit challenging to find your way around at first. Here is a
quick intro to the major components of the flatpak repo:
* `common`: contains the library, libflatpak. It also contains various pieces of code that are shared between the library, the client and the services. Non-public code can be recognized by having a `-private.h` header file.
* `app`: the commandline client. Each command has a `flatpak-builtins-` source file
* `data`: D-Bus interface definition files
* `session-helper`: The flatpak-session-helper service, which provides various helpers for the sandbox setup at runtime
* `system-helper`: The flatpak-system-helper service, which runs as root on the system bus and allows non-root users to modify system installations
* `portal`: The Flatpak portal service, which lets sandboxed apps request the creation of new sandboxes
* `doc`: The sources for the documentation, both man pages and library documentation
* `tests`: The testsuite
* `bubblewrap`: Flatpak's unprivileged sandboxing tool which is developed separately and exists here as a submodule
* `libglnx`: a small utility library for projects that use GLib on Linux, as a submodule
* `dbus-proxy`: a filtering proxy for D-Bus connections, as a submodule

