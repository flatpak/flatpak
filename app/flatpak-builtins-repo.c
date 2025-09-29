/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-repo-utils-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-variant-impl-private.h"

static gboolean opt_info;
static gboolean opt_branches;
static gboolean opt_subsets;
static gchar *opt_metadata_branch;
static gchar *opt_commits_branch;
static gchar *opt_subset;
static gboolean opt_json;

static gboolean
ostree_repo_mode_to_string (OstreeRepoMode mode,
                            const char   **out_mode,
                            GError       **error)
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
      ret_mode = "archive-z2";
      break;

    default:
      return glnx_throw (error, "Invalid mode '%d'", mode);
    }

  *out_mode = ret_mode;
  return TRUE;
}

static void
print_info (OstreeRepo *repo,
            GVariant   *index,
            GVariant   *summary)
{
  const char *title;
  const char *comment;
  const char *description;
  const char *homepage;
  const char *icon;
  const char *collection_id;
  const char *default_branch;
  const char *redirect_url;
  const char *deploy_collection_id;
  const char *authenticator_name;
  gboolean authenticator_install = FALSE;
  g_autoptr(GVariant) gpg_keys = NULL;
  OstreeRepoMode mode;
  const char *mode_string = "unknown";
  g_autoptr(GVariant) meta = NULL;
  g_autoptr(GVariant) refs = NULL;
  guint cache_version = 0;
  gboolean indexed_deltas = FALSE;

  mode = ostree_repo_get_mode (repo);
  ostree_repo_mode_to_string (mode, &mode_string, NULL);
  g_print (_("Repo mode: %s\n"), mode_string);

  if (index)
    meta = g_variant_get_child_value (index, 1);
  else
    meta = g_variant_get_child_value (summary, 1);

  g_print (_("Indexed summaries: %s\n"), index != NULL ? _("true") : _("false"));

  if (index)
    {
      VarSummaryIndexRef index_ref = var_summary_index_from_gvariant (index);
      VarSummaryIndexSubsummariesRef subsummaries = var_summary_index_get_subsummaries (index_ref);
      gsize n_subsummaries = var_summary_index_subsummaries_get_length (subsummaries);

      g_print (_("Subsummaries: "));
      for (gsize i = 0; i < n_subsummaries; i++)
        {
          VarSummaryIndexSubsummariesEntryRef entry = var_summary_index_subsummaries_get_at (subsummaries, i);
          const char *name = var_summary_index_subsummaries_entry_get_key (entry);

          if (i != 0)
            g_print (", ");
          g_print ("%s", name);
        }

      g_print ("\n");
    }

  g_variant_lookup (meta, "xa.cache-version", "u", &cache_version);
  g_print (_("Cache version: %d\n"), cache_version);

  g_variant_lookup (meta, "ostree.summary.indexed-deltas", "b", &indexed_deltas);
  g_print (_("Indexed deltas: %s\n"), indexed_deltas ? _("true") : _("false"));

  if (g_variant_lookup (meta, "xa.title", "&s", &title))
    g_print (_("Title: %s\n"), title);

  if (g_variant_lookup (meta, "xa.comment", "&s", &comment))
    g_print (_("Comment: %s\n"), comment);

  if (g_variant_lookup (meta, "xa.description", "&s", &description))
    g_print (_("Description: %s\n"), description);

  if (g_variant_lookup (meta, "xa.homepage", "&s", &homepage))
    g_print (_("Homepage: %s\n"), homepage);

  if (g_variant_lookup (meta, "xa.icon", "&s", &icon))
    g_print (_("Icon: %s\n"), icon);

  if (g_variant_lookup (meta, "collection-id", "&s", &collection_id))
    g_print (_("Collection ID: %s\n"), collection_id);

  if (g_variant_lookup (meta, "xa.default-branch", "&s", &default_branch))
    g_print (_("Default branch: %s\n"), default_branch);

  if (g_variant_lookup (meta, "xa.redirect-url", "&s", &redirect_url))
    g_print (_("Redirect URL: %s\n"), redirect_url);

  if (g_variant_lookup (meta, OSTREE_META_KEY_DEPLOY_COLLECTION_ID, "&s", &deploy_collection_id))
    g_print (_("Deploy collection ID: %s\n"), deploy_collection_id);

  if (g_variant_lookup (meta, "xa.authenticator-name", "&s", &authenticator_name))
    g_print (_("Authenticator name: %s\n"), authenticator_name);

  if (g_variant_lookup (meta, "xa.authenticator-install", "&s", &authenticator_install))
    g_print (_("Authenticator install: %s\n"), authenticator_install ? _("true") : _("false"));

  if ((gpg_keys = g_variant_lookup_value (meta, "xa.gpg-keys", G_VARIANT_TYPE_BYTESTRING)) != NULL)
    {
      const guchar *gpg_data = g_variant_get_data (gpg_keys);
      gsize gpg_size = g_variant_get_size (gpg_keys);
      g_autofree gchar *gpg_data_checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, gpg_data, gpg_size);

      g_print (_("GPG key hash: %s\n"), gpg_data_checksum);
    }

  refs = g_variant_get_child_value (summary, 0);
  g_print (_("%zd summary branches\n"), g_variant_n_children (refs));
}

