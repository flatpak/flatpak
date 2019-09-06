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

pkg_install_testing() {
    yum -y install --enablerepo=updates-testing "$@"
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
        # https://github.com/projectatomic/rpm-ostree/pull/1889/commits/9ff611758bea22b0ad4892cc16182dd1f7f47e89
        # https://fedoraproject.org/wiki/Common_F30_bugs#Conflicts_between_fedora-release_packages_when_installing_package_groups
        if rpm -q fedora-release-container; then
            yum -y swap fedora-release{-container,}
        fi
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
    yum -y update gcc gcc-c++ annobin # This had some weird conflict with gcc
    rpm -e --nodeps $pkg
}
