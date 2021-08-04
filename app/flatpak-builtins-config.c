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

static gboolean
looks_like_a_language (const char *s)
{
  int len = strlen (s);
  int i;

  if (g_str_equal (s, "C") ||
      g_str_equal (s, "POSIX"))
    return TRUE;

  if (len < 2)
    return FALSE;

  for (i = 0; i < len; i++)
    {
      if (!g_ascii_isalpha (s[i]) || !g_ascii_islower (s[i]))
        return FALSE;
    }

  return TRUE;
}

static gboolean
looks_like_a_territory (const char *s)
{
  gsize len = strlen (s);
  gsize i;

  if (len < 2)
    return FALSE;

  for (i = 0; i < len; i++)
    {
      if (!g_ascii_isalpha (s[i]) || !g_ascii_isupper (s[i]))
        return FALSE;
    }

  return TRUE;
}

static gboolean
looks_like_a_codeset_or_modifier (const char *s)
{
  gsize len = strlen (s);
  gsize i;

  if (len < 1)
    return FALSE;

  for (i = 0; i < len; i++)
    {
      if (!g_ascii_isalnum (s[i]) && s[i] != '-')
        return FALSE;
    }

  return TRUE;
}

static gboolean
looks_like_a_locale (const char *s)
{
  g_autofree gchar *locale = g_strdup (s);
  gchar *language, *territory, *codeset, *modifier;

  modifier = strchr (locale, '@');
  if (modifier != NULL)
    *modifier++ = '\0';

  codeset = strchr (locale, '.');
  if (codeset != NULL)
    *codeset++ = '\0';

  territory = strchr (locale, '_');
  if (territory != NULL)
    *territory++ = '\0';

  language = locale;

  if (!looks_like_a_language (language))
    return FALSE;
  if (territory != NULL && !looks_like_a_territory (territory))
    return FALSE;
  if (codeset != NULL && !looks_like_a_codeset_or_modifier (codeset))
    return FALSE;
  if (modifier != NULL && !looks_like_a_codeset_or_modifier (modifier))
    return FALSE;

  return TRUE;
}

static char *
parse_locale (const char *value, GError **error)
{
  g_auto(GStrv) strs = NULL;
  int i;

  strs = g_strsplit (value, ";", 0);
  for (i = 0; strs[i]; i++)
    {
      if (!looks_like_a_language (strs[i]) && !looks_like_a_locale (strs[i]))
        {
          flatpak_fail (error, _("'%s' does not look like a language/locale code"), strs[i]);
          return NULL;
        }
    }

  return g_strdup (value);
}

static char *
parse_lang (const char *value, GError **error)
{
  g_auto(GStrv) strs = NULL;
  int i;

  if (strcmp (value, "*") == 0 ||
      strcmp (value, "*all*") == 0)
    return g_strdup ("");

  strs = g_strsplit (value, ";", 0);
  for (i = 0; strs[i]; i++)
    {
      if (!looks_like_a_language (strs[i]))
        {
          flatpak_fail (error, _("'%s' does not look like a language code"), strs[i]);
          return NULL;
        }
    }

  return g_strdup (value);
}

static char *
print_locale (const char *value)
{
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
  char *(*parse)(const char *value, GError **error);
  char *(*print)(const char *value);
  char *(*get_default)(FlatpakDir * dir);
} ConfigKey;

ConfigKey keys[] = {
  { "languages", parse_lang, print_lang, get_lang_default },
  { "extra-languages", parse_locale, print_locale, NULL },
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
list_config (GOptionContext *context, int argc, char **argv, FlatpakDir *dir, GCancellable *cancellable, GError **error)
{
  int i;

  if (argc != 1)
    return usage_error (context, _("Too many arguments for --list"), error);

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
          g_print (_(" (default: %s)"), printed);
        }
      g_print ("\n");
    }

  return TRUE;
}

static gboolean
get_config (GOptionContext *context, int argc, char **argv, FlatpakDir *dir, GCancellable *cancellable, GError **error)
{
  ConfigKey *key;
  g_autofree char *value = NULL;

  if (argc < 2)
    return usage_error (context, _("You must specify KEY"), error);
  else if (argc > 2)
    return usage_error (context, _("Too many arguments for --get"), error);

  key = get_config_key (argv[1], error);
  if (key == NULL)
    return FALSE;

  value = print_config (dir, key);
  if (value)
    g_print ("%s\n", value);
  else
    g_print (_("*unset*\n"));

  return TRUE;
}

static gboolean
set_config (GOptionContext *context, int argc, char **argv, FlatpakDir *dir, GCancellable *cancellable, GError **error)
{
  ConfigKey *key;
  g_autofree char *parsed = NULL;

  if (argc < 3)
    return usage_error (context, _("You must specify KEY and VALUE"), error);
  else if (argc > 3)
    return usage_error (context, _("Too many arguments for --set"), error);

  key = get_config_key (argv[1], error);
  if (key == NULL)
    return FALSE;

  parsed = key->parse (argv[2], error);
  if (!parsed)
    return FALSE;

  if (!flatpak_dir_set_config (dir, key->name, parsed, error))
    return FALSE;

  return TRUE;
}

static gboolean
unset_config (GOptionContext *context, int argc, char **argv, FlatpakDir *dir, GCancellable *cancellable, GError **error)
{
  ConfigKey *key;

  if (argc < 2)
    return usage_error (context, _("You must specify KEY"), error);
  else if (argc > 2)
    return usage_error (context, _("Too many arguments for --unset"), error);

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
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  if (opt_get + opt_set + opt_unset + opt_list > 1)
    return usage_error (context, _("Can only use one of --list, --get, --set or --unset"), error);

  if (!opt_get && !opt_set && !opt_unset && !opt_list)
    opt_list = TRUE;

  if (opt_get)
    return get_config (context, argc, argv, dir, cancellable, error);
  else if (opt_set)
    return set_config (context, argc, argv, dir, cancellable, error);
  else if (opt_unset)
    return unset_config (context, argc, argv, dir, cancellable, error);
  else if (opt_list)
    return list_config (context, argc, argv, dir, cancellable, error);
  else
    return usage_error (context, _("Must specify one of --list, --get, --set or --unset"), error);

  return TRUE;
}

gboolean
flatpak_complete_config (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     NULL, NULL, NULL))
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
