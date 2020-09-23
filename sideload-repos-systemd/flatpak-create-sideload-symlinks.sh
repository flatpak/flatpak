#!/bin/bash

# This script is intended to be run by flatpak-sideload-usb-repo.service

shopt -s nullglob
media_dir=${1:?path to media directory is required}

# Add a link to any newly inserted drives which might have been copied to with
# "flatpak create-usb". If we were to check for a repo on the drive that would
# break the case of using it for sideloading directly after copying to it (e.g.
# for testing).
for f in "$media_dir"/*; do
    if ! test -d "$f"; then
        continue
    fi
    unique_name=automount$(systemd-escape "$f")
    if test -e "/run/flatpak/sideload-repos/$unique_name"; then
        continue
    fi
    ln -s "$f" "/run/flatpak/sideload-repos/$unique_name"
done

# Remove any broken symlinks e.g. from drives that were removed
for f in /run/flatpak/sideload-repos/automount*; do
    OWNER=$(stat -c '%u' "$f")
    if [ "$UID" != "$OWNER" ]; then
        continue
    fi
    if ! test -e "$f"; then
        rm "$f"
    fi
done
