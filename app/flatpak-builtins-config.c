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
#include <string.h>
#include <unistd.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "common/flatpak-dir-private.h"

static gboolean opt_get;
static gboolean opt_set;
static gboolean opt_unset;
static gboolean opt_list;

static GOptionEntry options[] = {
  { "list", 0, 0, G_OPTION_ARG_NONE, &opt_list, N_("List configuration keys and values"), NULL },
  { "get", 0, 0, G_OPTION_ARG_NONE, &opt_get, N_("Get configuration for KEY"), NULL },
  { "set", 0, 0, G_OPTION_ARG_NONE, &opt_set, N_("Set configuration for KEY to VALUE"), NULL },
  { "unset", 0, 0, G_OPTION_ARG_NONE, &opt_unset, N_("Unset configuration for KEY"), NULL },
  { NULL }
};

static char *
parse_lang (const char *value)
{
  if (strcmp (value, "*") == 0 ||
      strcmp (value, "*all*") == 0)
    return g_strdup ("");
  return g_strdup (value);
}

static char *
print_lang (const char *value)
{
  if (*value == 0)
    return g_strdup ("*all*");
  return g_strdup (value);
}

static char *
get_lang_default (FlatpakDir *dir)
{
  g_auto(GStrv) langs = flatpak_dir_get_default_locale_languages (dir);

  return g_strjoinv (";", langs);
}

typedef struct
{
  const char *name;
  char *(*parse)(const char *value);
  char *(*print)(const char *value);
  char *(*get_default)(FlatpakDir * dir);
} ConfigKey;

ConfigKey keys[] = {
  { "languages", parse_lang, print_lang, get_lang_default },
};

static ConfigKey *
get_config_key (const char *arg, GError **error)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      if (strcmp (keys[i].name, arg) == 0)
        return &keys[i];
    }

  flatpak_fail (error, _("Unknown configure key '%s'"), arg);
  return NULL;
}

static char *
print_config (FlatpakDir *dir, ConfigKey *key)
{
  g_autofree char *value = NULL;

  value = flatpak_dir_get_config (dir, key->name, NULL);
  if (value == NULL)
    return g_strdup ("*unset*");

  return key->print (value);
}

static gboolean
list_config (int argc, char **argv, FlatpakDir *dir, GCancellable *cancellable, GError **error)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      const char *key = keys[i].name;
      g_autofree char *value = print_config (dir, &keys[i]);
      g_autofree char *default_value = NULL;

      g_print ("%s: %s", key, value);

      if (keys[i].get_default)
        default_value = keys[i].get_default (dir);
      if (default_value)
        {
          g_autofree char *printed = keys[i].print (default_value);
          g_print (" (default: %s)", printed);
        }
      g_print ("\n");
    }

  return TRUE;
}

static gboolean
get_config (int argc, char **argv, FlatpakDir *dir, GCancellable *cancellable, GError **error)
{
  ConfigKey *key;
  g_autofree char *value = NULL;

  if (argc != 2)
    return flatpak_fail (error, _("You must specify key"));

  key = get_config_key (argv[1], error);
  if (key == NULL)
    return FALSE;

  value = print_config (dir, key);
  if (value)
    g_print ("%s\n", value);
  else
    g_print ("*unset*\n");

  return TRUE;
}

static gboolean
set_config (int argc, char **argv, FlatpakDir *dir, GCancellable *cancellable, GError **error)
{
  ConfigKey *key;
  g_autofree char *parsed = NULL;

  if (argc != 3)
    return flatpak_fail (error, _("You must specify both key and value"));

  key = get_config_key (argv[1], error);
  if (key == NULL)
    return FALSE;

  parsed = key->parse (argv[2]);
  if (!flatpak_dir_set_config (dir, key->name, parsed, error))
    return FALSE;

  return TRUE;
}

static gboolean
unset_config (int argc, char **argv, FlatpakDir *dir, GCancellable *cancellable, GError **error)
{
  ConfigKey *key;

  if (argc != 2)
    return flatpak_fail (error, _("You must specify key"));

  key = get_config_key (argv[1], error);
  if (key == NULL)
    return FALSE;

  if (!flatpak_dir_set_config (dir, key->name, argv[2], error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_config (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;

  context = g_option_context_new (_("[KEY [VALUE]] - Manage configuration"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR,
                                     &dirs, cancellable, error))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  if (opt_get)
    return get_config (argc, argv, dir, cancellable, error);
  else if (opt_set)
    return set_config (argc, argv, dir, cancellable, error);
  else if (opt_unset)
    return unset_config (argc, argv, dir, cancellable, error);
  else if (opt_list)
    return list_config (argc, argv, dir, cancellable, error);
  else
    return flatpak_fail (error, _("Must specify one of --list, --get, --set or --unset"));

  return TRUE;
}

gboolean
flatpak_complete_config (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* KEY */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      if (opt_set || opt_get || opt_unset)
        {
          int i;
          for (i = 0; i < G_N_ELEMENTS (keys); i++)
            flatpak_complete_word (completion, "%s", keys[i].name);
        }

      break;
    }

  return TRUE;
}
