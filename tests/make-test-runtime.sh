#!/bin/sh

set -e

DIR=`mktemp -d`

xdg-app build-init ${DIR} org.test.Platform org.test.Platform org.test.Platform
sed -i s/Application/Runtime/ ${DIR}/metadata

# Add bash and dependencies
mkdir -p ${DIR}/usr/bin
mkdir -p ${DIR}/usr/lib
ln -s ../lib ${DIR}/usr/lib64
ln -s ../lib ${DIR}/usr/lib32
BASH=`which bash`
LS=`which ls`
CAT=`which cat`
ECHO=`which echo`
READLINK=`which readlink`
cp ${BASH} ${DIR}/usr/bin
cp ${LS} ${DIR}/usr/bin
cp ${CAT} ${DIR}/usr/bin
cp ${ECHO} ${DIR}/usr/bin
cp ${READLINK} ${DIR}/usr/bin
ln -s bash ${DIR}/usr/bin/sh
for i in `ldd ${BASH} ${LS} ${CAT} ${ECHO} ${READLINK} | sed "s/.* => //" | awk '{ print $1}' | grep -v :$ | grep ^/ | sort -u`; do
    cp "$i" ${DIR}/usr/lib/
done

xdg-app build-export --runtime repo ${DIR}
rm -rf ${DIR}