static void
print_branches_for_subsummary (FlatpakTablePrinter *printer,
                               const char *subsummary,
                               GVariant   *summary)
{
  g_autoptr(GVariant) meta = NULL;
  guint summary_version = 0;
  g_autofree char *subset = NULL;

  if (subsummary != NULL)
    {
      const char *dash = strrchr (subsummary, '-');
      if (dash)
        subset = g_strndup (subsummary, dash - subsummary);
    }

  if (opt_subset != NULL)
    {
      if (subset == NULL ||
          strcmp (subset, opt_subset) != 0)
        return; /* Not the requested subset, ignore */
    }

  meta = g_variant_get_child_value (summary, 1);

  g_variant_lookup (meta, "xa.summary-version", "u", &summary_version);

  if (summary_version == 1)
    {
      g_autoptr(GVariant) refs = g_variant_get_child_value (summary, 0);
      GVariantIter iter;
      const char *ref;
      GVariant *refdata_iter = NULL;

      g_variant_iter_init (&iter, refs);
      while (g_variant_iter_next (&iter, "(&s@(taya{sv}))", &ref, &refdata_iter))
        {
          g_autoptr(GVariant) refdata = refdata_iter;
          g_autoptr(GVariant) ref_meta = g_variant_get_child_value (refdata, 2);
          g_autoptr(GVariant) data = g_variant_lookup_value (ref_meta, "xa.data", NULL);
          guint64 installed_size;
          guint64 download_size;
          const char *metadata;
          const char *eol;

          if (data == NULL)
            continue;

          int old_row = flatpak_table_printer_lookup_row (printer, ref);
          if (old_row >= 0)
            {
              if (subset)
                flatpak_table_printer_append_cell_with_comma_unique (printer, old_row, 3, subset);
              continue;
            }

          g_variant_get (data, "(tt&s)", &installed_size, &download_size, &metadata);

          g_autofree char *installed = g_format_size (GUINT64_FROM_BE (installed_size));
          g_autofree char *download = g_format_size (GUINT64_FROM_BE (download_size));

          flatpak_table_printer_set_key (printer, ref);
          flatpak_table_printer_add_column (printer, ref);
          flatpak_table_printer_add_decimal_column (printer, installed);
          flatpak_table_printer_add_decimal_column (printer, download);

          /* Subset */
          flatpak_table_printer_add_column (printer, subset);

          flatpak_table_printer_add_column (printer, ""); /* Options */

          if (g_variant_lookup (ref_meta, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE, "&s", &eol))
            flatpak_table_printer_append_with_comma_printf (printer, "eol=%s", eol);
          if (g_variant_lookup (ref_meta, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE_REBASE, "&s", &eol))
            flatpak_table_printer_append_with_comma_printf (printer, "eol-rebase=%s", eol);


          flatpak_table_printer_finish_row (printer);
        }
    }
  else
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

          refdata = g_variant_get_variant (cache);
          g_variant_iter_init (&iter, refdata);
          while (g_variant_iter_next (&iter, "{&s(tt&s)}", &ref, &installed_size, &download_size, &metadata))
            {
              g_autofree char *installed = g_format_size (GUINT64_FROM_BE (installed_size));
              g_autofree char *download = g_format_size (GUINT64_FROM_BE (download_size));

              int old_row = flatpak_table_printer_lookup_row (printer, ref);
              if (old_row >= 0)
                {
                  if (subset)
                    flatpak_table_printer_append_cell_with_comma_unique (printer, old_row, 3, subset);
                  continue;
                }

              flatpak_table_printer_set_key (printer, ref);
              flatpak_table_printer_add_column (printer, ref);
              flatpak_table_printer_add_decimal_column (printer, installed);
              flatpak_table_printer_add_decimal_column (printer, download);

              flatpak_table_printer_add_column (printer, subset);

              flatpak_table_printer_add_column (printer, ""); /* Options */

              if (sparse_cache)
                {
                  g_autoptr(GVariant) sparse = NULL;
                  if (g_variant_lookup (sparse_cache, ref, "@a{sv}", &sparse))
                    {
                      const char *eol;
                      if (g_variant_lookup (sparse, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE, "&s", &eol))
                        flatpak_table_printer_append_with_comma_printf (printer, "eol=%s", eol);
                      if (g_variant_lookup (sparse, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE_REBASE, "&s", &eol))
                        flatpak_table_printer_append_with_comma_printf (printer, "eol-rebase=%s", eol);
                    }
                }

              flatpak_table_printer_finish_row (printer);
            }

        }
    }
}

