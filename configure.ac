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

m4_define([flatpak_major_version], [0])
m4_define([flatpak_minor_version], [5])
m4_define([flatpak_micro_version], [2])
m4_define([flatpak_interface_age], [2])
m4_define([flatpak_binary_age],
          [m4_eval(10000 * flatpak_major_version + 100 * flatpak_minor_version + flatpak_micro_version)])
m4_define([flatpak_version],
          [flatpak_major_version.flatpak_minor_version.flatpak_micro_version])

AC_INIT([flatpak],[flatpak_version])

AC_USE_SYSTEM_EXTENSIONS

IT_PROG_INTLTOOL([0.35.0])

AC_PROG_CC
AM_PROG_CC_C_O
AC_DISABLE_STATIC

LT_PREREQ([2.2.6])
LT_INIT([disable-static])

AC_CONFIG_SRCDIR([common/flatpak-dir.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([1.11 no-define no-dist-gzip dist-xz tar-ustar foreign subdir-objects])
AC_PROG_SED

GETTEXT_PACKAGE=Flatpak
AC_SUBST([GETTEXT_PACKAGE])
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE],["$GETTEXT_PACKAGE"],[gettext domain])

AM_GLIB_GNU_GETTEXT

# Enable silent rules is available
AM_SILENT_RULES([yes])
AM_MAINTAINER_MODE([enable])

if test "x$GCC" = "xyes"; then
  case " $CFLAGS " in
  *[[\ \	]]-Wall[[\ \	]]*) ;;
  *) CFLAGS="$CFLAGS -Wall" ;;
  esac
fi

AX_VALGRIND_CHECK

PKG_PROG_PKG_CONFIG([0.24])

AC_ARG_WITH(dbus_service_dir,
		AS_HELP_STRING([--with-dbus-service-dir=PATH],[choose directory for dbus service files, [default=PREFIX/share/dbus-1/services]]),
            with_dbus_service_dir="$withval", with_dbus_service_dir=$datadir/dbus-1/services)
DBUS_SERVICE_DIR=$with_dbus_service_dir
AC_SUBST(DBUS_SERVICE_DIR)

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

AC_ARG_WITH(system_fonts_dir,
		AS_HELP_STRING([--with-system-fonts-dir=PATH],[Directory where system fonts are, [default=/usr/share/fonts]]),
            with_system_fonts_dir="$withval", with_system_fonts_dir=/usr/share/fonts)
SYSTEM_FONTS_DIR=$with_system_fonts_dir
AC_SUBST(SYSTEM_FONTS_DIR)

AC_CHECK_FUNCS(fdwalk)

AC_CHECK_HEADER([sys/xattr.h], [], AC_MSG_ERROR([You must have sys/xattr.h from glibc]))
AC_CHECK_HEADER([sys/capability.h], have_caps=yes, AC_MSG_ERROR([sys/capability.h header not found]))

AC_SUBST([GLIB_MKENUMS], [`$PKG_CONFIG --variable glib_mkenums glib-2.0`])
AC_SUBST([GLIB_COMPILE_RESOURCES], [`$PKG_CONFIG --variable glib_compile_resources gio-2.0`])
AC_SUBST([GDBUS_CODEGEN], [`$PKG_CONFIG --variable gdbus_codegen gio-2.0`])

POLKIT_GOBJECT_REQUIRED=0.98

PKG_CHECK_MODULES(BASE, [glib-2.0 gio-2.0 gio-unix-2.0])
AC_SUBST(BASE_CFLAGS)
AC_SUBST(BASE_LIBS)
PKG_CHECK_MODULES(SOUP, [libsoup-2.4])
AC_SUBST(SOUP_CFLAGS)
AC_SUBST(SOUP_LIBS)

PKG_CHECK_MODULES(POLKIT, \
		  polkit-gobject-1 >= $POLKIT_GOBJECT_REQUIRED)

