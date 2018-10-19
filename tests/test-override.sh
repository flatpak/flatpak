#!/bin/bash

set -euo pipefail

. $(dirname $0)/libtest.sh

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

echo "ok override --socket"

reset_overrides

${FLATPAK} override --user --device=dri org.test.Hello
${FLATPAK} override --user --nodevice=kvm org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Context\]$"
assert_file_has_content override "^devices=dri;!kvm;$"

echo "ok override --device"

reset_overrides

${FLATPAK} override --user --share=network org.test.Hello
${FLATPAK} override --user --unshare=ipc org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Context\]$"
assert_file_has_content override "^shared=network;!ipc;$"

echo "ok override --share"

reset_overrides

${FLATPAK} override --user --allow=multiarch org.test.Hello
${FLATPAK} override --user --disallow=bluetooth org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Context\]$"
assert_file_has_content override "^features=multiarch;!bluetooth;$"

echo "ok override --allow"

reset_overrides

${FLATPAK} override --user --env=FOO=BAR org.test.Hello
${FLATPAK} override --user --env=BAR= org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Environment\]$"
assert_file_has_content override "^FOO=BAR$"
assert_file_has_content override "^BAR=$"

echo "ok override --env"

${FLATPAK} override --user --filesystem=home org.test.Hello
${FLATPAK} override --user --filesystem=xdg-desktop/foo:create org.test.Hello
${FLATPAK} override --user --filesystem=xdg-config:ro org.test.Hello
${FLATPAK} override --user --filesystem=/media org.test.Hello
${FLATPAK} override --user --nofilesystem=xdg-documents org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Context\]$"
assert_file_has_content override "^filesystems=/media;home;!xdg-documents;xdg-desktop/foo:create;xdg-config:ro;$"

echo "ok override --filesystem"

reset_overrides

${FLATPAK} override --user --own-name=org.foo.Own org.test.Hello
${FLATPAK} override --user --talk-name=org.foo.Talk org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[Session Bus Policy\]$"
assert_file_has_content override "^org.foo.Own=own$
assert_file_has_content override "^org.foo.Talk=talk$

echo "ok override session bus names"

reset_overrides

${FLATPAK} override --user --system-own-name=org.foo.Own.System org.test.Hello
${FLATPAK} override --user --system-talk-name=org.foo.Talk.System org.test.Hello
${FLATPAK} override --user --show org.test.Hello > override

assert_file_has_content override "^\[System Bus Policy\]$"
assert_file_has_content override "^org.foo.Own.System=own$
assert_file_has_content override "^org.foo.Talk.System=talk$

echo "ok override system bus names"

reset_overrides

if [ -S "${XDG_RUNTIME_DIR}/wayland-0" ]; then
  ${FLATPAK} override --user --socket=wayland org.test.Hello
  ${FLATPAK} run --command=ls org.test.Hello -- /run/user/1000 > out
  assert_file_has_content out "wayland-0"

  ${FLATPAK} override --user --nosocket=wayland org.test.Hello
  ${FLATPAK} run --command=ls org.test.Hello -- /run/user/1000 > out
  assert_not_file_has_content out "wayland-0"

  echo "ok sandbox wayland socket"
else
  echo "ok sandbox wayland socket # skip not supported without Wayland"
fi

reset_overrides

if [ -d "/dev/dri" ]; then
  ${FLATPAK} override --user --device=dri org.test.Hello
  ${FLATPAK} run --command=ls org.test.Hello -- /dev > out
  assert_file_has_content out "dri"

  ${FLATPAK} override --user --nodevice=dri org.test.Hello
  ${FLATPAK} run --command=ls org.test.Hello -- /dev > out
  assert_not_file_has_content out "dri"

  echo "ok sandbox dri device"
else
  echo "ok sandbox dri device # skip not supported without /dev/dri"
fi

reset_overrides

${FLATPAK} override --user --env=FOO=BAR org.test.Hello

${FLATPAK} run --command=sh org.test.Hello -c 'echo $FOO' > out
assert_file_has_content out "BAR"
FOO=bar ${FLATPAK} run --command=sh org.test.Hello -c 'echo $FOO' > out
assert_file_has_content out "BAR"

echo "ok sandbox env"

reset_overrides

echo "hello" > $HOME/example

${FLATPAK} override --user --filesystem=home:ro org.test.Hello

${FLATPAK} run --command=ls org.test.Hello $HOME > out
assert_file_has_content out example

${FLATPAK} run --command=sh org.test.Hello -c "echo goodbye > $HOME/example" || true
assert_file_has_content $HOME/example hello

rm $HOME/example

echo "ok sandbox filesystem"

reset_overrides

${FLATPAK} override --user --persist=example org.test.Hello
${FLATPAK} run --command=sh org.test.Hello -c "echo goodbye > $HOME/example/bye"
assert_file_has_content $HOME/.var/app/org.test.Hello/example/bye goodbye

echo "ok persist"
