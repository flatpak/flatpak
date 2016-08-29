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

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"

static char *opt_title;
static char *opt_gpg_homedir;
static char **opt_gpg_key_ids;
static gboolean opt_prune;
static gboolean opt_generate_deltas;
static gint opt_prune_depth = -1;

static GOptionEntry options[] = {
  { "title", 0, 0, G_OPTION_ARG_STRING, &opt_title, N_("A nice name to use for this repository"), N_("TITLE") },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the summary with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },
  { "generate-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_generate_deltas, N_("Generate delta files"), NULL },
  { "prune", 0, 0, G_OPTION_ARG_NONE, &opt_prune, N_("Prune unused objects"), NULL },
  { "prune-depth", 0, 0, G_OPTION_ARG_INT, &opt_prune_depth, N_("Only traverse DEPTH parents for each commit (default: -1=infinite)"), N_("DEPTH") },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
  GVariant *params;
  char *ref;
  char *from;
  char *to;
} DeltaData;

static void
delta_data_free (DeltaData *data)
{
  g_object_unref (data->repo);
  g_variant_unref (data->params);
  g_free (data->ref);
  g_free (data->from);
  g_free (data->to);
  g_free (data);
}

static DeltaData *
delta_data_push (GThreadPool *pool,
                 OstreeRepo *repo,
                 GVariant *params,
                 const char *ref,
                 const char *from,
                 const char *to,
                 GError **error)
{
  DeltaData *data = g_new0 (DeltaData, 1);

  data->repo = g_object_ref (repo);
  data->params = g_variant_ref (params);
  data->ref = g_strdup (ref);
  data->from = g_strdup (from);
  data->to = g_strdup (to);

  if (!g_thread_pool_push (pool, data, error))
    {
      delta_data_free (data);
      return NULL;
    }

  return data;
}

static void
generate_delta_thread (gpointer       _data,
                       gpointer       user_data)
{
  DeltaData *data = (DeltaData*) _data;
  g_autoptr(GError) error = NULL;

  if (data->from == NULL)
    g_print (_("Generating delta: %s (%.10s)\n"), data->ref, data->to);
  else
    g_print (_("Generating delta: %s (%.10s-%.10s)\n"), data->ref, data->from, data->to);

  if (!ostree_repo_static_delta_generate (data->repo, OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR,
                                          data->from, data->to, NULL,
                                          data->params,
                                          NULL, &error))
    {
      if (data->from == NULL)
        g_printerr (_("Failed to generate delta %s (%.10s): %s\n"),
                    data->ref, data->to, error->message);
      else
        g_printerr (_("Failed to generate delta %s (%.10s-%.10s): %s\n"),
                    data->ref, data->from, data->to, error->message);
    }
}

static void
_ostree_parse_delta_name (const char  *delta_name,
                          char        **out_from,
                          char        **out_to)
{
  g_auto(GStrv) parts = g_strsplit (delta_name, "-", 2);

  if (parts[0] && parts[1])
    {
      *out_from = g_steal_pointer (&parts[0]);
      *out_to = g_steal_pointer (&parts[1]);
    }
  else
    {
      *out_from = NULL;
      *out_to = g_steal_pointer (&parts[0]);
    }
}

static char *
_ostree_get_relative_static_delta_path (const char *from,
                                        const char *to,
                                        const char *target)
{
  guint8 csum_to[OSTREE_SHA256_DIGEST_LEN];
  char to_b64[44];
  guint8 csum_to_copy[OSTREE_SHA256_DIGEST_LEN];
  GString *ret = g_string_new ("deltas/");

  ostree_checksum_inplace_to_bytes (to, csum_to);
  ostree_checksum_b64_inplace_from_bytes (csum_to, to_b64);
  ostree_checksum_b64_inplace_to_bytes (to_b64, csum_to_copy);

  g_assert (memcmp (csum_to, csum_to_copy, OSTREE_SHA256_DIGEST_LEN) == 0);

  if (from != NULL)
    {
      guint8 csum_from[OSTREE_SHA256_DIGEST_LEN];
      char from_b64[44];

      ostree_checksum_inplace_to_bytes (from, csum_from);
      ostree_checksum_b64_inplace_from_bytes (csum_from, from_b64);

      g_string_append_c (ret, from_b64[0]);
      g_string_append_c (ret, from_b64[1]);
      g_string_append_c (ret, '/');
      g_string_append (ret, from_b64 + 2);
      g_string_append_c (ret, '-');
    }

  g_string_append_c (ret, to_b64[0]);
  g_string_append_c (ret, to_b64[1]);
  if (from == NULL)
    g_string_append_c (ret, '/');
  g_string_append (ret, to_b64 + 2);

  if (target != NULL)
    {
      g_string_append_c (ret, '/');
      g_string_append (ret, target);
    }

  return g_string_free (ret, FALSE);
}

