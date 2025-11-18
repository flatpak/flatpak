/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 1995-1998 Free Software Foundation, Inc.
 * Copyright © 2014-2019 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "flatpak-dir-utils-private.h"

#include <glib/gi18n-lib.h>

#include "flatpak-dir-private.h"
#include "flatpak-metadata-private.h"
#include "flatpak-utils-private.h"

char **
flatpak_list_deployed_refs (const char   *type,
                            const char   *name_prefix,
                            const char   *arch,
                            const char   *branch,
                            GCancellable *cancellable,
                            GError      **error)
{
  gchar **ret = NULL;
  g_autoptr(GPtrArray) names = NULL;
  g_autoptr(GHashTable) hash = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  int i;

  hash = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, NULL);

  dirs = flatpak_dir_get_list (cancellable, error);
  if (dirs == NULL)
    goto out;

  for (i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      if (!flatpak_dir_collect_deployed_refs (dir, type, name_prefix,
                                              arch, branch, hash, cancellable,
                                              error))
        goto out;
    }

  names = g_ptr_array_new ();

  GLNX_HASH_TABLE_FOREACH (hash, FlatpakDecomposed *, ref)
    {
      g_ptr_array_add (names, flatpak_decomposed_dup_id (ref));
    }

  g_ptr_array_sort (names, flatpak_strcmp0_ptr);
  g_ptr_array_add (names, NULL);

  ret = (char **) g_ptr_array_free (names, FALSE);
  names = NULL;

out:
  return ret;
}

char **
flatpak_list_unmaintained_refs (const char   *name_prefix,
                                const char   *arch,
                                const char   *branch,
                                GCancellable *cancellable,
                                GError      **error)
{
  gchar **ret = NULL;
  g_autoptr(GPtrArray) names = NULL;
  g_autoptr(GHashTable) hash = NULL;
  const char *key;
  GHashTableIter iter;
  g_autoptr(GPtrArray) dirs = NULL;
  int i;

  hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  dirs = flatpak_dir_get_list (cancellable, error);
  if (dirs == NULL)
    return NULL;

  for (i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);

      if (!flatpak_dir_collect_unmaintained_refs (dir, name_prefix,
                                                  arch, branch, hash, cancellable,
                                                  error))
        return NULL;
    }

  names = g_ptr_array_new ();
  g_hash_table_iter_init (&iter, hash);
  while (g_hash_table_iter_next (&iter, (gpointer *) &key, NULL))
    g_ptr_array_add (names, g_strdup (key));

  g_ptr_array_sort (names, flatpak_strcmp0_ptr);
  g_ptr_array_add (names, NULL);

  ret = (char **) g_ptr_array_free (names, FALSE);
  names = NULL;

  return ret;
}

GFile *
flatpak_find_deploy_dir_for_ref (FlatpakDecomposed *ref,
                                 FlatpakDir       **dir_out,
                                 GCancellable      *cancellable,
                                 GError           **error)
{
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir = NULL;
  g_autoptr(GFile) deploy = NULL;
  int i;

  dirs = flatpak_dir_get_list (cancellable, error);
  if (dirs == NULL)
    return NULL;

  for (i = 0; deploy == NULL && i < dirs->len; i++)
    {
      dir = g_ptr_array_index (dirs, i);
      deploy = flatpak_dir_get_if_deployed (dir, ref, NULL, cancellable);
      if (deploy != NULL)
        break;
    }

  if (deploy == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED, _("%s not installed"), flatpak_decomposed_get_ref (ref));
      return NULL;
    }

  if (dir_out)
    *dir_out = g_object_ref (dir);
  return g_steal_pointer (&deploy);
}

GFile *
flatpak_find_files_dir_for_ref (FlatpakDecomposed *ref,
                                GCancellable      *cancellable,
                                GError           **error)
{
  g_autoptr(GFile) deploy = NULL;

  deploy = flatpak_find_deploy_dir_for_ref (ref, NULL, cancellable, error);
  if (deploy == NULL)
    return NULL;

  return g_file_get_child (deploy, "files");
}

