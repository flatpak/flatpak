AC_PREREQ([2.63])

# Making releases:
#   FLATPAK_MICRO_VERSION += 1;
#   FLATPAK_INTERFACE_AGE += 1;
#   FLATPAK_BINARY_AGE += 1;
# if any functions have been added, set FLATPAK_INTERFACE_AGE to 0.
# if backwards compatibility has been broken,
# set FLATPAK_BINARY_AGE and FLATPAK_INTERFACE_AGE to 0.
#
# in easier to understand terms:
#
# on the stable branch, interface age == micro
# on the unstable (ie master), interface age = 0

m4_define([flatpak_major_version], [1])
m4_define([flatpak_minor_version], [8])
m4_define([flatpak_micro_version], [1])
m4_define([flatpak_extra_version], [])
m4_define([flatpak_interface_age], [0])
m4_define([flatpak_binary_age],
          [m4_eval(10000 * flatpak_major_version + 100 * flatpak_minor_version + flatpak_micro_version)])
m4_define([flatpak_version],
          [flatpak_major_version.flatpak_minor_version.flatpak_micro_version()flatpak_extra_version])

AC_INIT([Flatpak],
        [flatpak_version],
        [https://github.com/flatpak/flatpak/issues],
        [flatpak],
        [http://flatpak.org/])

GLIB_REQS=2.44
SYSTEM_BWRAP_REQS=0.4.0
SYSTEM_DBUS_PROXY_REQS=0.1.0
OSTREE_REQS=2018.9

AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC
AM_PROG_CC_C_O
AC_DISABLE_STATIC
AC_PROG_CC_STDC

LT_PREREQ([2.2.6])
LT_INIT([disable-static])

AC_CONFIG_SRCDIR([common/flatpak-dir.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([1.13.4 no-define no-dist-gzip dist-xz tar-ustar foreign subdir-objects])
AC_PROG_SED
AC_PROG_YACC

AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.18.2])
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE, "$PACKAGE", [gettext domain])

# Enable silent rules is available
AM_SILENT_RULES([yes])
AM_MAINTAINER_MODE([enable])

dnl This list is shared with https://github.com/ostreedev/ostree
CC_CHECK_FLAGS_APPEND([WARN_CFLAGS], [CFLAGS], [\
-pipe \
-Wall \
-Werror=empty-body \
-Werror=strict-prototypes \
-Werror=missing-prototypes \
-Werror=implicit-function-declaration \
"-Werror=format=2 -Werror=format-security -Werror=format-nonliteral" \
-Werror=pointer-arith -Werror=init-self \
-Werror=missing-declarations \
-Werror=return-type \
-Werror=overflow \
-Werror=int-conversion \
-Werror=parenthesis \
-Werror=incompatible-pointer-types \
-Werror=misleading-indentation \
-Werror=missing-include-dirs \
])
AC_SUBST(WARN_CFLAGS)

AX_VALGRIND_CHECK

PKG_PROG_PKG_CONFIG([0.24])

AC_ARG_WITH(privileged_group,
		AS_HELP_STRING([--with-privileged-group=GROUP],[Name of privileged group, [default=wheel]]),
            with_privileged_group="$withval", with_privileged_group=wheel)
PRIVILEGED_GROUP=$with_privileged_group
AC_SUBST(PRIVILEGED_GROUP)

AC_ARG_WITH(dbus_service_dir,
		AS_HELP_STRING([--with-dbus-service-dir=PATH],[choose directory for dbus service files, [default=PREFIX/share/dbus-1/services]]),
            with_dbus_service_dir="$withval", with_dbus_service_dir=$datadir/dbus-1/services)
DBUS_SERVICE_DIR=$with_dbus_service_dir
AC_SUBST(DBUS_SERVICE_DIR)

AC_ARG_WITH(dbus_config_dir,
		AS_HELP_STRING([--with-dbus-config-dir=PATH],[choose directory for dbus config files, [default=SYSCONFDIR/dbus-1/system.d]]),
            with_dbus_config_dir="$withval", with_dbus_config_dir=${sysconfdir}/dbus-1/system.d)
DBUS_CONFIG_DIR=$with_dbus_config_dir
AC_SUBST(DBUS_CONFIG_DIR)

AC_ARG_WITH([systemduserunitdir],
            [AS_HELP_STRING([--with-systemduserunitdir=DIR],
                            [Directory for systemd user service files (default=PREFIX/lib/systemd/user)])],
    [],
    dnl This is deliberately not ${libdir}: systemd units always go in
    dnl .../lib, never .../lib64 or .../lib/x86_64-linux-gnu
    [with_systemduserunitdir='${prefix}/lib/systemd/user'])
AC_SUBST([systemduserunitdir], [$with_systemduserunitdir])

AC_ARG_WITH([systemdsystemunitdir],
            [AS_HELP_STRING([--with-systemdsystemunitdir=DIR],
                            [Directory for systemd system service files (default=PREFIX/lib/systemd/system)])],
    [],
    dnl This is deliberately not ${libdir}: systemd units always go in
    dnl .../lib, never .../lib64 or .../lib/x86_64-linux-gnu
    [with_systemdsystemunitdir='${prefix}/lib/systemd/system'])
AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])

