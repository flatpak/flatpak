# Devices in Flatpak

Flatpak limits the devices accessible to applications from inside the
sandbox. By default a very limited set of devices is exposed.

A more complete documentation is available in the [user
documentation](https://docs.flatpak.org/en/latest/sandbox-permissions.html).

## Device permissions

Device permissions are specified on the command line using the
`--device` command line argument.

Possible options are `all`, `dri`, `kvm`, `shm`, and `input`. The
latter is at the time of writing not available in any stable release.

`dri`: exposes DRI devices and is required for GL rendering. Most
modern toolkit require to use it.

`kvm`: exposes the Kernel Virtual Machine subsystem from `/dev/kvm`.

`shm`: exposes the shared memory device from `/dev/shm`. This is the
only one not exposed with `all`.

`input`: exposes input devices, include mice, game controllers and
events. This is not necessary for mouse a keyboard input, but it is
for direct device access or game controllers from `/dev/input`.

`all` exposes everyting. Excepted `shm`.

Also note that a different permission, `--socket=puslseaudio` is meant
to expose access the to the pulse audio system, which include exposing
ALSA devices from `/dev/snd` at the same time.

## Device fallback

Device fallback is a strategy to solve the problem of forward
compatibility.

Today if you export a flatpak with the new `input` device permission
it will not work on any older flatpak that don't know about it and the
application will not work out of the box. So it's tempting to just go
back to `all` and forget about more granular permissions.

The fallback mechanism is designed to express the following: we want
the devices _xyz_ to be exposed, and we have specified `all` to
fallback if _xyz_ isn't know. If flatpak knows about _xyz_ device,
remove the `all` permission.

The syntax is `--device=fallback:DEVICE,FALLBACK`.

Fallback is unecessary for any of `kvm`, `shm`, `dri`, or `all`.

So something like:

```
--device=fallback:input,all
--device=all
```

If Flatpak knows about `input` (Flatpak > 1.15.6) then it will expose
`input` and ignore `all`.  If flatpak does not know about `input` then
the fallback will be ignored, and `all` will be exposed.

However specifying:

```
--device=fallback:input,all
```

Will not allow this to work with a flatpak version that doesn't
support `input`. And it is equivalent to `--device=input` alone.

## Internals

The device permissions are stored in the `metadata` file. If a device
permission is not known then it is ignored, and if it is part of a
fallback, the fallback is written as `fallback:all` in the metadata.

In the case of the fallback, they are interpreted as
needed. `fallback:all` cause to fallback on (allow) `all` regardless.

## Flatpak builder

Flatpak-builder only need a compatible version of flatpak to work with
the fallback permission. This is similar to new permissions like
`input`.
