libglnx is the successor to libgsystem: https://git.gnome.org/browse/libgsystem

It is for modules which depend on both GLib and Linux, intended to be
used as a git submodule.

Porting from libgsystem
-----------------------

For all of the filesystem access code, libglnx exposes only
fd-relative API, not `GFile*`.  It does use `GCancellable` where
applicable.

For local allocation macros, you should start using the `g_auto`
macros from GLib.  A backport is included in libglnx.  There are a few
APIs not defined in GLib yet, such as `glnx_fd_close`.

`gs_transfer_out_value` is replaced by `g_steal_pointer`.

