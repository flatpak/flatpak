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
 *       Matthias Clasen <mclasen@redhat.com>
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
#include "flatpak-table-printer.h"


static gboolean
ostree_repo_mode_to_string (OstreeRepoMode   mode,
                            const char     **out_mode,
                            GError         **error)
{
  const char *ret_mode;

  switch (mode)
    {
    case OSTREE_REPO_MODE_BARE:
      ret_mode = "bare";
      break;
    case OSTREE_REPO_MODE_BARE_USER:
      ret_mode = "bare-user";
      break;
    case OSTREE_REPO_MODE_BARE_USER_ONLY:
      ret_mode = "bare-user-only";
      break;
    case OSTREE_REPO_MODE_ARCHIVE:
      /* Legacy alias */
      ret_mode ="archive-z2";
      break;
    default:
      return glnx_throw (error, "Invalid mode '%d'", mode);
    }

  *out_mode = ret_mode;
  return TRUE;
}

static void
print_info (OstreeRepo *repo,
            GVariant *meta)
{
  g_autoptr(GVariant) cache = NULL;
  const char *title;
  const char *collection_id;
  const char *default_branch;
  const char *redirect_url;
  const char *deploy_collection_id;
  g_autoptr(GVariant) gpg_keys = NULL;
  OstreeRepoMode mode;
  const char *mode_string = "unknown";

  mode = ostree_repo_get_mode (repo);
  ostree_repo_mode_to_string (mode, &mode_string, NULL);
  g_print (_("Repo mode: %s\n"), mode_string);

  if (g_variant_lookup (meta, "xa.title", "&s", &title))
    g_print (_("Title: %s\n"), title);

  if (g_variant_lookup (meta, "collection-id", "&s", &collection_id))
    g_print (_("Collection ID: %s\n"), collection_id);

  if (g_variant_lookup (meta, "xa.default-branch", "&s", &default_branch))
    g_print (_("Default branch: %s\n"), default_branch);

  if (g_variant_lookup (meta, "xa.redirect-url", "&s", &redirect_url))
    g_print (_("Redirect URL: %s\n"), redirect_url);

/* FIXME: Remove this check when we depend on ostree 2018.9 */
#ifndef OSTREE_META_KEY_DEPLOY_COLLECTION_ID
#define OSTREE_META_KEY_DEPLOY_COLLECTION_ID "ostree.deploy-collection-id"
#endif

  if (g_variant_lookup (meta, OSTREE_META_KEY_DEPLOY_COLLECTION_ID, "&s", &deploy_collection_id))
    g_print (_("Deploy collection ID: %s\n"), deploy_collection_id);

  if ((gpg_keys = g_variant_lookup_value (meta, "xa.gpg-keys", G_VARIANT_TYPE_BYTESTRING)) != NULL)
    {
      const guchar *gpg_data = g_variant_get_data (gpg_keys);
      gsize gpg_size = g_variant_get_size (gpg_keys);
      g_autofree gchar *gpg_data_checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, gpg_data, gpg_size);

      g_print (_("GPG key hash: %s\n"), gpg_data_checksum);
    }

  cache = g_variant_lookup_value (meta, "xa.cache", NULL);
  if (cache)
    {
      g_autoptr(GVariant) refdata = NULL;

      refdata = g_variant_get_variant (cache);
      g_print (_("%zd branches\n"), g_variant_n_children (refdata));
    }
}