AC_ARG_ENABLE([xauth],
              AC_HELP_STRING([--disable-xauth],
                             [Disable Xauth use]),
              [],
              [enable_xauth=yes])
if test "x$enable_xauth" = "xyes"; then
   PKG_CHECK_MODULES(XAUTH, [xau])
   AC_SUBST(XAUTH_CFLAGS)
   AC_SUBST(XAUTH_LIBS)
   AC_DEFINE([ENABLE_XAUTH], [1],
      [Define if using xauth])
fi

AC_ARG_ENABLE([sandboxed-triggers],
              AC_HELP_STRING([--disable-sandboxed-triggers],
                             [Disable sandboxed triggers]),
              [],
              [enable_sandboxed_triggers=yes])
if test "x$enable_sandboxed_triggers" = "xno"; then
   AC_DEFINE([DISABLE_SANDBOXED_TRIGGERS], [1],
      [Define if sandboxed triggers are disabled])
fi

PKG_CHECK_MODULES(OSTREE, [libgsystem >= 2015.1 ostree-1 >= 2016.5])
AC_SUBST(OSTREE_CFLAGS)
AC_SUBST(OSTREE_LIBS)
save_LIBS=$LIBS
save_CFLAGS=$CFLAGS
LIBS=$OSTREE_LIBS
CFLAGS=$OSTREE_CFLAGS

