/*
 * Copyright © 2014-2020 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include "flatpak-ref-utils-private.h"
#include "flatpak-run-private.h"
#include "flatpak-error.h"
#include "flatpak-utils-private.h"

FlatpakKinds
flatpak_kinds_from_kind (FlatpakRefKind kind)
{
  if (kind == FLATPAK_REF_KIND_RUNTIME)
    return FLATPAK_KINDS_RUNTIME;
  return FLATPAK_KINDS_APP;
}

static gboolean
is_valid_initial_name_character (gint c, gboolean allow_dash)
{
  return
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c == '_') || (allow_dash && c == '-');
}

static gboolean
is_valid_name_character (gint c, gboolean allow_dash)
{
  return
    is_valid_initial_name_character (c, allow_dash) ||
    (c >= '0' && c <= '9');
}

static const char *
find_last_char (const char *str, gsize len, int c)
{
  const char *p = str + len - 1;
  while (p >= str)
    {
      if (*p == c)
        return p;
      p--;
    }
  return NULL;
}

/**
 * flatpak_is_valid_name:
 * @string: The string to check
 * @len: The string length, or -1 for null-terminated
 * @error: Return location for an error
 *
 * Checks if @string is a valid application name.
 *
 * App names are composed of 3 or more elements separated by a period
 * ('.') character. All elements must contain at least one character.
 *
 * Each element must only contain the ASCII characters
 * "[A-Z][a-z][0-9]_-". Elements may not begin with a digit.
 * Additionally "-" is only allowed in the last element.
 *
 * App names must not begin with a '.' (period) character.
 *
 * App names must not exceed 255 characters in length.
 *
 * The above means that any app name is also a valid DBus well known
 * bus name, but not all DBus names are valid app names. The difference are:
 * 1) DBus name elements may contain '-' in the non-last element.
 * 2) DBus names require only two elements
 *
 * Returns: %TRUE if valid, %FALSE otherwise.
 *
 * Since: 2.26
 */
gboolean
flatpak_is_valid_name (const char *string,
                       gssize      len,
                       GError    **error)
{
  gboolean ret;
  const gchar *s;
  const gchar *end;
  const gchar *last_dot;
  int dot_count;
  gboolean last_element;

  g_return_val_if_fail (string != NULL, FALSE);

  ret = FALSE;

  if (len < 0)
    len = strlen (string);
  if (G_UNLIKELY (len == 0))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Name can't be empty"));
      goto out;
    }

  if (G_UNLIKELY (len > 255))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Name can't be longer than 255 characters"));
      goto out;
    }

  end = string + len;

  last_dot = find_last_char (string, len, '.');
  last_element = FALSE;

  s = string;
  if (G_UNLIKELY (*s == '.'))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Name can't start with a period"));
      goto out;
    }
  else if (G_UNLIKELY (!is_valid_initial_name_character (*s, last_element)))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Name can't start with %c"), *s);
      goto out;
    }

  s += 1;
  dot_count = 0;
  while (s != end)
    {
      if (*s == '.')
        {
          if (s == last_dot)
            last_element = TRUE;
          s += 1;
          if (G_UNLIKELY (s == end))
            {
              flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                  _("Name can't end with a period"));
              goto out;
            }
          if (!is_valid_initial_name_character (*s, last_element))
            {
              if (*s == '-')
                flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                    _("Only last name segment can contain -"));
              else
                flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                    _("Name segment can't start with %c"), *s);
              goto out;
            }
          dot_count++;
        }
      else if (G_UNLIKELY (!is_valid_name_character (*s, last_element)))
        {
          if (*s == '-')
            flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                _("Only last name segment can contain -"));
          else
            flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                _("Name can't contain %c"), *s);
          goto out;
        }
      s += 1;
    }

  if (G_UNLIKELY (dot_count < 2))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Names must contain at least 2 periods"));
      goto out;
    }

  ret = TRUE;

out:
  return ret;
}

gboolean
flatpak_has_name_prefix (const char *string,
                         const char *name)
{
  const char *rest;

  if (!g_str_has_prefix (string, name))
    return FALSE;

  rest = string + strlen (name);
  return
    *rest == 0 ||
    *rest == '.' ||
    !is_valid_name_character (*rest, FALSE);
}


