#!/bin/bash
set -eu

head -v -n-0 /etc/apt/sources.list || :
head -v -n-0 /etc/apt/sources.list.d/* || :
# Workaround for https://github.com/orgs/community/discussions/120966
sudo rm -f /etc/apt/sources.list.d/microsoft-prod.list
# Workaround for apparmor breaking bwrap by disabling unpriv userns
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
sudo systemctl reload apparmor
# One of the tests wants this
sudo mkdir /tmp/flatpak-com.example.App-OwnedByRoot
