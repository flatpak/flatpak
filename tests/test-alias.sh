#!/bin/bash
#
# Copyright (C) 2022 Matthew Leeds <mwleeds@protonmail.com>
#
# SPDX-License-Identifier: LGPL-2.0-or-later

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..3"

setup_repo
${FLATPAK} ${U} install -y test-repo org.test.Hello >&2

# List aliases (none)
${FLATPAK} ${U} alias > aliases
assert_file_empty aliases

# Make an alias
${FLATPAK} ${U} alias hello org.test.Hello
${FLATPAK} ${U} alias > aliases
if [ x${USE_SYSTEMDIR-} == xyes ]; then
    assert_file_has_content aliases "hello	org\.test\.Hello	system"
else
    assert_file_has_content aliases "hello	org\.test\.Hello	user"
fi

# Shouldn't be able to make an alias that already exists
if ${FLATPAK} ${U} alias hello org.test.Hello &> alias-error-log; then
    assert_not_reached "Should not be able to create an alias that already exists"
fi
assert_file_has_content alias-error-log "error: Error making alias 'hello': Error making symbolic link .*: File exists"

# Shouldn't be able to make an alias that for an app that's not installed
if ${FLATPAK} ${U} alias hello org.test.Bonjour &> alias-error-log; then
    assert_not_reached "Should not be able to create an alias for an uninstalled app"
fi
assert_file_has_content alias-error-log "error: App org.test.Bonjour not installed"

# Remove an alias
${FLATPAK} ${U} alias --remove hello
${FLATPAK} ${U} alias > aliases
assert_file_empty aliases

ok "alias command works"

if ${FLATPAK} ${U} alias .hello org.test.Hello &> alias-error-log; then
    assert_not_reached "Should not be able to create an alias starting with a period"
fi
assert_file_has_content alias-error-log "error: Alias can't start with \."

if ${FLATPAK} ${U} alias org.test.Goodbye org.test.Hello &> alias-error-log; then
    assert_not_reached "Should not be able to create an alias which contains a period"
fi
assert_file_has_content alias-error-log "error: Alias can't contain \."

if ${FLATPAK} ${U} alias 🦋 org.test.Hello &> alias-error-log; then
    assert_not_reached "Should not be able to create an alias which is a butterfly"
fi
# In flatpak_is_valid_alias() we look at the string byte by byte so multi-byte
# characters don't get printed properly in the error message.
assert_file_has_content alias-error-log "error: Alias can't start with "

ok "alias command rejects invalid alias characters"

if ${FLATPAK} ${U} alias --remove notarealalias &> alias-error-log; then
    assert_not_reached "Should not be able to remove an alias that doesn't exist"
fi
assert_file_has_content alias-error-log "error: Error removing alias 'notarealalias': it does not exist"

# clean up
rm aliases alias-error-log
${FLATPAK} ${U} uninstall -y org.test.Platform org.test.Hello >&2
${FLATPAK} ${U} remote-delete test-repo >&2

ok "alias --remove command rejects non-existent alias"
