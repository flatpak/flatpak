#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Ensure a real system-helper OCI import waits for cache cleanup's lock."""

import ctypes
import fcntl
import os
from pathlib import Path
import select
import subprocess
import sys


registry, lock_path, checksum, payload_path, ref = sys.argv[1:]
payload = Path(payload_path).read_bytes()
blob = Path(registry) / "blobs" / "sha256" / checksum
blob.write_bytes(payload)
# The importer must not open the blob until the exclusive repo lock is released.
libc = ctypes.CDLL(None, use_errno=True)
libc.inotify_init1.argtypes = [ctypes.c_int]
libc.inotify_init1.restype = ctypes.c_int
libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
libc.inotify_add_watch.restype = ctypes.c_int
notify = libc.inotify_init1(os.O_CLOEXEC | os.O_NONBLOCK)
assert notify >= 0, os.strerror(ctypes.get_errno())
assert libc.inotify_add_watch(notify, os.fsencode(blob), 0x20) >= 0  # IN_OPEN
lock = open(lock_path, "a+b")
fcntl.lockf(lock, fcntl.LOCK_EX)
process = subprocess.Popen(
    [
        "gdbus", "call", "--session",
        "--dest", "org.freedesktop.Flatpak.SystemHelper",
        "--object-path", "/org/freedesktop/Flatpak/SystemHelper",
        "--method", "org.freedesktop.Flatpak.SystemHelper.Deploy",
        str(list(os.fsencode(os.path.abspath(registry)) + b"\0")),
        "8",  # FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL
        ref, "oci-registry", "[]", "[]", "",
    ],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

try:
    assert not select.select([notify], [], [], 2)[0], (
        "OCI import read extra data while cleanup held the exclusive repository lock"
    )
    assert process.poll() is None, "system helper exited instead of waiting for cleanup"
finally:
    fcntl.lockf(lock, fcntl.LOCK_UN)
    lock.close()
    try:
        stdout, stderr = process.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        raise
    sys.stderr.write(stderr)

assert process.returncode == 0, (stdout, stderr)
assert select.select([notify], [], [], 0)[0], "system helper never opened the extra-data blob"
os.close(notify)
