#!/bin/bash

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_revokefs_without_fuse

echo "1..7"

setup_repo
install_repo

COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

${FLATPAK} info -rcos  org.test.Hello > info

assert_file_has_content info "^app/org\.test\.Hello/$(flatpak --default-arch)/master test-repo ${COMMIT}"

echo "ok info -rcos"

${FLATPAK} info --show-permissions  org.test.Hello > info

assert_file_empty info

echo "ok info --show-permissions"

${FLATPAK} info --show-location  org.test.Hello > info

assert_file_has_content info "app/org\.test\.Hello/$(flatpak --default-arch)/master/${COMMIT}"

echo "ok info --show-location"

${FLATPAK} info --show-runtime  org.test.Hello > info

assert_file_has_content info "^org\.test\.Platform/$(flatpak --default-arch)/master$"

echo "ok info --show-runtime"

${FLATPAK} info --show-sdk  org.test.Hello > info

assert_file_has_content info "^org\.test\.Platform/$(flatpak --default-arch)/master$"

echo "ok info --show-sdk"

${FLATPAK} info --show-extensions org.test.Hello > info

assert_file_has_content info "Extension: runtime/org\.test\.Hello\.Locale/$(flatpak --default-arch)/master$"

echo "ok info --show-extensions"

${FLATPAK} info --file-access=home org.test.Hello > info

assert_file_has_content info "^hidden$"

echo "ok info --file-access"
