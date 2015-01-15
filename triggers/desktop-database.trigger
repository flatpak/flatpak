#!/bin/sh

if test \( -x "$(which update-desktop-database 2>/dev/null)" \) -a \( -d /self/exports/share/applications \); then
    exec update-desktop-database -q /self/exports/share/applications
fi