gboolean
flatpak_name_matches_one_wildcard_prefix (const char         *name,
                                          const char * const *wildcarded_prefixes,
                                          gboolean            require_exact_match)
{
  const char * const *iter = wildcarded_prefixes;
  const char *remainder;
  gsize longest_match_len = 0;

  /* Find longest valid match */
  for (; *iter != NULL; ++iter)
    {
      const char *prefix = *iter;
      gsize prefix_len = strlen (prefix);
      gsize match_len = strlen (prefix);
      gboolean has_wildcard = FALSE;
      const char *end_of_match;

      if (g_str_has_suffix (prefix, ".*"))
        {
          has_wildcard = TRUE;
          prefix_len -= 2;
        }

      if (strncmp (name, prefix, prefix_len) != 0)
        continue;

      end_of_match = name + prefix_len;

      if (has_wildcard &&
          end_of_match[0] == '.' &&
          is_valid_initial_name_character (end_of_match[1], TRUE))
        {
          end_of_match += 2;
          while (*end_of_match != 0 &&
                 (is_valid_name_character (*end_of_match, TRUE) ||
                  (end_of_match[0] == '.' &&
                   is_valid_initial_name_character (end_of_match[1], TRUE))))
            end_of_match++;
        }

      match_len = end_of_match - name;

      if (match_len > longest_match_len)
        longest_match_len = match_len;
    }

  if (longest_match_len == 0)
    return FALSE;

  if (require_exact_match)
    return name[longest_match_len] == 0;

  /* non-exact matches can be exact, or can be followed by characters that would make
   * not be part of the last element in the matched prefix, due to being invalid or
   * a new element. As a special case we explicitly disallow dash here, even though
   * it iss typically allowed in the final element of a name, this allows you too sloppily
   * match org.the.App with org.the.App-symbolic[.png] or org.the.App-settings[.desktop].
   */
  remainder = name + longest_match_len;
  return
    *remainder == 0 ||
    *remainder == '.' ||
    !is_valid_name_character (*remainder, FALSE);
}


static gboolean
is_valid_arch_character (char c)
{
  return
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c >= '0' && c <= '9') ||
    (c == '_');
}

gboolean
flatpak_is_valid_arch (const char *string,
                       gssize      len,
                       GError    **error)
{
  const gchar *end;

  if (len < 0)
    len = strlen (string);

  if (G_UNLIKELY (len == 0))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Arch can't be empty"));
      return FALSE;
    }

  end = string + len;

  while (string != end)
    {
      if (G_UNLIKELY (!is_valid_arch_character (*string)))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                              _("Arch can't contain %c"), *string);
          return FALSE;
        }
      string += 1;
    }

  return TRUE;
}


static gboolean
is_valid_initial_branch_character (gint c)
{
  return
    (c >= '0' && c <= '9') ||
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c == '_') ||
    (c == '-');
}

static gboolean
is_valid_branch_character (gint c)
{
  return
    is_valid_initial_branch_character (c) ||
    (c == '.');
}

/**
 * flatpak_is_valid_branch:
 * @string: The string to check
 * @len: The string length, or -1 for null-terminated
 * @error: return location for an error
 *
 * Checks if @string is a valid branch name.
 *
 * Branch names must only contain the ASCII characters
 * "[A-Z][a-z][0-9]_-.".
 * Branch names may not begin with a period.
 * Branch names must contain at least one character.
 *
 * Returns: %TRUE if valid, %FALSE otherwise.
 *
 * Since: 2.26
 */
gboolean
flatpak_is_valid_branch (const char *string,
                         gssize      len,
                         GError    **error)
{
  gboolean ret;
  const gchar *s;
  const gchar *end;

  g_return_val_if_fail (string != NULL, FALSE);

  ret = FALSE;

  if (len < 0)
    len = strlen (string);
  if (G_UNLIKELY (len == 0))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Branch can't be empty"));
      goto out;
    }

  end = string + len;

  s = string;
  if (G_UNLIKELY (!is_valid_initial_branch_character (*s)))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Branch can't start with %c"), *s);
      goto out;
    }

  s += 1;
  while (s != end)
    {
      if (G_UNLIKELY (!is_valid_branch_character (*s)))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                              _("Branch can't contain %c"), *s);
          goto out;
        }
      s += 1;
    }

  ret = TRUE;

out:
  return ret;
}


/* Dashes are only valid in the last part of the app id, so
   we replace them with underscore so we can suffix the id */
char *
flatpak_make_valid_id_prefix (const char *orig_id)
{
  char *id, *t;

  id = g_strdup (orig_id);
  t = id;
  while (*t != 0 && *t != '/')
    {
      if (*t == '-')
        *t = '_';

      t++;
    }

  return id;
}

static gboolean
str_has_suffix (const gchar *str,
                gsize        str_len,
                const gchar *suffix)
{
  gsize suffix_len;

  suffix_len = strlen (suffix);
  if (str_len < suffix_len)
    return FALSE;

  return strncmp (str + str_len - suffix_len, suffix, suffix_len) == 0;
}


gboolean
flatpak_id_has_subref_suffix (const char *id,
                              gssize id_len)
{
  if (id_len < 0)
    id_len = strlen (id);

  return
    str_has_suffix (id, id_len, ".Locale") ||
    str_has_suffix (id, id_len, ".Debug") ||
    str_has_suffix (id, id_len, ".Sources");
}


static gboolean
str_has_prefix (const gchar *str,
                gsize        str_len,
                const gchar *prefix)
{
  gsize prefix_len;

  prefix_len = strlen (prefix);
  if (str_len < prefix_len)
    return FALSE;

  return strncmp (str, prefix, prefix_len) == 0;
}

static const char *
skip_segment (const char *s)
{
  const char *slash;

  slash = strchr (s, '/');
  if (slash)
    return slash + 1;
  return s + strlen (s);
}

