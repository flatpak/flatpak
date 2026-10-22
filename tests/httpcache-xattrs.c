/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/xattr.h>

ssize_t __real_lgetxattr (const char *path, const char *name, void *value, size_t size);
ssize_t __wrap_lgetxattr (const char *path, const char *name, void *value, size_t size);
int __real_fsetxattr (int fd, const char *name, const void *value, size_t size, int flags);
int __wrap_fsetxattr (int fd, const char *name, const void *value, size_t size, int flags);

/* These wrappers make httpcache return ENOTSUP for xattrs when
 * FLATPAK_TEST_NO_XATTRS is set. Linker wrapping leaves ASan's load order intact.
 * Keep ENOENT for missing files so the first download tests the write fallback.
 */
ssize_t
__wrap_lgetxattr (const char *path, const char *name, void *value, size_t size)
{
  struct stat st;

  if (getenv ("FLATPAK_TEST_NO_XATTRS") == NULL)
    return __real_lgetxattr (path, name, value, size);

  if (lstat (path, &st) < 0)
    return -1;

  errno = ENOTSUP;
  return -1;
}

int
__wrap_fsetxattr (int fd, const char *name, const void *value, size_t size, int flags)
{
  if (getenv ("FLATPAK_TEST_NO_XATTRS") == NULL)
    return __real_fsetxattr (fd, name, value, size, flags);

  errno = ENOTSUP;
  return -1;
}
