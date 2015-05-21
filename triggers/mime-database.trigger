#!/bin/sh

if test \( -x "$(which update-mime-database 2>/dev/null)" \) -a \( -d /app/exports/share/mime/packages \); then
    exec update-mime-database /app/exports/share/mime
fi
