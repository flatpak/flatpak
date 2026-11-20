#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

# shellcheck source=tests/libtest.sh
. "$(dirname "$0")/libtest.sh"

skip_without_bwrap
skip_revokefs_without_fuse

mkdir bundles

setup_repo

${FLATPAK} build-bundle repos/test --repo-url="file://$(pwd)/repos/test" --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg bundles/hello.flatpak org.test.Hello >&2
assert_has_file bundles/hello.flatpak

${FLATPAK} build-bundle repos/test --runtime --repo-url="file://$(pwd)/repos/test" --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg bundles/platform.flatpak org.test.Platform >&2
assert_has_file bundles/platform.flatpak

ok "create bundles server-side"

rm bundles/hello.flatpak
setup_progress_terminals
install_on_tty () {
    env -u G_MESSAGES_DEBUG PATH="$test_tmpdir/tty-bin:$PATH" LC_ALL=C FLATPAK_FANCY_OUTPUT=1 python3 -c '
import os, pty, signal, sys, time
pending = bytearray()
early = os.getenv("EARLY_INPUT")
interactive = "-y" not in sys.argv and early != "line"
def timeout(signum, frame):
    raise RuntimeError("waiting for confirmation prompt; last output: %r" % pending[-256:])
signal.signal(signal.SIGALRM, timeout)
if interactive or early:
    signal.alarm(30)
def read_output(fd):
    data = os.read(fd, 4096)
    pending.extend(data)
    if early and b"ready-for-input" in pending:
        os.write(fd, b"go\n" + (b"\n" if early == "line" else b" " * 240))
        if not interactive:
            signal.alarm(0)
        pending.clear()
    elif interactive and b"[Y/n]: " in pending:
        # Wait for the prompt writer to block for input, so the reply is
        # distinct from the typeahead case even under scheduler contention.
        stat = "/proc/%d/stat" % os.tcgetpgrp(fd)
        while open(stat).read().rsplit(")", 1)[1].split()[0] != "S":
            time.sleep(0.001)
        os.write(fd, b"\n")
        signal.alarm(0)
        pending.clear()
    return data
setup = "stty rows 5 cols 120; "
if early:
    setup += "echo ready-for-input; read -r gate; "
status = pty.spawn(["sh", "-c", setup + "exec \"$@\"", "sh", *sys.argv[1:]], master_read=read_output)
sys.exit(os.WEXITSTATUS(status) if os.WIFEXITED(status) else 1)
' "$FLATPAK" install ${U} "$@" </dev/null
}

TERM=xterm install_on_tty test-repo org.test.Hello > install-output || { cat install-output >&2; exit 1; }
assert_file_has_content install-output $'\033\\[K\033\\[1A'
assert_not_file_has_content install-output $'\033\\[6n'
assert_file_has_content install-output 'Installation complete\.'
python3 -c '
import re
data = open("install-output", "rb").read()
preview, progress = data.split(b"\033[?2026h", 1)
assert b"org.test.Hello" in preview[:preview.index(b"[Y/n]: ")]
first_frame = progress.split(b"\033[?2026l", 1)[0]
operation = rb" *[0-9]+\. +(?:\[[^]]*\] +)?(org\.test\.[A-Za-z.]+)"
ids = re.findall(operation, preview[preview.rindex(b"\033[1m"):])
assert sorted(ids) == [b"org.test.Hello", b"org.test.Hello.Locale", b"org.test.Platform"], ids
assert re.findall(operation, first_frame) == ids[-2:]
'
${FLATPAK} build-bundle $FL_DIR/repo --repo-url="file://$(pwd)/repos/test" --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg bundles/hello.flatpak org.test.Hello >&2
assert_has_file bundles/hello.flatpak

ok "create bundles client-side"