AC_ARG_WITH([systemduserenvgendir],
            [AS_HELP_STRING([--with-systemduserenvgendir=DIR],
                            [Directory for systemd user environment generators (default=PREFIX/lib/systemd/user-environment-generators)])],
    [],
    dnl This is deliberately not ${libdir}: systemd units always go in
    dnl .../lib, never .../lib64 or .../lib/x86_64-linux-gnu
    [with_systemduserenvgendir='${prefix}/lib/systemd/user-environment-generators'])
AC_SUBST([systemduserenvgendir], [$with_systemduserenvgendir])

AC_ARG_WITH(system_fonts_dir,
		AS_HELP_STRING([--with-system-fonts-dir=PATH],[Directory where system fonts are, [default=/usr/share/fonts]]),
            with_system_fonts_dir="$withval", with_system_fonts_dir=/usr/share/fonts)
SYSTEM_FONTS_DIR=$with_system_fonts_dir
AC_SUBST(SYSTEM_FONTS_DIR)

AC_ARG_WITH(system_font_cache_dirs,
		AS_HELP_STRING([--with-system-font-cache-dirs=PATHS],[Directory where the system font cache is, [default=/var/cache/fontconfig:/usr/lib/fontconfig/cache]]),
            with_system_font_cache_dirs="$withval", with_system_font_cache_dirs=/var/cache/fontconfig:/usr/lib/fontconfig/cache)
SYSTEM_FONT_CACHE_DIRS=$with_system_font_cache_dirs
AC_SUBST(SYSTEM_FONT_CACHE_DIRS)

AC_ARG_WITH(profile_dir,
		AS_HELP_STRING([--with-profile-dir=PATH],[choose directory for profile.d files, [default=SYSCONFDIR/profile.d]]),
            with_profile_dir="$withval", with_profile_dir=${sysconfdir}/profile.d)
PROFILE_DIR=$with_profile_dir
AC_SUBST(PROFILE_DIR)

AC_ARG_VAR([BWRAP], [Bubblewrap executable])
AC_ARG_WITH([system-bubblewrap],
            [AS_HELP_STRING([--with-system-bubblewrap], [Use system bwrap executable [default=check $BWRAP]])],
            [BWRAP="$withval"],
            [BWRAP="${BWRAP:-false}"])
CAP_LIB=
AS_CASE([$BWRAP],
        [yes],
            [BWRAP=bwrap],
        [no],
            [BWRAP=false],
        [auto],
            [AC_CHECK_PROG([BWRAP], [bwrap], [bwrap], [false])])
if test "x$BWRAP" != xfalse; then
   BWRAP_VERSION=`$BWRAP --version | sed 's,.*\ \([0-9]*\.[0-9]*\.[0-9]*\)$,\1,'`
   AX_COMPARE_VERSION([$SYSTEM_BWRAP_REQS],[gt],[$BWRAP_VERSION],
                      [AC_MSG_ERROR([You need at least version $SYSTEM_BWRAP_REQS of bubblewrap to use the system installed version])])
   AM_CONDITIONAL([WITH_SYSTEM_BWRAP], [true])
else
   AC_CHECK_LIB(cap, cap_from_text, CAP_LIB=-lcap)
   if test "$ac_cv_lib_cap_cap_from_text" != "yes"; then
      AC_MSG_ERROR([*** libcap needed by bubblewrap but not found])
   fi

   AM_CONDITIONAL([WITH_SYSTEM_BWRAP], [false])