GFile *
flatpak_find_unmaintained_extension_dir_if_exists (const char   *name,
                                                   const char   *arch,
                                                   const char   *branch,
                                                   GCancellable *cancellable)
{
  g_autoptr(GFile) extension_dir = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  int i;

  dirs = flatpak_dir_get_list (cancellable, &local_error);
  if (dirs == NULL)
    {
      g_warning ("Could not get the installations: %s", local_error->message);
      return NULL;
    }

  for (i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      extension_dir = flatpak_dir_get_unmaintained_extension_dir_if_exists (dir, name, arch, branch, cancellable);
      if (extension_dir != NULL)
        break;
    }

  if (extension_dir == NULL)
    return NULL;

  return g_steal_pointer (&extension_dir);
}

FlatpakDecomposed *
flatpak_find_current_ref (const char   *app_id,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_autoptr(FlatpakDecomposed) current_ref = NULL;
  g_autoptr(FlatpakDir) user_dir = flatpak_dir_get_user ();
  int i;

  current_ref = flatpak_dir_current_ref (user_dir, app_id, NULL);
  if (current_ref == NULL)
    {
      g_autoptr(GPtrArray) dirs = NULL;

      dirs = flatpak_dir_get_list (cancellable, error);
      if (dirs == NULL)
        return FALSE;

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          current_ref = flatpak_dir_current_ref (dir, app_id, cancellable);
          if (current_ref != NULL)
            break;
        }
    }

  if (current_ref)
    return g_steal_pointer (&current_ref);

  flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED, _("%s not installed"), app_id);
  return NULL;
}

FlatpakDeploy *
flatpak_find_deploy_for_ref_in (GPtrArray    *dirs,
                                const char   *ref_str,
                                const char   *commit,
                                GCancellable *cancellable,
                                GError      **error)
{
  FlatpakDeploy *deploy = NULL;
  int i;
  g_autoptr(GError) my_error = NULL;

  g_autoptr(FlatpakDecomposed) ref = flatpak_decomposed_new_from_ref (ref_str, error);
  if (ref == NULL)
    return NULL;

  for (i = 0; deploy == NULL && i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);

      flatpak_log_dir_access (dir);
      g_clear_error (&my_error);
      deploy = flatpak_dir_load_deployed (dir, ref, commit, cancellable, &my_error);
    }

  if (deploy == NULL)
    g_propagate_error (error, g_steal_pointer (&my_error));

  return deploy;
}

FlatpakDeploy *
flatpak_find_deploy_for_ref (const char   *ref,
                             const char   *commit,
                             FlatpakDir   *opt_user_dir,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GPtrArray) dirs = NULL;

  dirs = flatpak_dir_get_list (cancellable, error);
  if (dirs == NULL)
    return NULL;

  /* If an custom dir was passed, use that instead of the user dir.
   * This is used when running apply-extra-data where if the target
   * is a custom installation location the regular user one may not
   * have the (possibly just installed in this transaction) runtime.
   */
  if (opt_user_dir)
    g_ptr_array_insert (dirs, 0, g_object_ref (opt_user_dir));
  else
    g_ptr_array_insert (dirs, 0, flatpak_dir_get_user ());

  return flatpak_find_deploy_for_ref_in (dirs, ref, commit, cancellable, error);
}

void
flatpak_extension_free (FlatpakExtension *extension)
{
  g_free (extension->id);
  g_free (extension->installed_id);
  g_free (extension->commit);
  flatpak_decomposed_unref (extension->ref);
  g_free (extension->directory);
  g_free (extension->files_path);
  g_free (extension->add_ld_path);
  g_free (extension->subdir_suffix);
  g_strfreev (extension->merge_dirs);
  g_free (extension);
}

static int
flatpak_extension_compare (gconstpointer _a,
                           gconstpointer _b)
{
  const FlatpakExtension *a = _a;
  const FlatpakExtension *b = _b;

  return b->priority - a->priority;
}