static int
compare_segment (const char *s1, const char *s2)
{
  gint c1, c2;

  while (*s1 && *s1 != '/' &&
         *s2 && *s2 != '/')
    {
      c1 = *s1;
      c2 = *s2;
      if (c1 != c2)
        return c1 - c2;
      s1++;
      s2++;
    }

  c1 = *s1;
  if (c1 == '/')
    c1 = 0;
  c2 = *s2;
  if (c2 == '/')
    c2 = 0;

  return c1 - c2;
}

int
flatpak_compare_ref (const char *ref1, const char *ref2)
{
  int res;
  int i;

  /* Skip first element and do per-segment compares for rest */
  for (i = 0; i < 3; i++)
    {
      ref1 = skip_segment (ref1);
      ref2 = skip_segment (ref2);

      res = compare_segment (ref1, ref2);
      if (res != 0)
        return res;
    }
  return 0;
}

struct _FlatpakDecomposed {
  int ref_count;
  guint16 ref_offset;
  guint16 id_offset;
  guint16 arch_offset;
  guint16 branch_offset;
  char *data;

  /* This is only used when we're directly manipulating sideload repos, by giving
   * a file:// uri as the remote name. Typically we don't really care about collection ids
   * internally in flatpak as we use refs tied to a remote. */
  char *collection_id;
};

static gboolean
is_valid_initial_remote_name_character (gint c)
{
  return
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c >= '0' && c <= '9') ||
    (c == '_');
}

static gboolean
is_valid_remote_name_character (gint c)
{
  return
    is_valid_initial_remote_name_character (c) ||
    c == '-' ||
    c == '.';
}

static gboolean
is_valid_remote_name (const char *remote,
                      gsize len)
{
  const char *end;

  if (len == 0)
    return FALSE;

  end = remote + len;

  if (!is_valid_initial_remote_name_character (*remote++))
    return FALSE;

  while (remote < end)
    {
      char c = *remote++;
      if (!is_valid_remote_name_character (c))
        return FALSE;
    }

  return TRUE;
}