static void
print_branches (GVariant *meta)
{
  g_autoptr(GVariant) cache = NULL;
  g_autoptr(GVariant) sparse_cache = NULL;

  g_variant_lookup (meta, "xa.sparse-cache", "@a{sa{sv}}", &sparse_cache);
  cache = g_variant_lookup_value (meta, "xa.cache", NULL);
  if (cache)
    {
      g_autoptr(GVariant) refdata = NULL;
      GVariantIter iter;
      const char *ref;
      guint64 installed_size;
      guint64 download_size;
      const char *metadata;
      FlatpakTablePrinter *printer;

      printer = flatpak_table_printer_new ();
      flatpak_table_printer_set_column_title (printer, 0, _("Ref"));
      flatpak_table_printer_set_column_title (printer, 1, _("Installed"));
      flatpak_table_printer_set_column_title (printer, 2, _("Download"));
      flatpak_table_printer_set_column_title (printer, 3, _("Options"));

      refdata = g_variant_get_variant (cache);
      g_variant_iter_init (&iter, refdata);
      while (g_variant_iter_next (&iter, "{&s(tt&s)}", &ref, &installed_size, &download_size, &metadata))
        {
          g_autofree char *installed = g_format_size (GUINT64_FROM_BE (installed_size));
          g_autofree char *download = g_format_size (GUINT64_FROM_BE (download_size));

          flatpak_table_printer_add_column (printer, ref);
          flatpak_table_printer_add_decimal_column (printer, installed);
          flatpak_table_printer_add_decimal_column (printer, download);

          flatpak_table_printer_add_column (printer, ""); /* Options */

          if (sparse_cache)
            {
              g_autoptr(GVariant) sparse = NULL;
              if (g_variant_lookup (sparse_cache, ref, "@a{sv}", &sparse))
                {
                  const char *eol;
                  if (g_variant_lookup (sparse, "eol", "&s", &eol))
                    flatpak_table_printer_append_with_comma_printf (printer, "eol=%s", eol);
                  if (g_variant_lookup (sparse, "eolr", "&s", &eol))
                    flatpak_table_printer_append_with_comma_printf (printer, "eol-rebase=%s", eol);
                }
            }

          flatpak_table_printer_finish_row (printer);
        }

      flatpak_table_printer_print (printer);
      flatpak_table_printer_free (printer);
    }
}

static void
print_metadata (GVariant   *meta,
                const char *branch)
{
  g_autoptr(GVariant) cache = NULL;

  cache = g_variant_lookup_value (meta, "xa.cache", NULL);
  if (cache)
    {
      g_autoptr(GVariant) refdata = NULL;
      GVariantIter iter;
      const char *ref;
      guint64 installed_size;
      guint64 download_size;
      const char *metadata;

      refdata = g_variant_get_variant (cache);
      g_variant_iter_init (&iter, refdata);
      while (g_variant_iter_next (&iter, "{&s(tt&s)}", &ref, &installed_size, &download_size, &metadata))
        {
          if (strcmp (branch, ref) == 0)
            g_print ("%s\n", metadata);
        }
    }
}

static void
dump_indented_lines (const gchar *data)
{
  const char* indent = "    ";
  const gchar *pos;

  for (;;)
    {
      pos = strchr (data, '\n');
      if (pos)
        {
          g_print ("%s%.*s", indent, (int)(pos + 1 - data), data);
          data = pos + 1;
        }
      else
        {
          if (data[0] != '\0')
            g_print ("%s%s\n", indent, data);
          break;
        }
    }
}

static void
dump_deltas_for_commit (GPtrArray *deltas,
                        const char *checksum)
{
  int i;
  gboolean header_printed = FALSE;

  if (!deltas)
    return;

  for (i = 0; i < deltas->len; i++)
    {
      const char *delta = g_ptr_array_index (deltas, i);

      if (g_str_equal (delta, checksum))
        {
          if (!header_printed)
            {
              g_print ("Static Deltas:\n");
              header_printed = TRUE;
            }
          g_print ("  from scratch\n");
        } 
      else if (strchr (delta, '-'))
        {
          g_auto(GStrv) parts = g_strsplit (delta, "-", 0);

          if (g_str_equal (parts[1], checksum))
            {
              if (!header_printed)
                {
                  g_print ("Static Deltas:\n");
                  header_printed = TRUE;
                }
              g_print ("  from %s\n", parts[0]);
            }
        }
    }

  if (header_printed)
    g_print ("\n");
}

static gboolean
dump_commit (const char *commit,
             GVariant *variant,
             GPtrArray *deltas,
             GError **error)
{
  const gchar *subject;
  const gchar *body;
  guint64 timestamp;
  g_autofree char *str = NULL;

  /* See OSTREE_COMMIT_GVARIANT_FORMAT */
  g_variant_get (variant, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                 &subject, &body, &timestamp, NULL, NULL);

  timestamp = GUINT64_FROM_BE (timestamp);
  str = format_timestamp (timestamp);
  g_print ("Commit:  %s\n", commit);
  g_print ("Date:  %s\n", str);

  if (subject[0])
    {
      g_print ("\n");
      dump_indented_lines (subject);
    }
  else
    {
      g_print ("(no subject)\n");
    }

  if (body[0])
    {
      g_print ("\n");
      dump_indented_lines (body);
    }
  g_print ("\n");

  dump_deltas_for_commit (deltas, commit);

  return TRUE;
}