static gboolean
_ostree_repo_static_delta_delete (OstreeRepo                    *self,
                                  const char                    *delta_id,
                                  GCancellable                  *cancellable,
                                  GError                      **error)
{
  gboolean ret = FALSE;
  g_autofree char *from = NULL;
  g_autofree char *to = NULL;
  g_autofree char *deltadir = NULL;
  struct stat buf;
  int repo_dir_fd = ostree_repo_get_dfd (self);

  _ostree_parse_delta_name (delta_id, &from, &to);
  deltadir = _ostree_get_relative_static_delta_path (from, to, NULL);

  if (fstatat (repo_dir_fd, deltadir, &buf, 0) != 0)
    {
      if (errno == ENOENT)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "Can't find delta %s", delta_id);
      else
        glnx_set_error_from_errno (error);

      goto out;
    }

  if (!glnx_shutil_rm_rf_at (repo_dir_fd, deltadir,
                             cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}


static gboolean
generate_all_deltas (OstreeRepo *repo,
                     GPtrArray **unwanted_deltas,
                     GCancellable *cancellable,
                     GError **error)
{
  g_autoptr(GHashTable) all_refs = NULL;
  g_autoptr(GHashTable) all_deltas_hash = NULL;
  g_autoptr(GHashTable) wanted_deltas_hash = NULL;
  g_autoptr(GPtrArray) all_deltas = NULL;
  int i;
  GHashTableIter iter;
  gpointer key, value;
  g_autoptr(GVariantBuilder) parambuilder = NULL;
  g_autoptr(GVariant) params = NULL;
  GThreadPool *thread_pool;

  g_print ("Generating static deltas\n");

  parambuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  /* Fall back for 1 meg files */
  g_variant_builder_add (parambuilder, "{sv}",
                         "min-fallback-size", g_variant_new_uint32 (1));
  params = g_variant_ref_sink (g_variant_builder_end (parambuilder));

  if (!ostree_repo_list_static_delta_names (repo, &all_deltas,
                                            cancellable, error))
    return FALSE;

  wanted_deltas_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  all_deltas_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (i = 0; i < all_deltas->len; i++)
    g_hash_table_insert (all_deltas_hash,
                         g_strdup (g_ptr_array_index (all_deltas, i)),
                         NULL);

  if (!ostree_repo_list_refs (repo, NULL, &all_refs,
                              cancellable, error))
    return FALSE;

  thread_pool = g_thread_pool_new (generate_delta_thread, NULL,
                                   g_get_num_processors (), FALSE,
                                   error);
  if (thread_pool == NULL)
    return FALSE;

  g_hash_table_iter_init (&iter, all_refs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *ref = key;
      const char *commit = value;
      const char *from_parent = NULL;
      g_autoptr(GVariant) variant = NULL;
      g_autoptr(GVariant) parent_variant = NULL;
      g_autofree char *parent_commit = NULL;

      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit,
                                     &variant, NULL))
        {
          g_warning ("Couldn't load commit %s\n", commit);
          continue;
        }

      /* From empty */
      if (!g_hash_table_contains (all_deltas_hash, commit))
        {
          if (!delta_data_push (thread_pool, repo, params,
                                ref, NULL, commit,
                                error))
            goto error;
        }

      /* Mark this one as wanted */
      g_hash_table_insert (wanted_deltas_hash, g_strdup (commit), GINT_TO_POINTER (1));

      parent_commit = ostree_commit_get_parent (variant);

      if (parent_commit != NULL &&
          !ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, parent_commit,
                                     &parent_variant, NULL))
        {
          g_warning ("Couldn't load parent commit %s\n", parent_commit);
          continue;
        }

      /* From parent */
      if (parent_variant != NULL)
        {
          from_parent = g_strdup_printf ("%s-%s", parent_commit, commit);
          if (!g_hash_table_contains (all_deltas_hash, from_parent))
            {
              if (!delta_data_push (thread_pool, repo, params,
                                    ref, parent_commit, commit,
                                    error))
                goto error;
            }

          /* Mark this one as wanted */
          g_hash_table_insert (wanted_deltas_hash, g_strdup (from_parent), GINT_TO_POINTER (1));
        }
    }

  *unwanted_deltas = g_ptr_array_new_with_free_func (g_free);
  for (i = 0; i < all_deltas->len; i++)
    {
      const char *delta = g_ptr_array_index (all_deltas, i);
      if (!g_hash_table_contains (wanted_deltas_hash, delta))
        g_ptr_array_add (*unwanted_deltas, g_strdup (delta));
    }

  /* This block until all are done */
  g_thread_pool_free (thread_pool, FALSE, TRUE);

  return TRUE;

 error:
  /* TODO: In this error case we're leaking all the DeltaDatas we have not yet
     processed. I don't know how to fix this though... */
  g_thread_pool_free (thread_pool, TRUE, FALSE);
  return FALSE;
}

