/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_ref;

static GOptionEntry options[] = {
  { "ref", 0, 0, G_OPTION_ARG_STRING, &opt_ref, "Override the ref used for the imported bundle", "REF" },
  { NULL }
};

static gboolean
import_bundle (OstreeRepo *repo, GFile *file,
              GCancellable *cancellable, GError **error)
{
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *bundle_ref = NULL;
  g_autofree char *to_checksum = NULL;
  const char *ref;

  metadata = xdg_app_bundle_load (file, &to_checksum,
                                  &bundle_ref,
                                  NULL,
                                  NULL,
                                  NULL,
                                  error);
  if (metadata == NULL)
    return FALSE;

  if (opt_ref != NULL)
    ref = opt_ref;
  else
    ref = bundle_ref;

  g_print ("Importing %s (%s)\n", ref, to_checksum);
  if (!xdg_app_pull_from_bundle (repo, file,
                                 NULL, ref, FALSE,
                                 cancellable,
                                 error))
    return FALSE;

  return TRUE;
}

gboolean
xdg_app_builtin_build_import (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  const char *location;
  const char *filename;

  context = g_option_context_new ("LOCATION FILENAME - Import a file bundle into a local repository");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 3)
    return usage_error (context, "LOCATION and FILENAME must be specified", error);

  location = argv[1];
  filename = argv[2];

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!xdg_app_supports_bundles (repo))
    return xdg_app_fail (error, "Your version of ostree is too old to support single-file bundles");

  if (!g_file_query_exists (repofile, cancellable))
    return xdg_app_fail (error, "'%s' is not a valid repository", location);

  file = g_file_new_for_commandline_arg (filename);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (!import_bundle (repo, file, cancellable, error))
    return FALSE;

  return TRUE;
}
