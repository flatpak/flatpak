/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <glib/gi18n.h>


#include <gio/gunixinputstream.h>
#include "flatpak-chain-input-stream-private.h"

#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"


gboolean
looks_like_branch (const char *branch)
{
  const char *dot;

  /* In particular, / is not a valid branch char, so
     this lets us distinguish full or partial refs as
     non-branches. */
  if (!flatpak_is_valid_branch (branch, NULL))
    return FALSE;

  /* Dots are allowed in branches, but not really used much, while
     app ids require at least two, so thats a good check to
     distinguish the two */
  dot = strchr (branch, '.');
  if (dot != NULL)
    {
      if (strchr (dot + 1, '.') != NULL)
        return FALSE;
    }

  return TRUE;
}

static SoupSession *
get_soup_session (void)
{
  static SoupSession *soup_session = NULL;

  if (soup_session == NULL)
    {
      const char *http_proxy;

      soup_session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT, "flatpak-builder ",
                                                    SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                    SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                    SOUP_SESSION_TIMEOUT, 60,
                                                    SOUP_SESSION_IDLE_TIMEOUT, 60,
                                                    NULL);
      http_proxy = g_getenv ("http_proxy");
      if (http_proxy)
        {
          g_autoptr(SoupURI) proxy_uri = soup_uri_new (http_proxy);
          if (!proxy_uri)
            g_warning ("Invalid proxy URI '%s'", http_proxy);
          else
            g_object_set (soup_session, SOUP_SESSION_PROXY_URI, proxy_uri, NULL);
        }
    }

  return soup_session;
}

GBytes *
download_uri (const char *url,
              GError    **error)
{
  SoupSession *session;

  g_autoptr(SoupRequest) req = NULL;
  g_autoptr(GInputStream) input = NULL;
  g_autoptr(GOutputStream) out = NULL;

  session = get_soup_session ();

  req = soup_session_request (session, url, error);
  if (req == NULL)
    return NULL;

  input = soup_request_send (req, NULL, error);
  if (input == NULL)
    return NULL;

  out = g_memory_output_stream_new_resizable ();
  if (!g_output_stream_splice (out,
                               input,
                               G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                               NULL,
                               error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
}

FlatpakDir *
flatpak_find_installed_pref (const char *pref, FlatpakKinds kinds, const char *default_arch, const char *default_branch,
                             gboolean search_all, gboolean search_user, gboolean search_system, char **search_installations,
                             char **out_ref, GCancellable *cancellable, GError **error)
{
  g_autofree char *id = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;

  g_autoptr(GError) lookup_error = NULL;
  FlatpakDir *dir = NULL;
  g_autofree char *ref = NULL;
  FlatpakKinds kind = 0;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  g_autoptr(GPtrArray) system_dirs = NULL;

  if (!flatpak_split_partial_ref_arg (pref, kinds, default_arch, default_branch,
                                      &kinds, &id, &arch, &branch, error))
    return NULL;

  if (search_user || search_all)
    {
      user_dir = flatpak_dir_get_user ();

      ref = flatpak_dir_find_installed_ref (user_dir,
                                            id,
                                            branch,
                                            arch,
                                            kinds, &kind,
                                            &lookup_error);
      if (ref)
        dir = user_dir;

      if (g_error_matches (lookup_error, G_IO_ERROR, G_IO_ERROR_FAILED))
        {
          g_propagate_error (error, g_steal_pointer (&lookup_error));
          return NULL;
        }
    }

  if (ref == NULL && search_all)
    {
      int i;

      system_dirs = flatpak_dir_get_system_list (cancellable, error);
      if (system_dirs == NULL)
        return FALSE;

      for (i = 0; i < system_dirs->len; i++)
        {
          FlatpakDir *system_dir = g_ptr_array_index (system_dirs, i);

          g_clear_error (&lookup_error);

          ref = flatpak_dir_find_installed_ref (system_dir,
                                                id,
                                                branch,
                                                arch,
                                                kinds, &kind,
                                                &lookup_error);
          if (ref)
            {
              dir = system_dir;
              break;
            }

          if (g_error_matches (lookup_error, G_IO_ERROR, G_IO_ERROR_FAILED))
            {
              g_propagate_error (error, g_steal_pointer (&lookup_error));
              return NULL;
            }
        }
    }
  else
    {
      if (ref == NULL && search_installations != NULL)
        {
          int i = 0;

          for (i = 0; search_installations[i] != NULL; i++)
            {
              g_autoptr(FlatpakDir) installation_dir = NULL;

              installation_dir = flatpak_dir_get_system_by_id (search_installations[i], cancellable, error);
              if (installation_dir == NULL)
                return FALSE;

              if (installation_dir)
                {
                  g_clear_error (&lookup_error);

                  ref = flatpak_dir_find_installed_ref (installation_dir,
                                                        id,
                                                        branch,
                                                        arch,
                                                        kinds, &kind,
                                                        &lookup_error);
                  if (ref)
                    {
                      dir = installation_dir;
                      break;
                    }

                  if (g_error_matches (lookup_error, G_IO_ERROR, G_IO_ERROR_FAILED))
                    {
                      g_propagate_error (error, g_steal_pointer (&lookup_error));
                      return NULL;
                    }
                }
            }
        }

      if (ref == NULL && search_system)
        {
          system_dir = flatpak_dir_get_system_default ();

          g_clear_error (&lookup_error);

          ref = flatpak_dir_find_installed_ref (system_dir,
                                                id,
                                                branch,
                                                arch,
                                                kinds, &kind,
                                                &lookup_error);

          if (ref)
            dir = system_dir;
        }
    }

  if (ref == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&lookup_error));
      return NULL;
    }

  *out_ref = g_steal_pointer (&ref);
  return g_object_ref (dir);
}


