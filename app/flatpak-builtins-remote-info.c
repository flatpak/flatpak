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

static gchar *
format_timestamp (guint64 timestamp)
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
flatpak_builtin_remote_info (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(FlatpakDir) preferred_dir = NULL;
  g_autoptr(GVariant) commit_v = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;
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
  const char *on = "";
  const char *off = "";
  gboolean friendly = TRUE;
  const char *xa_metadata = NULL;
  const char *collection_id = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  guint64 installed_size = 0;
  guint64 download_size = 0;
  g_autofree char *formatted_installed_size = NULL;
  g_autofree char *formatted_download_size = NULL;
  const gchar *subject;
  const gchar *body;
  guint64 timestamp;
  g_autofree char *formatted_timestamp = NULL;

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

  ref = flatpak_dir_find_remote_ref (preferred_dir, remote, id, branch, default_branch, arch,
                                     matched_kinds, &kind, cancellable, error);
  if (ref == NULL)
    return FALSE;

  commit_v = flatpak_dir_fetch_remote_commit (preferred_dir, remote, ref, opt_commit, &commit, cancellable, error);
  if (commit_v == NULL)
    return FALSE;

  if (flatpak_fancy_output ())
    {
      on = FLATPAK_ANSI_BOLD_ON;
      off = FLATPAK_ANSI_BOLD_OFF; /* bold off */
    }

  if (opt_show_ref || opt_show_commit || opt_show_parent || opt_show_metadata || opt_show_runtime || opt_show_sdk)
    friendly = FALSE;

  parts = g_strsplit (ref, "/", 0);

  if (friendly)
    {
      g_variant_get (commit_v, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                     &subject, &body, NULL, NULL, NULL);

      parent = ostree_commit_get_parent (commit_v);
      timestamp = ostree_commit_get_timestamp (commit_v);

      commit_metadata = g_variant_get_child_value (commit_v, 0);
      g_variant_lookup (commit_metadata, "xa.metadata", "&s", &xa_metadata);
      if (xa_metadata == NULL)
        g_printerr (_("Warning: Commit has no flatpak metadata\n"));
      else
        {
          metakey = g_key_file_new ();
          if (!g_key_file_load_from_data (metakey, xa_metadata, -1, 0, error))
            return FALSE;
        }

      g_variant_lookup (commit_metadata, "ostree.collection-binding", "&s", &collection_id);

      if (g_variant_lookup (commit_metadata, "xa.installed-size", "t", &installed_size))
        installed_size = GUINT64_FROM_BE (installed_size);

      if (g_variant_lookup (commit_metadata, "xa.download-size", "t", &download_size))
        download_size = GUINT64_FROM_BE (download_size);

      formatted_installed_size = g_format_size (installed_size);
      formatted_download_size = g_format_size (download_size);
      formatted_timestamp = format_timestamp (timestamp);

      g_print ("%s%s%s %s\n", on, _("Ref:"), off, ref);
      g_print ("%s%s%s %s\n", on, _("ID:"), off, parts[1]);
      g_print ("%s%s%s %s\n", on, _("Arch:"), off, parts[2]);
      g_print ("%s%s%s %s\n", on, _("Branch:"), off, parts[3]);
      if (collection_id != NULL)
        g_print ("%s%s%s %s\n", on, _("Collection ID:"), off, collection_id);
      g_print ("%s%s%s %s\n", on, _("Date:"), off, formatted_timestamp);
      g_print ("%s%s%s %s\n", on, _("Subject:"), off, subject);
      g_print ("%s%s%s %s\n", on, _("Commit:"), off, commit);
      g_print ("%s%s%s %s\n", on, _("Parent:"), off, parent ? parent : "-");
      g_print ("%s%s%s %s\n", on, _("Download size:"), off, formatted_download_size);
      g_print ("%s%s%s %s\n", on, _("Installed size:"), off, formatted_installed_size);
      if (strcmp (parts[0], "app") == 0 && metakey != NULL)
        {
          g_autofree char *runtime = NULL;
          g_autofree char *sdk = NULL;
          runtime = g_key_file_get_string (metakey, "Application", "runtime", error);
          g_print ("%s%s%s %s\n", on, _("Runtime:"), off, runtime ? runtime : "-");
          sdk = g_key_file_get_string (metakey, "Application", "sdk", error);
          g_print ("%s%s%s %s\n", on, _("Sdk:"), off, sdk ? sdk : "-");
        }

      if (opt_log)
        {
          g_autofree char *p = g_strdup (parent);

          g_print ("%s%s%s", on, _("History:\n"), off);

          while (p)
            {
              g_autofree char *p_parent = NULL;
              const gchar *p_subject;
              guint64 p_timestamp;
              g_autofree char *p_formatted_timestamp = NULL;
              g_autoptr(GVariant) p_commit_v = NULL;

              p_commit_v = flatpak_dir_fetch_remote_commit (preferred_dir, remote, ref, p, NULL, cancellable, NULL);
              if (p_commit_v == NULL)
                break;

              p_parent = ostree_commit_get_parent (p_commit_v);
              p_timestamp = ostree_commit_get_timestamp (p_commit_v);
              p_formatted_timestamp = format_timestamp (p_timestamp);

              g_variant_get (p_commit_v, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                             &p_subject, NULL, NULL, NULL, NULL);

              g_print ("%s%s%s %s\n", on, _(" Subject:"), off, p_subject);
              g_print ("%s%s%s %s\n", on, _(" Date:"), off, p_formatted_timestamp);
              g_print ("%s%s%s %s\n", on, _(" Commit:"), off, p);

              g_free (p);
              p = g_steal_pointer (&p_parent);
              if (p)
                g_print ("\n");
            }
        }
    }
  else
    {
      g_autoptr(GVariant) c_v = g_variant_ref (commit_v);
      g_autofree char *c = g_strdup (commit);

      do
        {
          g_autofree char *p = ostree_commit_get_parent (c_v);
          g_autoptr(GVariant) c_m = g_variant_get_child_value (c_v, 0);
          gboolean first = TRUE;

          g_variant_lookup (c_m, "xa.metadata", "&s", &xa_metadata);
          if (xa_metadata == NULL)
            g_printerr (_("Warning: Commit %s has no flatpak metadata\n"), c);
          else
            {
              metakey = g_key_file_new ();
              if (!g_key_file_load_from_data (metakey, xa_metadata, -1, 0, error))
                return FALSE;
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
            g_print ("%s", xa_metadata);

          g_free (c);
          c = g_steal_pointer (&p);

          g_variant_unref (c_v);
          c_v = NULL;

          if (c && opt_log)
            c_v = flatpak_dir_fetch_remote_commit (preferred_dir, remote, ref, c, NULL, cancellable, NULL);
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
