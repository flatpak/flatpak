#!/bin/bash
#
# Copyright (C) 2026 Abhinav Mir <atg271@gmail.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

source "$(dirname "$0")/libtest.sh"

setup_repo
install_repo

# The appstream data has never been downloaded, so it is older than the ttl
# and a plain update would refresh it.
assert_not_has_file "$FL_DIR/appstream/test-repo/$ARCH/.timestamp"

httpd_clear_log
${FLATPAK} ${U} update -y --no-pull >&2

assert_not_file_has_content httpd-log "GET"
assert_not_has_file "$FL_DIR/appstream/test-repo/$ARCH/.timestamp"

ok "update --no-pull does not fetch appstream data"

httpd_clear_log
${FLATPAK} ${U} update -y >&2

assert_file_has_content httpd-log "GET"
assert_has_file "$FL_DIR/appstream/test-repo/$ARCH/.timestamp"

ok "update fetches appstream data"

done_testing
