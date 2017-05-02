# Basic Dockerfile with the mimimum amount of packages needed to build flatpak
# After this, just clone this repo, then:
# ./autogen.sh
# make -j3

FROM fedora

RUN dnf -y install git\
                   which\
                   autoconf\
                   automake\
                   gtk-doc\
                   intltool\
                   libtool\
                   glib2-devel\
                   gobject-introspection-devel\
                   libcap-devel\
                   libsoup-devel\
                   polkit-devel\
                   libXau-devel\
                   libgsystem-devel\
                   ostree-devel\
                   fuse-devel\
                   json-glib-devel\
                   libseccomp-devel\
                   elfutils-libelf-devel\
                   elfutils-devel\
                   redhat-rpm-config
