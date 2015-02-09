#include "config.h"

#include "xdg-app-utils.h"
#include "xdg-app-dir.h"

#include <glib.h>
#include "libgsystem.h"
#include <libsoup/soup.h>

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

char **
xdg_app_list_deployed_refs (const char *type,
			    const char *name_prefix,
			    const char *branch,
			    const char *arch,
			    GCancellable *cancellable,
			    GError **error)
{
  gchar **ret = NULL;
  gs_unref_ptrarray GPtrArray *names = NULL;
  gs_unref_hashtable GHashTable *hash = NULL;
  gs_unref_object XdgAppDir *user_dir = NULL;
  gs_unref_object XdgAppDir *system_dir = NULL;
  const char *key;
  GHashTableIter iter;

  hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  user_dir = xdg_app_dir_get_user ();
  system_dir = xdg_app_dir_get_system ();

  if (!xdg_app_dir_collect_deployed_refs (user_dir, type, name_prefix,
					  branch, arch, hash, cancellable,
					  error))
    goto out;

  if (!xdg_app_dir_collect_deployed_refs (system_dir, type, name_prefix,
					  branch, arch, hash, cancellable,
					  error))
    goto out;

  names = g_ptr_array_new ();
  g_hash_table_iter_init (&iter, hash);
  while (g_hash_table_iter_next (&iter, (gpointer *)&key, NULL))
    g_ptr_array_add (names, g_strdup (key));

  g_ptr_array_sort (names, (GCompareFunc)g_strcmp0);
  g_ptr_array_add (names, NULL);

  ret = (char **)g_ptr_array_free (names, FALSE);
  names = NULL;

 out:
  return ret;
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

static gboolean
load_contents (const char *uri, GBytes **contents, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gs_free char *scheme = NULL;

  scheme = g_uri_parse_scheme (uri);
  if (strcmp (scheme, "file") == 0)
    {
      char *buffer;
      gsize length;
      gs_unref_object GFile *file = NULL;

      g_debug ("Loading summary %s using GIO", uri);
      file = g_file_new_for_uri (uri);
      if (!g_file_load_contents (file, cancellable, &buffer, &length, NULL, NULL))
        goto out;

      *contents = g_bytes_new_take (buffer, length);
    }
  else
    {
      gs_unref_object SoupSession *session = NULL;
      gs_unref_object SoupMessage *msg = NULL;

      g_debug ("Loading summary %s using libsoup", uri);
      session = soup_session_new ();
      msg = soup_message_new ("GET", uri);
      soup_session_send_message (session, msg);

      if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
        goto out;

      *contents = g_bytes_new (msg->response_body->data, msg->response_body->length);
    }

  ret = TRUE;

  g_debug ("Received %ld bytes", g_bytes_get_size (*contents));

out:
  return ret;
}

gboolean
ostree_repo_load_summary (OstreeRepo *repo,
                          const char *repository,
                          GHashTable **refs,
                          gchar **title,
                          GCancellable *cancellable,
                          GError **error)
{
  gboolean ret = FALSE;
  gs_free char *url = NULL;
  gs_free char *summary_url = NULL;
  gs_unref_bytes GBytes *bytes = NULL;
  gs_unref_hashtable GHashTable *local_refs = NULL;
  gs_free char *local_title = NULL;

  if (!ostree_repo_remote_get_url (repo, repository, &url, error))
    goto out;

  summary_url = g_build_filename (url, "summary", NULL);
  if (load_contents (summary_url, &bytes, cancellable, NULL))
    {
      gs_unref_variant GVariant *summary;
      gs_unref_variant GVariant *ref_list;
      gs_unref_variant GVariant *extensions;
      GVariantDict dict;
      int i, n;

      local_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

      summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes, FALSE);
      ref_list = g_variant_get_child_value (summary, 0);
      extensions = g_variant_get_child_value (summary, 1);

      n = g_variant_n_children (ref_list);
      g_debug ("Summary contains %d refs", n);
      for (i = 0; i < n; i++)
        {
          gs_unref_variant GVariant *ref = NULL;
          gs_unref_variant GVariant *csum_v = NULL;
          char *refname;
          char *checksum;

          ref = g_variant_get_child_value (ref_list, i);
          g_variant_get (ref, "(&s(t@aya{sv}))", &refname, NULL, &csum_v, NULL);

          if (!ostree_validate_rev (refname, error))
            goto out;

          checksum = ostree_checksum_from_bytes_v (csum_v);
          g_debug ("%s summary: %s -> %s", repository, refname, checksum);
          g_hash_table_insert (local_refs, g_strdup (refname), checksum);
        }

       g_variant_dict_init (&dict, extensions);
       g_variant_dict_lookup (&dict, "xa.title", "s", &local_title);
       g_debug ("%s summary: title %s", repository, local_title);
       g_variant_dict_end (&dict);
    }

  *refs = g_hash_table_ref (local_refs);
  *title = g_strdup (local_title);

  ret = TRUE;
out:
  return ret;
}
