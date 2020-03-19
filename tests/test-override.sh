#!/bin/bash

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_revokefs_without_fuse

reset_overrides () {
    ${FLATPAK} override --user --reset org.test.Hello
    ${FLATPAK} override --user --show org.test.Hello > info
    assert_file_empty info
}

echo "1..13"

setup_repo
install_repo

reset_overrides

${FLATPAK} override --user --socket=wayland org.test.Hello
${FLATPAK} override --user --nosocket=ssh-auth org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Context\]$"
assert_file_has_content override "^sockets=wayland;!ssh-auth;$"

ok "override --socket"

reset_overrides

${FLATPAK} override --user --device=dri org.test.Hello
${FLATPAK} override --user --nodevice=kvm org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Context\]$"
assert_file_has_content override "^devices=dri;!kvm;$"

ok "override --device"

reset_overrides

${FLATPAK} override --user --share=network org.test.Hello
${FLATPAK} override --user --unshare=ipc org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Context\]$"
assert_file_has_content override "^shared=network;!ipc;$"

ok "override --share"

reset_overrides

${FLATPAK} override --user --allow=multiarch org.test.Hello
${FLATPAK} override --user --disallow=bluetooth org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Context\]$"
assert_file_has_content override "^features=multiarch;!bluetooth;$"

ok "override --allow"

reset_overrides

${FLATPAK} override --user --env=FOO=BAR org.test.Hello
${FLATPAK} override --user --env=BAR= org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Environment\]$"
assert_file_has_content override "^FOO=BAR$"
assert_file_has_content override "^BAR=$"

ok "override --env"

${FLATPAK} override --user --filesystem=home org.test.Hello
${FLATPAK} override --user --filesystem=xdg-desktop/foo:create org.test.Hello
${FLATPAK} override --user --filesystem=xdg-config:ro org.test.Hello
${FLATPAK} override --user --filesystem=/media org.test.Hello
${FLATPAK} override --user --nofilesystem=xdg-documents org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Context\]$"
assert_file_has_content override "^filesystems=.*/media;.*$"
assert_file_has_content override "^filesystems=.*home;.*$"
assert_file_has_content override "^filesystems=.*xdg-documents;.*$"
assert_file_has_content override "^filesystems=.*xdg-desktop/foo:create;.*$"
assert_file_has_content override "^filesystems=.*xdg-config:ro;.*$"

ok "override --filesystem"

reset_overrides

${FLATPAK} override --user --own-name=org.foo.Own org.test.Hello
${FLATPAK} override --user --talk-name=org.foo.Talk org.test.Hello
${FLATPAK} override --user --talk-name=org.foo.NoTalk org.test.Hello
${FLATPAK} override --user --no-talk-name=org.foo.NoTalk org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Session Bus Policy\]$"
assert_file_has_content override "^org\.foo\.Own=own$"
assert_file_has_content override "^org\.foo\.Talk=talk$"
assert_file_has_content override "^org\.foo\.NoTalk=none$"

ok "override session bus names"

reset_overrides

${FLATPAK} override --user --system-own-name=org.foo.Own.System org.test.Hello
${FLATPAK} override --user --system-talk-name=org.foo.Talk.System org.test.Hello
${FLATPAK} override --user --system-talk-name=org.foo.NoTalk.System org.test.Hello
${FLATPAK} override --user --system-no-talk-name=org.foo.NoTalk.System org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[System Bus Policy\]$"
assert_file_has_content override "^org\.foo\.Own\.System=own$"
assert_file_has_content override "^org\.foo\.Talk\.System=talk$"
assert_file_has_content override "^org\.foo\.NoTalk\.System=none$"

ok "override system bus names"

reset_overrides

if skip_one_without_bwrap "sandbox wayland socket"; then
  :
elif [ -S "${XDG_RUNTIME_DIR}/wayland-0" ]; then
  ${FLATPAK} override --user --socket=wayland org.test.Hello
  ${FLATPAK} run --command=ls org.test.Hello -- /run/user/1000 > out
  assert_file_has_content out "wayland-0"

  ${FLATPAK} override --user --nosocket=wayland org.test.Hello
  ${FLATPAK} run --command=ls org.test.Hello -- /run/user/1000 > out
  assert_not_file_has_content out "wayland-0"

  ok "sandbox wayland socket"
else
  ok "sandbox wayland socket # skip not supported without Wayland"
fi

reset_overrides

if skip_one_without_bwrap "sandbox dri device"; then
  :
elif [ -d "/dev/dri" ]; then
  ${FLATPAK} override --user --device=dri org.test.Hello
  ${FLATPAK} run --command=ls org.test.Hello -- /dev > out
  assert_file_has_content out "dri"

  ${FLATPAK} override --user --nodevice=dri org.test.Hello
  ${FLATPAK} run --command=ls org.test.Hello -- /dev > out
  assert_not_file_has_content out "dri"

  ok "sandbox dri device"
else
  ok "sandbox dri device # skip not supported without /dev/dri"
fi

reset_overrides

if ! skip_one_without_bwrap "sandbox dri device"; then
  ${FLATPAK} override --user --env=FOO=BAR org.test.Hello

  ${FLATPAK} run --command=sh org.test.Hello -c 'echo $FOO' > out
  assert_file_has_content out "BAR"
  FOO=bar ${FLATPAK} run --command=sh org.test.Hello -c 'echo $FOO' > out
  assert_file_has_content out "BAR"

  ok "sandbox env"
fi

reset_overrides

if ! skip_one_without_bwrap "sandbox filesystem"; then
  echo "hello" > $HOME/example

  ${FLATPAK} override --user --filesystem=home:ro org.test.Hello

  ${FLATPAK} run --command=ls org.test.Hello $HOME > out
  assert_file_has_content out example

  ${FLATPAK} run --command=sh org.test.Hello -c "echo goodbye > $HOME/example" || true
  assert_file_has_content $HOME/example hello

  rm $HOME/example

  ok "sandbox filesystem"
fi

reset_overrides

if ! skip_one_without_bwrap "persist"; then
  ${FLATPAK} override --user --persist=example org.test.Hello
  ${FLATPAK} run --command=sh org.test.Hello -c "echo goodbye > $HOME/example/bye"
  assert_file_has_content $HOME/.var/app/org.test.Hello/example/bye goodbye

  ok "persist"
fi
