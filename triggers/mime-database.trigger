#!/bin/sh

if test \( -x "$(which update-mime-database 2>/dev/null)" \) -a \( -d "$1/exports/share/mime/packages" \); then
    exec update-mime-database "$1/exports/share/mime"
fi
