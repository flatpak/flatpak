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

static gboolean opt_user;
static gboolean opt_system;
static gboolean opt_show_ref;
static gboolean opt_show_commit;
static gboolean opt_show_origin;
static gboolean opt_show_size;
static gboolean opt_show_metadata;
static gboolean opt_show_extensions;
static char *opt_arch;
static char **opt_installations;

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

gboolean
flatpak_builtin_info (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autofree char *ref = NULL;
  FlatpakDir *dir = NULL;
  g_autoptr(GVariant) deploy_data = NULL;
  const char *commit = NULL;
  const char *pref = NULL;
  const char *default_branch = NULL;
  const char *origin = NULL;
  guint64 size;
  gboolean search_all = FALSE;
  gboolean first = TRUE;
  FlatpakKinds kinds;

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

  commit = flatpak_deploy_data_get_commit (deploy_data);
  origin = flatpak_deploy_data_get_origin (deploy_data);
  size = flatpak_deploy_data_get_installed_size (deploy_data);

  if (!opt_show_extensions && !opt_show_metadata &&
      !opt_show_ref && !opt_show_origin && !opt_show_commit && !opt_show_size)
    opt_show_ref = opt_show_origin = opt_show_commit = opt_show_size = TRUE;

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
      g_autofree char *formatted = g_format_size (size);

      maybe_print_space (&first);
      g_print ("%s", formatted);
    }

  if (!first)
    g_print ("\n");

  if (opt_show_extensions)
    {
       g_autoptr(FlatpakDeploy) deploy = NULL;
       g_autoptr(GKeyFile) metakey = NULL;
       g_auto(GStrv) parts = NULL;
       GList *extensions, *l;

       deploy = flatpak_find_deploy_for_ref (ref, NULL, NULL);
       metakey = flatpak_deploy_get_metadata (deploy);
       parts = g_strsplit (ref, "/", 0);
       extensions = flatpak_list_extensions (metakey, parts[2], parts[3]);
       for (l = extensions; l; l = l->next)
         {
           FlatpakExtension *ext = l->data;
           g_autofree const char **subpaths = NULL;
           g_autoptr(GVariant) ext_deploy_data = NULL;

           if (ext->is_unmaintained)
             {
               commit = "unmaintained";
               origin = NULL;
               size = 0;
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
               subpaths = flatpak_deploy_data_get_subpaths (ext_deploy_data);
             }

           g_print ("extension: %s", ext->id);

           if (opt_show_ref)
             g_print (" %s", ext->ref);

           if (opt_show_origin)
             g_print (" %s", origin ? origin : "-");

           if (opt_show_commit)
             g_print (" %s", commit);

           if (opt_show_size && size > 0)
             {
               g_autofree char *formatted = NULL;

               formatted = g_format_size (size);
               g_print (" %s", formatted);
             }

           if (subpaths)
             {
               g_autofree char *subpath_str = NULL;

               subpath_str = g_strjoinv (",", (char **)subpaths);
               g_print (" %s", subpath_str);
             }

           g_print ("\n");
         }
    }

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

      g_print ("%s\n", data);
    }

  return TRUE;
}

gboolean
flatpak_complete_info (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  g_autoptr(GPtrArray) system_dirs = NULL;
  g_autoptr(GError) error = NULL;
  gboolean search_all = FALSE;
  FlatpakKinds kinds;
  int i, j;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  kinds = FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME;

  if (!opt_user && !opt_system && opt_installations == NULL)
    search_all = TRUE;

  if (opt_user || search_all)
    user_dir = flatpak_dir_get_user ();

  if (search_all)
    {
      system_dirs = flatpak_dir_get_system_list (NULL, &error);
      if (system_dirs == NULL)
        {
          flatpak_completion_debug ("find system installations error: %s", error->message);
          return FALSE;
        }
    }

  if (opt_system)
    system_dir = flatpak_dir_get_system_default ();

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      if (user_dir)
        {
          g_auto(GStrv) refs = flatpak_dir_find_installed_refs (user_dir, NULL, NULL, opt_arch,
                                                                kinds, &error);
          if (refs == NULL)
            flatpak_completion_debug ("find local refs error: %s", error->message);
          for (i = 0; refs != NULL && refs[i] != NULL; i++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
              if (parts)
                flatpak_complete_word (completion, "%s ", parts[1]);
            }
        }

      if (system_dirs)
        {
          for (i = 0; i < system_dirs->len; i++)
            {
              FlatpakDir *dir = g_ptr_array_index (system_dirs, i);
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
        }

      if (system_dir)
        {
          g_auto(GStrv) refs = flatpak_dir_find_installed_refs (system_dir, NULL, NULL, opt_arch,
                                                                kinds, &error);
          if (refs == NULL)
            flatpak_completion_debug ("find local refs error: %s", error->message);
          for (i = 0; refs != NULL && refs[i] != NULL; i++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
              if (parts)
                flatpak_complete_word (completion, "%s ", parts[1]);
            }
        }
      break;

    case 2: /* BRANCH */
      if (user_dir)
        {
          g_auto(GStrv) refs = flatpak_dir_find_installed_refs (user_dir, completion->argv[1], NULL, opt_arch,
                                                                kinds, &error);
          if (refs == NULL)
            flatpak_completion_debug ("find remote refs error: %s", error->message);
          for (i = 0; refs != NULL && refs[i] != NULL; i++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
              if (parts)
                flatpak_complete_word (completion, "%s ", parts[3]);
            }
        }

      if (system_dirs)
        {
          for (i = 0; i < system_dirs->len; i++)
            {
              FlatpakDir *dir = g_ptr_array_index (system_dirs, i);
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
        }

      if (system_dir)
        {
          g_auto(GStrv) refs = flatpak_dir_find_installed_refs (system_dir, completion->argv[1], NULL, opt_arch,
                                                                kinds, &error);
          if (refs == NULL)
            flatpak_completion_debug ("find remote refs error: %s", error->message);
          for (i = 0; refs != NULL && refs[i] != NULL; i++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
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
