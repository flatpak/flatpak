#!/bin/bash
#
# Copyright 2026 The Flatpak contributors
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

. $(dirname $0)/libtest.sh

export LC_ALL=C

BUILD_FINISH_ARGS="--share=network --filesystem=host --talk-name=org.test.Name" setup_repo

${FLATPAK} ${U} install -y test-repo org.test.Hello > install.out

assert_file_has_content install.out '^org.test.Hello permissions:$'
assert_file_has_content install.out '^  network$'
assert_file_has_content install.out '^  file access: host$'
assert_file_has_content install.out '^  dbus access: org.test.Name$'
assert_not_file_has_content install.out '\[[0-9]\]'

ok "permissions are listed with their details"

done_testing