static void
print_branches (OstreeRepo *repo,
                GVariant   *index,
                GVariant   *summary)
{
  g_autoptr(FlatpakTablePrinter) printer = NULL;

  printer = flatpak_table_printer_new ();
  flatpak_table_printer_set_column_title (printer, 0, _("Ref"));
  flatpak_table_printer_set_column_title (printer, 1, _("Installed"));
  flatpak_table_printer_set_column_title (printer, 2, _("Download"));
  flatpak_table_printer_set_column_title (printer, 3, _("Subsets"));
  flatpak_table_printer_set_column_title (printer, 4, _("Options"));

  if (index != NULL)
    {
      VarSummaryIndexRef index_ref = var_summary_index_from_gvariant (index);
      VarSummaryIndexSubsummariesRef subsummaries = var_summary_index_get_subsummaries (index_ref);
      gsize n_subsummaries = var_summary_index_subsummaries_get_length (subsummaries);

      for (gsize i = 0; i < n_subsummaries; i++)
        {
          VarSummaryIndexSubsummariesEntryRef entry = var_summary_index_subsummaries_get_at (subsummaries, i);
          const char *name = var_summary_index_subsummaries_entry_get_key (entry);
          VarSubsummaryRef subsummary = var_summary_index_subsummaries_entry_get_value (entry);
          gsize checksum_bytes_len;
          const guchar *checksum_bytes;
          g_autofree char *digest = NULL;
          g_autoptr(GVariant) subsummary_v = NULL;
          g_autoptr(GError) error = NULL;

          checksum_bytes = var_subsummary_peek_checksum (subsummary, &checksum_bytes_len);
          if (G_UNLIKELY (checksum_bytes_len != OSTREE_SHA256_DIGEST_LEN))
            {
              g_printerr ("Invalid checksum for digested summary\n");
              continue;
            }
          digest = ostree_checksum_from_bytes (checksum_bytes);

          subsummary_v = flatpak_repo_load_digested_summary (repo, digest, &error);
          if (subsummary_v == NULL)
            {
              g_printerr ("Failed to load subsummary %s (digest %s)\n", name, digest);
              continue;
            }

          print_branches_for_subsummary (printer, name, subsummary_v);
        }
    }
  else
    print_branches_for_subsummary (printer, NULL, summary);

  flatpak_table_printer_sort (printer, (GCompareFunc) strcmp);

  opt_json ? flatpak_table_printer_print_json (printer) : flatpak_table_printer_print (printer);
}

