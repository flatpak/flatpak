#!/bin/sh

if test \( -x "$(which update-desktop-database 2>/dev/null)" \) -a \( -d /app/exports/share/applications \); then
    exec update-desktop-database -q /app/exports/share/applications
fi
