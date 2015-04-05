AC_PREREQ([2.63])

AC_INIT([xdg-app],[0.1])

AC_USE_SYSTEM_EXTENSIONS

AC_PROG_CC
AM_PROG_CC_C_O
AC_DISABLE_STATIC

LT_PREREQ([2.2.6])
LT_INIT([disable-static])

AC_CONFIG_SRCDIR([xdg-app-helper.c])
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
PKG_CHECK_MODULES(OSTREE, [libgsystem >= 2015.1 ostree-1 >= 2015.3])
AC_SUBST(OSTREE_CFLAGS)
AC_SUBST(OSTREE_LIBS)

AC_ARG_WITH(priv-mode,
            AS_HELP_STRING([--with-priv-mode=setuid/caps/none],
                           [How to gain privileges]),
            [],
	    [with_priv_mode="setuid"])

AM_CONDITIONAL(PRIV_MODE_SETUID, test "x$with_priv_mode" = "xsetuid")
AM_CONDITIONAL(PRIV_MODE_FILECAPS, test "x$with_priv_mode" = "xcaps")

AC_ARG_ENABLE(sudo,
              AS_HELP_STRING([--enable-sudo],[Use sudo to set setuid flags on binaries during install]),
              [SUDO_BIN="sudo"], [SUDO_BIN=""])
AC_SUBST([SUDO_BIN])

AC_ARG_ENABLE(documentation,
              AC_HELP_STRING([--enable-documentation], [Build documentation]),,
              enable_documentation=yes)
if test x$enable_documentation = xyes; then
   AC_PATH_PROG([XSLTPROC], [xsltproc])
   if test x$XSLTPROC = x; then
      AC_MSG_ERROR([xsltproc is required to build documentation])
   fi
fi
AM_CONDITIONAL(BUILD_DOCUMENTATION, test x$enable_documentation = xyes)

AC_CONFIG_FILES([
Makefile
doc/Makefile
])
AC_OUTPUT
