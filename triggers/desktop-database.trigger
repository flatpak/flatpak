#!/bin/sh

if command -v update-desktop-database >/dev/null && test -d "$1/exports/share/applications"; then
    exec update-desktop-database -q "$1/exports/share/applications"
fi