fi
AC_SUBST([CAP_LIB])

AC_ARG_VAR([DBUS_PROXY], [dbus-proxy executable])
AC_ARG_WITH([system-dbus-proxy],
            [AS_HELP_STRING([--with-system-dbus-proxy], [Use system xdg-dbus-proxy executable [default=check $DBUS_PROXY]])],
            [DBUS_PROXY="$withval"],
            [DBUS_PROXY="${DBUS_PROXY:-false}"])
AS_CASE([$DBUS_PROXY],
        [yes],
            [DBUS_PROXY=xdg-dbus-proxy],
        [no],
            [DBUS_PROXY=false],
        [auto],
            [AC_CHECK_PROG([DBUS_PROXY], [xdg-dbus-proxy], [xdg-dbus-proxy], [false])])
if test "x$DBUS_PROXY" != xfalse; then
   DBUS_PROXY_VERSION=[$( $DBUS_PROXY --version  | sed 's,.*\ \([0-9]*\.[0-9]*\.[0-9]*\)$,\1,')]
   AX_COMPARE_VERSION([$SYSTEM_DBUS_PROXY_REQS],[gt],[$DBUS_PROXY_VERSION],
                      [AC_MSG_ERROR([You need at least version $SYSTEM_DBUS_PROXY_REQS of xdg-dbus-proxy to use the system installed version (have $DBUS_PROXY_VERSION)])])
   AM_CONDITIONAL([WITH_SYSTEM_DBUS_PROXY], [true])
else
   AM_CONDITIONAL([WITH_SYSTEM_DBUS_PROXY], [false])
fi


AC_CHECK_FUNCS(fdwalk)
LIBGLNX_CONFIGURE

AC_CHECK_HEADER([sys/xattr.h], [], [AC_MSG_ERROR([You must have sys/xattr.h from glibc])])
AC_CHECK_HEADER([sys/capability.h], have_caps=yes, [AC_MSG_ERROR([sys/capability.h header not found])])

AC_SUBST([GLIB_MKENUMS], [`$PKG_CONFIG --variable glib_mkenums glib-2.0`])
AC_SUBST([GLIB_COMPILE_RESOURCES], [`$PKG_CONFIG --variable glib_compile_resources gio-2.0`])
AC_SUBST([GDBUS_CODEGEN], [`$PKG_CONFIG --variable gdbus_codegen gio-2.0`])

POLKIT_GOBJECT_REQUIRED=0.98

PKG_CHECK_MODULES(ARCHIVE, [libarchive >= 2.8.0])
PKG_CHECK_MODULES(BASE, [glib-2.0 >= $GLIB_REQS gio-2.0 gio-unix-2.0])
PKG_CHECK_MODULES(SOUP, [libsoup-2.4])
PKG_CHECK_MODULES(XML, [libxml-2.0 >= 2.4])
PKG_CHECK_MODULES(ZSTD, [libzstd >= 0.8.1], [have_zstd=yes], [have_zstd=no])
if test $have_zstd = yes; then
  AC_DEFINE(HAVE_ZSTD, 1, [Define if libzstd is available])
fi
PKG_CHECK_MODULES(DCONF, [dconf >= 0.26], [have_dconf=yes], [have_dconf=no])
if test $have_dconf = yes; then
  AC_DEFINE(HAVE_DCONF, 1, [Define if dconf is available])
fi
AC_ARG_WITH([systemd], AS_HELP_STRING([--with-systemd],
    [Build with systemd support [default=auto]]), [], [with_systemd=auto],)
if test "x$with_systemd" != "xno"; then
  PKG_CHECK_MODULES(SYSTEMD, [libsystemd], [have_libsystemd=yes], [have_libsystemd=no])
  if test $have_libsystemd = yes; then
    AC_DEFINE(HAVE_LIBSYSTEMD, 1, [Define if libsystemd is available])
  elif test "x$with_systemd" == "xyes"; then
    AC_MSG_ERROR([systemd was requested but it could not be found])
  fi
else
  have_libsystemd=no
