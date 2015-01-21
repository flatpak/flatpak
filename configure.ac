AC_PREREQ([2.63])

AC_INIT([xdg-app],[0.0.1])

AC_PROG_CC
AM_PROG_CC_C_O
AC_DISABLE_STATIC

LT_PREREQ([2.2.6])
LT_INIT([disable-static])

AC_CONFIG_SRCDIR([xdg-app-helper.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([1.11 no-define no-dist-gzip dist-bzip2 tar-ustar foreign])

# Enable silent rules is available
AM_SILENT_RULES([yes])
AM_MAINTAINER_MODE([enable])

if test "x$GCC" = "xyes"; then
  case " $CFLAGS " in
  *[[\ \	]]-Wall[[\ \	]]*) ;;
  *) CFLAGS="$CFLAGS -Wall" ;;
  esac
fi

PKG_CHECK_MODULES(SOUP, [libsoup-2.4])
AC_SUBST(SOUP_CFLAGS)
AC_SUBST(SOUP_LIBS)
PKG_CHECK_MODULES(OSTREE, [glib-2.0 libgsystem >= 2015.1 gio-2.0 ostree-1 >= 2015.1])
AC_SUBST(OSTREE_CFLAGS)
AC_SUBST(OSTREE_LIBS)

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
