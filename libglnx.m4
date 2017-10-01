AC_DEFUN([LIBGLNX_CONFIGURE],
[
AC_CHECK_DECLS([renameat2, memfd_create],
        [], [], [[
#include <sys/types.h>
#include <unistd.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sched.h>
#include <linux/loop.h>
#include <linux/random.h>
]])

AC_ARG_ENABLE(otmpfile,
              [AS_HELP_STRING([--disable-otmpfile],
                              [Disable use of O_TMPFILE [default=no]])],,
              [enable_otmpfile=yes])
AS_IF([test $enable_otmpfile = yes], [], [
  AC_DEFINE([DISABLE_OTMPFILE], 1, [Define if we should avoid using O_TMPFILE])])

AC_ARG_ENABLE(wrpseudo-compat,
              [AS_HELP_STRING([--enable-wrpseudo-compat],
                              [Disable use syscall() and filesystem calls to for compatibility with wrpseudo [default=no]])],,
              [enable_wrpseudo_compat=no])
AS_IF([test $enable_wrpseudo_compat = no], [], [
  AC_DEFINE([ENABLE_WRPSEUDO_COMPAT], 1, [Define if we should be compatible with wrpseudo])])

dnl end LIBGLNX_CONFIGURE
])
