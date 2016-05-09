#!/bin/sh

set -e

./make-test-runtime.sh
./make-test-app.sh

URL=file://`pwd`/repo

REF=`(cd repo/refs/heads; echo app/org.test.Hello/*/master)`

flatpak build-bundle repo hello.pak org.test.Hello
flatpak build-bundle repo hello-key.pak --gpg-keys=test-keyring/pubring.gpg org.test.Hello
flatpak build-bundle repo --repo-url=${URL} hello-origin.pak org.test.Hello
flatpak build-bundle repo --repo-url=${URL} --gpg-keys=test-keyring/pubring.gpg hello-key-origin.pak org.test.Hello

ostree gpg-sign --repo=repo --gpg-homedir=test-keyring ${REF} 7B0961FD

flatpak build-bundle repo hello-signed.pak org.test.Hello
flatpak build-bundle repo hello-key-signed.pak --gpg-keys=test-keyring/pubring.gpg org.test.Hello
flatpak build-bundle repo --repo-url=${URL} hello-origin-signed.pak org.test.Hello
flatpak build-bundle repo --repo-url=${URL} --gpg-keys=test-keyring/pubring.gpg hello-key-origin-signed.pak org.test.Hello

REF=`(cd repo/refs/heads; echo runtime/org.test.Platform/*/master)`
ostree gpg-sign --repo=repo --gpg-homedir=test-keyring ${REF} 7B0961FD
flatpak build-bundle --runtime repo --repo-url=${URL} --gpg-keys=test-keyring/pubring.gpg platform.pak org.test.Platform
