#!/bin/bash

set -euo pipefail

. "$(dirname "$0")"/libtest.sh

skip_without_bwrap
skip_revokefs_without_fuse

echo "1..4"

# Override the httpd function to enable header logging before setup_repo calls
# it. The web-server.py will log Flatpak-Ref and Flatpak-Upgrade-From headers
# to httpd-headers-log.
httpd () {
    if [ $# -eq 0 ] ; then
        set web-server.py repos "$(pwd)"/httpd-headers-log
    fi

    COMMAND=$1
    shift

    rm -f httpd-pipe
    mkfifo httpd-pipe
    PYTHONUNBUFFERED=1 "$(dirname "$0")"/$COMMAND "$@" 3> httpd-pipe 2>&1 | tee -a httpd-log >&2 &
    read < httpd-pipe
}

touch httpd-headers-log

setup_repo

truncate -s 0 httpd-headers-log

install_repo

APP_REF="app/org.test.Hello/${ARCH}/master"

assert_not_file_has_content httpd-headers-log "Flatpak-Upgrade-From"

ok "no Flatpak-Upgrade-From header on fresh install"

assert_file_has_content httpd-headers-log "Flatpak-Ref: ${APP_REF}"

ok "Flatpak-Ref header sent on fresh install"

INSTALLED_COMMIT=$(${FLATPAK} ${U} info --show-commit org.test.Hello)

make_updated_app
truncate -s 0 httpd-headers-log

${FLATPAK} ${U} update -y org.test.Hello >&2

assert_file_has_content httpd-headers-log "Flatpak-Upgrade-From: ${INSTALLED_COMMIT}"

ok "Flatpak-Upgrade-From header sent with correct commit hash on update"

assert_file_has_content httpd-headers-log "Flatpak-Ref: ${APP_REF}"

ok "Flatpak-Ref header sent on update"
