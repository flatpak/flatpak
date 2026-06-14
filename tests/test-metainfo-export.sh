#!/bin/bash
#
# Copyright (C) 2026 Mia McMahill <electricbrass@proton.me>
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

echo "1..2"

setup_repo
install_repo

APP_DIR="$(mktemp -d)"
APP_ID="org.test.MetainfoReleases"

$FLATPAK build-init "$APP_DIR" "$APP_ID" org.test.Platform org.test.Platform >&2

mkdir -p "${APP_DIR}/files/share/metainfo/"
mkdir -p "${APP_DIR}/files/share/metainfo/releases"

cat <<EOF > "${APP_DIR}/files/share/metainfo/${APP_ID}.metainfo.xml"
<?xml version="1.0" encoding="utf-8"?>
<component type="desktop-application">
  <id>${APP_ID}</id>
  <metadata_license></metadata_license>
  <name>Metainfo Releases Export Test</name>
  <summary>Test metainfo releases.xml export</summary>
  <description><p>This is a test app.</p></description>
  <releases type="external"/>
</component>
EOF

cat <<EOF > "${APP_DIR}/files/share/metainfo/releases/${APP_ID}.releases.xml"
<releases>
  <release version="1.0.0" date="2026-06-14"/>
</releases>
EOF

$FLATPAK build-finish "$APP_DIR" >&2

assert_has_file "${APP_DIR}/export/share/metainfo/${APP_ID}.metainfo.xml"
assert_file_has_content "${APP_DIR}/export/share/metainfo/${APP_ID}.metainfo.xml" \
  '<name>Metainfo Releases Export Test</name>'
assert_has_file "${APP_DIR}/export/share/metainfo/releases/${APP_ID}.releases.xml"
assert_file_has_content "${APP_DIR}/export/share/metainfo/releases/${APP_ID}.releases.xml" \
  '<release version="1.0.0" date="2026-06-14"/>'

ok "build-finish exported metainfo and releases"

REPO="$(mktemp -d)"

$FLATPAK build-export "$REPO" "$APP_DIR" >&2

$FLATPAK ${U} install -y "$REPO" "$APP_ID" >&2

assert_has_file "${FL_DIR}/exports/share/metainfo/${APP_ID}.metainfo.xml"
assert_file_has_content "${APP_DIR}/export/share/metainfo/${APP_ID}.metainfo.xml" \
  '<name>Metainfo Releases Export Test</name>'
assert_has_file "${FL_DIR}/exports/share/metainfo/releases/${APP_ID}.releases.xml"
assert_file_has_content "${APP_DIR}/export/share/metainfo/releases/${APP_ID}.releases.xml" \
  '<release version="1.0.0" date="2026-06-14"/>'

ok "install exported metainfo and releases"
