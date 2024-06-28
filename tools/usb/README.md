# USB device list generators

These are examples of functional Python 3 scripts to generate the USB
devices lists from some well-known packages.

## epsonscan2-parse.py

This will generate the list for Epson Scan2 based on the udev rules
shipped with the package.

## gphoto2-parse.py

This will parse the output of `print-camera-list` from libgphoto to
generate the list of USB devices.

## libsane-parse.py

This will generate the list for SANE based on the udev rules shipped
with the package.

## utsushi-parse.py

This will parse the SANE .desc shipping with Utsushi to generate the
USB device list.
