#!/bin/sh

if test \( -x "$(which gtk-update-icon-cache 2>/dev/null)" \) -a \( -d "$1/exports/share/icons/hicolor" \); then
    cp /usr/share/icons/hicolor/index.theme "$1/exports/share/icons/hicolor/"
    for dir in "$1"/exports/share/icons/*; do
        if test -f "$dir/index.theme"; then
            if ! gtk-update-icon-cache --quiet "$dir"; then
                echo "Failed to run gtk-update-icon-cache for $dir"
                exit 1
            fi
        fi
    done
fi
