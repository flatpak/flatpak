Flatpak uses a traditional autoconf-style build mechanism. The exact steps
required depend on your distribution. Below are some steps that should work on
Debian and Fedora, based on the configure options used to build those
distributions' packages, These options will install into `/usr`, which will
overwrite your distribution-provided system copy of Flatpak. **You should only
do this if you understand the risks of it to the stability of your system, and
you probably want to do it in a VM or on a development machine that's expected
to break sometimes!**

## On Debian
```
git clone https://github.com/flatpak/flatpak
cd flatpak
sudo apt build-dep flatpak
NOCONFIGURE=1 ./autogen.sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --libdir=\${prefix}/lib/x86_64-linux-gnu --runstatedir=/run --disable-auto-sideloading --disable-selinux-module --enable-gdm-env-file --enable-installed-tests --with-dbus-config-dir=/usr/share/dbus-1/system.d --with-privileged-group=sudo --with-run-media-dir=/media --with-system-bubblewrap=bwrap --with-system-dbus-proxy=xdg-dbus-proxy --with-systemdsystemunitdir=/lib/systemd/system --with-system-helper-user=_flatpak --enable-docbook-docs --enable-documentation --disable-gtk-doc
make -j$(nproc)
make check -j$(nproc)
sudo make install
```

## On Fedora

```
git clone https://github.com/flatpak/flatpak
cd flatpak
sudo dnf builddep flatpak
sudo dnf install gettext-devel socat
NOCONFIGURE=1 ./autogen.sh
./configure --prefix=/usr --sysconfdir=/etc --libdir=/usr/lib64 --localstatedir=/var --enable-docbook-docs --enable-installed-tests --enable-selinux-module --with-system-bubblewrap --with-system-dbus-proxy
make -j$(nproc)
make check -j$(nproc)
sudo make install
```

## More info
Dependencies you will need include: autoconf, automake, libtool, bison,
gettext, gtk-doc, gobject-introspection, libcap, libarchive, libxml2, libsoup,
gpgme, polkit, libXau, ostree, json-glib, appstream, libseccomp (or their devel
packages).

Most configure arguments are documented in `./configure --help`. However,
there are some options that are a bit more complicated.

Flatpak relies on a project called
[Bubblewrap](https://github.com/containers/bubblewrap) for the low-level
sandboxing. By default, an in-tree copy of this is built (distributed in the
tarball or using git submodules in the git tree). This will build a helper
called flatpak-bwrap. If your system has a recent enough version of Bubblewrap
already, you can use `--with-system-bubblewrap` to use that instead.

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
at least if the two versions are close enough.

## This repository

The Flatpak project consists of multiple pieces, and it can be
a bit challenging to find your way around at first. Here is a
quick intro to each of the important subdirectories:
* `app`: the commandline client. Each command has a `flatpak-builtins-` source file
* `common`: contains the library, libflatpak. It also contains various pieces
  of code that are shared between the library, the client and the services.
  Non-public code can be recognized by having a `-private.h` header file.
* `completion`: commandline auto completion support
* `data`: D-Bus interface definition files and GVariant schemas
* `doc`: The sources for the documentation, both man pages and library documentation
* `icon-validator`: A small utility that is used to validate icons
* `oci-authenticator`: service used for authenticating the user for installing
  from oci remotes (e.g. for paid apps)
* `po`: translations
* `portal`: The Flatpak portal service, which lets sandboxed apps request the
  creation of new sandboxes
* `revokefs`: A FUSE filesystem that is used to transfer files downloaded by
  the user to the system-helper without copying
* `session-helper`: The flatpak-session-helper service, which provides various
  helpers for the sandbox setup at runtime
* `tests`: The testsuite
* `subprojects/bubblewrap`: Flatpak's unprivileged sandboxing tool which is
  developed separately and exists here as a submodule
* `subprojects/libglnx`: a small utility library for projects that use GLib on
  Linux, as a submodule
* `subprojects/dbus-proxy`: a filtering proxy for D-Bus connections, as a submodule
* `subprojects/variant-schema-compiler`: a tool for generating code to
  efficiently access data encoded using GVariant, as a submodule
* `system-helper`: The flatpak-system-helper service, which runs as root on the
  system bus and allows non-root users to modify system installations
