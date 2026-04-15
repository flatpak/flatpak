#!/bin/bash
set -euo pipefail

echo Fuck

. $(dirname $0)/libtest.sh

skip_without_bwrap

echo "1..1"

mkdir repos

httpd web-server.py repos
repo_port=$(cat httpd-port)

httpd oci-registry-server.py --dir=.
oci_port=$(cat httpd-port)

client="python3 $test_srcdir/oci-registry-client.py --url=http://127.0.0.1:${oci_port}"

setup_repo runtime-repo
setup_empty_repo oci

cat > runtime-repo.flatpakrepo << EOF
[Flatpak Repo]
Version=1
Url=http://127.0.0.1:${repo_port}/runtime-repo
Title=Runtime Repo
EOF

${FLATPAK} build-bundle --oci $FL_GPGARGS \
    --runtime-repo="file://$(pwd)/runtime-repo.flatpakrepo" \
    repos/runtime-repo oci/app-image org.test.Hello >&2

$client add hello latest $(pwd)/oci/app-image

${FLATPAK} remote-add ${U} oci-registry "oci+http://127.0.0.1:${oci_port}" >&2
${FLATPAK} update ${U} --appstream oci-registry >&2

${FLATPAK} ${U} install -y oci-registry org.test.Hello >&2

${FLATPAK} ${U} remotes > remotes-list
assert_file_has_content remotes-list 'runtime-repo-repo'

ok "runtime repo from OCI metadata"