static FlatpakDecomposed *
_flatpak_decomposed_new (char            *ref,
                         gboolean         allow_refspec,
                         gboolean         take,
                         GError         **error)
{
  g_autoptr(GError) local_error = NULL;
  const char *p;
  const char *slash;
  gsize ref_offset;
  gsize id_offset;
  gsize arch_offset;
  gsize branch_offset;
  gsize len;
  FlatpakDecomposed *decomposed;

  /* We want to use uint16 to store offset, so fail on uselessly large refs */
  len = strlen (ref);
  if (len > 0xffff)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Ref too long"));
      return NULL;
    }

  p = ref;
  if (allow_refspec)
    {
      const char *colon = strchr (p, ':');
      if (colon != NULL)
        {
          if (!is_valid_remote_name (ref, colon - ref))
            {
              flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid remote name"));
              return NULL;
            }
          p = colon + 1;
        }
    }
  ref_offset = p - ref;

  if (g_str_has_prefix (p, "app/"))
    p += strlen ("app/");
  else if (g_str_has_prefix (p, "runtime/"))
    p += strlen ("runtime/");
  else
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("%s is not application or runtime"), ref);
      return NULL;
    }

  id_offset = p - ref;

  slash = strchr (p, '/');
  if (slash == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Wrong number of components in %s"), ref);
      return NULL;
    }

  if (!flatpak_is_valid_name (p, slash - p, &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid name %.*s: %s"), (int)(slash - p), p, local_error->message);
      return NULL;
    }

  p = slash + 1;

  arch_offset = p - ref;

  slash = strchr (p, '/');
  if (slash == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Wrong number of components in %s"), ref);
      return NULL;
    }

  if (!flatpak_is_valid_arch (p, slash - p, &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid arch: %.*s: %s"), (int)(slash - p), p, local_error->message);
      return NULL;
    }

  p = slash + 1;
  branch_offset = p - ref;

  slash = strchr (p, '/');
  if (slash != NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Wrong number of components in %s"), ref);
      return NULL;
    }

  if (!flatpak_is_valid_branch (p, -1, &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid branch %s: %s"), p, local_error->message);
      return NULL;
    }

  if (take)
    {
      decomposed = g_malloc (sizeof (FlatpakDecomposed));
      decomposed->data = ref;
    }
  else
    {
      char *inline_data;

      /* Store the dup:ed ref inline */
      decomposed = g_malloc (sizeof (FlatpakDecomposed) + strlen (ref) + 1);
      inline_data = (char *)decomposed + sizeof (FlatpakDecomposed);

      strcpy (inline_data, ref);
      decomposed->data = inline_data;
    }
  decomposed->ref_count = 1;
  decomposed->collection_id = NULL;
  decomposed->ref_offset = (guint16)ref_offset;
  decomposed->id_offset = (guint16)id_offset;
  decomposed->arch_offset = (guint16)arch_offset;
  decomposed->branch_offset = (guint16)branch_offset;

  return decomposed;
}

FlatpakDecomposed *
flatpak_decomposed_new_from_ref (const char         *ref,
                                 GError            **error)
{
  return _flatpak_decomposed_new ((char *)ref, FALSE, FALSE, error);
}

FlatpakDecomposed *
flatpak_decomposed_new_from_refspec (const char         *refspec,
                                     GError            **error)
{
  return _flatpak_decomposed_new ((char *)refspec, TRUE, FALSE, error);
}

FlatpakDecomposed *
flatpak_decomposed_new_from_ref_take (char         *ref,
                                      GError      **error)
{
  return _flatpak_decomposed_new (ref, FALSE, TRUE, error);
}

FlatpakDecomposed *
flatpak_decomposed_new_from_refspec_take (char         *refspec,
                                          GError       **error)
{
  return _flatpak_decomposed_new (refspec, TRUE, TRUE, error);
}

FlatpakDecomposed *
flatpak_decomposed_new_from_col_ref      (const char         *ref,
                                          const char         *collection_id,
                                          GError            **error)
{
  g_autoptr(FlatpakDecomposed) decomposed = NULL;

  if (collection_id != NULL &&
      !ostree_validate_collection_id (collection_id, error))
    return FALSE;

  decomposed = flatpak_decomposed_new_from_ref (ref, error);
  if (decomposed == NULL)
    return FALSE;

  decomposed->collection_id = g_strdup (collection_id);

  return g_steal_pointer (&decomposed);
}

static FlatpakDecomposed *
_flatpak_decomposed_new_from_decomposed (FlatpakDecomposed  *old,
                                         FlatpakKinds        opt_kind,
                                         const char         *opt_id,
                                         gssize              opt_id_len,
                                         const char         *opt_arch,
                                         gssize              opt_arch_len,
                                         const char         *opt_branch,
                                         gssize              opt_branch_len,
                                         GError            **error)
{
  FlatpakDecomposed *decomposed;
  g_autoptr(GError) local_error = NULL;
  char *inline_data;
  const char *kind_str;
  gsize kind_len;
  gsize id_len;
  gsize arch_len;
  gsize branch_len;
  gsize ref_len;
  char *ref;
  gsize offset;

  if (old == NULL)
    {
      g_assert (opt_kind != 0);
      g_assert (opt_id != NULL);
      g_assert (opt_arch != NULL);
      g_assert (opt_branch != NULL);
    }

  if (opt_kind == 0)
    kind_str = flatpak_decomposed_get_kind_str (old);
  else if (opt_kind == FLATPAK_KINDS_APP)
    kind_str = "app";
  else
    kind_str = "runtime";

  kind_len = strlen (kind_str);

  if (opt_id)
    {
      if (opt_id_len == -1)
        id_len = strlen (opt_id);
      else
        id_len = opt_id_len;

      if (!flatpak_is_valid_name (opt_id, id_len, &local_error))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid name %s: %s"), opt_id, local_error->message);
          return NULL;
        }
    }
  else
    {
      opt_id = flatpak_decomposed_peek_id (old, &id_len);
    }

  if (opt_arch)
    {
      if (opt_arch_len == -1)
        arch_len = strlen (opt_arch);
      else
        arch_len = opt_arch_len;

      if (!flatpak_is_valid_arch (opt_arch, arch_len, &local_error))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid arch: %s: %s"), opt_arch, local_error->message);
          return NULL;
        }

    }
  else
    {
      opt_arch = flatpak_decomposed_peek_arch (old, &arch_len);
    }

  if (opt_branch)
    {
      if (opt_branch_len == -1)
        branch_len = strlen (opt_branch);
      else
        branch_len = opt_branch_len;

      if (!flatpak_is_valid_branch (opt_branch, branch_len, &local_error))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid branch: %s: %s"), opt_branch, local_error->message);
          return NULL;
        }
    }
  else
    {
      opt_branch = flatpak_decomposed_peek_branch (old, &branch_len);
    }

  ref_len = kind_len + 1 + id_len + 1 + arch_len + 1 + branch_len;
  if (ref_len > 0xffff)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Ref too long"));
      return NULL;
    }

  /* Store the ref inline */
  decomposed = g_malloc (sizeof (FlatpakDecomposed) + ref_len + 1);
  inline_data = (char *)decomposed + sizeof (FlatpakDecomposed);

  decomposed->ref_count = 1;
  decomposed->data = inline_data;
  decomposed->collection_id = NULL;

  ref = inline_data;
  offset = 0;

  decomposed->ref_offset = (guint16)offset;
  memcpy (ref + offset, kind_str, kind_len);
  offset += kind_len;

  memcpy (ref + offset, "/", 1);
  offset += 1;

  decomposed->id_offset = (guint16)offset;
  memcpy (ref + offset, opt_id, id_len);
  offset += id_len;

  memcpy (ref + offset, "/", 1);
  offset += 1;

  decomposed->arch_offset = (guint16)offset;
  memcpy (ref + offset, opt_arch, arch_len);
  offset += arch_len;

  memcpy (ref + offset, "/", 1);
  offset += 1;

  decomposed->branch_offset = (guint16)offset;
  memcpy (ref + offset, opt_branch, branch_len);
  offset += branch_len;

  g_assert (offset == ref_len);
  *(ref + offset) = 0;

  return decomposed;
}

