#!/bin/sh

set -e

DIR=`mktemp -d`

REPONAME=$1
shift
APP_ID=$1
shift
COLLECTION_ID=$1
shift

if [ x$APP_ID = x ]; then
    APP_ID=org.test.Hello
fi

EXTRA="${1-}"

ARCH=`flatpak --default-arch`

# Init dir
cat > ${DIR}/metadata <<EOF
[Application]
name=$APP_ID
runtime=org.test.Platform/$ARCH/master
sdk=org.test.Platform/$ARCH/master
EOF

mkdir -p ${DIR}/files/bin
cat > ${DIR}/files/bin/hello.sh <<EOF
#!/bin/sh
echo "Hello world, from a sandbox$EXTRA"
EOF
chmod a+x ${DIR}/files/bin/hello.sh

mkdir -p ${DIR}/files/share/applications
cat > ${DIR}/files/share/applications/org.test.Hello.desktop <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Hello
Exec=hello.sh
Icon=$APP_ID
MimeType=x-test/Hello;
EOF

mkdir -p ${DIR}/files/share/icons/hicolor/64x64/apps
cp $(dirname $0)/org.test.Hello.png ${DIR}/files/share/icons/hicolor/64x64/apps/${APP_ID}.png
cp $(dirname $0)/org.test.Hello.png ${DIR}/files/share/icons/hicolor/64x64/apps/dont-export.png
mkdir -p ${DIR}/files/share/icons/HighContrast/64x64/apps
cp $(dirname $0)/org.test.Hello.png ${DIR}/files/share/icons/HighContrast/64x64/apps/${APP_ID}.png


mkdir -p ${DIR}/files/share/app-info/xmls
mkdir -p ${DIR}/files/share/app-info/icons/flatpak/64x64
gzip -c > ${DIR}/files/share/app-info/xmls/${APP_ID}.xml.gz <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<components version="0.8">
  <component type="desktop">
    <id>$APP_ID.desktop</id>
    <name>Hello world test app: $APP_ID</name>
    <summary>Print a greeting</summary>
    <description><p>This is a test app.</p></description>
    <categories>
      <category>Utility</category>
    </categories>
    <icon height="64" width="64" type="cached">64x64/org.gnome.gedit.png</icon>
  </component>
</components>
EOF
cp $(dirname $0)/org.test.Hello.png ${DIR}/files/share/app-info/icons/flatpak/64x64/${APP_ID}.png

if [ x$COLLECTION_ID != x ]; then
    collection_args=--collection-id=${COLLECTION_ID}
else
    collection_args=
fi

flatpak build-finish --command=hello.sh ${DIR}
mkdir -p repos
flatpak build-export ${collection_args} ${GPGARGS-} ${EXPORT_ARGS-} repos/${REPONAME} ${DIR}
rm -rf ${DIR}