fi
PKG_CHECK_MODULES([MALCONTENT], [malcontent-0 >= 0.4.0], [have_libmalcontent=yes], [have_libmalcontent=no])
AS_IF([test "$have_libmalcontent" = "yes"],[
  AC_DEFINE([HAVE_LIBMALCONTENT], [1], [Define if libmalcontent is available])
])
AM_CONDITIONAL([HAVE_LIBMALCONTENT],[test "$have_libmalcontent" = "yes"])

PKG_CHECK_MODULES(GLIB260, glib-2.0 >= 2.60,
                  [AC_DEFINE(GLIB_VERSION_MIN_REQUIRED, GLIB_VERSION_2_60, [Ignore massive GTimeVal deprecation warnings in 2.62])],
                  [true])

save_LIBS=$LIBS
LIBS=$ARCHIVE_LIBS
AC_CHECK_FUNCS(archive_read_support_filter_all)
LIBS=$save_LIBS

LIBGPGME_DEPENDENCY="1.1.8"
PKG_CHECK_MODULES(DEP_GPGME, gpgme-pthread >= $LIBGPGME_DEPENDENCY, have_gpgme=yes, [
    m4_ifdef([AM_PATH_GPGME_PTHREAD], [
       AM_PATH_GPGME_PTHREAD($LIBGPGME_DEPENDENCY, have_gpgme=yes, have_gpgme=no)
    ],[ have_gpgme=no ])
])
AS_IF([ test x$have_gpgme = xno ], [
   AC_MSG_ERROR([Need GPGME_PTHREAD version $LIBGPGME_DEPENDENCY or later])
])

AC_ARG_ENABLE(selinux-module,
        [AS_HELP_STRING([--enable-selinux-module],[Enable selinux module for system-helper])],
        enable_selinux_module=$enableval, enable_selinux_module=auto)
if test x$enable_selinux_module = xauto ; then
   AC_CHECK_FILE([/usr/share/selinux/devel/Makefile], [enable_selinux_module=yes],  [enable_selinux_module=no])
fi
if test x$enable_selinux_module = xyes ; then
   AC_CHECK_FILE([/usr/share/selinux/devel/Makefile], [],  [AC_MSG_ERROR([selinux-policy-devel needed to build selinux module])])
fi
AM_CONDITIONAL(BUILD_SELINUX_MODULE, test x$enable_selinux_module = xyes)

AC_ARG_ENABLE([system-helper],
              AC_HELP_STRING([--disable-system-helper],
                             [Disable system helper]),
              [],
              [enable_system_helper=yes])
if test "x$enable_system_helper" = "xyes"; then
  PKG_CHECK_MODULES(POLKIT, \
		    polkit-agent-1 >= $POLKIT_GOBJECT_REQUIRED)
  AC_DEFINE([USE_SYSTEM_HELPER], [1], [Define if using system-helper])
fi
AM_CONDITIONAL(BUILD_SYSTEM_HELPER, test x$enable_system_helper = xyes)

AC_ARG_ENABLE([auto-sideloading],
              AC_HELP_STRING([--enable-auto-sideloading],
                             [Enable systemd units which make Flatpak sideload from inserted USB drives]),
              [],
              [enable_auto_sideloading=no])
AM_CONDITIONAL(BUILD_AUTO_SIDELOADING, test x$enable_auto_sideloading = xyes)

PKG_CHECK_MODULES(FUSE, fuse >= 2.9.2)

AC_ARG_ENABLE([xauth],
              AC_HELP_STRING([--disable-xauth],
                             [Disable Xauth use]),
              [],
              [enable_xauth=yes])
if test "x$enable_xauth" = "xyes"; then
   PKG_CHECK_MODULES(XAUTH, [xau])
   AC_DEFINE([ENABLE_XAUTH], [1],
      [Define if using xauth])
fi

AC_ARG_ENABLE([gdm-env-file],
              [AC_HELP_STRING([--enable-gdm-env-file], [Install gdm env.d file (not needed if systemd generators work)])],
              install_gdm_env_file=$enableval, install_gdm_env_file=no)
AM_CONDITIONAL(INSTALL_GDM_ENV_FILE, test x$install_gdm_env_file = xyes)

AC_ARG_ENABLE([sandboxed-triggers],
              AC_HELP_STRING([--disable-sandboxed-triggers],
                             [Disable sandboxed triggers]),
              [],
              [enable_sandboxed_triggers=yes])
