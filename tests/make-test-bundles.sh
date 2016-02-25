#!/bin/sh

set -e

./make-test-runtime.sh
./make-test-app.sh

URL=file://`pwd`/repo

REF=`(cd repo/refs/heads; echo app/org.test.Hello/*/master)`

xdg-app build-bundle repo hello.xdgapp org.test.Hello
xdg-app build-bundle repo hello-key.xdgapp --gpg-keys=test-keyring/pubring.gpg org.test.Hello
xdg-app build-bundle repo --repo-url=${URL} hello-origin.xdgapp org.test.Hello
xdg-app build-bundle repo --repo-url=${URL} --gpg-keys=test-keyring/pubring.gpg hello-key-origin.xdgapp org.test.Hello

ostree gpg-sign --repo=repo --gpg-homedir=test-keyring ${REF} 7B0961FD

xdg-app build-bundle repo hello-signed.xdgapp org.test.Hello
xdg-app build-bundle repo hello-key-signed.xdgapp --gpg-keys=test-keyring/pubring.gpg org.test.Hello
xdg-app build-bundle repo --repo-url=${URL} hello-origin-signed.xdgapp org.test.Hello
xdg-app build-bundle repo --repo-url=${URL} --gpg-keys=test-keyring/pubring.gpg hello-key-origin-signed.xdgapp org.test.Hello

REF=`(cd repo/refs/heads; echo runtime/org.test.Platform/*/master)`
ostree gpg-sign --repo=repo --gpg-homedir=test-keyring ${REF} 7B0961FD
xdg-app build-bundle --runtime repo --repo-url=${URL} --gpg-keys=test-keyring/pubring.gpg platform.xdgapp org.test.Platform
