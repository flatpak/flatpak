/*
 * Copyright © 2016 Red Hat, Inc
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
#include "flatpak-utils-private.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-run-private.h"
#include "flatpak-variant-impl-private.h"

static gboolean opt_user;
static gboolean opt_system;
static gboolean opt_show_ref;
static gboolean opt_show_commit;
static gboolean opt_show_origin;
static gboolean opt_show_size;
static gboolean opt_show_metadata;
static gboolean opt_show_runtime;
static gboolean opt_show_sdk;
static gboolean opt_show_permissions;
static gboolean opt_show_extensions;
static gboolean opt_show_location;
static char *opt_arch;
static char **opt_installations;
static char *opt_file_access;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to use"), N_("ARCH") },
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, N_("Show user installations"), NULL },
  { "system", 0, 0, G_OPTION_ARG_NONE, &opt_system, N_("Show system-wide installations"), NULL },
  { "installation", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_installations, N_("Show specific system-wide installations"), N_("NAME") },
  { "show-ref", 'r', 0, G_OPTION_ARG_NONE, &opt_show_ref, N_("Show ref"), NULL },
  { "show-commit", 'c', 0, G_OPTION_ARG_NONE, &opt_show_commit, N_("Show commit"), NULL },
  { "show-origin", 'o', 0, G_OPTION_ARG_NONE, &opt_show_origin, N_("Show origin"), NULL },
  { "show-size", 's', 0, G_OPTION_ARG_NONE, &opt_show_size, N_("Show size"), NULL },
  { "show-metadata", 'm', 0, G_OPTION_ARG_NONE, &opt_show_metadata, N_("Show metadata"), NULL },
  { "show-runtime", 0, 0, G_OPTION_ARG_NONE, &opt_show_runtime, N_("Show runtime"), NULL },
  { "show-sdk", 0, 0, G_OPTION_ARG_NONE, &opt_show_sdk, N_("Show sdk"), NULL },
  { "show-permissions", 'M', 0, G_OPTION_ARG_NONE, &opt_show_permissions, N_("Show permissions"), NULL },
  { "file-access", 0, 0, G_OPTION_ARG_FILENAME, &opt_file_access, N_("Query file access"), N_("PATH") },
  { "show-extensions", 'e', 0, G_OPTION_ARG_NONE, &opt_show_extensions, N_("Show extensions"), NULL },
  { "show-location", 'l', 0, G_OPTION_ARG_NONE, &opt_show_location, N_("Show location"), NULL },
  { NULL }
};

/* Print space unless this is the first item */
static void
maybe_print_space (gboolean *first)
{
  if (*first)
    *first = FALSE;
  else
    g_print (" ");
}

