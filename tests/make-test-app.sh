#!/bin/sh

set -e

DIR=`mktemp -d`

# Init dir
xdg-app build-init ${DIR} org.test.Hello org.test.Platform org.test.Platform

mkdir -p ${DIR}/files/bin
cat > ${DIR}/files/bin/hello.sh <<EOF
#!/bin/sh
echo "Hello world, from a sandbox"
EOF
chmod a+x ${DIR}/files/bin/hello.sh

xdg-app build-finish --command=hello.sh ${DIR}
xdg-app build-export repo ${DIR}
rm -rf ${DIR}
