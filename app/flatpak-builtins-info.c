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
#include "flatpak-utils.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-run.h"

static gboolean opt_user;
static gboolean opt_system;
static gboolean opt_show_ref;
static gboolean opt_show_commit;
static gboolean opt_show_origin;
static gboolean opt_show_size;
static gboolean opt_show_metadata;
static gboolean opt_show_permissions;
static gboolean opt_show_extensions;
static char *opt_arch;
static char **opt_installations;
static char *opt_file_access;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to use"), N_("ARCH") },
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, N_("Show user installations"), NULL },
  { "system", 0, 0, G_OPTION_ARG_NONE, &opt_system, N_("Show system-wide installations"), NULL },
  { "installation", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_installations, N_("Show specific system-wide installations"), NULL },
  { "show-ref", 'r', 0, G_OPTION_ARG_NONE, &opt_show_ref, N_("Show ref"), NULL },
  { "show-commit", 'c', 0, G_OPTION_ARG_NONE, &opt_show_commit, N_("Show commit"), NULL },
  { "show-origin", 'o', 0, G_OPTION_ARG_NONE, &opt_show_origin, N_("Show origin"), NULL },
  { "show-size", 's', 0, G_OPTION_ARG_NONE, &opt_show_size, N_("Show size"), NULL },
  { "show-metadata", 'm', 0, G_OPTION_ARG_NONE, &opt_show_metadata, N_("Show metadata"), NULL },
  { "show-permissions", 'M', 0, G_OPTION_ARG_NONE, &opt_show_permissions, N_("Show permissions"), NULL },
  { "file-access", 0, 0, G_OPTION_ARG_FILENAME, &opt_file_access, N_("Query file access"), N_("PATH") },
  { "show-extensions", 'e', 0, G_OPTION_ARG_NONE, &opt_show_extensions, N_("Show extensions"), NULL },
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

static gchar *
format_timestamp (guint64  timestamp)
{
  GDateTime *dt;
  gchar *str;

  dt = g_date_time_new_from_unix_utc (timestamp);
  if (dt == NULL)
    return g_strdup ("?");

  str = g_date_time_format (dt, "%Y-%m-%d %H:%M:%S +0000");
  g_date_time_unref (dt);

  return str;
}

