#!/usr/bin/bash

make() {
    /usr/bin/make -j $(getconf _NPROCESSORS_ONLN) "$@"
}

build() {
    env NOCONFIGURE=1 ./autogen.sh
    ./configure --prefix=/usr --libdir=/usr/lib64 "$@"
    make V=1
}

pkg_install() {
    yum -y install "$@"
}

pkg_install_if_os() {
    os=$1
    shift
    (. /etc/os-release;
         if test "${os}" = "${ID}"; then
            pkg_install "$@"
         else
             echo "Skipping installation on OS ${ID}: $@"
         fi
    )
}

pkg_builddep() {
    # This is sadly the only case where it's a different command
    if test -x /usr/bin/dnf; then
        dnf builddep -y "$@"
    else
        yum-builddep -y "$@"
    fi
}

pkg_install_builddeps() {
    pkg=$1
    if test -x /usr/bin/dnf; then
        yum -y install dnf-plugins-core
        yum install -y 'dnf-command(builddep)'
        # Base buildroot
        pkg_install @buildsys-build
    else
        yum -y install yum-utils
        # Base buildroot, copied from the mock config sadly
        yum -y install bash bzip2 coreutils cpio diffutils system-release findutils gawk gcc gcc-c++ grep gzip info make patch redhat-rpm-config rpm-build sed shadow-utils tar unzip util-linux which xz
    fi
    # builddeps+runtime deps
    pkg_builddep $pkg
    pkg_install $pkg
    rpm -e $pkg
}