static FlatpakExtension *
flatpak_extension_new (const char        *id,
                       const char        *extension,
                       FlatpakDecomposed *ref,
                       const char        *directory,
                       const char        *add_ld_path,
                       const char        *subdir_suffix,
                       char             **merge_dirs,
                       GFile             *files,
                       GFile             *deploy_dir,
                       gboolean           is_unmaintained,
                       OstreeRepo        *repo)
{
  FlatpakExtension *ext = g_new0 (FlatpakExtension, 1);
  g_autoptr(GBytes) deploy_data = NULL;

  ext->id = g_strdup (id);
  ext->installed_id = g_strdup (extension);
  ext->ref = flatpak_decomposed_ref (ref);
  ext->directory = g_strdup (directory);
  ext->files_path = g_file_get_path (files);
  ext->add_ld_path = g_strdup (add_ld_path);
  ext->subdir_suffix = g_strdup (subdir_suffix);
  ext->merge_dirs = g_strdupv (merge_dirs);
  ext->is_unmaintained = is_unmaintained;

  /* Unmaintained extensions won't have a deploy or commit; see
   * https://github.com/flatpak/flatpak/issues/167 */
  if (deploy_dir && !is_unmaintained)
    {
      deploy_data = flatpak_load_deploy_data (deploy_dir, ref, repo, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);
      if (deploy_data)
        ext->commit = g_strdup (flatpak_deploy_data_get_commit (deploy_data));
    }

  if (is_unmaintained)
    ext->priority = 1000;
  else
    {
      g_autoptr(GKeyFile) keyfile = g_key_file_new ();
      g_autofree char *metadata_path = g_build_filename (ext->files_path, "../metadata", NULL);

      if (g_key_file_load_from_file (keyfile, metadata_path, G_KEY_FILE_NONE, NULL))
        ext->priority = g_key_file_get_integer (keyfile,
                                                FLATPAK_METADATA_GROUP_EXTENSION_OF,
                                                FLATPAK_METADATA_KEY_PRIORITY,
                                                NULL);
    }

  return ext;
}

static GList *
add_extension (GKeyFile   *metakey,
               const char *group,
               const char *extension,
               const char *arch,
               const char *branch,
               GList      *res)
{
  FlatpakExtension *ext;
  g_autofree char *directory = g_key_file_get_string (metakey, group,
                                                      FLATPAK_METADATA_KEY_DIRECTORY,
                                                      NULL);
  g_autofree char *add_ld_path = g_key_file_get_string (metakey, group,
                                                        FLATPAK_METADATA_KEY_ADD_LD_PATH,
                                                        NULL);
  g_auto(GStrv) merge_dirs = g_key_file_get_string_list (metakey, group,
                                                         FLATPAK_METADATA_KEY_MERGE_DIRS,
                                                         NULL, NULL);
  g_autofree char *enable_if = g_key_file_get_string (metakey, group,
                                                      FLATPAK_METADATA_KEY_ENABLE_IF,
                                                      NULL);
  g_autofree char *subdir_suffix = g_key_file_get_string (metakey, group,
                                                          FLATPAK_METADATA_KEY_SUBDIRECTORY_SUFFIX,
                                                          NULL);
  g_autoptr(FlatpakDecomposed) ref = NULL;
  gboolean is_unmaintained = FALSE;
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(FlatpakDir) dir = NULL;

  if (directory == NULL)
    return res;

  ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_RUNTIME, extension, arch, branch, NULL);
  if (ref == NULL)
    return res;

  files = flatpak_find_unmaintained_extension_dir_if_exists (extension, arch, branch, NULL);

  if (files == NULL)
    {
      deploy_dir = flatpak_find_deploy_dir_for_ref (ref, &dir, NULL, NULL);
      if (deploy_dir)
        files = g_file_get_child (deploy_dir, "files");
    }
  else
    is_unmaintained = TRUE;

  /* Prefer a full extension (org.freedesktop.Locale) over subdirectory ones (org.freedesktop.Locale.sv) */
  if (files != NULL)
    {
      if (flatpak_extension_matches_reason (extension, enable_if, TRUE))
        {
          ext = flatpak_extension_new (extension, extension, ref, directory,
                                       add_ld_path, subdir_suffix, merge_dirs,
                                       files, deploy_dir, is_unmaintained,
                                       is_unmaintained ? NULL : flatpak_dir_get_repo (dir));
          res = g_list_prepend (res, ext);
        }
    }
  else if (g_key_file_get_boolean (metakey, group,
                                   FLATPAK_METADATA_KEY_SUBDIRECTORIES, NULL))
    {
      g_autofree char *prefix = g_strconcat (extension, ".", NULL);
      g_auto(GStrv) ids = NULL;
      g_auto(GStrv) unmaintained_refs = NULL;
      int j;

      ids = flatpak_list_deployed_refs ("runtime", prefix, arch, branch,
                                        NULL, NULL);
      for (j = 0; ids != NULL && ids[j] != NULL; j++)
        {
          const char *id = ids[j];
          g_autofree char *extended_dir = g_build_filename (directory, id + strlen (prefix), NULL);
          g_autoptr(FlatpakDecomposed) dir_ref = NULL;
          g_autoptr(GFile) subdir_deploy_dir = NULL;
          g_autoptr(GFile) subdir_files = NULL;
          g_autoptr(FlatpakDir) subdir_dir = NULL;

          dir_ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_RUNTIME, id, arch, branch, NULL);
          if (dir_ref == NULL)
            continue;

          subdir_deploy_dir = flatpak_find_deploy_dir_for_ref (dir_ref, &subdir_dir, NULL, NULL);
          if (subdir_deploy_dir)
            subdir_files = g_file_get_child (subdir_deploy_dir, "files");

          if (subdir_files && flatpak_extension_matches_reason (id, enable_if, TRUE))
            {
              ext = flatpak_extension_new (extension, id, dir_ref, extended_dir,
                                           add_ld_path, subdir_suffix, merge_dirs,
                                           subdir_files, subdir_deploy_dir, FALSE,
                                           flatpak_dir_get_repo (subdir_dir));
              ext->needs_tmpfs = TRUE;
              res = g_list_prepend (res, ext);
            }
        }

      unmaintained_refs = flatpak_list_unmaintained_refs (prefix, arch, branch,
                                                          NULL, NULL);
      for (j = 0; unmaintained_refs != NULL && unmaintained_refs[j] != NULL; j++)
        {
          g_autofree char *extended_dir = g_build_filename (directory, unmaintained_refs[j] + strlen (prefix), NULL);
          g_autoptr(FlatpakDecomposed) dir_ref = NULL;
          g_autoptr(GFile) subdir_files = flatpak_find_unmaintained_extension_dir_if_exists (unmaintained_refs[j], arch, branch, NULL);

          dir_ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_RUNTIME, unmaintained_refs[j], arch, branch, NULL);
          if (dir_ref == NULL)
            continue;

          if (subdir_files && flatpak_extension_matches_reason (unmaintained_refs[j], enable_if, TRUE))
            {
              ext = flatpak_extension_new (extension, unmaintained_refs[j], dir_ref,
                                           extended_dir, add_ld_path, subdir_suffix,
                                           merge_dirs, subdir_files, NULL, TRUE, NULL);
              ext->needs_tmpfs = TRUE;
              res = g_list_prepend (res, ext);
            }
        }
    }

  return res;
}

