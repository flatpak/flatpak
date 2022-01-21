#!/bin/sh

if command -v update-mime-database >/dev/null && test -d "$1/exports/share/mime/packages"; then
    exec update-mime-database "$1/exports/share/mime"
fi
