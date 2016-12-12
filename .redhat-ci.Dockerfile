FROM fedora:25
MAINTAINER Alexander Larsson <alexl@redhat.com>

RUN dnf install -y \
        gcc \
        sudo \
        which \
        attr \
        fuse \
        gjs \
        parallel \
        clang \
        libubsan \
        gnome-desktop-testing \
        redhat-rpm-config \
        elfutils \
        ostree-devel \
        libarchive-devel \
        json-glib-devel \
        fuse-devel \
 && dnf clean all

# create an unprivileged user for testing
RUN adduser testuser