AC_CHECK_MEMBER([OstreeRepoExportArchiveOptions.path_prefix],
                [AC_DEFINE([HAVE_OSTREE_EXPORT_PATH_PREFIX], 1,
                          [Have OstreeRepoExportArchiveOptions.path_prefix])],
               , [[#include <ostree.h>]])

LIBS=$save_LIBS
CFLAGS=$save_CFLAGS


PKG_CHECK_MODULES(FUSE, [fuse])
AC_SUBST(FUSE_CFLAGS)
AC_SUBST(FUSE_LIBS)

PKG_CHECK_MODULES(JSON, [json-glib-1.0])
AC_SUBST(JSON_CFLAGS)
AC_SUBST(JSON_LIBS)

AC_ARG_ENABLE([seccomp],
              AC_HELP_STRING([--disable-seccomp],
                             [Disable seccomp]),
              [],
              [enable_seccomp=yes])

if test "x$enable_seccomp" = "xyes"; then
   PKG_CHECK_MODULES(LIBSECCOMP, [libseccomp])
   AC_SUBST(LIBSECCOMP_CFLAGS)
   AC_SUBST(LIBSECCOMP_LIBS)
   AC_DEFINE([ENABLE_SECCOMP], [1],
      [Define if using seccomp])
fi

AC_ARG_WITH(priv-mode,
            AS_HELP_STRING([--with-priv-mode=setuid/caps/none],
                           [How to set privilege-raising during install (only needed if userns not working)]),
            [],
            [with_priv_mode="none"])

AM_CONDITIONAL(PRIV_MODE_FILECAPS, test "x$with_priv_mode" = "xcaps")
AM_CONDITIONAL(PRIV_MODE_SETUID, test "x$with_priv_mode" = "xsetuid")

AC_ARG_ENABLE(sudo,
              AS_HELP_STRING([--enable-sudo],[Use sudo to set setuid flags on binaries during install (only needed if userns disabled)]),
              [SUDO_BIN="sudo"], [SUDO_BIN=""])
AC_SUBST([SUDO_BIN])

LIBARCHIVE_DEPENDENCY="libarchive >= 2.8.0"

AC_ARG_WITH(libarchive,
	    AS_HELP_STRING([--without-libarchive], [Do not use libarchive]),
	    :, with_libarchive=maybe)

AS_IF([ test x$with_libarchive != xno ], [
    AC_MSG_CHECKING([for $LIBARCHIVE_DEPENDENCY])
    PKG_CHECK_EXISTS($LIBARCHIVE_DEPENDENCY, have_libarchive=yes, have_libarchive=no)
    AC_MSG_RESULT([$have_libarchive])
    AS_IF([ test x$have_libarchive = xno && test x$with_libarchive != xmaybe ], [
       AC_MSG_ERROR([libarchive is enabled but could not be found])
    ])
    AS_IF([ test x$have_libarchive = xyes], [
        AC_DEFINE([HAVE_LIBARCHIVE], 1, [Define if we have libarchive.pc])
	PKG_CHECK_MODULES(LIBARCHIVE, $LIBARCHIVE_DEPENDENCY)
        save_LIBS=$LIBS
        LIBS=$LIBARCHIVE_LIBS
        AC_CHECK_FUNCS(archive_read_support_filter_all)
        LIBS=$save_LIBS
	with_libarchive=yes
    ], [
	with_libarchive=no
    ])
], [ with_libarchive=no ])
AM_CONDITIONAL(USE_LIBARCHIVE, test $with_libarchive != no)

dnl ************************
dnl *** check for libelf ***
dnl ************************
PKG_CHECK_MODULES([LIBELF], [libelf >= 0.8.12], [have_libelf=yes], [have_libelf=maybe])
AS_IF([ test $have_libelf = maybe ], [
  save_LIBS=$LIBS
  AC_CHECK_LIB([elf], [elf_begin], [:], [have_libelf=no])
  AC_CHECK_LIB([elf], [elf_getshdrstrndx], [:], [have_libelf=no])
  AC_CHECK_LIB([elf], [elf_getshdrnum], [:], [have_libelf=no])
  AC_CHECK_HEADER([libelf.h], [:], [have_libelf=no])
  LIBS=$save_LIBS

  if test $have_libelf != no; then
    LIBELF_LIBS=-lelf
    have_libelf=yes
  fi
])

if test x$have_libelf != xyes; then
  AC_MSG_ERROR([libelf not found])
fi

dnl This only checks for the header, not the library.
AC_ARG_WITH([dwarf-header],
            [AS_HELP_STRING([--with-dwarf-header],
                            [path containing dwarf.h])],
            [AS_IF([test "x$with_dwarf_header" = "xno"],
                   [AC_MSG_WARN([either a path containing dwarf.h must be
provided, or it will be searched for in includedir])])],
            [with_dwarf_header=yes])

AS_IF([test "x$with_dwarf_header" = "xyes"],
      [CPPFLAGS="$CPPFLAGS -I$(eval echo \"$includedir\")/libdwarf"
       AC_CHECK_HEADER([dwarf.h])
       AS_IF([test "x$ac_cv_header_dwarf_h" != "xyes"],
             [AC_MSG_ERROR([dwarf.h is required but was not found; locate it using --with-dwarf-header=/path/containing/header])])],
      [CPPFLAGS="$CPPFLAGS -I$with_dwarf_header"
       AC_CHECK_HEADER([dwarf.h])
       AS_IF([test "x$ac_cv_header_dwarf_h" != "xyes"],
             [AC_MSG_ERROR([dwarf.h is required but was not found])])])

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

GLIB_TESTS

FLATPAK_MAJOR_VERSION=flatpak_major_version
FLATPAK_MINOR_VERSION=flatpak_minor_version
FLATPAK_MICRO_VERSION=flatpak_micro_version
FLATPAK_INTERFACE_AGE=flatpak_interface_age
FLATPAK_VERSION=flatpak_version
AC_SUBST(FLATPAK_MAJOR_VERSION)
AC_SUBST(FLATPAK_MINOR_VERSION)
AC_SUBST(FLATPAK_MICRO_VERSION)
AC_SUBST(FLATPAK_INTERFACE_AGE)
AC_SUBST(FLATPAK_VERSION)

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
lib/flatpak-version-macros.h
doc/reference/version.xml
po/Makefile.in
])
AC_OUTPUT