static void
print_subsets (OstreeRepo *repo,
               GVariant   *index)
{
  g_autoptr(FlatpakTablePrinter) printer = NULL;

  printer = flatpak_table_printer_new ();
  flatpak_table_printer_set_column_title (printer, 0, _("Subset"));
  flatpak_table_printer_set_column_title (printer, 1, _("Digest"));
  flatpak_table_printer_set_column_title (printer, 2, _("History length"));

  if (index != NULL)
    {
      VarSummaryIndexRef index_ref = var_summary_index_from_gvariant (index);
      VarSummaryIndexSubsummariesRef subsummaries = var_summary_index_get_subsummaries (index_ref);
      gsize n_subsummaries = var_summary_index_subsummaries_get_length (subsummaries);

      for (gsize i = 0; i < n_subsummaries; i++)
        {
          VarSummaryIndexSubsummariesEntryRef entry = var_summary_index_subsummaries_get_at (subsummaries, i);
          const char *name = var_summary_index_subsummaries_entry_get_key (entry);
          VarSubsummaryRef subsummary = var_summary_index_subsummaries_entry_get_value (entry);
          gsize checksum_bytes_len;
          const guchar *checksum_bytes;
          g_autofree char *digest = NULL;
          VarArrayofChecksumRef history = var_subsummary_get_history (subsummary);
          gsize history_len = var_arrayof_checksum_get_length (history);

          if (opt_subset != NULL && !g_str_has_prefix (name, opt_subset))
            continue;

          checksum_bytes = var_subsummary_peek_checksum (subsummary, &checksum_bytes_len);
          if (G_UNLIKELY (checksum_bytes_len != OSTREE_SHA256_DIGEST_LEN))
            {
              g_printerr ("Invalid checksum for digested summary\n");
              continue;
            }
          digest = ostree_checksum_from_bytes (checksum_bytes);

          flatpak_table_printer_add_column (printer, name);
          flatpak_table_printer_add_column (printer, digest);
          flatpak_table_printer_take_column (printer, g_strdup_printf ("%"G_GSIZE_FORMAT, history_len));
          flatpak_table_printer_finish_row (printer);
        }
    }

  opt_json ? flatpak_table_printer_print_json (printer) : flatpak_table_printer_print (printer);
}


static void
print_metadata (OstreeRepo *repo,
                GVariant   *index,
                GVariant   *summary,
                const char *branch)
{
  g_autoptr(GVariant) meta = NULL;
  guint summary_version = 0;
  guint64 installed_size;
  guint64 download_size;
  const char *metadata;
  GVariantIter iter;
  const char *ref;
  g_autoptr(GVariant) subsummary_v = NULL;

  if (index)
    {
      g_autofree char *arch = flatpak_get_arch_for_ref (branch);

      if (arch != NULL)
        {
          VarSummaryIndexRef index_ref = var_summary_index_from_gvariant (index);
          VarSummaryIndexSubsummariesRef subsummaries = var_summary_index_get_subsummaries (index_ref);
          gsize n_subsummaries = var_summary_index_subsummaries_get_length (subsummaries);

          for (gsize i = 0; i < n_subsummaries; i++)
            {
              VarSummaryIndexSubsummariesEntryRef entry = var_summary_index_subsummaries_get_at (subsummaries, i);
              const char *name = var_summary_index_subsummaries_entry_get_key (entry);
              VarSubsummaryRef subsummary = var_summary_index_subsummaries_entry_get_value (entry);
              gsize checksum_bytes_len;
              const guchar *checksum_bytes;

              if (strcmp (name, arch) == 0)
                {
                  g_autofree char *digest = NULL;
                  g_autoptr(GError) error = NULL;

                  checksum_bytes = var_subsummary_peek_checksum (subsummary, &checksum_bytes_len);
                  if (G_UNLIKELY (checksum_bytes_len != OSTREE_SHA256_DIGEST_LEN))
                    break;

                  digest = ostree_checksum_from_bytes (checksum_bytes);

                  subsummary_v = flatpak_repo_load_digested_summary (repo, digest, &error);
                  if (subsummary_v == NULL)
                    g_printerr ("Failed to load subsummary %s (digest %s)\n", name, digest);
                  break;
                }
            }
        }
    }

  if (subsummary_v)
    summary = subsummary_v;

  meta = g_variant_get_child_value (summary, 1);

  g_variant_lookup (meta, "xa.summary-version", "u", &summary_version);

  if (summary_version == 1)
    {
      g_autoptr(GVariant) refs = g_variant_get_child_value (summary, 0);
      GVariant *refdata_iter = NULL;

      g_variant_iter_init (&iter, refs);
      while (g_variant_iter_next (&iter, "(&s@(taya{sv}))", &ref, &refdata_iter))
        {
          g_autoptr(GVariant) refdata = refdata_iter;
          g_autoptr(GVariant) ref_meta = g_variant_get_child_value (refdata, 2);

          if (strcmp (branch, ref) == 0)
            {
              g_autoptr(GVariant) data = g_variant_lookup_value (ref_meta, "xa.data", NULL);
              if (data)
                {
                  g_variant_get (data, "(tt&s)", &installed_size, &download_size, &metadata);
                  g_print ("%s\n", metadata);
                  break;
                }
            }
        }
    }
  else /* Version 0 */
    {
      g_autoptr(GVariant) cache = g_variant_lookup_value (meta, "xa.cache", NULL);
      if (cache)
        {
          g_autoptr(GVariant) refdata = g_variant_get_variant (cache);
          g_variant_iter_init (&iter, refdata);
          while (g_variant_iter_next (&iter, "{&s(tt&s)}", &ref, &installed_size, &download_size, &metadata))
            {
              if (strcmp (branch, ref) == 0)
                {
                  g_print ("%s\n", metadata);
                  break;
                }
            }
        }
    }
}

