AC_DEFUN([LIBGLNX_CONFIGURE],
[
AC_CHECK_DECLS([
        renameat2,
        ],
        [], [], [[
#include <sys/types.h>
#include <unistd.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sched.h>
#include <linux/loop.h>
#include <linux/random.h>
]])
])
