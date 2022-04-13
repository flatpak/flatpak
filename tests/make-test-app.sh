#!/bin/bash

set -e

# Don't inherit the -x from the testsuite
set +x

DIR=`mktemp -d`

REPO=$1
shift
APP_ID=$1
shift
BRANCH=$1
shift
COLLECTION_ID=$1
shift

if [ x$APP_ID = x ]; then
    APP_ID=org.test.Hello
fi

RUNTIME_BRANCH=${RUNTIME_BRANCH:-$BRANCH}

EXTRA="${1-}"

ARCH=`flatpak --default-arch`

# Init dir
cat > ${DIR}/metadata <<EOF
[Application]
name=$APP_ID
runtime=org.test.Platform/$ARCH/$RUNTIME_BRANCH
sdk=org.test.Platform/$ARCH/$RUNTIME_BRANCH
EOF

if [ x${REQUIRED_VERSION-} != x ]; then
cat >> ${DIR}/metadata <<EOF
required-flatpak=$REQUIRED_VERSION
EOF
fi

cat >> ${DIR}/metadata <<EOF
[Extension $APP_ID.Locale]
directory=share/runtime/locale
autodelete=true
locale-subset=true
EOF

cat >> ${DIR}/metadata <<EOF
[Extension $APP_ID.Plugin]
directory=share/hello/extra
autodelete=true
no-autodownload=true
subdirectories=true
merge-dirs=plug-ins
EOF

if [ "$EXTRA" = "EXTENSIONS" ]; then
cat >> ${DIR}/metadata <<EOF
version=v2
EOF
else
cat >> ${DIR}/metadata <<EOF
version=v1
EOF
fi

mkdir -p ${DIR}/files/bin
cat > ${DIR}/files/bin/hello.sh <<EOF
#!/bin/sh
echo "Hello world, from a sandbox$EXTRA"
if [ "$EXTRA" = "SPIN" ]; then
  exec sh
fi
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
cat > ${DIR}/files/share/applications/org.test.Hello.Again.desktop <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Hello Again
Exec=hello.sh --again
Icon=$APP_ID
MimeType=x-test/Hello;
X-Flatpak-RenamedFrom=hello-again.desktop;
EOF

mkdir -p ${DIR}/files/share/gnome-shell/search-providers
cat > ${DIR}/files/share/gnome-shell/search-providers/org.test.Hello.search-provider.ini <<EOF
[Shell Search Provider]
DesktopId=org.test.Hello.desktop
BusName=org.test.Hello.SearchProvider
ObjectPath=/org/test/Hello/SearchProvider
Version=2
EOF

mkdir -p ${DIR}/files/share/icons/hicolor/64x64/apps
cp $(dirname $0)/org.test.Hello.png ${DIR}/files/share/icons/hicolor/64x64/apps/${APP_ID}.png
cp $(dirname $0)/org.test.Hello.png ${DIR}/files/share/icons/hicolor/64x64/apps/dont-export.png
mkdir -p ${DIR}/files/share/icons/HighContrast/64x64/apps
cp $(dirname $0)/org.test.Hello.png ${DIR}/files/share/icons/HighContrast/64x64/apps/${APP_ID}.png


mkdir -p ${DIR}/files/share/metainfo
cat <<EOF > ${DIR}/files/share/metainfo/${APP_ID}.metainfo.xml
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
    <releases>
      <release timestamp="1525132800" version="0.0.1"/>
    </releases>
  </component>
</components>
EOF

# Also check that the legacy path works
mkdir -p ${DIR}/files/share/appdata
cat <<EOF > ${DIR}/files/share/appdata/${APP_ID}.cmd.appdata.xml
<?xml version="1.0" encoding="UTF-8"?>
<components version="0.8">
  <component type="console-application">
    <id>$APP_ID.cmd</id>
    <name>Command line client for Hello world test app</name>
    <summary>Adds cool functionality</summary>
    <provides>
      <binary>hello</binary>
    </provides>
  </component>
</components>
EOF

mkdir -p ${DIR}/files/share/app-info/xmls
mkdir -p ${DIR}/files/share/app-info/icons/flatpak/64x64
gzip -c ${DIR}/files/share/metainfo/${APP_ID}.metainfo.xml > ${DIR}/files/share/app-info/xmls/${APP_ID}.xml.gz
gzip -c ${DIR}/files/share/appdata/${APP_ID}.cmd.appdata.xml > ${DIR}/files/share/app-info/xmls/${APP_ID}.cmd.xml.gz
cp $(dirname $0)/org.test.Hello.png ${DIR}/files/share/app-info/icons/flatpak/64x64/${APP_ID}.png

if [ x$COLLECTION_ID != x ]; then
    collection_args=--collection-id=${COLLECTION_ID}
else
    collection_args=
fi

mkdir -p ${DIR}/files/share/locale
mkdir -p ${DIR}/files/share/runtime/locale/de
ln -s -t ${DIR}/files/share/locale ../../share/runtime/locale/de/share/de
mkdir -p ${DIR}/files/share/runtime/locale/fr
ln -s -t ${DIR}/files/share/locale ../../share/runtime/locale/fr/share/fr

flatpak build-finish ${BUILD_FINISH_ARGS-} --command=hello.sh ${DIR} >&2
mkdir -p repos
flatpak build-export --no-update-summary --disable-sandbox ${collection_args} ${GPGARGS-} ${EXPORT_ARGS-} ${REPO} ${DIR} ${BRANCH} >&2
rm -rf ${DIR}

# build a locale extension

DIR=`mktemp -d`

# Init dir
cat > ${DIR}/metadata <<EOF
[Runtime]
name=${APP_ID}.Locale

[ExtensionOf]
ref=app/$APP_ID/$ARCH/$BRANCH
EOF

cat > de.po <<EOF
msgid "Hello world"
msgstr "Hallo Welt"
EOF
mkdir -p ${DIR}/files/de/share/de/LC_MESSAGES
msgfmt --output-file ${DIR}/files/de/share/de/LC_MESSAGES/helloworld.mo de.po
cat > fr.po <<EOF
msgid "Hello world"
msgstr "Bonjour le monde"
EOF
mkdir -p ${DIR}/files/fr/share/fr/LC_MESSAGES
msgfmt --output-file ${DIR}/files/fr/share/fr/LC_MESSAGES/helloworld.mo fr.po

flatpak build-finish ${DIR} >&2
mkdir -p repos
flatpak build-export --no-update-summary --runtime ${collection_args} ${GPGARGS-} ${EXPORT_ARGS-} ${REPO} ${DIR} ${BRANCH} >&2
rm -rf ${DIR}

# build a plugin extension

DIR=`mktemp -d`

# Init dir
cat > ${DIR}/metadata <<EOF
[Runtime]
name=${APP_ID}.Plugin.fun

[ExtensionOf]
ref=app/$APP_ID/$ARCH/$BRANCH
EOF

mkdir -p ${DIR}/files/plug-ins/fun

flatpak build-finish ${DIR} >&2
mkdir -p repos

if [ "$EXTRA" = "EXTENSIONS" ]; then
  flatpak build-export --no-update-summary --runtime ${collection_args} ${GPGARGS-} ${EXPORT_ARGS-} ${REPO} ${DIR} v2 >&2
else
  flatpak build-export --no-update-summary --runtime ${collection_args} ${GPGARGS-} ${EXPORT_ARGS-} ${REPO} ${DIR} v1 >&2
fi

rm -rf ${DIR}