${FLATPAK} uninstall ${U} -y org.test.Hello >&2
EARLY_INPUT=line TERM=xterm install_on_tty --bundle bundles/hello.flatpak > early-output
EARLY_INPUT=partial TERM=xterm install_on_tty --reinstall --bundle bundles/hello.flatpak > partial-output
python3 -c '
for path in ("early-output", "partial-output"):
    data = open(path, "rb").read()
    preview, progress = data.split(b"\033[?2026h", 1)
    assert b"[Y/n]: " in preview
    assert progress.startswith((b"\r\n", b"\n")), "rewound after typeahead"
'

# Installing again without reinstall option should fail...
assert_fail ${FLATPAK} install ${U} -y --bundle bundles/hello.flatpak >&2
# Now with reinstall option it should pass...
TERM=dumb install_on_tty -y --bundle bundles/hello.flatpak --reinstall > plain-output
assert_file_has_content plain-output 'Installation complete\.'
assert_not_file_has_content plain-output $'\033'

# This should have installed the runtime dependency too
assert_has_file $FL_DIR/repo/refs/remotes/test-repo/runtime/org.test.Platform/$ARCH/master

assert_has_file $FL_DIR/repo/refs/remotes/hello-origin/app/org.test.Hello/$ARCH/master
APP_COMMIT=$(cat "$FL_DIR/repo/refs/remotes/hello-origin/app/org.test.Hello/$ARCH/master")
assert_has_file "$FL_DIR/repo/objects/$(echo "$APP_COMMIT" | cut -b 1-2)/$(echo "$APP_COMMIT" | cut -b 3-).commit"
assert_has_file "$FL_DIR/repo/objects/$(echo "$APP_COMMIT" | cut -b 1-2)/$(echo "$APP_COMMIT" | cut -b 3-).commitmeta"

assert_has_dir $FL_DIR/app/org.test.Hello
assert_has_symlink $FL_DIR/app/org.test.Hello/current
assert_symlink_has_content $FL_DIR/app/org.test.Hello/current ^$ARCH/master$
assert_has_dir $FL_DIR/app/org.test.Hello/$ARCH/master
assert_has_symlink $FL_DIR/app/org.test.Hello/$ARCH/master/active
ID=`readlink $FL_DIR/app/org.test.Hello/$ARCH/master/active`
assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/master/active/deploy
assert_has_file $FL_DIR/app/org.test.Hello/$ARCH/master/active/metadata
assert_has_dir $FL_DIR/app/org.test.Hello/$ARCH/master/active/files
assert_has_dir $FL_DIR/app/org.test.Hello/$ARCH/master/active/export
assert_has_file $FL_DIR/exports/share/applications/org.test.Hello.desktop
# Ensure Exec key is rewritten
assert_file_has_content $FL_DIR/exports/share/applications/org.test.Hello.desktop "^Exec=.*flatpak run --branch=master --arch=$ARCH --command=hello\.sh org\.test\.Hello$"
assert_has_file $FL_DIR/exports/share/icons/hicolor/64x64/apps/org.test.Hello.png
assert_has_file $FL_DIR/exports/share/icons/HighContrast/64x64/apps/org.test.Hello.png

$FLATPAK list ${U} | grep org.test.Hello > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep hello-origin > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep current > /dev/null
$FLATPAK list ${U} -d | grep org.test.Hello | grep ${ID:0:12} > /dev/null

$FLATPAK info ${U} org.test.Hello > /dev/null
$FLATPAK info ${U} org.test.Hello | grep hello-origin > /dev/null
$FLATPAK info ${U} org.test.Hello | grep $ID > /dev/null

$FLATPAK remote-list ${U} -d | grep hello-origin > /dev/null
$FLATPAK remote-list ${U} -d | grep hello-origin | grep no-enumerate > /dev/null
assert_has_file $FL_DIR/repo/hello-origin.trustedkeys.gpg

ok "install app bundle"

