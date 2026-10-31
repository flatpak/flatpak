# shellcheck shell=bash
# Shared OSTree helpers for Flatpak tests.
#
# SPDX-License-Identifier: LGPL-2.1-or-later

# Initialize a test archive repo with the options we always want:
# archive-z2 mode and min-free-space-percent=0 (#5373).
# Extra arguments after REPO are passed to `ostree init` (e.g. --collection-id=…).
ostree_init_test_repo () {
    local repo=$1
    shift

    ostree --repo="${repo}" init --mode=archive-z2 "$@" >&2
    ostree --repo="${repo}" config set core.min-free-space-percent 0 >&2
}
