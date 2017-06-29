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
#include "flatpak-utils.h"
#include "flatpak-table-printer.h"

static void
print_info (GVariant *meta)
{
  g_autoptr(GVariant) cache = NULL;
  const char *title;
  const char *default_branch;
  const char *redirect_url;
  g_autoptr(GVariant) gpg_keys = NULL;

  if (g_variant_lookup (meta, "xa.title", "&s", &title))
    g_print ("Title: %s\n", title);

  if (g_variant_lookup (meta, "xa.default-branch", "&s", &default_branch))
    g_print ("Default branch: %s\n", default_branch);

  if (g_variant_lookup (meta, "xa.redirect-url", "&s", &redirect_url))
    g_print ("Redirect URL: %s\n", redirect_url);

  if ((gpg_keys = g_variant_lookup_value (meta, "xa.gpg-keys", G_VARIANT_TYPE_BYTESTRING)) != NULL)
    {
      const guchar *gpg_data = g_variant_get_data (gpg_keys);
      gsize gpg_size = g_variant_get_size (gpg_keys);
      g_autofree gchar *gpg_data_checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, gpg_data, gpg_size);

      g_print ("GPG key hash: %s\n", gpg_data_checksum);
    }

  cache = g_variant_lookup_value (meta, "xa.cache", NULL);
  if (cache)
    {
      g_autoptr(GVariant) refdata = NULL;

      refdata = g_variant_get_variant (cache);
      g_print ("%zd branches\n", g_variant_n_children (refdata));
    }
}

static void
print_branches (GVariant *meta)
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
      FlatpakTablePrinter *printer;

      printer = flatpak_table_printer_new ();
      flatpak_table_printer_set_column_title (printer, 0, _("Ref"));
      flatpak_table_printer_set_column_title (printer, 1, _("Installed"));
      flatpak_table_printer_set_column_title (printer, 2, _("Download"));

      refdata = g_variant_get_variant (cache);
      g_variant_iter_init (&iter, refdata);
      while (g_variant_iter_next (&iter, "{&s(tt&s)}", &ref, &installed_size, &download_size, &metadata))
        {
          g_autofree char *installed = g_format_size (GUINT64_FROM_BE (installed_size));
          g_autofree char *download = g_format_size (GUINT64_FROM_BE (download_size));

          flatpak_table_printer_add_column (printer, ref);
          flatpak_table_printer_add_decimal_column (printer, installed);
          flatpak_table_printer_add_decimal_column (printer, download);
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
            g_print ("%s", metadata);
        }
    }
}

static gboolean opt_info;
static gboolean opt_branches;
static gchar *opt_metadata_branch;

static GOptionEntry options[] = {
  { "info", 0, 0, G_OPTION_ARG_NONE, &opt_info, N_("Print general information about the repository"), NULL },
  { "branches", 0, 0, G_OPTION_ARG_NONE, &opt_branches, N_("List the branches in the repository"), NULL },
  { "metadata", 0, 0, G_OPTION_ARG_STRING, &opt_metadata_branch, N_("Print metadata for a branch"), N_("BRANCH") },
  { NULL }
};

gboolean
flatpak_builtin_repo (int argc, char **argv,
                      GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autofree char *location = NULL;
  g_autofree char *data = NULL;
  gsize size;
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariant) meta = NULL;

  context = g_option_context_new (_("LOCATION - Repository maintenance"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("LOCATION must be specified"), error);

  location = g_build_filename (argv[1], "summary", NULL);

  if (!g_file_get_contents (location, &data, &size, error)) {
    exit (1);
  }

  summary = g_variant_new_from_data (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                     data, size,
                                     FALSE, NULL, NULL);
  g_variant_ref_sink (summary);
  meta = g_variant_get_child_value (summary, 1);

  if (opt_info)
    print_info (meta);

  if (opt_branches)
    print_branches (meta);

  if (opt_metadata_branch)
    print_metadata (meta, opt_metadata_branch);

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