if test "x$enable_sandboxed_triggers" = "xno"; then
   AC_DEFINE([DISABLE_SANDBOXED_TRIGGERS], [1],
      [Define if sandboxed triggers are disabled])
fi

PKG_CHECK_MODULES(OSTREE, [ostree-1 >= $OSTREE_REQS])

PKG_CHECK_MODULES(JSON, [json-glib-1.0])

PKG_CHECK_MODULES(APPSTREAM_GLIB, [appstream-glib >= 0.5.10])

PKG_CHECK_MODULES(GDK_PIXBUF, [gdk-pixbuf-2.0])

AC_ARG_ENABLE([seccomp],
              AC_HELP_STRING([--disable-seccomp],
                             [Disable seccomp]),
              [],
              [enable_seccomp=yes])

if test "x$enable_seccomp" = "xyes"; then
   PKG_CHECK_MODULES(LIBSECCOMP, [libseccomp])
   AC_DEFINE([ENABLE_SECCOMP], [1],
      [Define if using seccomp])
fi

AC_ARG_WITH(priv-mode,
            AS_HELP_STRING([--with-priv-mode=setuid/none],
                           [How to set privilege-raising during install (only needed if userns not working)]),
            [],
            [with_priv_mode="none"])

AM_CONDITIONAL(PRIV_MODE_SETUID, test "x$with_priv_mode" = "xsetuid")

AC_ARG_ENABLE(sudo,
              AS_HELP_STRING([--enable-sudo],[Use sudo to set setuid flags on binaries during install (only needed if userns disabled)]),
              [SUDO_BIN="sudo"], [SUDO_BIN=""])
AC_SUBST([SUDO_BIN])

AC_ARG_WITH(system-install-dir,
           [AS_HELP_STRING([--with-system-install-dir=DIR],
                           [Location of system installation [LOCALSTATEDIR/lib/flatpak]])],
           [],
           [with_system_install_dir='$(localstatedir)/lib/flatpak'])
SYSTEM_INSTALL_DIR=$with_system_install_dir
AC_SUBST(SYSTEM_INSTALL_DIR)

AC_ARG_WITH(run-media-dir,
           [AS_HELP_STRING([--with-run-media-dir=DIR],
                           [Location of auto-mounted USB drives [/run/media]])],
           [],
           [with_run_media_dir='/run/media'])
RUN_MEDIA_DIR=$with_run_media_dir
AC_SUBST(RUN_MEDIA_DIR)

AC_ARG_WITH([sysusersdir],
            [AS_HELP_STRING([--with-sysusersdir=DIR],
                            [Directory for systemd sysusers.d configuration files (default=PREFIX/lib/sysusers.d)])],
    [],
    dnl This is deliberately not ${libdir}: systemd units always go in
    dnl .../lib, never .../lib64 or .../lib/x86_64-linux-gnu
    [with_sysusersdir='${prefix}/lib/sysusers.d'])
AC_SUBST([sysusersdir], [$with_sysusersdir])

AC_ARG_WITH(system-helper-user,
           [AS_HELP_STRING([--with-system-helper-user=USERNAME],
                           [Name of the system helper user])],
           with_system_helper_user="$withval", with_system_helper_user=flatpak)
SYSTEM_HELPER_USER=$with_system_helper_user
AC_SUBST(SYSTEM_HELPER_USER)

AC_ARG_ENABLE(documentation,
              AC_HELP_STRING([--enable-documentation], [Build documentation]),,
              enable_documentation=yes)
