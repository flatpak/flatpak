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


static void
set_error_from_errno (GError **error,
                      gint     saved_errno)
{
  g_set_error_literal (error,
                       G_IO_ERROR,
                       g_io_error_from_errno (saved_errno),
                       g_strerror (saved_errno));
  errno = saved_errno;
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
  int source_dfd = -1;
  int destination_dfd = -1;
  DIR *srcd = NULL;
  struct dirent *dent;

  if (source_name == NULL)
    source_dfd = source_parent_fd; /* We take ownership of the passed fd and close it */
  else
    {
      if (!gs_file_open_dir_fd_at (source_parent_fd, source_name,
                                   &source_dfd,
                                   cancellable, error))
        goto out;
    }

  if (destination_name == NULL)
    destination_dfd = destination_parent_fd; /* We take ownership of the passed fd and close it */
  else
    {
      do
        res = mkdirat (destination_parent_fd, destination_name, 0777);
      while (G_UNLIKELY (res == -1 && errno == EINTR));
      if (res == -1)
        {
          if (errno != EEXIST)
            {
              set_error_from_errno (error, errno);
              goto out;
            }
        }

      if (!gs_file_open_dir_fd_at (destination_parent_fd, destination_name,
                                   &destination_dfd,
                                   cancellable, error))
        goto out;
    }

  srcd = fdopendir (source_dfd);
  if (!srcd)
    {
      set_error_from_errno (error, errno);
      goto out;
    }

  while ((dent = readdir (srcd)) != NULL)
    {
      const char *name = dent->d_name;
      struct stat child_stbuf;

      if (strcmp (name, ".") == 0 ||
          strcmp (name, "..") == 0)
        continue;

      if (fstatat (source_dfd, name, &child_stbuf,
                   AT_SYMLINK_NOFOLLOW) != 0)
        {
          set_error_from_errno (error, errno);
          goto out;
        }

      if (S_ISDIR (child_stbuf.st_mode))
        {
          gs_free gchar *target = g_build_filename ("..", source_symlink_prefix, name, NULL);
          if (!overlay_symlink_tree_dir (source_dfd, name, target, destination_dfd, name,
                                         cancellable, error))
            goto out;
        }
      else
        {
          gs_free gchar *target = g_build_filename (source_symlink_prefix, name, NULL);

          if (unlinkat (destination_dfd, name, 0) != 0 && errno != ENOENT)
            {
              set_error_from_errno (error, errno);
              goto out;
            }

          if (symlinkat (target, destination_dfd, name) != 0)
            {
              set_error_from_errno (error, errno);
              goto out;
            }
        }
    }

  ret = TRUE;
 out:
  if (srcd != NULL)
    closedir (srcd); /* This closes source_dfd */
  else if (source_dfd != -1)
    close (source_dfd);
  if (destination_dfd != -1)
    close (destination_dfd);

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
  int source_dfd = -1;
  int destination_dfd = -1;

  if (!gs_file_open_dir_fd (source, &source_dfd, cancellable, error))
    goto out;

  if (!gs_file_ensure_directory (destination, TRUE, cancellable, error) ||
      !gs_file_open_dir_fd (destination, &destination_dfd, cancellable, error))
    {
      close (source_dfd);
      goto out;
    }

  /* The fds are closed by this call */
  if (!overlay_symlink_tree_dir (source_dfd, NULL,
                                 symlink_prefix,
                                 destination_dfd, NULL,
                                 cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}