FlatpakDecomposed *
flatpak_decomposed_new_from_decomposed (FlatpakDecomposed  *old,
                                        FlatpakKinds        opt_kind,
                                        const char         *opt_id,
                                        const char         *opt_arch,
                                        const char         *opt_branch,
                                        GError            **error)
{
  return _flatpak_decomposed_new_from_decomposed (old, opt_kind,
                                                  opt_id, -1,
                                                  opt_arch, -1,
                                                  opt_branch, -1,
                                                  error);
}

FlatpakDecomposed *
flatpak_decomposed_new_from_parts (FlatpakKinds        kind,
                                   const char         *id,
                                   const char         *arch,
                                   const char         *branch,
                                   GError            **error)
{
  g_assert (kind == FLATPAK_KINDS_APP || kind == FLATPAK_KINDS_RUNTIME);
  g_assert (id != NULL);

  if (branch == NULL)
    branch = "master";

  if (arch == NULL)
    arch = flatpak_get_arch ();

  return flatpak_decomposed_new_from_decomposed (NULL, kind, id, arch, branch, error);
}

FlatpakDecomposed *
flatpak_decomposed_new_from_pref (FlatpakKinds        kind,
                                  const char         *pref,
                                  GError            **error)
{
  const char *slash;
  const char *id;
  const char *arch;
  const char *branch;

  g_assert (kind == FLATPAK_KINDS_APP || kind == FLATPAK_KINDS_RUNTIME);
  g_assert (pref != NULL);

  id = pref;
  slash = strchr (id, '/');
  if (slash == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Wrong number of components in partial ref %s"), pref);
      return NULL;
    }

  arch = slash + 1;
  slash = strchr (arch, '/');
  if (slash == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Wrong number of components in partial ref %s"), pref);
      return NULL;
    }

  branch = slash + 1;
  slash = strchr (branch, '/');
  if (slash != NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Wrong number of components in partial ref %s"), pref);
      return NULL;
    }

  return _flatpak_decomposed_new_from_decomposed (NULL, kind,
                                                  id, arch - id - 1,
                                                  arch, branch - arch - 1,
                                                  branch, -1,
                                                  error);
}

FlatpakDecomposed *
flatpak_decomposed_ref (FlatpakDecomposed  *ref)
{
  g_atomic_int_inc (&ref->ref_count);
  return ref;
}

void
flatpak_decomposed_unref (FlatpakDecomposed  *ref)
{
  if (g_atomic_int_dec_and_test (&ref->ref_count))
    {
      char *inline_data = (char *)ref + sizeof (FlatpakDecomposed);
      if (ref->data != inline_data)
        g_free (ref->data);
      g_free (ref->collection_id);
      g_free (ref);
    }
}

const char *
flatpak_decomposed_get_ref (FlatpakDecomposed  *ref)
{
  return (const char *)&ref->data[ref->ref_offset];
}

char *
flatpak_decomposed_dup_ref (FlatpakDecomposed  *ref)
{
  return g_strdup (flatpak_decomposed_get_ref (ref));
}

const char *
flatpak_decomposed_get_refspec (FlatpakDecomposed  *ref)
{
  return (const char *)&ref->data[0];
}

char *
flatpak_decomposed_dup_refspec (FlatpakDecomposed  *ref)
{
  return g_strdup (flatpak_decomposed_get_refspec (ref));
}

char *
flatpak_decomposed_dup_remote (FlatpakDecomposed  *ref)
{
  if (ref->ref_offset == 0)
    return NULL;

  return g_strndup (ref->data, ref->ref_offset - 1);
}

/* Note: These are always NULL for regular refs, as they generally
 * tied to a remote and uses the collection_id from that.
 * The only case this is set is if we enumerate a remote
 * of the form `file:///path/to/repo`, as we don't then know
 * which remote name it is from.
 */
const char *
flatpak_decomposed_get_collection_id (FlatpakDecomposed  *ref)
{
  return ref->collection_id;
}

/* Note: These are always NULL for regular refs, as they generally
 * tied to a remote and uses the collection_id from that.
 * The only case this is set is if we enumerate a remote
 * of the form `file:///path/to/repo`, as we don't then know
 * which remote name it is from.
 */
char *
flatpak_decomposed_dup_collection_id (FlatpakDecomposed  *ref)
{
  return g_strdup (ref->collection_id);
}

gboolean
flatpak_decomposed_equal (FlatpakDecomposed  *ref_a,
                          FlatpakDecomposed  *ref_b)
{
  return strcmp (ref_a->data, ref_b->data) == 0 &&
    g_strcmp0 (ref_a->collection_id, ref_b->collection_id) == 0;
}

gint
flatpak_decomposed_strcmp (FlatpakDecomposed  *ref_a,
                           FlatpakDecomposed  *ref_b)
{
  int res = strcmp (ref_a->data, ref_b->data);
  if (res != 0)
    return res;

  return g_strcmp0 (ref_a->collection_id, ref_b->collection_id);
}

