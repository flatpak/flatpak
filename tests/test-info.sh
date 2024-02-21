#!/bin/bash

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_revokefs_without_fuse

echo "1..9"

INCLUDE_SPECIAL_CHARACTER=1 setup_repo
install_repo

COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

${FLATPAK} info -rcos  org.test.Hello > info

assert_file_has_content info "^app/org\.test\.Hello/$(flatpak --default-arch)/master test-repo ${COMMIT}"

ok "info -rcos"

${FLATPAK} info --show-metadata  org.test.Hello > info

# CVE-2023-28101
assert_file_has_content info "name=org\.test\.Hello"
assert_file_has_content info "^A=x\\\\x09y"

ok "info --show-metadata"

${FLATPAK} info --show-permissions  org.test.Hello > info

assert_file_has_content info "^A=x\\\\x09y"

ok "info --show-permissions"

${FLATPAK} info --show-location  org.test.Hello > info

assert_file_has_content info "app/org\.test\.Hello/$(flatpak --default-arch)/master/${COMMIT}"

ok "info --show-location"

${FLATPAK} info --show-runtime  org.test.Hello > info

assert_file_has_content info "^org\.test\.Platform/$(flatpak --default-arch)/master$"

ok "info --show-runtime"

${FLATPAK} info --show-sdk  org.test.Hello > info

assert_file_has_content info "^org\.test\.Platform/$(flatpak --default-arch)/master$"

ok "info --show-sdk"

${FLATPAK} info --show-extensions org.test.Hello > info

assert_file_has_content info "Extension: runtime/org\.test\.Hello\.Locale/$(flatpak --default-arch)/master$"

ok "info --show-extensions"

${FLATPAK} info --file-access=home org.test.Hello > info

assert_file_has_content info "^hidden$"

ok "info --file-access"

${FLATPAK} info org.test.Hello > info

assert_file_has_content info "^Hello world test app: org\.test\.Hello - Print a greeting$"

ok "info (name header)"
