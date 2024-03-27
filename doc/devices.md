# Devices in Flatpak

Flatpak limit the devices accessible to application from inside the
sandbox. By default a very limited set of devices is exposed.

## Device permissions

Device permissions are specified on the command line using the
`--device` command line argument.

Possible options are `all`, `dri`, `kvm`, `shm`, and more recently
`input`. The latter is at the time of writing not available in any
stable release.

`dri`: exposes DRI devices and is required for GL rendering. Most
modern toolkit want to use it.

`kvm`: exposes KVM subsystem.

`shm`: exposes the shared memory device. This is the only one not
exposed with `all`.

`input`: exposes input devices, include mice, game controllers and
events. This is not necessary for mouse a keyboard input, but it is
for direct device access or game controllers.

`all` exposes everyting. Accept `shm`.

Also note that a different permission, `--socket=puslseaudio` is meant
to expose access the to the pulse audio system, which include exposing
ALSA devices at the same time.

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

The syntax is `--device=fallback:DEVICE,all`. Using with DEVICE set to
`kvm`, `shm`, `dri`, or `all` is moot.

So something like:

```
--device=all --device=fallback:input,all
```

If flatpak knows about `input` then it will expose `input` and not
`all`.  If flatpak does know about `input` then the fallback will be
ignored, and `all` will be exposed.

However specifying:

```
--device=fallback:input,all
```

Will not allow this to work with a flatpak version that doesn't
support `input`. And it is equivalent to `--device=input` alone.
