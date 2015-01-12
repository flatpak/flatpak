#include "config.h"

#include "xdg-app-utils.h"
#include "xdg-app-dir.h"

#include <glib.h>
#include "libgsystem.h"

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>

const char *
xdg_app_get_arch (void)
{
  static struct utsname buf;
  static char *arch = NULL;

  if (arch == NULL)
    {
      if (uname (&buf))
        arch = "unknown";
      else
        arch = buf.machine;
    }

  return arch;
}

char *
xdg_app_build_untyped_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename (runtime, arch, branch, NULL);
}

char *
xdg_app_build_runtime_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename ("runtime", runtime, arch, branch, NULL);
}

char *
xdg_app_build_app_ref (const char *app,
                       const char *branch,
                       const char *arch)
{
  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename ("app", app, arch, branch, NULL);
}

GFile *
xdg_app_find_deploy_dir_for_ref (const char *ref,
                                 GCancellable *cancellable,
                                 GError **error)
{
  gs_unref_object XdgAppDir *user_dir = NULL;
  gs_unref_object XdgAppDir *system_dir = NULL;
  GFile *deploy = NULL;

  user_dir = xdg_app_dir_get_user ();
  system_dir = xdg_app_dir_get_system ();

  deploy = xdg_app_dir_get_if_deployed (user_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    deploy = xdg_app_dir_get_if_deployed (system_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s not installed", ref);
      return NULL;
    }

  return deploy;

}

static gboolean
overlay_symlink_tree_dir (int            source_parent_fd,
                          const char    *source_name,
                          const char    *source_symlink_prefix,
                          int            destination_parent_fd,
                          const char    *destination_name,
                          GCancellable  *cancellable,
                          GError       **error)
{
  gboolean ret = FALSE;
  int res;
  gs_dirfd_iterator_cleanup GSDirFdIterator source_iter;
  gs_fd_close int destination_dfd = -1;
  struct dirent *dent;

  if (!gs_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    goto out;

  do
    res = mkdirat (destination_parent_fd, destination_name, 0777);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno != EEXIST)
        {
          gs_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (!gs_file_open_dir_fd_at (destination_parent_fd, destination_name,
                               &destination_dfd,
                               cancellable, error))
    goto out;

  while (TRUE)
    {
      gboolean is_dir = FALSE;

      if (!gs_dirfd_iterator_next_dent (&source_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (dent->d_type == DT_DIR)
        is_dir = TRUE;
      else if (dent->d_type == DT_UNKNOWN)
        {
          struct stat stbuf;
          if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
            {
              int errsv = errno;
              if (errsv == ENOENT)
                continue;
              else
                {
                  gs_set_error_from_errno (error, errsv);
                  goto out;
                }
            }
          is_dir = S_ISDIR (stbuf.st_mode);
        }

      if (is_dir)
        {
          gs_free gchar *target = g_build_filename ("..", source_symlink_prefix, dent->d_name, NULL);
          if (!overlay_symlink_tree_dir (source_iter.fd, dent->d_name, target, destination_dfd, dent->d_name,
                                         cancellable, error))
            goto out;
        }
      else
        {
          gs_free gchar *target = g_build_filename (source_symlink_prefix, dent->d_name, NULL);

          if (unlinkat (destination_dfd, dent->d_name, 0) != 0 && errno != ENOENT)
            {
              gs_set_error_from_errno (error, errno);
              goto out;
            }

          if (symlinkat (target, destination_dfd, dent->d_name) != 0)
            {
              gs_set_error_from_errno (error, errno);
              goto out;
            }
        }
    }

  ret = TRUE;
 out:

  return ret;
}

gboolean
xdg_app_overlay_symlink_tree (GFile    *source,
                              GFile    *destination,
                              const char *symlink_prefix,
                              GCancellable  *cancellable,
                              GError       **error)
{
  gboolean ret = FALSE;

  if (!gs_file_ensure_directory (destination, TRUE, cancellable, error))
    goto out;

  /* The fds are closed by this call */
  if (!overlay_symlink_tree_dir (AT_FDCWD, gs_file_get_path_cached (source),
                                 symlink_prefix,
                                 AT_FDCWD, gs_file_get_path_cached (destination),
                                 cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}

static gboolean
remove_dangling_symlinks (int            parent_fd,
                          const char    *name,
                          GCancellable  *cancellable,
                          GError       **error)
{
  gboolean ret = FALSE;
  struct dirent *dent;
  GSDirFdIterator iter;

  if (!gs_dirfd_iterator_init_at (parent_fd, name, FALSE, &iter, error))
    goto out;

  while (TRUE)
    {
      gboolean is_dir = FALSE;
      gboolean is_link = FALSE;

      if (!gs_dirfd_iterator_next_dent (&iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (dent->d_type == DT_DIR)
        is_dir = TRUE;
      else if (dent->d_type == DT_LNK)
        is_link = TRUE;
      else if (dent->d_type == DT_UNKNOWN)
        {
          struct stat stbuf;
          if (fstatat (iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
            {
              int errsv = errno;
              if (errsv == ENOENT)
                continue;
              else
                {
                  gs_set_error_from_errno (error, errsv);
                  goto out;
                }
            }
          is_dir = S_ISDIR (stbuf.st_mode);
          is_link = S_ISLNK (stbuf.st_mode);
        }

      if (is_dir)
        {
          if (!remove_dangling_symlinks (iter.fd, dent->d_name, cancellable, error))
            goto out;
        }
      else if (is_link)
        {
          struct stat stbuf;
          if (fstatat (iter.fd, dent->d_name, &stbuf, 0) != 0 && errno == ENOENT)
            {
              if (unlinkat (iter.fd, dent->d_name, 0) != 0)
                {
                  gs_set_error_from_errno (error, errno);
                  goto out;
                }
            }
        }
    }

  ret = TRUE;
 out:

  return ret;
}

gboolean
xdg_app_remove_dangling_symlinks (GFile    *dir,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  gboolean ret = FALSE;

  /* The fd is closed by this call */
  if (!remove_dangling_symlinks (AT_FDCWD, gs_file_get_path_cached (dir),
                                 cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}