gint
flatpak_decomposed_strcmp_p (FlatpakDecomposed  **ref_a,
                             FlatpakDecomposed  **ref_b)
{
  return flatpak_decomposed_strcmp (*ref_a, *ref_b);
}

gboolean
flatpak_decomposed_equal_except_branch (FlatpakDecomposed  *ref_a,
                                        FlatpakDecomposed  *ref_b)
{
  return
    ref_a->branch_offset == ref_b->branch_offset &&
    strncmp (ref_a->data, ref_b->data, ref_a->branch_offset) == 0 &&
    g_strcmp0 (ref_a->collection_id, ref_b->collection_id) == 0;
}


guint
flatpak_decomposed_hash (FlatpakDecomposed  *ref)
{
  guint h = g_str_hash (ref->data);

  if (ref->collection_id)
    h |= g_str_hash (ref->collection_id);

  return h;
}

gboolean
flatpak_decomposed_is_app (FlatpakDecomposed  *ref)
{
  const char *r = flatpak_decomposed_get_ref (ref);
  return r[0] == 'a';
}

gboolean
flatpak_decomposed_is_runtime (FlatpakDecomposed  *ref)
{
  const char *r = flatpak_decomposed_get_ref (ref);
  return r[0] == 'r';
}

FlatpakKinds
flatpak_decomposed_get_kinds (FlatpakDecomposed  *ref)
{
  if (flatpak_decomposed_is_app (ref))
    return FLATPAK_KINDS_APP;
  else
    return FLATPAK_KINDS_RUNTIME;
}

FlatpakRefKind
flatpak_decomposed_get_kind (FlatpakDecomposed  *ref)
{
  if (flatpak_decomposed_is_app (ref))
    return FLATPAK_REF_KIND_APP;
  else
    return FLATPAK_REF_KIND_RUNTIME;
}

const char *
flatpak_decomposed_get_kind_str (FlatpakDecomposed  *ref)
{
  if (flatpak_decomposed_is_app (ref))
    return "app";
  else
    return "runtime";
}

const char *
flatpak_decomposed_get_kind_metadata_group (FlatpakDecomposed  *ref)
{
  if (flatpak_decomposed_is_app (ref))
    return FLATPAK_METADATA_GROUP_APPLICATION;
  else
    return FLATPAK_METADATA_GROUP_RUNTIME;
}

/* A slashed string ends at '/' instead of nul */
static gboolean
slashed_str_equal (const char *slashed_str, const char *str)
{
  char c;
  while ((c = *str) != 0)
    {
      char s_c = *slashed_str;
      if (s_c == '/')
        return FALSE; /* slashed_str stopped early */

      if (s_c != c)
        return FALSE; /* slashed_str not same */

      str++;
      slashed_str++;
    }

  if (*slashed_str != '/') /* str stopped early */
    return FALSE;

  return TRUE;
}

/* These are for refs, so ascii case only */
static gboolean
slashed_str_strcasestr (const char *haystack,
                        gsize haystack_len,
                        const char *needle)
{
  gssize needle_len = strlen (needle);

  if (needle_len > haystack_len)
    return FALSE;

  if (needle_len == 0)
    return TRUE;

  for (gssize i = 0; i <= haystack_len - needle_len; i++)
    {
      if (g_ascii_strncasecmp (haystack + i, needle, needle_len) == 0)
        return TRUE;
    }

  return FALSE;
}

const char *
flatpak_decomposed_get_pref (FlatpakDecomposed  *ref)
{
  return &ref->data[ref->id_offset];
}

char *
flatpak_decomposed_dup_pref (FlatpakDecomposed  *ref)
{
  return g_strdup (flatpak_decomposed_get_pref (ref));
}

const char *
flatpak_decomposed_peek_id (FlatpakDecomposed  *ref,
                            gsize              *out_len)
{
  if (out_len)
    *out_len = ref->arch_offset - ref->id_offset - 1;
  return &ref->data[ref->id_offset];
}

char *
flatpak_decomposed_dup_id (FlatpakDecomposed  *ref)
{
  gsize len;
  const char *ref_id = flatpak_decomposed_peek_id (ref, &len);

  return g_strndup (ref_id, len);
}

