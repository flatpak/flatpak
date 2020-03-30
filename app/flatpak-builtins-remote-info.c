/*
 * Copyright © 2017 Red Hat, Inc
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
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-variant-impl-private.h"

static char *opt_arch;
static char *opt_commit;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_show_ref;
static gboolean opt_show_commit;
static gboolean opt_show_parent;
static gboolean opt_show_metadata;
static gboolean opt_log;
static gboolean opt_show_runtime;
static gboolean opt_show_sdk;
static gboolean opt_cached;
static gboolean opt_sideloaded;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to install for"), N_("ARCH") },
  { "commit", 0, 0, G_OPTION_ARG_STRING, &opt_commit, N_("Commit to show info for"), N_("COMMIT") },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { "log", 0, 0, G_OPTION_ARG_NONE, &opt_log, N_("Display log"), NULL },
  { "show-ref", 'r', 0, G_OPTION_ARG_NONE, &opt_show_ref, N_("Show ref"), NULL },
  { "show-commit", 'c', 0, G_OPTION_ARG_NONE, &opt_show_commit, N_("Show commit"), NULL },
  { "show-parent", 'p', 0, G_OPTION_ARG_NONE, &opt_show_parent, N_("Show parent"), NULL },
  { "show-metadata", 'm', 0, G_OPTION_ARG_NONE, &opt_show_metadata, N_("Show metadata"), NULL },
  { "show-runtime", 0, 0, G_OPTION_ARG_NONE, &opt_show_runtime, N_("Show runtime"), NULL },
  { "show-sdk", 0, 0, G_OPTION_ARG_NONE, &opt_show_sdk, N_("Show sdk"), NULL },
  { "cached", 0, 0, G_OPTION_ARG_NONE, &opt_cached, N_("Use local caches even if they are stale"), NULL },
  { "sideloaded", 0, 0, G_OPTION_ARG_NONE, &opt_sideloaded, N_("Only list refs available as sideloads"), NULL },
  { NULL }
};

static void
maybe_print_space (gboolean *first)
{
  if (*first)
    *first = FALSE;
  else
    g_print (" ");
}

gboolean
flatpak_builtin_remote_info (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(FlatpakDir) preferred_dir = NULL;
  g_autoptr(GVariant) commit_v = NULL;
  const char *remote;
  const char *pref;
  g_autofree char *default_branch = NULL;
  FlatpakKinds kinds;
  FlatpakKinds matched_kinds;
  g_autofree char *id = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;
  g_auto(GStrv) parts = NULL;
  FlatpakKinds kind;
  g_autofree char *ref = NULL;
  g_autofree char *commit = NULL;
  g_autofree char *parent = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  gboolean friendly = TRUE;
  const char *xa_metadata = NULL;
  const char *collection_id = NULL;
  const char *eol = NULL;
  const char *eol_rebase = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  guint64 installed_size = 0;
  guint64 download_size = 0;
  g_autofree char *formatted_installed_size = NULL;
  g_autofree char *formatted_download_size = NULL;
  const gchar *subject = NULL;
  guint64 timestamp;
  g_autofree char *formatted_timestamp = NULL;
  VarMetadataRef sparse_cache;

  context = g_option_context_new (_(" REMOTE REF - Show information about an application or runtime in a remote"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, cancellable, error))
    return FALSE;

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  if (argc < 3)
    return usage_error (context, _("REMOTE and REF must be specified"), error);

  remote = argv[1];
  pref = argv[2];

  if (!flatpak_resolve_duplicate_remotes (dirs, remote, &preferred_dir, cancellable, error))
    return FALSE;

  default_branch = flatpak_dir_get_remote_default_branch (preferred_dir, remote);
  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  if (!flatpak_split_partial_ref_arg (pref, kinds, opt_arch, NULL,
                                      &matched_kinds, &id, &arch, &branch, error))
    return FALSE;

  ref = flatpak_dir_find_remote_ref (preferred_dir, remote, NULL, id, branch, default_branch, arch,
                                     matched_kinds, &kind, cancellable, error);
  if (ref == NULL)
    return FALSE;

  state = get_remote_state (preferred_dir, remote, opt_cached, opt_sideloaded, cancellable, error);
  if (state == NULL)
    return FALSE;

  if (opt_cached)
    {
      if (opt_commit)
        commit = g_strdup (opt_commit);
      else
        {
          flatpak_remote_state_lookup_ref (state, ref, &commit, NULL, NULL, NULL, error);
          if (commit == NULL)
            {
              if (error != NULL && *error == NULL)
                flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                                    _("Couldn't find latest checksum for ref %s in remote %s"),
                                    ref, remote);
              return FALSE;
            }
        }
    }
  else
    {
      commit_v = flatpak_dir_fetch_remote_commit (preferred_dir, remote, ref, opt_commit, NULL, &commit, cancellable, error);
      if (commit_v == NULL)
        return FALSE;
    }

  if (flatpak_remote_state_lookup_sparse_cache (state, ref, &sparse_cache, NULL))
    {
      eol = var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE, NULL);
      eol_rebase = var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE_REBASE, NULL);
    }

  if (opt_show_ref || opt_show_commit || opt_show_parent || opt_show_metadata || opt_show_runtime || opt_show_sdk)
    friendly = FALSE;

  parts = g_strsplit (ref, "/", 0);

  if (friendly)
    {
      int len;
      int rows, cols;
      int width;
      g_autoptr(AsStore) store = as_store_new ();
      AsApp *app = NULL;
      const char *version = NULL;
      const char *license = NULL;

      flatpak_get_window_size (&rows, &cols);

#if AS_CHECK_VERSION (0, 6, 1)
      as_store_set_add_flags (store, as_store_get_add_flags (store) | AS_STORE_ADD_FLAG_USE_UNIQUE_ID);
#endif

      flatpak_dir_load_appstream_store (preferred_dir, remote, parts[2], store, NULL, NULL);
      app = as_store_find_app (store, ref);
      if (app)
        {
          const char *name = as_app_get_localized_name (app);
          const char *comment = as_app_get_localized_comment (app);

          print_wrapped (MIN (cols, 80), "\n%s - %s\n", name, comment);

          version = as_app_get_version (app);
          license = as_app_get_project_license (app);
        }

      if (commit_v)
        {
          VarCommitRef commit = var_commit_from_gvariant (commit_v);
          VarMetadataRef commit_metadata;

          subject = var_commit_get_subject (commit);
          parent = ostree_commit_get_parent (commit_v);
          timestamp = ostree_commit_get_timestamp (commit_v);

          commit_metadata = var_commit_get_metadata (commit);
          xa_metadata = var_metadata_lookup_string (commit_metadata, "xa.metadata", NULL);
          if (xa_metadata == NULL)
            g_printerr (_("Warning: Commit has no flatpak metadata\n"));

          if (xa_metadata == NULL)
            g_printerr (_("Warning: Commit has no flatpak metadata\n"));
          else
            {
              metakey = g_key_file_new ();
              if (!g_key_file_load_from_data (metakey, xa_metadata, -1, 0, error))
                return FALSE;
            }

          collection_id = var_metadata_lookup_string (commit_metadata, "ostree.collection-binding", NULL);

          installed_size = GUINT64_FROM_BE (var_metadata_lookup_uint64 (commit_metadata, "xa.installed-size", 0));
          download_size = GUINT64_FROM_BE (var_metadata_lookup_uint64 (commit_metadata, "xa.download-size", 0));

          formatted_installed_size = g_format_size (installed_size);
          formatted_download_size = g_format_size (download_size);
          formatted_timestamp = format_timestamp (timestamp);
        }

      len = 0;
      len = MAX (len, g_utf8_strlen (_("ID:"), -1));
      len = MAX (len, g_utf8_strlen (_("Ref:"), -1));
      len = MAX (len, g_utf8_strlen (_("Arch:"), -1));
      len = MAX (len, g_utf8_strlen (_("Branch:"), -1));
      if (version != NULL)
        len = MAX (len, g_utf8_strlen (_("Version:"), -1));
      if (license != NULL)
        len = MAX (len, g_utf8_strlen (_("License:"), -1));
      if (collection_id != NULL)
        len = MAX (len, g_utf8_strlen (_("Collection:"), -1));
      if (formatted_download_size)
        len = MAX (len, g_utf8_strlen (_("Download:"), -1));
      if (formatted_installed_size)
        len = MAX (len, g_utf8_strlen (_("Installed:"), -1));
      if (strcmp (parts[0], "app") == 0 && metakey != NULL)
        {
          len = MAX (len, g_utf8_strlen (_("Runtime:"), -1));
          len = MAX (len, g_utf8_strlen (_("Sdk:"), -1));
        }
      if (formatted_timestamp)
        len = MAX (len, g_utf8_strlen (_("Date:"), -1));
      if (subject)
        len = MAX (len, g_utf8_strlen (_("Subject:"), -1));
      len = MAX (len, g_utf8_strlen (_("Commit:"), -1));
      if (parent)
        len = MAX (len, g_utf8_strlen (_("Parent:"), -1));
      if (eol)
        len = MAX (len, strlen (_("End-of-life:")));
      if (eol_rebase)
        len = MAX (len, strlen (_("End-of-life-rebase:")));
      if (opt_log)
        len = MAX (len, g_utf8_strlen (_("History:"), -1));

      width = cols - (len + 1);

      print_aligned (len, _("ID:"), parts[1]);
      print_aligned (len, _("Ref:"), ref);
      print_aligned (len, _("Arch:"), parts[2]);
      print_aligned (len, _("Branch:"), parts[3]);
      if (version != NULL)
        print_aligned (len, _("Version:"), version);
      if (license != NULL)
        print_aligned (len, _("License:"), license);
      if (collection_id != NULL)
        print_aligned (len, _("Collection:"), collection_id);
      if (formatted_download_size)
        print_aligned (len, _("Download:"), formatted_download_size);
      if (formatted_installed_size)
        print_aligned (len, _("Installed:"), formatted_installed_size);
      if (strcmp (parts[0], "app") == 0 && metakey != NULL)
        {
          g_autofree char *runtime = g_key_file_get_string (metakey, "Application", "runtime", error);
          print_aligned (len, _("Runtime:"), runtime ? runtime : "-");
        }
      g_print ("\n");
      if (strcmp (parts[0], "app") == 0 && metakey != NULL)
        {
          g_autofree char *sdk = g_key_file_get_string (metakey, "Application", "sdk", error);
          print_aligned (len, _("Sdk:"), sdk ? sdk : "-");
        }
      {
        g_autofree char *formatted_commit = ellipsize_string (commit, width);
        print_aligned (len, _("Commit:"), formatted_commit);
      }
      if (parent)
        {
          g_autofree char *formatted_commit = ellipsize_string (parent, width);
          print_aligned (len, _("Parent:"), formatted_commit);
        }
      if (eol)
        {
          g_autofree char *formatted_eol = ellipsize_string (eol, width);
          print_aligned (len, _("End-of-life:"), formatted_eol);
        }
      if (eol_rebase)
        {
          g_autofree char *formatted_eol = ellipsize_string (eol_rebase, width);
          print_aligned (len, _("End-of-life-rebase:"), formatted_eol);
        }

      if (subject)
        print_aligned (len, _("Subject:"), subject);
      if (formatted_timestamp)
        print_aligned (len, _("Date:"), formatted_timestamp);

      if (opt_log)
        {
          g_autofree char *p = g_strdup (parent);

          print_aligned (len, _("History:"), "\n");

          while (p)
            {
              g_autofree char *p_parent = NULL;
              const gchar *p_subject;
              guint64 p_timestamp;
              g_autofree char *p_formatted_timestamp = NULL;
              g_autoptr(GVariant) p_commit_v = NULL;
              VarCommitRef p_commit;

              p_commit_v = flatpak_dir_fetch_remote_commit (preferred_dir, remote, ref, p, NULL, NULL, cancellable, NULL);
              if (p_commit_v == NULL)
                break;

              p_parent = ostree_commit_get_parent (p_commit_v);
              p_timestamp = ostree_commit_get_timestamp (p_commit_v);
              p_formatted_timestamp = format_timestamp (p_timestamp);

              p_commit = var_commit_from_gvariant (commit_v);
              p_subject = var_commit_get_subject (p_commit);

              print_aligned (len, _(" Commit:"), p);
              print_aligned (len, _(" Subject:"), p_subject);
              print_aligned (len, _(" Date:"), p_formatted_timestamp);

              g_free (p);
              p = g_steal_pointer (&p_parent);
              if (p)
                g_print ("\n");
            }
        }
    }
  else
    {
      g_autoptr(GVariant) c_v = NULL;
      g_autofree char *c = g_strdup (commit);

      if (commit_v)
        c_v = g_variant_ref (commit_v);

      do
        {
          g_autofree char *p = NULL;
          g_autoptr(GVariant) c_m = NULL;
          gboolean first = TRUE;

          if (c_v)
            {
              c_m = g_variant_get_child_value (c_v, 0);
              p = ostree_commit_get_parent (c_v);
            }

          if (c_m)
            {
              g_variant_lookup (c_m, "xa.metadata", "&s", &xa_metadata);
              if (xa_metadata == NULL)
                g_printerr (_("Warning: Commit %s has no flatpak metadata\n"), c);
              else
                {
                  metakey = g_key_file_new ();
                  if (!g_key_file_load_from_data (metakey, xa_metadata, -1, 0, error))
                    return FALSE;
                }
            }

          if (opt_show_ref)
            {
              maybe_print_space (&first);
              g_print ("%s", ref);
            }

          if (opt_show_commit)
            {
              maybe_print_space (&first);
              g_print ("%s", c);
            }

          if (opt_show_parent)
            {
              maybe_print_space (&first);
              g_print ("%s", p ? p : "-");
            }

          if (opt_show_runtime)
            {
              g_autofree char *runtime = NULL;
              maybe_print_space (&first);

              if (metakey)
                {
                  if (strcmp (parts[0], "app") == 0)
                    runtime = g_key_file_get_string (metakey, "Application", "runtime", NULL);
                  else
                    runtime = g_key_file_get_string (metakey, "Runtime", "runtime", NULL);
                }
              g_print ("%s", runtime ? runtime : "-");
            }

          if (opt_show_sdk)
            {
              g_autofree char *sdk = NULL;
              maybe_print_space (&first);

              if (metakey)
                {
                  if (strcmp (parts[0], "app") == 0)
                    sdk = g_key_file_get_string (metakey, "Application", "sdk", NULL);
                  else
                    sdk = g_key_file_get_string (metakey, "Runtime", "sdk", NULL);
                }
              g_print ("%s", sdk ? sdk : "-");
            }

          if (!first)
            g_print ("\n");

          if (opt_show_metadata)
            {
              g_print ("%s", xa_metadata ? xa_metadata : "");
              if (xa_metadata == NULL || !g_str_has_suffix (xa_metadata, "\n"))
                g_print ("\n");
            }

          g_free (c);
          c = g_steal_pointer (&p);

          if (c_v)
            g_variant_unref (c_v);
          c_v = NULL;

          if (c && opt_log)
            c_v = flatpak_dir_fetch_remote_commit (preferred_dir, remote, ref, c, NULL, NULL, cancellable, NULL);
        }
      while (c_v != NULL);
    }

  return TRUE;
}

gboolean
flatpak_complete_remote_info (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakKinds kinds;
  int i;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, NULL, NULL))
    return FALSE;

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  switch (completion->argc)
    {
    case 0:
    case 1: /* REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          int j;
          g_auto(GStrv) remotes = flatpak_dir_list_remotes (dir, NULL, NULL);
          if (remotes == NULL)
            return FALSE;
          for (j = 0; remotes[j] != NULL; j++)
            flatpak_complete_word (completion, "%s ", remotes[j]);
        }

      break;

    default: /* REF */
      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          flatpak_complete_partial_ref (completion, kinds, opt_arch, dir, completion->argv[1]);
        }

      break;
    }

  return TRUE;
}