GList *
flatpak_list_extensions (GKeyFile   *metakey,
                         const char *arch,
                         const char *default_branch)
{
  g_auto(GStrv) groups = NULL;
  int i, j;
  GList *res;

  res = NULL;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      char *extension;

      if (g_str_has_prefix (groups[i], FLATPAK_METADATA_GROUP_PREFIX_EXTENSION) &&
          *(extension = (groups[i] + strlen (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION))) != 0)
        {
          g_autofree char *version = g_key_file_get_string (metakey, groups[i],
                                                            FLATPAK_METADATA_KEY_VERSION,
                                                            NULL);
          g_auto(GStrv) versions = g_key_file_get_string_list (metakey, groups[i],
                                                               FLATPAK_METADATA_KEY_VERSIONS,
                                                               NULL, NULL);
          g_autofree char *name = NULL;
          const char *default_branches[] = { default_branch, NULL};
          const char **branches;

          flatpak_parse_extension_with_tag (extension, &name, NULL);

          if (versions)
            branches = (const char **) versions;
          else
            {
              if (version)
                default_branches[0] = version;
              branches = default_branches;
            }

          for (j = 0; branches[j] != NULL; j++)
            res = add_extension (metakey, groups[i], name, arch, branches[j], res);
        }
    }

  return g_list_sort (g_list_reverse (res), flatpak_extension_compare);
}

void
flatpak_log_dir_access (FlatpakDir *dir)
{
  if (dir != NULL)
    {
      GFile *dir_path = NULL;
      g_autofree char *dir_path_str = NULL;
      g_autofree char *dir_name = NULL;

      dir_path = flatpak_dir_get_path (dir);
      if (dir_path != NULL)
        dir_path_str = g_file_get_path (dir_path);
      dir_name = flatpak_dir_get_name (dir);
      g_info ("Opening %s flatpak installation at path %s", dir_name, dir_path_str);
    }
}
