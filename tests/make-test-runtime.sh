#!/bin/sh

set -e

# Don't inherit the -x from the testsuite
set +x

DIR=`mktemp -d`

REPONAME=$1
shift
ID=$1
shift
COLLECTION_ID=$1
shift

mkdir ${DIR}/files
mkdir ${DIR}/usr
cat > ${DIR}/metadata <<EOF
[Runtime]
name=${ID}
EOF

# On Debian derivatives, /usr/sbin and /sbin aren't in ordinary users'
# PATHs, but ldconfig is kept in /sbin
PATH="$PATH:/usr/sbin:/sbin"

# Add bash and dependencies
mkdir -p ${DIR}/usr/bin
mkdir -p ${DIR}/usr/lib
ln -s ../lib ${DIR}/usr/lib64
ln -s ../lib ${DIR}/usr/lib32
if test -f /sbin/ldconfig.real; then
    cp /sbin/ldconfig.real ${DIR}/usr/bin/ldconfig
else
    cp `which ldconfig` ${DIR}/usr/bin
fi
LIBS=`mktemp`
BINS=`mktemp`

add_bin() {
    local f=$1
    shift

    if grep -qFe "${f}" $BINS; then
        # Already handled
        return 0
    fi

    echo $f >> $BINS

    # Add library dependencies
    (ldd "${f}" | sed "s/.* => //"  | awk '{ print $1}' | grep ^/ | sort -u -o $LIBS $LIBS -)  || true

    local shebang=$(sed -n '1s/^#!\([^ ]*\).*/\1/p' "${f}")
    if [ x$shebang != x ]; then
        add_bin "$shebang"
    fi
}

for i in $@; do
    I=`which $i`
    add_bin $I
    if test $i == python2; then
        mkdir -p ${DIR}/usr/lib/python2.7/lib-dynload
        # This is a hardcoded minimal set of modules we need in the current tests.
        # Pretty hacky stuff. Add modules as needed.
        PYDIR=/usr/lib/python2.7
        if test -d /usr/lib64/python2.7; then PYDIR=/usr/lib64/python2.7; fi
        for py in site os stat posixpath genericpath warnings \
                       linecache types UserDict abc _abcoll \
                       _weakrefset copy_reg traceback sysconfig \
                       re sre_compile sre_parse sre_constants \
                       _sysconfigdata ; do
            cp ${PYDIR}/$py.py ${DIR}/usr/lib/python2.7
        done
        # These might not exist, depending how Python was configured; and the
        # part after ${so} might be "module" or ".x86_64-linux-gnu" or
        # something else
        for so in _locale strop ; do
            cp ${PYDIR}/lib-dynload/${so}*.so ${DIR}/usr/lib/python2.7/lib-dynload || :
        done
        for plat in $( cd ${PYDIR} && echo plat-* ); do
            test -e ${PYDIR}/${plat} || continue
            mkdir -p ${DIR}/usr/lib/python2.7/${plat}
            cp ${PYDIR}/${plat}/*.py ${DIR}/usr/lib/python2.7/${plat}/
        done
    fi
done
for i in `cat $BINS`; do
    echo Adding binary $i 1>&2
    cp "$i" ${DIR}/usr/bin/
done
for i in `cat $LIBS`; do
    echo Adding library $i 1>&2
    cp "$i" ${DIR}/usr/lib/
done
ln -s bash ${DIR}/usr/bin/sh

# We copy the C.UTF8 locale and call it en_US. Its a bit of a lie, but
# the real en_US locale is often not available, because its in the
# local archive.
mkdir -p ${DIR}/usr/lib/locale/
cp -r /usr/lib/locale/C.* ${DIR}/usr/lib/locale/en_US

if [ x$COLLECTION_ID != x ]; then
    collection_args=--collection-id=${COLLECTION_ID}
else
    collection_args=
fi

mkdir -p repos
flatpak build-export ${collection_args} --runtime ${GPGARGS-} repos/${REPONAME} ${DIR}
rm -rf ${DIR}