if test x$enable_documentation = xyes; then
   AC_PATH_PROG([XSLTPROC], [xsltproc])
   if test x$XSLTPROC = x; then
      AC_MSG_ERROR([xsltproc is required to build documentation])
   fi

  dnl check for DocBook DTD in the local catalog
  JH_CHECK_XML_CATALOG([-//OASIS//DTD DocBook XML V4.1.2//EN],
     [DocBook XML DTD V4.1.2], [have_docbook_dtd=yes], [have_docbook_dtd=no])
  if test "$have_docbook_dtd" != yes; then
    AC_MSG_ERROR([DocBook DTD is required for --enable-documentation])
  fi

  dnl check for DocBook XSL stylesheets in the local catalog
  JH_CHECK_XML_CATALOG([http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl],
     [DocBook XSL Stylesheets], [have_docbook_style=yes],[have_docbook_style=no])
  if test "$have_docbook_style" != yes; then
    AC_MSG_ERROR([DocBook XSL Stylesheets are required for --enable-documentation])
  fi
fi
AM_CONDITIONAL(BUILD_DOCUMENTATION, test x$enable_documentation = xyes)

GOBJECT_INTROSPECTION_CHECK([1.40.0])

# gtkdocize greps for ^GTK_DOC_CHECK, so we need to put it on its own line
m4_ifdef([GTK_DOC_CHECK], [
GTK_DOC_CHECK([1.20], [--flavour no-tmpl])

AC_ARG_ENABLE([gtk-doc-check],
              [AS_HELP_STRING([--enable-gtk-doc-check],
                              [Check completeness of documentation in 'make check'])],
    [],
    [enable_gtk_doc_check=no])
],[
enable_gtk_doc="disabled (no gtk-doc)"
enable_gtk_doc_check="disabled (no gtk-doc)"
AM_CONDITIONAL([ENABLE_GTK_DOC], [false])
])
AM_CONDITIONAL([ENABLE_GTK_DOC_CHECK], [test "x$enable_gtk_doc_check" = xyes])


AC_ARG_ENABLE(docbook-docs,
        [AS_HELP_STRING([--enable-docbook-docs],[build documentation (requires xmlto)])],
        enable_docbook_docs=$enableval, enable_docbook_docs=auto)
AC_PATH_PROG(XMLTO, xmlto, no)
AC_MSG_CHECKING([whether to build DocBook documentation])
if test x$XMLTO = xno ; then
        have_docbook=no
else
        have_docbook=yes
fi
if test x$enable_docbook_docs = xauto ; then
        if test x$have_docbook = xno ; then
                enable_docbook_docs=no
        else
                enable_docbook_docs=yes
        fi
fi
if test x$enable_docbook_docs = xyes; then
        if test x$have_docbook = xno; then
                AC_MSG_ERROR([Building DocBook docs explicitly required, but DocBook not found])
        fi
        AC_MSG_RESULT(yes)
else
        AC_MSG_RESULT(no)
fi
AM_CONDITIONAL(DOCBOOK_DOCS_ENABLED, test x$enable_docbook_docs = xyes)
AC_ARG_VAR([XMLTO],[Define/override the 'xmlto' location.])
AC_ARG_VAR([XMLTO_FLAGS],[Define/override 'xmlto' options, like '--skip-validation'.])


##################################################
# Visibility handling
##################################################

HIDDEN_VISIBILITY_CFLAGS=""
case "$host" in
  *)
    dnl on other compilers, check if we can do -fvisibility=hidden
    SAVED_CFLAGS="${CFLAGS}"
    CFLAGS="-fvisibility=hidden"
    AC_MSG_CHECKING([for -fvisibility=hidden compiler flag])
    AC_TRY_COMPILE([], [int main (void) { return 0; }],
                   AC_MSG_RESULT(yes)
                   enable_fvisibility_hidden=yes,
                   AC_MSG_RESULT(no)
                   enable_fvisibility_hidden=no)
    CFLAGS="${SAVED_CFLAGS}"

    AS_IF([test "${enable_fvisibility_hidden}" = "yes"], [
      AC_DEFINE([FLATPAK_EXTERN], [__attribute__((visibility("default"))) extern],
                [defines how to decorate public symbols while building])
      HIDDEN_VISIBILITY_CFLAGS="-fvisibility=hidden"
    ], [
      AC_DEFINE([FLATPAK_EXTERN], [extern],
                [defines how to decorate public symbols while building])
    ])
    ;;
esac
AC_SUBST(HIDDEN_VISIBILITY_CFLAGS)

##########################################
# Coverage testing
##########################################
AC_ARG_ENABLE(coverage,
  AS_HELP_STRING([--enable-coverage],
                 [enable coverage testing with gcov]),
  [use_lcov=$enableval], [use_lcov=no])