static gboolean
open_source_stream (char         **gpg_import,
                    GInputStream **out_source_stream,
                    GCancellable  *cancellable,
                    GError       **error)
{
  g_autoptr(GInputStream) source_stream = NULL;
  guint n_keyrings = 0;
  g_autoptr(GPtrArray) streams = NULL;

  if (gpg_import != NULL)
    n_keyrings = g_strv_length (gpg_import);

  guint ii;

  streams = g_ptr_array_new_with_free_func (g_object_unref);

  for (ii = 0; ii < n_keyrings; ii++)
    {
      GInputStream *input_stream = NULL;

      if (strcmp (gpg_import[ii], "-") == 0)
        {
          input_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
        }
      else
        {
          g_autoptr(GFile) file = g_file_new_for_commandline_arg (gpg_import[ii]);
          input_stream = G_INPUT_STREAM (g_file_read (file, cancellable, error));

          if (input_stream == NULL)
            {
              g_prefix_error (error, "The file %s specified for --gpg-import was not found: ", gpg_import[ii]);
              return FALSE;
            }
        }

      /* Takes ownership. */
      g_ptr_array_add (streams, input_stream);
    }

  /* Chain together all the --keyring options as one long stream. */
  source_stream = (GInputStream *) flatpak_chain_input_stream_new (streams);

  *out_source_stream = g_steal_pointer (&source_stream);

  return TRUE;
}

GBytes *
flatpak_load_gpg_keys (char        **gpg_import,
                       GCancellable *cancellable,
                       GError      **error)
{
  g_autoptr(GInputStream) input_stream = NULL;
  g_autoptr(GOutputStream) output_stream = NULL;
  gssize n_bytes_written;

  if (!open_source_stream (gpg_import, &input_stream, cancellable, error))
    return FALSE;

  output_stream = g_memory_output_stream_new_resizable ();

  n_bytes_written = g_output_stream_splice (output_stream, input_stream,
                                            G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                            G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                            NULL, error);
  if (n_bytes_written < 0)
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (output_stream));
}

gboolean
flatpak_resolve_duplicate_remotes (GPtrArray    *dirs,
                                   const char   *remote_name,
                                   FlatpakDir  **out_dir,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  g_autoptr(GPtrArray) dirs_with_remote = NULL;
  int chosen = 0;
  int i;

  dirs_with_remote = g_ptr_array_new ();
  for (i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      g_auto(GStrv) remotes = NULL;
      int j = 0;

      remotes = flatpak_dir_list_remotes (dir, cancellable, error);
      if (remotes == NULL)
        return FALSE;

      for (j = 0; remotes[j] != NULL; j++)
        {
          const char *this_remote = remotes[j];

          if (g_strcmp0 (remote_name, this_remote) == 0)
            g_ptr_array_add (dirs_with_remote, dir);
        }
    }

  if (dirs_with_remote->len == 1)
    chosen = 1;
  else if (dirs_with_remote->len > 1)
    {
      g_print (_("Remote ‘%s’ found in multiple installations:\n"), remote_name);
      for (i = 0; i < dirs_with_remote->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs_with_remote, i);
          g_autofree char *dir_name = flatpak_dir_get_name (dir);
          g_print ("%d) %s\n", i + 1, dir_name);
        }
      chosen = flatpak_number_prompt (0, dirs_with_remote->len, _("Which do you want to use (0 to abort)?"));
      if (chosen == 0)
        return flatpak_fail (error, _("No remote chosen to resolve ‘%s’ which exists in multiple installations"), remote_name);
    }

  if (out_dir)
    {
      if (dirs_with_remote->len == 0)
        return flatpak_fail_error (error, FLATPAK_ERROR_REMOTE_NOT_FOUND,
                                   "Remote \"%s\" not found", remote_name);
      else
        *out_dir = g_object_ref (g_ptr_array_index (dirs_with_remote, chosen - 1));
    }

  return TRUE;
}