static gboolean
log_commit (OstreeRepo *repo,
            const char *checksum,
            gboolean is_recurse,
            GPtrArray *deltas,
            GError **error)
{
  g_autoptr(GVariant) variant = NULL;
  g_autofree char *parent = NULL;
  gboolean ret = FALSE;
  GError *local_error = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &variant, &local_error))
    {
      if (is_recurse && g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_print ("<< History beyond this commit not fetched >>\n");
          g_clear_error (&local_error);
          ret = TRUE;
        }
      else
        {
          g_propagate_error (error, local_error);
        }
      goto out;
    }

  if (!dump_commit (checksum, variant, deltas, error))
    goto out;

  /* Get the parent of this commit */
  parent = ostree_commit_get_parent (variant);
  if (parent && !log_commit (repo, parent, TRUE, deltas, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

static gboolean
print_commits (OstreeRepo *repo,
               const char *ref,
               GError **error)
{
  g_autofree char *checksum = NULL;
  g_autoptr(GPtrArray) deltas = NULL;

  if (!ostree_repo_list_static_delta_names (repo, &deltas, NULL, error))
    return FALSE;

  if (!ostree_repo_resolve_rev (repo, ref, FALSE, &checksum, error))
    return FALSE;

  if (!log_commit (repo, checksum, FALSE, deltas, error))
    return FALSE;

  return TRUE;
}

static gboolean opt_info;
static gboolean opt_branches;
static gchar *opt_metadata_branch;
static gchar *opt_commits_branch;

static GOptionEntry options[] = {
  { "info", 0, 0, G_OPTION_ARG_NONE, &opt_info, N_("Print general information about the repository"), NULL },
  { "branches", 0, 0, G_OPTION_ARG_NONE, &opt_branches, N_("List the branches in the repository"), NULL },
  { "metadata", 0, 0, G_OPTION_ARG_STRING, &opt_metadata_branch, N_("Print metadata for a branch"), N_("BRANCH") },
  { "commits", 0, 0, G_OPTION_ARG_STRING, &opt_commits_branch, N_("Show commits for a branch"), N_("BRANCH") },
  { NULL }
};

gboolean
flatpak_builtin_repo (int argc, char **argv,
                      GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) location = NULL;
  g_autoptr(GVariant) meta = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *ostree_metadata_ref = NULL;
  g_autofree char *ostree_metadata_checksum = NULL;

  context = g_option_context_new (_("LOCATION - Repository maintenance"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("LOCATION must be specified"), error);

  location = g_file_new_for_commandline_arg (argv[1]);
  repo = ostree_repo_new (location);
  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  /* Try loading the metadata from the ostree-metadata branch first. If that
   * fails, fall back to the summary file. */
  ostree_metadata_ref = OSTREE_REPO_METADATA_REF;
  if (!ostree_repo_resolve_rev_ext (repo, ostree_metadata_ref,
                                    TRUE, 0, &ostree_metadata_checksum, error))
    return FALSE;

  if (ostree_metadata_checksum != NULL)
    {
      g_autoptr(GVariant) commit_v = NULL;

      if (!ostree_repo_load_commit (repo, ostree_metadata_checksum, &commit_v, NULL, error))
        {
          g_prefix_error (error, "Error getting repository metadata from %s ref: ", ostree_metadata_ref);
          return FALSE;
        }

      meta = g_variant_get_child_value (commit_v, 0);
      g_debug ("Using repository metadata from the ostree-metadata branch");
    }
  else
    {
      g_autoptr(GVariant) summary = NULL;

      summary = flatpak_repo_load_summary (repo, error);
      if (summary == NULL)
        {
          g_prefix_error (error, "Error getting repository metadata from summary file: ");
          return FALSE;
        }
      meta = g_variant_get_child_value (summary, 1);
      g_debug ("Using repository metadata from the summary file");
    }

  if (!opt_info && !opt_branches && !opt_metadata_branch && !opt_commits_branch)
    opt_info = TRUE;

  /* Print out the metadata. */
  if (opt_info)
    print_info (repo, meta);

  if (opt_branches)
    print_branches (meta);

  if (opt_metadata_branch)
    print_metadata (meta, opt_metadata_branch);

  if (opt_commits_branch)
    {
      if (!print_commits (repo, opt_commits_branch, error))
        return FALSE;
    }

  return TRUE;
}


gboolean
flatpak_complete_repo (FlatpakCompletion *completion)
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
