#!/bin/bash
# Copyright 2021 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.0-or-later

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_bwrap

echo "1..18"

setup_repo
install_repo

cp -a "$G_TEST_BUILDDIR/try-syscall" "$test_tmpdir/try-syscall"

# How this works:
# try-syscall tries to make various syscalls, some benign, some not.
#
# The parameters are chosen to make them fail with EBADF or EFAULT if
# not blocked. If they are blocked, we get ENOSYS or EPERM. If the syscall
# is impossible for a particular architecture, we get ENOENT.
#
# The exit status is an errno value, which we can compare with the expected
# errno value.

eval "$("$test_tmpdir/try-syscall" print-errno-values)"

try_syscall () {
  ${FLATPAK} run \
    --filesystem="$test_tmpdir" \
    --command="$test_tmpdir/try-syscall" \
    $extra_argv \
    org.test.Hello "$@"
}

for extra_argv in "" "--allow=multiarch"; do
  echo "# testing with extra argv: '$extra_argv'"

  echo "# chmod (benign)"
  e=0
  try_syscall chmod || e="$?"
  assert_streq "$e" "$EFAULT"
  ok "chmod not blocked"

  echo "# chroot (harmful)"
  e=0
  try_syscall chroot || e="$?"
  assert_streq "$e" "$EPERM"
  ok "chroot blocked with EPERM"

  echo "# clone3 (harmful)"
  e=0
  try_syscall clone3 || e="$?"
  # This is either ENOSYS because the kernel genuinely doesn't implement it,
  # or because we successfully blocked it. We can't tell which.
  assert_streq "$e" "$ENOSYS"
  ok "clone3 blocked with ENOSYS (CVE-2021-41133)"

  echo "# ioctl TIOCNOTTY (benign)"
  e=0
  try_syscall "ioctl TIOCNOTTY" || e="$?"
  assert_streq "$e" "$EBADF"
  ok "ioctl TIOCNOTTY not blocked"

  echo "# ioctl TIOCSTI (CVE-2017-5226)"
  e=0
  try_syscall "ioctl TIOCSTI" || e="$?"
  assert_streq "$e" "$EPERM"
  ok "ioctl TIOCSTI blocked (CVE-2017-5226)"

  echo "# ioctl TIOCSTI (trying to repeat CVE-2019-10063)"
  e=0
  try_syscall "ioctl TIOCSTI CVE-2019-10063" || e="$?"
  if test "$e" = "$ENOENT"; then
    echo "ok # SKIP Cannot replicate CVE-2019-10063 on 32-bit architecture"
  else
    assert_streq "$e" "$EPERM"
    ok "ioctl TIOCSTI with high bits blocked (CVE-2019-10063)"
  fi

  echo "# ioctl TIOCLINUX (CVE-2023-28100)"
  e=0
  try_syscall "ioctl TIOCLINUX" || e="$?"
  assert_streq "$e" "$EPERM"
  ok "ioctl TIOCLINUX blocked"

  echo "# listen (benign)"
  e=0
  try_syscall "listen" || e="$?"
  assert_streq "$e" "$EBADF"
  ok "listen not blocked"

  echo "# prctl (benign)"
  e=0
  try_syscall "prctl" || e="$?"
  assert_streq "$e" "$EFAULT"
  ok "prctl not blocked"
done
