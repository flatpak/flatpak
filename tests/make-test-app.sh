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

mkdir -p ${DIR}/files/share/app-info/xmls
mkdir -p ${DIR}/files/share/app-info/icons/xdg-app/64x64
gzip -c > ${DIR}/files/share/app-info/xmls/org.test.Hello.xml.gz <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<components version="0.8">
  <component type="desktop">
    <id>org.test.Hello.desktop</id>
    <name>Hello world test app</name>
    <summary>Print a greeting</summary>
    <description><p>This is a test app.</p></description>
    <categories>
      <category>Utility</category>
    </categories>
    <icon height="64" width="64" type="cached">64x64/org.gnome.gedit.png</icon>
  </component>
</components>
EOF
cp org.test.Hello.png ${DIR}/files/share/app-info/icons/xdg-app/64x64/

xdg-app build-finish --command=hello.sh ${DIR}
xdg-app build-export repo ${DIR}
rm -rf ${DIR}
