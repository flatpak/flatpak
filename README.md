<p align="center">
  <img src="https://github.com/flatpak/flatpak/blob/master/flatpak.png?raw=true" alt="Flatpak icon"/>
</p>

Flatpak is a system for building, distributing and running sandboxed
desktop applications on Linux.

See https://flatpak.org/ for more information.

Community discussion happens in [#flatpak on Freenode](ircs://chat.freenode.net/flatpak) and on [the mailing list](https://lists.freedesktop.org/mailman/listinfo/flatpak).

Read documentation for the flatpak [commandline tools](http://docs.flatpak.org/en/latest/command-reference.html) and for the libflatpak [library API](http://flatpak.github.io/flatpak/reference/html/index.html).

# Contributing

Flatpak welcomes contributions from anyone! Here are some ways you can help:
* Fix [one of the issues](https://github.com/flatpak/flatpak/issues/) and submit a PR
* Update flatpak's translations and submit a PR
* Update flatpak's documentation, hosted at http://docs.flatpak.org and developed over in [flatpak-docs](https://github.com/flatpak/flatpak-docs)
* Find a bug and [submit a detailed report](https://github.com/flatpak/flatpak/issues/new) including your OS, flatpak version, and the steps to reproduce
* Add your favorite application to [Flathub](https://flathub.org) by writing a flatpak-builder manifest and [submitting it](https://github.com/flathub/flathub/wiki/App-Submission)
* Improve the Flatpak support in your favorite Linux distribution

# Hacking
Flatpak uses a traditional autoconf-style build mechanism. To build just do
```
 ./configure [args]
 make
 make install
```

Most configure arguments are documented in `./configure --help`. However,
there are some options that are a bit more complicated.

Flatpak relies on a project called
[Bubblewrap](https://github.com/projectatomic/bubblewrap) for the
low-level sandboxing.  By default, an in-tree copy of this is built
(distributed in the tarball or using git submodules in the git
tree). This will build a helper called flatpak-bwrap. If your system
has a recent enough version of Bubblewrap already, you can use
`--with-system-bubblewrap` to use that instead.

Bubblewrap can run in two modes, either using unprivileged user
namespaces or setuid mode. This requires that the kernel supports this,
which some distributions disable. For instance, Debian and Arch 
([linux](https://www.archlinux.org/packages/?name=linux) kernel v4.14.5 or later), support user namespaces with the `kernel.unprivileged_userns_clone` sysctl enabled.

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
`$PREFIX/var/lib/flatpak`, which will not match existing installed
flatpaks. You can use `--with-system-install-dir=/var/lib/flatpak`
to make both installations use the same location.

Secondly, Flatpak ships with a root-privileged policykit helper for
system-installation, called `flatpak-system-helper`. This is dbus
activated (on the system-bus) and if you install in a non-standard
location it is likely that this will not be found by dbus and
policykit. However, if the system installation is synchronized,
you can often use the system installed helper instead - at least
if the two versions are close in versions.
