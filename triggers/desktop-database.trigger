#!/bin/sh

if test \( -x "$(which update-desktop-database 2>/dev/null)" \) -a \( -d "$1/exports/share/applications" \); then
    exec update-desktop-database -q "$1/exports/share/applications"
fi