char *
flatpak_decomposed_dup_readable_id (FlatpakDecomposed  *ref)
{
  gsize len;
  const char *ref_id = flatpak_decomposed_peek_id (ref, &len);
  const char *start;
  gboolean is_debug, is_sources, is_locale, is_docs, is_sdk, is_platform, is_baseapp;
  GString *s;

  is_debug = str_has_suffix (ref_id, len, ".Debug");
  if (is_debug)
    len -= strlen (".Debug");

  is_sources = str_has_suffix (ref_id, len, ".Sources");
  if (is_sources)
    len -= strlen (".Sources");

  is_locale = str_has_suffix (ref_id, len, ".Locale");
  if (is_locale)
    len -= strlen (".Locale");

  is_docs = str_has_suffix (ref_id, len, ".Docs");
  if (is_docs)
    len -= strlen (".Docs");

  is_baseapp = str_has_suffix (ref_id, len, ".BaseApp");
  if (is_baseapp)
    len -= strlen (".BaseApp");

  is_platform = str_has_suffix (ref_id, len, ".Platform");
  if (is_platform)
    len -= strlen (".Platform");

  is_sdk = str_has_suffix (ref_id, len, ".Sdk");
  if (is_sdk)
    len -= strlen (".Sdk");

  start = ref_id + len;

  while (start > ref_id && start[-1] != '.')
    start--;

  len -= (start - ref_id);

  s = g_string_new ("");
  g_string_append_len (s, start, len);
  if (is_sdk)
    g_string_append (s, _(" development platform"));
  if (is_platform)
    g_string_append (s, _(" platform"));
  if (is_baseapp)
    g_string_append (s, _(" application base"));

  if (is_debug)
    g_string_append (s, _(" debug symbols"));
  if (is_sources)
    g_string_append (s, _(" sourcecode"));
  if (is_locale)
    g_string_append (s, _(" translations"));
  if (is_docs)
    g_string_append (s, _(" docs"));

  return g_string_free (s, FALSE);
}

gboolean
flatpak_decomposed_is_id (FlatpakDecomposed  *ref,
                          const char         *id)
{
  const char *ref_id = flatpak_decomposed_peek_id (ref, NULL);

  return slashed_str_equal (ref_id, id);
}

gboolean
flatpak_decomposed_id_has_suffix (FlatpakDecomposed  *ref,
                                  const char         *suffix)
{
  gsize id_len;
  const char *ref_id = flatpak_decomposed_peek_id (ref, &id_len);
  return str_has_suffix (ref_id, id_len, suffix);
}

gboolean
flatpak_decomposed_id_has_prefix (FlatpakDecomposed  *ref,
                                  const char         *prefix)
{
  gsize id_len;
  const char *ref_id = flatpak_decomposed_peek_id (ref, &id_len);
  return str_has_prefix (ref_id, id_len, prefix);
}


/* See if the given id looks similar to this ref. The
 * Levenshtein distance constant was chosen pretty arbitrarily. */
gboolean
flatpak_decomposed_is_id_fuzzy (FlatpakDecomposed  *ref,
                                const char         *id)
{
  gsize ref_id_len;
  const char *ref_id = flatpak_decomposed_peek_id (ref, &ref_id_len);

  if (slashed_str_strcasestr (ref_id, ref_id_len, id))
    return TRUE;

  return flatpak_levenshtein_distance (id, -1, ref_id, ref_id_len) <= 2;
}

gboolean
flatpak_decomposed_id_is_subref (FlatpakDecomposed  *ref)
{
  gsize ref_id_len;
  const char *ref_id = flatpak_decomposed_peek_id (ref, &ref_id_len);

  return flatpak_id_has_subref_suffix (ref_id, ref_id_len);
}

const char *
flatpak_decomposed_peek_arch (FlatpakDecomposed  *ref,
                              gsize *out_len)
{
  if (out_len)
    *out_len = ref->branch_offset - ref->arch_offset - 1;
  return &ref->data[ref->arch_offset];
}

char *
flatpak_decomposed_dup_arch (FlatpakDecomposed  *ref)
{
  gsize len;
  const char *ref_arch = flatpak_decomposed_peek_arch (ref, &len);

  return g_strndup (ref_arch, len);
}

gboolean
flatpak_decomposed_is_arch (FlatpakDecomposed  *ref,
                            const char         *arch)
{
  const char *ref_arch = flatpak_decomposed_peek_arch (ref, NULL);

  return slashed_str_equal (ref_arch, arch);
}

gboolean
flatpak_decomposed_is_arches (FlatpakDecomposed  *ref,
                              gssize              len,
                              const char        **arches)
{
  const char *ref_arch = flatpak_decomposed_peek_arch (ref, NULL);

  if (len < 0)
    {
      len = 0;
      while (arches[len] != NULL)
        len++;
    }

  for (int i = 0; i < len; i++)
    {
      if (slashed_str_equal (ref_arch, arches[i]))
        return TRUE;
    }

  return FALSE;
}

/* We can add a getter for this, because the branch is last so guaranteed to be null-terminated */
const char *
flatpak_decomposed_get_branch (FlatpakDecomposed  *ref)
{
  return &ref->data[ref->branch_offset];
}

const char *
flatpak_decomposed_peek_branch (FlatpakDecomposed  *ref,
                                gsize              *out_len)
{
  const char *branch = flatpak_decomposed_get_branch (ref);
  if (out_len)
    *out_len = strlen (branch);
  return branch;
}

char *
flatpak_decomposed_dup_branch (FlatpakDecomposed  *ref)
{
  const char *ref_branch = flatpak_decomposed_peek_branch (ref, NULL);

  return g_strdup (ref_branch);
}

gboolean
flatpak_decomposed_is_branch (FlatpakDecomposed  *ref,
                              const char         *branch)
{
  const char *ref_branch = flatpak_decomposed_get_branch (ref);

  return strcmp (ref_branch, branch) == 0;
}