gboolean
flatpak_builtin_info (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autofree char *ref_str = NULL;
  g_autoptr(FlatpakDecomposed) ref = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GBytes) deploy_data = NULL;
  g_autoptr(FlatpakDeploy) deploy = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  const char *commit = NULL;
  const char *alt_id = NULL;
  const char *eol;
  const char *eol_rebase;
  const char *name;
  const char *summary;
  const char *version;
  const char *license;
  const char *pref = NULL;
  const char *default_branch = NULL;
  const char *origin = NULL;
  guint64 size;
  gboolean search_all = FALSE;
  gboolean first = TRUE;
  FlatpakKinds kinds;
  const char *path;
  g_autofree char *formatted_size = NULL;
  gboolean friendly = TRUE;
  g_autofree const char **subpaths = NULL;
  int len = 0;
  int rows, cols;
  int width;

  context = g_option_context_new (_("NAME [BRANCH] - Get info about an installed app or runtime"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("NAME must be specified"), error);
  pref = argv[1];

  if (argc >= 3)
    default_branch = argv[2];

  if (argc > 3)
    return usage_error (context, _("Too many arguments"), error);

  kinds = FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME;

  if (!opt_user && !opt_system && opt_installations == NULL)
    search_all = TRUE;

  dir = flatpak_find_installed_pref (pref, kinds, opt_arch, default_branch,
                                     search_all, opt_user, opt_system, opt_installations,
                                     &ref, cancellable, error);
  if (dir == NULL)
    return FALSE;

  deploy_data = flatpak_dir_get_deploy_data (dir, flatpak_decomposed_get_ref (ref), FLATPAK_DEPLOY_VERSION_CURRENT, cancellable, error);
  if (deploy_data == NULL)
    return FALSE;

  deploy = flatpak_dir_load_deployed (dir, ref, NULL, cancellable, error);
  if (deploy == NULL)
    return FALSE;

  commit = flatpak_deploy_data_get_commit (deploy_data);
  alt_id = flatpak_deploy_data_get_alt_id (deploy_data);
  origin = flatpak_deploy_data_get_origin (deploy_data);
  size = flatpak_deploy_data_get_installed_size (deploy_data);
  formatted_size = g_format_size (size);
  deploy_dir = flatpak_deploy_get_dir (deploy);
  path = flatpak_file_get_path_cached (deploy_dir);
  subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
  eol = flatpak_deploy_data_get_eol (deploy_data);
  eol_rebase = flatpak_deploy_data_get_eol_rebase (deploy_data);
  name = flatpak_deploy_data_get_appdata_name (deploy_data);
  summary = flatpak_deploy_data_get_appdata_summary (deploy_data);
  version = flatpak_deploy_data_get_appdata_version (deploy_data);
  license = flatpak_deploy_data_get_appdata_license (deploy_data);

  metakey = flatpak_deploy_get_metadata (deploy);

  if (opt_show_ref || opt_show_origin || opt_show_commit || opt_show_size || opt_show_metadata || opt_show_permissions ||
      opt_file_access || opt_show_location || opt_show_runtime || opt_show_sdk)
    friendly = FALSE;

  if (friendly)
    {
      g_autoptr(GVariant) commit_v = NULL;
      VarMetadataRef commit_metadata;
      guint64 timestamp;
      g_autofree char *formatted_timestamp = NULL;
      const gchar *subject = NULL;
      g_autofree char *parent = NULL;
      g_autofree char *latest = NULL;
      const char *xa_metadata = NULL;
      const char *collection_id = NULL;

      flatpak_get_window_size (&rows, &cols);

      if (name)
        {
          if (summary)
            print_wrapped (MIN (cols, 80), "\n%s - %s\n", name, summary);
          else
            print_wrapped (MIN (cols, 80), "\n%s\n", name);
        }

      latest = flatpak_dir_read_latest (dir, origin, flatpak_decomposed_get_ref (ref), NULL, NULL, NULL);
      if (latest == NULL)
        latest = g_strdup (_("ref not present in origin"));

      if (ostree_repo_load_commit (flatpak_dir_get_repo (dir), commit, &commit_v, NULL, NULL))
        {
          VarCommitRef var_commit = var_commit_from_gvariant (commit_v);

          subject = var_commit_get_subject (var_commit);
          parent = ostree_commit_get_parent (commit_v);
          timestamp = ostree_commit_get_timestamp (commit_v);

          formatted_timestamp = format_timestamp (timestamp);

          commit_metadata = var_commit_get_metadata (var_commit);
          xa_metadata = var_metadata_lookup_string (commit_metadata, "xa.metadata", NULL);
          if (xa_metadata == NULL)
            g_printerr (_("Warning: Commit has no flatpak metadata\n"));

          collection_id = var_metadata_lookup_string (commit_metadata, "ostree.collection-binding", NULL);
        }

      len = 0;
      len = MAX (len, g_utf8_strlen (_("ID:"), -1));
      len = MAX (len, g_utf8_strlen (_("Ref:"), -1));
      len = MAX (len, g_utf8_strlen (_("Arch:"), -1));
      len = MAX (len, g_utf8_strlen (_("Branch:"), -1));
      if (version)
        len = MAX (len, g_utf8_strlen (_("Version:"), -1));
      if (license)
        len = MAX (len, g_utf8_strlen (_("License:"), -1));
      if (collection_id != NULL)
        len = MAX (len, g_utf8_strlen (_("Collection:"), -1));
      len = MAX (len, g_utf8_strlen (_("Installation:"), -1));
      len = MAX (len, g_utf8_strlen (_("Installed:"), -1));
      if (flatpak_decomposed_is_app (ref))
        {
          len = MAX (len, g_utf8_strlen (_("Runtime:"), -1));
          len = MAX (len, g_utf8_strlen (_("Sdk:"), -1));
        }
      if (formatted_timestamp)
        len = MAX (len, g_utf8_strlen (_("Date:"), -1));
      if (subject)
        len = MAX (len, g_utf8_strlen (_("Subject:"), -1));
      if (strcmp (commit, latest) != 0)
        {
          len = MAX (len, g_utf8_strlen (_("Active commit:"), -1));
          len = MAX (len, g_utf8_strlen (_("Latest commit:"), -1));
        }
      else
        len = MAX (len, g_utf8_strlen (_("Commit:"), -1));
      if (parent)
        len = MAX (len, g_utf8_strlen (_("Parent:"), -1));
      if (alt_id)
        len = MAX (len, g_utf8_strlen (_("Alt-id:"), -1));
      if (eol)
        len = MAX (len, g_utf8_strlen (_("End-of-life:"), -1));
      if (eol_rebase)
        len = MAX (len, g_utf8_strlen (_("End-of-life-rebase:"), -1));
      if (subpaths[0] != NULL)
        len = MAX (len, g_utf8_strlen (_("Subdirectories:"), -1));
      len = MAX (len, g_utf8_strlen (_("Extension:"), -1));

      width = cols - (len + 1);

      print_aligned_take (len, _("ID:"), flatpak_decomposed_dup_id (ref));
      print_aligned (len, _("Ref:"), flatpak_decomposed_get_ref (ref));
      print_aligned_take (len, _("Arch:"), flatpak_decomposed_dup_arch (ref));
      print_aligned_take (len, _("Branch:"), flatpak_decomposed_dup_branch (ref));
      if (version)
        print_aligned (len, _("Version:"), version);
      if (license)
        print_aligned (len, _("License:"), license);
      print_aligned (len, _("Origin:"), origin ? origin : "-");
      if (collection_id)
        print_aligned (len, _("Collection:"), collection_id);
      print_aligned (len, _("Installation:"), flatpak_dir_get_name_cached (dir));
      print_aligned (len, _("Installed:"), formatted_size);
      if (flatpak_decomposed_is_app (ref))
        {
          g_autofree char *runtime = NULL;
          runtime = g_key_file_get_string (metakey,
                                           FLATPAK_METADATA_GROUP_APPLICATION,
                                           FLATPAK_METADATA_KEY_RUNTIME,
                                           error);
          print_aligned (len, _("Runtime:"), runtime ? runtime : "-");
        }
      if (flatpak_decomposed_is_app (ref))
        {
          g_autofree char *sdk = NULL;
          sdk = g_key_file_get_string (metakey,
                                       FLATPAK_METADATA_GROUP_APPLICATION,
                                       FLATPAK_METADATA_KEY_SDK,
                                       error);
          print_aligned (len, _("Sdk:"), sdk ? sdk : "-");
        }
      g_print ("\n");

      if (strcmp (commit, latest) != 0)
        {
          g_autofree char *formatted_commit = ellipsize_string (commit, width);
          print_aligned (len, _("Active commit:"), formatted_commit);
          g_free (formatted_commit);
          formatted_commit = ellipsize_string (latest, width);
          print_aligned (len, _("Latest commit:"), formatted_commit);
        }
      else
        {
          g_autofree char *formatted_commit = ellipsize_string (commit, width);
          print_aligned (len, _("Commit:"), formatted_commit);
        }
      if (parent)
        {
          g_autofree char *formatted_commit = ellipsize_string (parent, width);
          print_aligned (len, _("Parent:"), formatted_commit);
        }
      if (subject)
        print_aligned (len, _("Subject:"), subject);
      if (formatted_timestamp)
        print_aligned (len, _("Date:"), formatted_timestamp);
      if (subpaths[0] != NULL)
        {
          g_autofree char *s = g_strjoinv (",", (char **) subpaths);
          print_aligned (len, _("Subdirectories:"), s);
        }

      if (alt_id)
        print_aligned (len, _("Alt-id:"), alt_id);
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
    }
  else
    {
      if (opt_show_ref)
        {
          maybe_print_space (&first);
          g_print ("%s", flatpak_decomposed_get_ref (ref));
        }

      if (opt_show_origin)
        {
          maybe_print_space (&first);
          g_print ("%s", origin ? origin : "-");
        }

      if (opt_show_commit)
        {
          maybe_print_space (&first);
          g_print ("%s", commit);
        }

      if (opt_show_size)
        {
          maybe_print_space (&first);
          g_print ("%" G_GUINT64_FORMAT, size);
        }

      if (opt_show_location)
        {
          maybe_print_space (&first);
          g_print ("%s", path);
        }

      if (opt_show_runtime)
        {
          g_autofree char *runtime = NULL;
          maybe_print_space (&first);

          runtime = g_key_file_get_string (metakey,
                                           flatpak_decomposed_get_kind_metadata_group (ref),
                                           FLATPAK_METADATA_KEY_RUNTIME,
                                           NULL);
          g_print ("%s", runtime ? runtime : "-");
        }

      if (opt_show_sdk)
        {
          g_autofree char *sdk = NULL;
          maybe_print_space (&first);

          sdk = g_key_file_get_string (metakey,
                                       flatpak_decomposed_get_kind_metadata_group (ref),
                                       FLATPAK_METADATA_KEY_SDK,
                                       NULL);
          g_print ("%s", sdk ? sdk : "-");
        }

      if (!first)
        g_print ("\n");

      if (opt_show_metadata)
        {
          g_autoptr(GFile) file = NULL;
          g_autofree char *data = NULL;
          gsize data_size;

          file = g_file_get_child (deploy_dir, "metadata");

          if (!g_file_load_contents (file, cancellable, &data, &data_size, NULL, error))
            return FALSE;

          g_print ("%s", data);
        }

      if (opt_show_permissions || opt_file_access)
        {
          g_autoptr(FlatpakContext) app_context = NULL;
          g_autoptr(GKeyFile) keyfile = NULL;
          g_autofree gchar *contents = NULL;

          app_context = flatpak_context_load_for_deploy (deploy, error);
          if (app_context == NULL)
            return FALSE;

          if (opt_show_permissions)
            {
              keyfile = g_key_file_new ();
              flatpak_context_save_metadata (app_context, TRUE, keyfile);
              contents = g_key_file_to_data (keyfile, NULL, error);
              if (contents == NULL)
                return FALSE;

              g_print ("%s", contents);
            }

          if (opt_file_access)
            {
              g_autofree char *id = flatpak_decomposed_dup_id (ref);
              g_autoptr(FlatpakExports) exports = flatpak_context_get_exports (app_context, id);
              FlatpakFilesystemMode mode;

              mode = flatpak_exports_path_get_mode (exports, opt_file_access);
              if (mode == 0)
                g_print ("hidden\n");
              else if (mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY)
                g_print ("read-only\n");
              else
                g_print ("read-write\n");
            }
        }
    }

  if (opt_show_extensions)
    {
      GList *extensions, *l;
      g_autofree char *ref_arch = flatpak_decomposed_dup_arch (ref);
      g_autofree char *ref_branch = flatpak_decomposed_dup_branch (ref);

      len = MAX (len, g_utf8_strlen (_("Extension:"), -1));
      len = MAX (len, g_utf8_strlen (_("ID:"), -1));
      len = MAX (len, g_utf8_strlen (_("Origin:"), -1));
      len = MAX (len, g_utf8_strlen (_("Commit:"), -1));
      len = MAX (len, g_utf8_strlen (_("Installed:"), -1));
      len = MAX (len, g_utf8_strlen (_("Subpaths:"), -1));

      flatpak_get_window_size (&rows, &cols);
      width = cols - (len + 1);

      extensions = flatpak_list_extensions (metakey, ref_arch, ref_branch);
      for (l = extensions; l; l = l->next)
        {
          FlatpakExtension *ext = l->data;
          g_autofree const char **ext_subpaths = NULL;
          g_autoptr(GBytes) ext_deploy_data = NULL;
          g_autofree char *formatted = NULL;
          g_autofree char *ext_formatted_size = NULL;
          g_autofree char *formatted_commit = NULL;

          if (ext->is_unmaintained)
            {
              formatted_commit = g_strdup (_("unmaintained"));
              origin = NULL;
              size = 0;
              ext_formatted_size = g_strdup (_("unknown"));
              ext_subpaths = NULL;
            }
          else
            {
              ext_deploy_data = flatpak_dir_get_deploy_data (dir, ext->ref, FLATPAK_DEPLOY_VERSION_CURRENT, cancellable, error);
              if (ext_deploy_data == NULL)
                return FALSE;

              commit = flatpak_deploy_data_get_commit (ext_deploy_data);
              formatted_commit = ellipsize_string (commit, width);
              origin = flatpak_deploy_data_get_origin (ext_deploy_data);
              size = flatpak_deploy_data_get_installed_size (ext_deploy_data);
              formatted = g_format_size (size);
              ext_subpaths = flatpak_deploy_data_get_subpaths (ext_deploy_data);
              if (ext_subpaths && ext_subpaths[0] && size > 0)
                ext_formatted_size = g_strconcat ("<", formatted, NULL);
              else
                ext_formatted_size = g_steal_pointer (&formatted);
            }

          g_print ("\n");
          print_aligned (len, _("Extension:"), ext->ref);
          print_aligned (len, _("ID:"), ext->id);
          print_aligned (len, _("Origin:"), origin ? origin : "-");
          print_aligned (len, _("Commit:"), formatted_commit);
          print_aligned (len, _("Installed:"), ext_formatted_size);

          if (ext_subpaths && ext_subpaths[0])
            {
              g_autofree char *s = g_strjoinv (",", (char **) ext_subpaths);
              print_aligned (len, _("Subpaths:"), s);
            }
        }

      g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);
    }

  return TRUE;
}

gboolean
flatpak_complete_info (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GError) error = NULL;
  FlatpakKinds kinds;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO, &dirs, NULL, NULL))
    return FALSE;

  kinds = FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME;

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, opt_arch,
                                                                       kinds, FIND_MATCHING_REFS_FLAGS_NONE, &error);
          if (refs == NULL)
            flatpak_completion_debug ("find local refs error: %s", error->message);

          flatpak_complete_ref_id (completion, refs);
        }
      break;

    case 2: /* BRANCH */
      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (dir, completion->argv[1], NULL, opt_arch,
                                                                       kinds, FIND_MATCHING_REFS_FLAGS_NONE, &error);
          if (refs == NULL)
            flatpak_completion_debug ("find remote refs error: %s", error->message);

          flatpak_complete_ref_branch (completion, refs);
        }

      break;

    default:
      break;
    }

  return TRUE;
}
