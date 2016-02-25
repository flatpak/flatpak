#!/bin/sh

set -e

DIR=`mktemp -d`

xdg-app build-init ${DIR} org.test.Platform org.test.Platform org.test.Platform
sed -i s/Application/Runtime/ ${DIR}/metadata

# Add bash and dependencies
mkdir -p ${DIR}/usr/bin
mkdir -p ${DIR}/usr/lib
ln -s ../lib ${DIR}/usr/lib64
BASH=`which bash`
cp ${BASH} ${DIR}/usr/bin
ln -s bash ${DIR}/usr/bin/sh
for i in `ldd ${BASH}  | sed "s/.* => //" | awk '{ print $1}' | grep ^/`; do
    cp "$i" ${DIR}/usr/lib/
done

xdg-app build-export --runtime repo ${DIR}
rm -rf ${DIR}