if command -v update-desktop-database >/dev/null &&
   command -v update-mime-database >/dev/null &&
   command -v gtk-update-icon-cache >/dev/null &&
   test -f /usr/share/icons/hicolor/index.theme; then
    # Ensure triggers ran
    assert_has_file $FL_DIR/exports/share/applications/mimeinfo.cache
    assert_file_has_content $FL_DIR/exports/share/applications/mimeinfo.cache x-test/Hello
    assert_has_file $FL_DIR/exports/share/icons/hicolor/icon-theme.cache
    assert_has_file $FL_DIR/exports/share/icons/hicolor/index.theme

    ok "install app bundle triggers"
else
    ok "install app bundle triggers triggers # skip  Dependencies not available"
fi

${FLATPAK} uninstall -y --force-remove ${U} org.test.Platform >&2

assert_not_has_file $FL_DIR/repo/refs/remotes/platform-origin/runtime/org.test.Platform/$ARCH/master

${FLATPAK} install -y ${U} --bundle bundles/platform.flatpak >&2

assert_has_file $FL_DIR/repo/refs/remotes/platform-origin/runtime/org.test.Platform/$ARCH/master
RUNTIME_COMMIT=$(cat "$FL_DIR/repo/refs/remotes/platform-origin/runtime/org.test.Platform/$ARCH/master")
assert_has_file "$FL_DIR/repo/objects/$(echo "$RUNTIME_COMMIT" | cut -b 1-2)/$(echo "$RUNTIME_COMMIT" | cut -b 3-).commit"
assert_has_file "$FL_DIR/repo/objects/$(echo "$RUNTIME_COMMIT" | cut -b 1-2)/$(echo "$RUNTIME_COMMIT" | cut -b 3-).commitmeta"

assert_has_dir $FL_DIR/runtime/org.test.Platform
assert_has_dir $FL_DIR/runtime/org.test.Platform/$ARCH/master
assert_has_symlink $FL_DIR/runtime/org.test.Platform/$ARCH/master/active
ID=`readlink $FL_DIR/runtime/org.test.Platform/$ARCH/master/active`
assert_has_file $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/deploy
assert_has_file $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/metadata
assert_has_dir $FL_DIR/runtime/org.test.Platform/$ARCH/master/active/files

$FLATPAK list ${U} --runtime | grep org.test.Platform > /dev/null
$FLATPAK list ${U} -d --runtime | grep org.test.Platform | grep platform-origin > /dev/null
$FLATPAK list ${U} -d --runtime | grep org.test.Platform | grep ${ID:0:12} > /dev/null

$FLATPAK info ${U} org.test.Platform > /dev/null
$FLATPAK info ${U} org.test.Platform | grep platform-origin > /dev/null
$FLATPAK info ${U} org.test.Platform | grep $ID > /dev/null

$FLATPAK remote-list ${U} -d | grep platform-origin > /dev/null
$FLATPAK remote-list ${U} -d | grep platform-origin | grep no-enumerate > /dev/null
assert_has_file $FL_DIR/repo/platform-origin.trustedkeys.gpg

ok "install runtime bundle"

run org.test.Hello &> hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

ok "run"


OLD_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

${FLATPAK} ${U} update -y -v org.test.Hello master >&2
ALSO_OLD_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`
assert_streq "$OLD_COMMIT" "$ALSO_OLD_COMMIT"

ok "null update"

make_updated_app

${FLATPAK} ${U} update -y org.test.Hello >&2

NEW_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

assert_not_streq "$OLD_COMMIT" "$NEW_COMMIT"

run org.test.Hello &> hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATED$'

ok "update"

make_updated_app test org.test.Collection.test master UPDATED2

${FLATPAK} build-bundle repos/test --repo-url="file://$(pwd)/repos/test" --gpg-keys=${FL_GPG_HOMEDIR}/pubring.gpg bundles/hello2.flatpak org.test.Hello >&2
assert_has_file bundles/hello2.flatpak

${FLATPAK} install ${U} -y --bundle bundles/hello2.flatpak >&2

NEW2_COMMIT=`${FLATPAK} ${U} info --show-commit org.test.Hello`

assert_not_streq "$NEW_COMMIT" "$NEW2_COMMIT"

run org.test.Hello &> hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxUPDATED2$'

ok "update as bundle"

done_testing