static void
dump_indented_lines (const gchar *data)
{
  const char * indent = "    ";
  const gchar *pos;

  for (;;)
    {
      pos = strchr (data, '\n');
      if (pos)
        {
          g_print ("%s%.*s", indent, (int) (pos + 1 - data), data);
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
dump_deltas_for_commit (GPtrArray  *deltas,
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
             GVariant   *variant,
             GPtrArray  *deltas,
             GError    **error)
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
            gboolean    is_recurse,
            GPtrArray  *deltas,
            GError    **error)
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
print_commits (OstreeRepo   *repo,
               const char   *collection_id,
               const char   *ref,
               GCancellable *cancellable,
               GError      **error)
{
  g_autofree char *checksum = NULL;
  g_autoptr(GPtrArray) deltas = NULL;

  if (!ostree_repo_list_static_delta_names (repo, &deltas, NULL, error))
    return FALSE;

  if (!flatpak_repo_resolve_rev (repo, collection_id, NULL, ref, FALSE, &checksum,
                                 cancellable, error))
    return FALSE;

  if (!log_commit (repo, checksum, FALSE, deltas, error))
    return FALSE;

  return TRUE;
}


static GOptionEntry options[] = {
  { "info", 0, 0, G_OPTION_ARG_NONE, &opt_info, N_("Print general information about the repository"), NULL },
  { "branches", 0, 0, G_OPTION_ARG_NONE, &opt_branches, N_("List the branches in the repository"), NULL },
  { "metadata", 0, 0, G_OPTION_ARG_STRING, &opt_metadata_branch, N_("Print metadata for a branch"), N_("BRANCH") },
  { "commits", 0, 0, G_OPTION_ARG_STRING, &opt_commits_branch, N_("Show commits for a branch"), N_("BRANCH") },
  { "subsets", 0, 0, G_OPTION_ARG_NONE, &opt_subsets, N_("Print information about the repo subsets"), NULL },
  { "subset", 0, 0, G_OPTION_ARG_STRING, &opt_subset, N_("Limit information to subsets with this prefix"), NULL },
  { "json", 'j', 0, G_OPTION_ARG_NONE, &opt_json, N_("Show output in JSON format"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_repo (int argc, char **argv,
                      GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) location = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GVariant) index = NULL;
  g_autoptr(GVariant) summary = NULL;
  const char *collection_id;

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

  collection_id = ostree_repo_get_collection_id (repo);

  index = flatpak_repo_load_summary_index (repo, NULL);
  summary = flatpak_repo_load_summary (repo, error);
  if (summary == NULL)
    {
      g_prefix_error (error, "Error getting repository metadata from summary file: ");
      return FALSE;
    }

  if (!opt_info && !opt_branches && !opt_metadata_branch && !opt_commits_branch && !opt_subsets)
    opt_info = TRUE;

  /* Print out the metadata. */
  if (opt_info)
    print_info (repo, index, summary);

  if (opt_branches)
    print_branches (repo, index, summary);

  if (opt_metadata_branch)
    print_metadata (repo, index, summary, opt_metadata_branch);

  if (opt_subsets)
    print_subsets (repo, index);

  if (opt_commits_branch)
    {
      if (!print_commits (repo, collection_id, opt_commits_branch, cancellable, error))
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