static const char *
next_element (const char **partial_ref)
{
  const char *slash;
  const char *end;

  slash = (const char *) strchr (*partial_ref, '/');
  if (slash != NULL)
    {
      end = slash;
      *partial_ref = slash + 1;
    }
  else
    {
      end = *partial_ref + strlen (*partial_ref);
      *partial_ref = end;
    }

  return end;
}

FlatpakKinds
flatpak_kinds_from_bools (gboolean app, gboolean runtime)
{
  FlatpakKinds kinds = 0;

  if (app)
    kinds |= FLATPAK_KINDS_APP;

  if (runtime)
    kinds |= FLATPAK_KINDS_RUNTIME;

  if (kinds == 0)
    kinds = FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME;

  return kinds;
}

static gboolean
_flatpak_split_partial_ref_arg (const char   *partial_ref,
                                gboolean      validate,
                                FlatpakKinds  default_kinds,
                                const char   *default_arch,
                                const char   *default_branch,
                                FlatpakKinds *out_kinds,
                                char        **out_id,
                                char        **out_arch,
                                char        **out_branch,
                                GError      **error)
{
  const char *id_start = NULL;
  const char *id_end = NULL;
  g_autofree char *id = NULL;
  const char *arch_start = NULL;
  const char *arch_end = NULL;
  g_autofree char *arch = NULL;
  const char *branch_start = NULL;
  const char *branch_end = NULL;
  g_autofree char *branch = NULL;
  g_autoptr(GError) local_error = NULL;
  FlatpakKinds kinds = 0;

  if (g_str_has_prefix (partial_ref, "app/"))
    {
      partial_ref += strlen ("app/");
      kinds = FLATPAK_KINDS_APP;
    }
  else if (g_str_has_prefix (partial_ref, "runtime/"))
    {
      partial_ref += strlen ("runtime/");
      kinds = FLATPAK_KINDS_RUNTIME;
    }
  else
    kinds = default_kinds;

  id_start = partial_ref;
  id_end = next_element (&partial_ref);
  id = g_strndup (id_start, id_end - id_start);

  if (validate && !flatpak_is_valid_name (id, -1, &local_error))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid id %s: %s"), id, local_error->message);

  arch_start = partial_ref;
  arch_end = next_element (&partial_ref);
  if (arch_end != arch_start)
    arch = g_strndup (arch_start, arch_end - arch_start);
  else
    arch = g_strdup (default_arch);

  branch_start = partial_ref;
  branch_end = next_element (&partial_ref);
  if (branch_end != branch_start)
    branch = g_strndup (branch_start, branch_end - branch_start);
  else
    branch = g_strdup (default_branch);

  if (validate && branch != NULL && !flatpak_is_valid_branch (branch, -1, &local_error))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid branch %s: %s"), branch, local_error->message);

  if (out_kinds)
    *out_kinds = kinds;
  if (out_id != NULL)
    *out_id = g_steal_pointer (&id);
  if (out_arch != NULL)
    *out_arch = g_steal_pointer (&arch);
  if (out_branch != NULL)
    *out_branch = g_steal_pointer (&branch);

  return TRUE;
}

gboolean
flatpak_split_partial_ref_arg (const char   *partial_ref,
                               FlatpakKinds  default_kinds,
                               const char   *default_arch,
                               const char   *default_branch,
                               FlatpakKinds *out_kinds,
                               char        **out_id,
                               char        **out_arch,
                               char        **out_branch,
                               GError      **error)
{
  return _flatpak_split_partial_ref_arg (partial_ref,
                                         TRUE,
                                         default_kinds,
                                         default_arch,
                                         default_branch,
                                         out_kinds,
                                         out_id,
                                         out_arch,
                                         out_branch,
                                         error);
}

gboolean
flatpak_split_partial_ref_arg_novalidate (const char   *partial_ref,
                                          FlatpakKinds  default_kinds,
                                          const char   *default_arch,
                                          const char   *default_branch,
                                          FlatpakKinds *out_kinds,
                                          char        **out_id,
                                          char        **out_arch,
                                          char        **out_branch)
{
  return _flatpak_split_partial_ref_arg (partial_ref,
                                         FALSE,
                                         default_kinds,
                                         default_arch,
                                         default_branch,
                                         out_kinds,
                                         out_id,
                                         out_arch,
                                         out_branch,
                                         NULL);
}

char *
flatpak_build_untyped_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (arch == NULL)
    arch = flatpak_get_arch ();

  return g_build_filename (runtime, arch, branch, NULL);
}

char *
flatpak_build_runtime_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (branch == NULL)
    branch = "master";

  if (arch == NULL)
    arch = flatpak_get_arch ();

  return g_build_filename ("runtime", runtime, arch, branch, NULL);
}

char *
flatpak_build_app_ref (const char *app,
                       const char *branch,
                       const char *arch)
{
  if (branch == NULL)
    branch = "master";

  if (arch == NULL)
    arch = flatpak_get_arch ();

  return g_build_filename ("app", app, arch, branch, NULL);
}

