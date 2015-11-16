AC_PREREQ([2.63])

AC_INIT([xdg-app],[0.4.5])

AC_USE_SYSTEM_EXTENSIONS

AC_PROG_CC
AM_PROG_CC_C_O
AC_DISABLE_STATIC

LT_PREREQ([2.2.6])
LT_INIT([disable-static])

AC_CONFIG_SRCDIR([lib/xdg-app-helper.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([1.11 no-define no-dist-gzip dist-xz tar-ustar foreign subdir-objects])
AC_PROG_SED

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


AC_ARG_WITH(system_fonts_dir,
		AS_HELP_STRING([--with-system-fonts-dir=PATH],[Directory where system fonts are, [default=/usr/share/fonts]]),
            with_system_fonts_dir="$withval", with_system_fonts_dir=/usr/share/fonts)
SYSTEM_FONTS_DIR=$with_system_fonts_dir
AC_SUBST(SYSTEM_FONTS_DIR)

AC_CHECK_FUNCS(fdwalk)

AC_CHECK_HEADER([sys/xattr.h], [], AC_MSG_ERROR([You must have sys/xattr.h from glibc]))
AC_CHECK_HEADER([sys/capability.h], have_caps=yes, AC_MSG_ERROR([sys/capability.h header not found]))

AC_SUBST([GLIB_COMPILE_RESOURCES], [`$PKG_CONFIG --variable glib_compile_resources gio-2.0`])
AC_SUBST([GDBUS_CODEGEN], [`$PKG_CONFIG --variable gdbus_codegen gio-2.0`])

PKG_CHECK_MODULES(BASE, [glib-2.0 gio-2.0 gio-unix-2.0])
AC_SUBST(BASE_CFLAGS)
AC_SUBST(BASE_LIBS)
PKG_CHECK_MODULES(SOUP, [libsoup-2.4])
AC_SUBST(SOUP_CFLAGS)
AC_SUBST(SOUP_LIBS)
PKG_CHECK_MODULES(XAUTH, [xau])
AC_SUBST(XAUTH_CFLAGS)
AC_SUBST(XAUTH_LIBS)

PKG_CHECK_MODULES(OSTREE, [libgsystem >= 2015.1 ostree-1 >= 2015.3])
AC_SUBST(OSTREE_CFLAGS)
AC_SUBST(OSTREE_LIBS)
save_LIBS=$LIBS
LIBS=$OSTREE_LIBS
AC_CHECK_FUNCS(ostree_repo_remote_gpg_import)
LIBS=$save_LIBS

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


AC_ARG_ENABLE([userns],
              AC_HELP_STRING([--disable-userns],
                             [Disable User namespaces (requires setuid/setcaps)]),
              [],
              [enable_userns=yes])

AM_CONDITIONAL(DISABLE_USERNS, test "x$enable_userns" = "xno")
if test "x$enable_userns" = "xno"; then
  AC_DEFINE([DISABLE_USERNS], [1],
      [Define if not using user namespaces])
fi

AC_ARG_WITH(priv-mode,
            AS_HELP_STRING([--with-priv-mode=setuid/caps/none],
                           [How to set privilege-raising during install (only needed if userns disabled)]),
            [],
            [with_priv_mode="setuid"])

AM_CONDITIONAL(PRIV_MODE_FILECAPS, test "x$with_priv_mode" = "xcaps")
AM_CONDITIONAL(PRIV_MODE_SETUID, test "x$with_priv_mode" = "xsetuid")
AM_CONDITIONAL(PRIV_MODE_FILECAPS, test "x$with_priv_mode" = "xcaps")

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

AC_CONFIG_FILES([
Makefile
doc/Makefile
xdg-app.pc
])
AC_OUTPUT