if test x$use_lcov = xyes; then
  AC_PATH_PROG(LCOV, lcov)
  AC_PATH_PROG(GENHTML, genhtml)

  # remove all optimization from CFLAGS
  changequote({,})
  CFLAGS=`echo "$CFLAGS" | $SED -e 's/-O[0-9]*//g'`
  changequote([,])

  CFLAGS="$CFLAGS -O0 -fprofile-arcs -ftest-coverage"
  LDFLAGS="$LDFLAGS -lgcov"
fi

GLIB_TESTS

FLATPAK_MAJOR_VERSION=flatpak_major_version
FLATPAK_MINOR_VERSION=flatpak_minor_version
FLATPAK_MICRO_VERSION=flatpak_micro_version
FLATPAK_EXTRA_VERSION=flatpak_extra_version
FLATPAK_INTERFACE_AGE=flatpak_interface_age
FLATPAK_VERSION=flatpak_version
AC_SUBST(FLATPAK_MAJOR_VERSION)
AC_SUBST(FLATPAK_MINOR_VERSION)
AC_SUBST(FLATPAK_MICRO_VERSION)
AC_SUBST(FLATPAK_INTERFACE_AGE)
AC_SUBST(FLATPAK_VERSION)
AC_DEFINE_UNQUOTED([PACKAGE_MAJOR_VERSION], $FLATPAK_MAJOR_VERSION, [Major version])
AC_DEFINE_UNQUOTED([PACKAGE_MINOR_VERSION], $FLATPAK_MINOR_VERSION, [Minor version])
AC_DEFINE_UNQUOTED([PACKAGE_MICRO_VERSION], $FLATPAK_MICRO_VERSION, [Micro version])
AC_DEFINE_UNQUOTED([PACKAGE_EXTRA_VERSION], $FLATPAK_EXTRA_VERSION, [Extra version])

# libtool versioning
#LT_RELEASE=$FLATPAK_MAJOR_VERSION.$FLATPAK_MINOR_VERSION
#LT_CURRENT=`expr $FLATPAK_MICRO_VERSION - $FLATPAK_INTERFACE_AGE`
#LT_REVISION=$FLATPAK_INTERFACE_AGE
#LT_AGE=`expr $FLATPAK_BINARY_AGE - $FLATPAK_INTERFACE_AGE`
#LT_CURRENT_MINUS_AGE=`expr $LT_CURRENT - $LT_AGE`

m4_define([lt_current], [m4_eval(10000 * flatpak_major_version +  100 * flatpak_minor_version + flatpak_micro_version - flatpak_interface_age)])
m4_define([lt_revision], [flatpak_interface_age])
m4_define([lt_age], [m4_eval(flatpak_binary_age - flatpak_interface_age)])
LT_VERSION_INFO="lt_current:lt_revision:lt_age"
LT_CURRENT_MINUS_AGE=m4_eval(lt_current - lt_age)
AC_SUBST(LT_VERSION_INFO)
AC_SUBST(LT_CURRENT_MINUS_AGE)

AC_CONFIG_FILES([
Makefile
doc/Makefile
doc/reference/Makefile
flatpak.pc
common/flatpak-version-macros.h
doc/reference/version.xml
doc/flatpak-docs.xml
po/Makefile.in
])
AC_OUTPUT

if test x$BWRAP = xfalse ; then
  build_bwrap="yes"
else
  build_bwrap="no"
fi

if test x$DBUS_PROXY = xfalse ; then
  build_dbus_proxy="yes"
else
  build_dbus_proxy="no"
fi

echo ""
echo "          Flatpak $FLATPAK_VERSION"
echo "          ============="
echo ""
echo "          Build system helper:    $enable_system_helper"
echo "          Build selinux module:   $enable_selinux_module"
echo "          Build bubblewrap:       $build_bwrap"
echo "          Build dbus-proxy:       $build_dbus_proxy"
echo "          Use sandboxed triggers: $enable_sandboxed_triggers"
echo "          Use seccomp:            $enable_seccomp"
echo "          Privileged group:       $PRIVILEGED_GROUP"
echo "          Privilege mode:         $with_priv_mode"
echo "          Use dconf:              $have_dconf"
echo "          Use libsystemd:         $have_libsystemd"
echo "          Use libmalcontent:      $have_libmalcontent"
echo "          Use libzstd:            $have_zstd"
echo "          Use auto sideloading:   $enable_auto_sideloading"
echo ""