gboolean
flatpak_builtin_info (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autofree char *ref = NULL;
  FlatpakDir *dir = NULL;
  g_autoptr(GVariant) deploy_data = NULL;
  g_autoptr(FlatpakDeploy) deploy = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  const char *commit = NULL;
  const char *alt_id = NULL;
  const char *pref = NULL;
  const char *default_branch = NULL;
  const char *origin = NULL;
  guint64 size;
  gboolean search_all = FALSE;
  gboolean first = TRUE;
  FlatpakKinds kinds;
  const char *on = "";
  const char *off = "";
  g_auto(GStrv) parts = NULL;
  g_autofree char *path = NULL;
  g_autofree char *formatted = NULL;
  gboolean friendly = TRUE;
  g_autofree const char **subpaths = NULL;

  context = g_option_context_new (_("NAME [BRANCH] - Get info about installed app and/or runtime"));
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

  deploy_data = flatpak_dir_get_deploy_data (dir, ref, cancellable, error);
  if (deploy_data == NULL)
    return FALSE;

  deploy = flatpak_find_deploy_for_ref (ref, NULL, NULL, error);
  if (deploy == NULL)
    return FALSE;

  if (flatpak_fancy_output ())
    {
      on = FLATPAK_ANSI_BOLD_ON;
      off = FLATPAK_ANSI_BOLD_OFF; /* bold off */
    }

  parts = g_strsplit (ref, "/", 0);

  commit = flatpak_deploy_data_get_commit (deploy_data);
  alt_id = flatpak_deploy_data_get_alt_id (deploy_data);
  origin = flatpak_deploy_data_get_origin (deploy_data);
  size = flatpak_deploy_data_get_installed_size (deploy_data);
  formatted = g_format_size (size);
  path = g_file_get_path (flatpak_deploy_get_dir (deploy));
  subpaths = flatpak_deploy_data_get_subpaths (deploy_data);

  metakey = flatpak_deploy_get_metadata (deploy);

  if (opt_show_ref || opt_show_origin || opt_show_commit || opt_show_size || opt_show_metadata || opt_show_permissions || opt_file_access)
    friendly = FALSE;

  if (friendly)
    {
      g_autoptr(GVariant) commit_v = NULL;
      g_autoptr(GVariant) commit_metadata = NULL;
      guint64 timestamp;
      g_autofree char *formatted_timestamp = NULL;
      const gchar *subject = NULL;
      const gchar *body = NULL;
      g_autofree char *parent = NULL;
      const char *latest;
      const char *xa_metadata = NULL;
      const char *collection_id = NULL;

      latest = flatpak_dir_read_latest (dir, origin, ref, NULL, NULL, NULL);
      if (latest == NULL)
        latest = _("ref not present in origin");

      if (ostree_repo_load_commit (flatpak_dir_get_repo (dir), commit, &commit_v, NULL, NULL))
        {
          g_variant_get (commit_v, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                         &subject, &body, NULL, NULL, NULL);
          parent = ostree_commit_get_parent (commit_v);
          timestamp = ostree_commit_get_timestamp (commit_v);
          formatted_timestamp = format_timestamp (timestamp);

          commit_metadata = g_variant_get_child_value (commit_v, 0);
          g_variant_lookup (commit_metadata, "xa.metadata", "&s", &xa_metadata);
          if (xa_metadata == NULL)
            g_printerr (_("Warning: Commit has no flatpak metadata\n"));

#ifdef FLATPAK_ENABLE_P2P
          g_variant_lookup (commit_metadata, "ostree.collection-binding", "&s", &collection_id);
#endif
        }

      g_print ("%s%s%s %s\n", on, _("Ref:"), off, ref);
      g_print ("%s%s%s %s\n", on, _("ID:"), off, parts[1]);
      g_print ("%s%s%s %s\n", on, _("Arch:"), off, parts[2]);
      g_print ("%s%s%s %s\n", on, _("Branch:"), off, parts[3]);
      g_print ("%s%s%s %s\n", on, _("Origin:"), off, origin ? origin : "-");
      if (collection_id)
        g_print ("%s%s%s %s\n", on, _("Collection ID:"), off, collection_id);
      if (formatted_timestamp)
        g_print ("%s%s%s %s\n", on, _("Date:"), off, formatted_timestamp);
      if (subject)
        g_print ("%s%s%s %s\n", on, _("Subject:"), off, subject);

      if (strcmp (commit, latest) != 0)
        {
          g_print ("%s%s%s %s\n", on, _("Active commit:"), off, commit);
          g_print ("%s%s%s %s\n", on, _("Latest commit:"), off, latest);
        }
      else
        g_print ("%s%s%s %s\n", on, _("Commit:"), off, commit);
      if (alt_id)
        g_print ("%s%s%s %s\n", on, _("alt-id:"), off, alt_id);
      g_print ("%s%s%s %s\n", on, _("Parent:"), off, parent ? parent : "-");
      g_print ("%s%s%s %s\n", on, _("Location:"), off, path);
      g_print ("%s%s%s %s\n", on, _("Installed size:"), off, formatted);
      if (strcmp (parts[0], "app") == 0)
        {
          g_autofree char *runtime = NULL;
          runtime = g_key_file_get_string (metakey, "Application", "runtime", error);
          g_print ("%s%s%s %s\n", on, _("Runtime:"), off, runtime ? runtime : "-");
        }
      if (subpaths[0] != NULL)
        {
          int i;
          g_print ("%s%s%s ", on, _("Installed subdirectories:"), off);
          for (i = 0; subpaths[i] != NULL; i++)
            g_print (i == 0 ? "%s" : ",%s", subpaths[i]);
          g_print ("\n");
        }
    }
  else
    {
      if (opt_show_ref)
        {
          maybe_print_space (&first);
          g_print ("%s", ref);
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
          g_print ("%s", formatted);
        }

      if (!first)
        g_print ("\n");

      if (opt_show_metadata)
        {
          g_autoptr(GFile) deploy_dir = NULL;
          g_autoptr(GFile) file = NULL;
          g_autofree char *data = NULL;
          gsize data_size;

          deploy_dir = flatpak_dir_get_if_deployed (dir, ref, NULL, cancellable);
          file = g_file_get_child (deploy_dir, "metadata");

          if (!g_file_load_contents (file, cancellable, &data, &data_size, NULL, error))
            return FALSE;

          g_print ("%s", data);
        }

      if (opt_show_permissions || opt_file_access)
        {
          g_autoptr(FlatpakContext) context = NULL;
          g_autoptr(GKeyFile) keyfile = NULL;
          g_autofree gchar *contents = NULL;

          context = flatpak_context_load_for_deploy (deploy, error);
          if (context == NULL)
            return FALSE;

          if (opt_show_permissions)
            {
              keyfile = g_key_file_new ();
              flatpak_context_save_metadata (context, TRUE, keyfile);
              contents = g_key_file_to_data (keyfile, NULL, error);
              if (contents == NULL)
                return FALSE;

              g_print ("%s", contents);
            }

          if (opt_file_access)
            {
              g_autoptr(FlatpakExports) exports = flatpak_context_get_exports (context, parts[1]);
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

       extensions = flatpak_list_extensions (metakey, parts[2], parts[3]);
       for (l = extensions; l; l = l->next)
         {
           FlatpakExtension *ext = l->data;
           g_autofree const char **subpaths = NULL;
           g_autoptr(GVariant) ext_deploy_data = NULL;
           g_autofree char *formatted = NULL;

           if (ext->is_unmaintained)
             {
               commit = "unmaintained";
               origin = NULL;
               size = 0;
               formatted = g_strdup ("unknown");
               subpaths = NULL;
             }
           else
             {
               ext_deploy_data = flatpak_dir_get_deploy_data (dir, ext->ref, cancellable, error);
               if (ext_deploy_data == NULL)
                 return FALSE;

               commit = flatpak_deploy_data_get_commit (ext_deploy_data);
               origin = flatpak_deploy_data_get_origin (ext_deploy_data);
               size = flatpak_deploy_data_get_installed_size (ext_deploy_data);
               formatted = g_format_size (size);
               subpaths = flatpak_deploy_data_get_subpaths (ext_deploy_data);
             }

           g_print ("\n%s%s%s %s\n", on, _("Extension:"), off, ext->ref);
           g_print ("%s%s%s %s\n", on, _("ID:"), off, ext->id);
           g_print ("%s%s%s %s\n", on, _("Origin:"), off, origin ? origin : "-");
           g_print ("%s%s%s %s\n", on, _("Commit:"), off, commit);
           g_print ("%s%s%s %s%s\n", on, _("Installed size:"), off, subpaths && subpaths[0] ? "<" : "", formatted);

           if (subpaths && subpaths[0])
             {
               g_autofree char *subpath_str = NULL;

               subpath_str = g_strjoinv (",", (char **)subpaths);
               g_print ("%s%s%s %s\n", on, _("Subpaths:"), off, subpath_str);
             }
         }
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
  int i, j;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS, &dirs, NULL, NULL))
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
          g_auto(GStrv) refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, opt_arch,
                                                                kinds, &error);
          if (refs == NULL)
            flatpak_completion_debug ("find local refs error: %s", error->message);
          for (j = 0; refs != NULL && refs[j] != NULL; j++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (refs[j], NULL);
              if (parts)
                flatpak_complete_word (completion, "%s ", parts[1]);
            }
        }
      break;

    case 2: /* BRANCH */
      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          g_auto(GStrv) refs = flatpak_dir_find_installed_refs (dir, completion->argv[1], NULL, opt_arch,
                                                                kinds, &error);
          if (refs == NULL)
            flatpak_completion_debug ("find remote refs error: %s", error->message);
          for (j = 0; refs != NULL && refs[j] != NULL; j++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (refs[j], NULL);
              if (parts)
                flatpak_complete_word (completion, "%s ", parts[3]);
            }
        }

      break;

    default:
      break;
    }

  return TRUE;
}
