#!/bin/sh

if test \( -x "$(which update-mime-database 2>/dev/null)" \) -a \( -d /self/exports/share/mime/packages \); then
    exec update-mime-database /self/exports/share/mime
fi