gboolean
flatpak_builtin_build_update_repo (int argc, char **argv,
                                   GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *location;
  g_autoptr(GPtrArray) unwanted_deltas = NULL;

  context = g_option_context_new (_("LOCATION - Update repository metadata"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("LOCATION must be specified"), error);

  location = argv[1];

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (opt_title &&
      !flatpak_repo_set_title (repo, opt_title, error))
    return FALSE;

  g_print (_("Updating appstream branch\n"));
  if (!flatpak_repo_generate_appstream (repo, (const char **) opt_gpg_key_ids, opt_gpg_homedir, cancellable, error))
    return FALSE;

  if (opt_generate_deltas &&
      !generate_all_deltas (repo, &unwanted_deltas, cancellable, error))
    return FALSE;

  g_print (_("Updating summary\n"));
  if (!flatpak_repo_update (repo, (const char **) opt_gpg_key_ids, opt_gpg_homedir, cancellable, error))
    return FALSE;

  if (unwanted_deltas != NULL)
    {
      int i;
      for (i = 0; i < unwanted_deltas->len; i++)
        {
          const char *delta = g_ptr_array_index (unwanted_deltas, i);
          g_print ("Deleting unwanted delta: %s\n", delta);
          g_autoptr(GError) my_error = NULL;
          if (!_ostree_repo_static_delta_delete (repo, delta, cancellable, &my_error))
            g_printerr ("Unable to delete delta %s: %s\n", delta, my_error->message);
        }
    }

  if (opt_prune)
    {
      gint n_objects_total;
      gint n_objects_pruned;
      guint64 objsize_total;
      g_autofree char *formatted_freed_size = NULL;

      g_print ("Pruning old commits\n");
      if (!ostree_repo_prune (repo, OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY, opt_prune_depth,
                              &n_objects_total, &n_objects_pruned, &objsize_total,
                              cancellable, error))
        return FALSE;

      formatted_freed_size = g_format_size_full (objsize_total, 0);

      g_print (_("Total objects: %u\n"), n_objects_total);
      if (n_objects_pruned == 0)
        g_print (_("No unreachable objects\n"));
      else
        g_print (_("Deleted %u objects, %s freed\n"),
                 n_objects_pruned, formatted_freed_size);
    }

  return TRUE;
}


gboolean
flatpak_complete_build_update_repo (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* LOCATION */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_dir (completion);
      break;
    }

  return TRUE;
}