/* Returns: the time in seconds since the file was modified, or %G_MAXUINT64 on error */
static guint64
get_file_age (GFile *file)
{
  guint64 now;
  guint64 mtime;

  g_autoptr(GFileInfo) info = NULL;

  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_TIME_MODIFIED,
                            G_FILE_QUERY_INFO_NONE,
                            NULL,
                            NULL);
  if (info == NULL)
    return G_MAXUINT64;

  mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  now = (guint64) g_get_real_time () / G_USEC_PER_SEC;
  if (mtime > now)
    return G_MAXUINT64;

  return (guint64) (now - mtime);
}

static void
no_progress_cb (OstreeAsyncProgress *progress, gpointer user_data)
{
}

static guint64
get_appstream_timestamp (FlatpakDir *dir,
                         const char *remote,
                         const char *arch)
{
  g_autoptr(GFile) ts_file = NULL;
  g_autofree char *ts_file_path = NULL;
  g_autofree char *subdir = NULL;

  subdir = g_strdup_printf ("appstream/%s/%s/.timestamp", remote, arch);
  ts_file = g_file_resolve_relative_path (flatpak_dir_get_path (dir), subdir);
  ts_file_path = g_file_get_path (ts_file);
  return get_file_age (ts_file);
}


gboolean
update_appstream (GPtrArray    *dirs,
                  const char   *remote,
                  const char   *arch,
                  guint64       ttl,
                  gboolean      quiet,
                  GCancellable *cancellable,
                  GError      **error)
{
  gboolean changed;
  gboolean res;
  int i, j;

  g_return_val_if_fail (dirs != NULL, FALSE);

  if (arch == NULL)
    arch = flatpak_get_arch ();

  if (remote == NULL)
    {
      for (j = 0; j < dirs->len; j++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, j);
          g_auto(GStrv) remotes = NULL;

          remotes = flatpak_dir_list_remotes (dir, cancellable, error);
          if (remotes == NULL)
            return FALSE;

          for (i = 0; remotes[i] != NULL; i++)
            {
              g_autoptr(GError) local_error = NULL;
              g_autoptr(OstreeAsyncProgress) progress = NULL;
              guint64 ts_file_age;

              ts_file_age = get_appstream_timestamp (dir, remotes[i], arch);
              if (ts_file_age < ttl)
                {
                  g_debug ("%s:%s appstream age %" G_GUINT64_FORMAT " is less than ttl %" G_GUINT64_FORMAT, remotes[i], arch, ts_file_age, ttl);
                  continue;
                }
              else
                g_debug ("%s:%s appstream age %" G_GUINT64_FORMAT " is greater than ttl %" G_GUINT64_FORMAT, remotes[i], arch, ts_file_age, ttl);

              if (flatpak_dir_get_remote_disabled (dir, remotes[i]) ||
                  flatpak_dir_get_remote_noenumerate (dir, remotes[i]))
                continue;

              if (flatpak_dir_is_user (dir))
                {
                  if (quiet)
                    g_debug (_("Updating appstream data for user remote %s"), remotes[i]);
                  else
                    {
                      g_print (_("Updating appstream data for user remote %s"), remotes[i]);
                      g_print ("\n");
                    }
                }
              else
                {
                  if (quiet)
                    g_debug (_("Updating appstream data for remote %s"), remotes[i]);
                  else
                    {
                      g_print (_("Updating appstream data for remote %s"), remotes[i]);
                      g_print ("\n");
                    }
                }
              progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);
              if (!flatpak_dir_update_appstream (dir, remotes[i], arch, &changed,
                                                 progress, cancellable, &local_error))
                {
                  if (quiet)
                    g_debug ("%s: %s", _("Error updating"), local_error->message);
                  else
                    g_printerr ("%s: %s\n", _("Error updating"), local_error->message);
                }
              ostree_async_progress_finish (progress);
            }
        }
    }
  else
    {
      gboolean found = FALSE;

      for (j = 0; j < dirs->len; j++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, j);

          if (flatpak_dir_has_remote (dir, remote, NULL))
            {
              g_autoptr(OstreeAsyncProgress) progress = NULL;
              guint64 ts_file_age;

              found = TRUE;

              ts_file_age = get_appstream_timestamp (dir, remote, arch);
              if (ts_file_age < ttl)
                {
                  g_debug ("%s:%s appstream age %" G_GUINT64_FORMAT " is less than ttl %" G_GUINT64_FORMAT, remote, arch, ts_file_age, ttl);
                  continue;
                }
              else
                g_debug ("%s:%s appstream age %" G_GUINT64_FORMAT " is greater than ttl %" G_GUINT64_FORMAT, remote, arch, ts_file_age, ttl);

              progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);
              res = flatpak_dir_update_appstream (dir, remote, arch, &changed,
                                                  progress, cancellable, error);
              ostree_async_progress_finish (progress);
              if (!res)
                return FALSE;
            }
        }

      if (!found)
        return flatpak_fail_error (error, FLATPAK_ERROR_REMOTE_NOT_FOUND,
                                   _("Remote \"%s\" not found"), remote);
    }

  return TRUE;
}
