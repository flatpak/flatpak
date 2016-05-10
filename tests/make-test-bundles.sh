#!/bin/sh

set -e
set -x

./make-test-runtime.sh org.test.Platform bash ls cat echo readlink
./make-test-app.sh

URL=file://`pwd`/repo

REF=`(cd repo/refs/heads; echo app/org.test.Hello/*/master)`

flatpak build-bundle repo hello.flatpak org.test.Hello
flatpak build-bundle repo hello-key.flatpak --gpg-keys=test-keyring/pubring.gpg org.test.Hello
flatpak build-bundle repo --repo-url=${URL} hello-origin.flatpak org.test.Hello
flatpak build-bundle repo --repo-url=${URL} --gpg-keys=test-keyring/pubring.gpg hello-key-origin.flatpak org.test.Hello

ostree gpg-sign --repo=repo --gpg-homedir=test-keyring ${REF} 7B0961FD

flatpak build-bundle repo hello-signed.flatpak org.test.Hello
flatpak build-bundle repo hello-key-signed.flatpak --gpg-keys=test-keyring/pubring.gpg org.test.Hello
flatpak build-bundle repo --repo-url=${URL} hello-origin-signed.flatpak org.test.Hello
flatpak build-bundle repo --repo-url=${URL} --gpg-keys=test-keyring/pubring.gpg hello-key-origin-signed.flatpak org.test.Hello

REF=`(cd repo/refs/heads; echo runtime/org.test.Platform/*/master)`
ostree gpg-sign --repo=repo --gpg-homedir=test-keyring ${REF} 7B0961FD
flatpak build-bundle --runtime repo --repo-url=${URL} --gpg-keys=test-keyring/pubring.gpg platform.flatpak org.test.Platform
