#!/bin/sh

if command -v kbuildsycoca5 >/dev/null; then
    # If either the applications or icons have changed, run it
    if test -d "$1/exports/share/applications" || 
        test -d "$1/exports/share/icons/hicolor" ||
        test -d "$1/exports/share/mime/packages"; then
        exec kbuildsycoca5 
    fi
fi
