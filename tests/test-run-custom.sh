#!/bin/bash
#
# Copyright (C) 2026 Red Hat, Inc.
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

. "$(dirname "$0")/libtest.sh"

skip_without_bwrap
skip_revokefs_without_fuse

echo "1..12"

# Use stable rather than master as the branch so we can test that the run
# command automatically finds the branch correctly
setup_repo "" "" stable
install_repo "" stable

setup_repo_no_add custom org.test.Collection.Custom master
make_updated_runtime custom org.test.Collection.Custom master CUSTOM
make_updated_app custom org.test.Collection.Custom master CUSTOM

ostree checkout -U --repo=repos/custom runtime/org.test.Platform/${ARCH}/master custom-runtime >&2
ostree checkout -U --repo=repos/custom app/org.test.Hello/$ARCH/master custom-app >&2

cat custom-runtime/files/bin/runtime_hello.sh > runtime_hello
assert_file_has_content runtime_hello "runtimeCUSTOM"
cat custom-app/files/bin/hello.sh > app_hello
assert_file_has_content app_hello "sandboxCUSTOM"

run org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

run --command=/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtime$'

ok "setup"

assert_not run --app-path="" --command=/app/bin/hello.sh org.test.Hello > /dev/null

run --app-path="" --command=/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtime$'

run --app-path="" --command=/run/parent/app/bin/hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

assert_not run --app-path="" --command=/run/parent/usr/bin/runtime_hello.sh org.test.Hello > /dev/null

ok "empty app path"

run --app-path=custom-app/files --command=/app/bin/hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxCUSTOM$'

run --app-path=custom-app/files --command=/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtime$'

run --app-path=custom-app/files --command=/run/parent/app/bin/hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

assert_not run --app-path=custom-app/files --command=/run/parent/usr/bin/runtime_hello.sh org.test.Hello > /dev/null

ok "custom app path"

assert_not run --app-path=path-which-does-not-exist org.test.Hello > /dev/null

ok "bad custom app path"

exec 3< custom-app/files
run --app-fd=3 --command=/app/bin/hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxCUSTOM$'
exec 3>&-

assert_not run --app-fd=3 --command=/app/bin/hello.sh org.test.Hello > /dev/null

ok "custom app fd"

run --usr-path=custom-runtime/files --command=/app/bin/hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

run --usr-path=custom-runtime/files --command=/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtimeCUSTOM$'

assert_not run --usr-path=custom-runtime/files --command=/run/parent/app/bin/hello.sh org.test.Hello > /dev/null

run --usr-path=custom-runtime/files --command=/run/parent/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtime$'

ok "custom usr path"

assert_not run --usr-path=path-which-does-not-exist org.test.Hello > /dev/null

ok "bad custom usr path"

exec 3< custom-runtime/files
run --usr-fd=3 --command=/app/bin/hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'
exec 3>&-

exec 3< custom-runtime/files
run --usr-fd=3 --command=/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtimeCUSTOM$'
exec 3>&-

assert_not run --usr-fd=3 --command=/app/bin/hello.sh org.test.Hello > /dev/null

ok "custom usr fd"

run --usr-path=custom-runtime/files --app-path=custom-app/files \
    --command=/app/bin/hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandboxCUSTOM$'

run --usr-path=custom-runtime/files --app-path=custom-app/files \
    --command=/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtimeCUSTOM$'

run --usr-path=custom-runtime/files --app-path=custom-app/files \
    --command=/run/parent/app/bin/hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

run --usr-path=custom-runtime/files --app-path=custom-app/files \
    --command=/run/parent/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtime$'

ok "custom usr and app path"

assert_not run --usr-path=custom-runtime/files --app-path="" \
               --command=/app/bin/hello.sh org.test.Hello > /dev/null

run --usr-path=custom-runtime/files --app-path="" \
    --command=/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtimeCUSTOM$'

run --usr-path=custom-runtime/files --app-path="" \
    --command=/run/parent/app/bin/hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a sandbox$'

run --usr-path=custom-runtime/files --app-path="" \
    --command=/run/parent/usr/bin/runtime_hello.sh org.test.Hello > hello_out
assert_file_has_content hello_out '^Hello world, from a runtime$'

ok "custom usr and empty app path"

path="$(readlink -f .)/foo"
echo "bar" > "${path}"

exec 3< "${path}"
run --bind-fd=3 --command=cat org.test.Hello "${path}" > hello_out
assert_file_has_content hello_out '^bar$'

exec 3< "${path}"
run --bind-fd=3 --command=bash org.test.Hello -c "echo baz > ${path}" > /dev/null
assert_file_has_content "${path}" '^baz$'
exec 3>&-

exec 3< "${path}"
run --ro-bind-fd=3 --command=cat org.test.Hello "${path}" > hello_out
assert_file_has_content hello_out '^baz$'
exec 3>&-

exec 3< "${path}"
assert_not run --ro-bind-fd=3 --command=bash org.test.Hello -c "echo baz > ${path}" > /dev/null
exec 3>&-

ok "bind-fd and ro-bind-fd"

exec 3< custom-app/files
exec 4< custom-runtime/files
exec 5< "${path}"
exec 6< "${path}"
run --app-fd=3 --usr-fd=4 --bind-fd=5 --ro-bind-fd=6 \
    --command=sh org.test.Hello \
    -c 'for fd in $(ls /proc/self/fd); do readlink -f /proc/self/fd/$fd; done' > hello_out
exec 6>&-
exec 5>&-
exec 4>&-
exec 3>&-

wd="$(readlink -f .)"
while read fdpath; do
  if [[ "$fdpath" == "$wd"* && "$fdpath" != "$wd/hello_out" ]]; then
    assert_not_reached "A fd for '$fdpath' unexpectedly made it to the app"
  fi
done < hello_out

ok "check no fd leak"