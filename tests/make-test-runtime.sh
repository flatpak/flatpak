#!/bin/sh

set -e

DIR=`mktemp -d`

ID=$1
shift

xdg-app build-init ${DIR} ${ID} ${ID} ${ID}
sed -i s/Application/Runtime/ ${DIR}/metadata

# Add bash and dependencies
mkdir -p ${DIR}/usr/bin
mkdir -p ${DIR}/usr/lib
ln -s ../lib ${DIR}/usr/lib64
ln -s ../lib ${DIR}/usr/lib32
T=`mktemp`
for i in $@; do
    I=`which $i`
    cp $I ${DIR}/usr/bin
    ldd $I | sed "s/.* => //"  | awk '{ print $1}' | grep ^/ | grep ^/ >> $T
done
ln -s bash ${DIR}/usr/bin/sh
for i in `sort -u $T`; do
    cp "$i" ${DIR}/usr/lib/
done

xdg-app build-export --runtime repo ${DIR}
rm -rf ${DIR}
