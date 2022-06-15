/*
 * Copyright © 1995-1998 Free Software Foundation, Inc.
 * Copyright © 2014-2019 Red Hat, Inc
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

#include "flatpak-uri-private.h"

#if !GLIB_CHECK_VERSION (2, 66, 0)

struct _GUri {
  gchar     *scheme;
  gchar     *userinfo;
  gchar     *host;
  gint       port;
  gchar     *path;
  gchar     *query;
  gchar     *fragment;

  gchar     *user;
  gchar     *password;
  gchar     *auth_params;

  GUriFlags  flags;
  int ref_count;
};

GUri *
flatpak_g_uri_ref (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);
  g_atomic_int_inc (&uri->ref_count);
  return uri;
}

void
flatpak_g_uri_unref (GUri *uri)
{
  g_return_if_fail (uri != NULL);

  if (g_atomic_int_dec_and_test (&uri->ref_count))
    {
      g_free (uri->scheme);
      g_free (uri->userinfo);
      g_free (uri->host);
      g_free (uri->path);
      g_free (uri->query);
      g_free (uri->fragment);
      g_free (uri->user);
      g_free (uri->password);
      g_free (uri->auth_params);
      g_free (uri);
    }
}

static gboolean
flatpak_g_uri_char_is_unreserved (gchar ch)
{
  if (g_ascii_isalnum (ch))
    return TRUE;
  return ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

#define XDIGIT(c) ((c) <= '9' ? (c) - '0' : ((c) & 0x4F) - 'A' + 10)
#define HEXCHAR(s) ((XDIGIT (s[1]) << 4) + XDIGIT (s[2]))

static gssize
uri_decoder (gchar       **out,
             const gchar  *illegal_chars,
             const gchar  *start,
             gsize         length,
             gboolean      just_normalize,
             gboolean      www_form,
             GUriFlags     flags,
             GError      **error)
{
  gchar c;
  GString *decoded;
  const gchar *invalid, *s, *end;
  gssize len;

  if (!(flags & G_URI_FLAGS_ENCODED))
    just_normalize = FALSE;

  decoded = g_string_sized_new (length + 1);
  for (s = start, end = s + length; s < end; s++)
    {
      if (*s == '%')
        {
          if (s + 2 >= end ||
              !g_ascii_isxdigit (s[1]) ||
              !g_ascii_isxdigit (s[2]))
            {
              /* % followed by non-hex or the end of the string; this is an error */
              if (!(flags & G_URI_FLAGS_PARSE_RELAXED))
                {
                  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                       /* xgettext: no-c-format */
                                       _("Invalid %-encoding in URI"));
                  g_string_free (decoded, TRUE);
                  return -1;
                }

              /* In non-strict mode, just let it through; we *don't*
               * fix it to "%25", since that might change the way that
               * the URI's owner would interpret it.
               */
              g_string_append_c (decoded, *s);
              continue;
            }

          c = HEXCHAR (s);
          if (illegal_chars && strchr (illegal_chars, c))
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                   _("Illegal character in URI"));
              g_string_free (decoded, TRUE);
              return -1;
            }
          if (just_normalize && !flatpak_g_uri_char_is_unreserved (c))
            {
              /* Leave the % sequence there but normalize it. */
              g_string_append_c (decoded, *s);
              g_string_append_c (decoded, g_ascii_toupper (s[1]));
              g_string_append_c (decoded, g_ascii_toupper (s[2]));
              s += 2;
            }
          else
            {
              g_string_append_c (decoded, c);
              s += 2;
            }
        }
      else if (www_form && *s == '+')
        g_string_append_c (decoded, ' ');
      /* Normalize any illegal characters. */
      else if (just_normalize && (!g_ascii_isgraph (*s)))
        g_string_append_printf (decoded, "%%%02X", (guchar)*s);
      else
        g_string_append_c (decoded, *s);
    }

  len = decoded->len;
  g_assert (len >= 0);

  if (!(flags & G_URI_FLAGS_ENCODED) &&
      !g_utf8_validate (decoded->str, len, &invalid))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           _("Non-UTF-8 characters in URI"));
      g_string_free (decoded, TRUE);
      return -1;
    }

  if (out)
    *out = g_string_free (decoded, FALSE);
  else
    g_string_free (decoded, TRUE);

  return len;
}

static gboolean
uri_decode (gchar       **out,
            const gchar  *illegal_chars,
            const gchar  *start,
            gsize         length,
            gboolean      www_form,
            GUriFlags     flags,
            GError      **error)
{
  return uri_decoder (out, illegal_chars, start, length, FALSE, www_form, flags,
                      error) != -1;
}

static gboolean
uri_normalize (gchar       **out,
               const gchar  *start,
               gsize         length,
               GUriFlags     flags,
               GError      **error)
{
  return uri_decoder (out, NULL, start, length, TRUE, FALSE, flags,
                      error) != -1;
}

static gboolean
parse_ip_literal (const gchar  *start,
                  gsize         length,
                  GUriFlags     flags,
                  gchar       **out,
                  GError      **error)
{
  gchar *pct, *zone_id = NULL;
  gchar *addr = NULL;
  gsize addr_length = 0;
  gsize zone_id_length = 0;
  gchar *decoded_zone_id = NULL;

  if (start[length - 1] != ']')
    goto bad_ipv6_literal;

  /* Drop the square brackets */
  addr = g_strndup (start + 1, length - 2);
  addr_length = length - 2;

  /* If there's an IPv6 scope ID, split out the zone. */
  pct = strchr (addr, '%');
  if (pct != NULL)
    {
      *pct = '\0';

      if (addr_length - (pct - addr) >= 4 &&
          *(pct + 1) == '2' && *(pct + 2) == '5')
        {
          zone_id = pct + 3;
          zone_id_length = addr_length - (zone_id - addr);
        }
      else if (flags & G_URI_FLAGS_PARSE_RELAXED &&
               addr_length - (pct - addr) >= 2)
        {
          zone_id = pct + 1;
          zone_id_length = addr_length - (zone_id - addr);
        }
      else
        goto bad_ipv6_literal;

      g_assert (zone_id_length >= 1);
    }

  /* addr must be an IPv6 address */
  if (!g_hostname_is_ip_address (addr) || !strchr (addr, ':'))
    goto bad_ipv6_literal;

  /* Zone ID must be valid. It can contain %-encoded characters. */
  if (zone_id != NULL &&
      !uri_decode (&decoded_zone_id, NULL, zone_id, zone_id_length, FALSE,
                   flags, NULL))
    goto bad_ipv6_literal;

  /* Success */
  if (out != NULL && decoded_zone_id != NULL)
    *out = g_strconcat (addr, "%", decoded_zone_id, NULL);
  else if (out != NULL)
    *out = g_steal_pointer (&addr);

  g_free (addr);
  g_free (decoded_zone_id);

  return TRUE;

bad_ipv6_literal:
  g_free (addr);
  g_free (decoded_zone_id);
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
               _("Invalid IPv6 address ‘%.*s’ in URI"),
               (gint)length, start);

  return FALSE;
}

static gboolean
parse_host (const gchar  *start,
            gsize         length,
            GUriFlags     flags,
            gchar       **out,
            GError      **error)
{
  gchar *decoded = NULL, *host;
  gchar *addr = NULL;

  if (*start == '[')
    {
      if (!parse_ip_literal (start, length, flags, &host, error))
        return FALSE;
      goto ok;
    }

  if (g_ascii_isdigit (*start))
    {
      addr = g_strndup (start, length);
      if (g_hostname_is_ip_address (addr))
        {
          host = addr;
          goto ok;
        }
      g_free (addr);
    }

  if (flags & G_URI_FLAGS_NON_DNS)
    {
      if (!uri_normalize (&decoded, start, length, flags,
                          error))
        return FALSE;
      host = g_steal_pointer (&decoded);
      goto ok;
    }

  flags &= ~G_URI_FLAGS_ENCODED;
  if (!uri_decode (&decoded, NULL, start, length, FALSE, flags,
                   error))
    return FALSE;

  /* You're not allowed to %-encode an IP address, so if it wasn't
   * one before, it better not be one now.
   */
  if (g_hostname_is_ip_address (decoded))
    {
      g_free (decoded);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   _("Illegal encoded IP address ‘%.*s’ in URI"),
                   (gint)length, start);
      return FALSE;
    }

  if (g_hostname_is_non_ascii (decoded))
    {
      host = g_hostname_to_ascii (decoded);
      if (host == NULL)
        {
          g_free (decoded);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("Illegal internationalized hostname ‘%.*s’ in URI"),
                       (gint) length, start);
          return FALSE;
        }
    }
  else
    {
      host = g_steal_pointer (&decoded);
    }

 ok:
  if (out)
    *out = g_steal_pointer (&host);
  g_free (host);
  g_free (decoded);

  return TRUE;
}

static gboolean
parse_port (const gchar  *start,
            gsize         length,
            gint         *out,
            GError      **error)
{
  gchar *end;
  gulong parsed_port;

  /* strtoul() allows leading + or -, so we have to check this first. */
  if (!g_ascii_isdigit (*start))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   _("Could not parse port ‘%.*s’ in URI"),
                   (gint)length, start);
      return FALSE;
    }

  /* We know that *(start + length) is either '\0' or a non-numeric
   * character, so strtoul() won't scan beyond it.
   */
  parsed_port = strtoul (start, &end, 10);
  if (end != start + length)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   _("Could not parse port ‘%.*s’ in URI"),
                   (gint)length, start);
      return FALSE;
    }
  else if (parsed_port > 65535)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   _("Port ‘%.*s’ in URI is out of range"),
                   (gint)length, start);
      return FALSE;
    }

  if (out)
    *out = parsed_port;
  return TRUE;
}

static gboolean
parse_userinfo (const gchar  *start,
                gsize         length,
                GUriFlags     flags,
                gchar       **user,
                gchar       **password,
                gchar       **auth_params,
                GError      **error)
{
  const gchar *user_end = NULL, *password_end = NULL, *auth_params_end;

  auth_params_end = start + length;
  if (flags & G_URI_FLAGS_HAS_AUTH_PARAMS)
    password_end = memchr (start, ';', auth_params_end - start);
  if (!password_end)
    password_end = auth_params_end;
  if (flags & G_URI_FLAGS_HAS_PASSWORD)
    user_end = memchr (start, ':', password_end - start);
  if (!user_end)
    user_end = password_end;

  if (!uri_normalize (user, start, user_end - start, flags,
                      error))
    return FALSE;

  if (*user_end == ':')
    {
      start = user_end + 1;
      if (!uri_normalize (password, start, password_end - start, flags,
                          error))
        {
          if (user)
            g_clear_pointer (user, g_free);
          return FALSE;
        }
    }
  else if (password)
    *password = NULL;

  if (*password_end == ';')
    {
      start = password_end + 1;
      if (!uri_normalize (auth_params, start, auth_params_end - start, flags,
                          error))
        {
          if (user)
            g_clear_pointer (user, g_free);
          if (password)
            g_clear_pointer (password, g_free);
          return FALSE;
        }
    }
  else if (auth_params)
    *auth_params = NULL;

  return TRUE;
}

static gchar *
uri_cleanup (const gchar *uri_string)
{
  GString *copy;
  const gchar *end;

  /* Skip leading whitespace */
  while (g_ascii_isspace (*uri_string))
    uri_string++;

  /* Ignore trailing whitespace */
  end = uri_string + strlen (uri_string);
  while (end > uri_string && g_ascii_isspace (*(end - 1)))
    end--;

  /* Copy the rest, encoding unencoded spaces and stripping other whitespace */
  copy = g_string_sized_new (end - uri_string);
  while (uri_string < end)
    {
      if (*uri_string == ' ')
        g_string_append (copy, "%20");
      else if (g_ascii_isspace (*uri_string))
        ;
      else
        g_string_append_c (copy, *uri_string);
      uri_string++;
    }

  return g_string_free (copy, FALSE);
}

static gboolean
should_normalize_empty_path (const char *scheme)
{
  const char * const schemes[] = { "https", "http", "wss", "ws" };
  gsize i;
  for (i = 0; i < G_N_ELEMENTS (schemes); ++i)
    {
      if (!strcmp (schemes[i], scheme))
        return TRUE;
    }
  return FALSE;
}

static int
normalize_port (const char *scheme,
                int         port)
{
  const char *default_schemes[3] = { NULL };
  int i;

  switch (port)
    {
    case 21:
      default_schemes[0] = "ftp";
      break;
    case 80:
      default_schemes[0] = "http";
      default_schemes[1] = "ws";
      break;
    case 443:
      default_schemes[0] = "https";
      default_schemes[1] = "wss";
      break;
    default:
      break;
    }

  for (i = 0; default_schemes[i]; ++i)
    {
      if (!strcmp (scheme, default_schemes[i]))
        return -1;
    }

  return port;
}

static int
default_scheme_port (const char *scheme)
{
  if (strcmp (scheme, "http") == 0 || strcmp (scheme, "ws") == 0)
    return 80;

  if (strcmp (scheme, "https") == 0 || strcmp (scheme, "wss") == 0)
    return 443;

  if (strcmp (scheme, "ftp") == 0)
    return 21;

  return -1;
}

static gboolean
flatpak_g_uri_split_internal (const gchar  *uri_string,
                      GUriFlags     flags,
                      gchar       **scheme,
                      gchar       **userinfo,
                      gchar       **user,
                      gchar       **password,
                      gchar       **auth_params,
                      gchar       **host,
                      gint         *port,
                      gchar       **path,
                      gchar       **query,
                      gchar       **fragment,
                      GError      **error)
{
  const gchar *end, *colon, *at, *path_start, *semi, *question;
  const gchar *p, *bracket, *hostend;
  gchar *cleaned_uri_string = NULL;
  gchar *normalized_scheme = NULL;

  if (scheme)
    *scheme = NULL;
  if (userinfo)
    *userinfo = NULL;
  if (user)
    *user = NULL;
  if (password)
    *password = NULL;
  if (auth_params)
    *auth_params = NULL;
  if (host)
    *host = NULL;
  if (port)
    *port = -1;
  if (path)
    *path = NULL;
  if (query)
    *query = NULL;
  if (fragment)
    *fragment = NULL;

  if ((flags & G_URI_FLAGS_PARSE_RELAXED) && strpbrk (uri_string, " \t\n\r"))
    {
      cleaned_uri_string = uri_cleanup (uri_string);
      uri_string = cleaned_uri_string;
    }

  /* Find scheme */
  p = uri_string;
  while (*p && (g_ascii_isalpha (*p) ||
               (p > uri_string && (g_ascii_isdigit (*p) ||
                                   *p == '.' || *p == '+' || *p == '-'))))
    p++;

  if (p > uri_string && *p == ':')
    {
      normalized_scheme = g_ascii_strdown (uri_string, p - uri_string);
      if (scheme)
        *scheme = g_steal_pointer (&normalized_scheme);
      p++;
    }
  else
    {
      if (scheme)
        *scheme = NULL;
      p = uri_string;
    }

  /* Check for authority */
  if (strncmp (p, "//", 2) == 0)
    {
      p += 2;

      path_start = p + strcspn (p, "/?#");
      at = memchr (p, '@', path_start - p);
      if (at)
        {
          if (flags & G_URI_FLAGS_PARSE_RELAXED)
            {
              gchar *next_at;

              /* Any "@"s in the userinfo must be %-encoded, but
               * people get this wrong sometimes. Since "@"s in the
               * hostname are unlikely (and also wrong anyway), assume
               * that if there are extra "@"s, they belong in the
               * userinfo.
               */
              do
                {
                  next_at = memchr (at + 1, '@', path_start - (at + 1));
                  if (next_at)
                    at = next_at;
                }
              while (next_at);
            }

          if (user || password || auth_params ||
              (flags & (G_URI_FLAGS_HAS_PASSWORD|G_URI_FLAGS_HAS_AUTH_PARAMS)))
            {
              if (!parse_userinfo (p, at - p, flags,
                                   user, password, auth_params,
                                   error))
                goto fail;
            }

          if (!uri_normalize (userinfo, p, at - p, flags,
                              error))
            goto fail;

          p = at + 1;
        }

      if (flags & G_URI_FLAGS_PARSE_RELAXED)
        {
          semi = strchr (p, ';');
          if (semi && semi < path_start)
            {
              /* Technically, semicolons are allowed in the "host"
               * production, but no one ever does this, and some
               * schemes mistakenly use semicolon as a delimiter
               * marking the start of the path. We have to check this
               * after checking for userinfo though, because a
               * semicolon before the "@" must be part of the
               * userinfo.
               */
              path_start = semi;
            }
        }

      /* Find host and port. The host may be a bracket-delimited IPv6
       * address, in which case the colon delimiting the port must come
       * (immediately) after the close bracket.
       */
      if (*p == '[')
        {
          bracket = memchr (p, ']', path_start - p);
          if (bracket && *(bracket + 1) == ':')
            colon = bracket + 1;
          else
            colon = NULL;
        }
      else
        colon = memchr (p, ':', path_start - p);

      hostend = colon ? colon : path_start;
      if (!parse_host (p, hostend - p, flags, host, error))
        goto fail;

      if (colon && colon != path_start - 1)
        {
          p = colon + 1;
          if (!parse_port (p, path_start - p, port, error))
            goto fail;
        }

      p = path_start;
    }

  /* Find fragment. */
  end = p + strcspn (p, "#");
  if (*end == '#')
    {
      if (!uri_normalize (fragment, end + 1, strlen (end + 1),
                          flags | (flags & G_URI_FLAGS_ENCODED_FRAGMENT ? G_URI_FLAGS_ENCODED : 0),
                          error))
        goto fail;
    }

  /* Find query */
  question = memchr (p, '?', end - p);
  if (question)
    {
      if (!uri_normalize (query, question + 1, end - (question + 1),
                          flags | (flags & G_URI_FLAGS_ENCODED_QUERY ? G_URI_FLAGS_ENCODED : 0),
                          error))
        goto fail;
      end = question;
    }

  if (!uri_normalize (path, p, end - p,
                      flags | (flags & G_URI_FLAGS_ENCODED_PATH ? G_URI_FLAGS_ENCODED : 0),
                      error))
    goto fail;

  /* Scheme-based normalization */
  if (flags & G_URI_FLAGS_SCHEME_NORMALIZE && ((scheme && *scheme) || normalized_scheme))
    {
      const char *scheme_str = scheme && *scheme ? *scheme : normalized_scheme;

      if (should_normalize_empty_path (scheme_str) && path && !**path)
        {
          g_free (*path);
          *path = g_strdup ("/");
        }

      if (port && *port == -1)
        *port = default_scheme_port (scheme_str);
    }

  g_free (normalized_scheme);
  g_free (cleaned_uri_string);
  return TRUE;

 fail:
  if (scheme)
    g_clear_pointer (scheme, g_free);
  if (userinfo)
    g_clear_pointer (userinfo, g_free);
  if (host)
    g_clear_pointer (host, g_free);
  if (port)
    *port = -1;
  if (path)
    g_clear_pointer (path, g_free);
  if (query)
    g_clear_pointer (query, g_free);
  if (fragment)
    g_clear_pointer (fragment, g_free);

  g_free (normalized_scheme);
  g_free (cleaned_uri_string);
  return FALSE;
}


/* Implements the "Remove Dot Segments" algorithm from section 5.2.4 of
 * RFC 3986.
 *
 * See https://tools.ietf.org/html/rfc3986#section-5.2.4
 */
static void
remove_dot_segments (gchar *path)
{
  /* The output can be written to the same buffer that the input
   * is read from, as the output pointer is only ever increased
   * when the input pointer is increased as well, and the input
   * pointer is never decreased. */
  gchar *input = path;
  gchar *output = path;

  if (!*path)
    return;

  while (*input)
    {
      /*  A.  If the input buffer begins with a prefix of "../" or "./",
       *      then remove that prefix from the input buffer; otherwise,
       */
      if (strncmp (input, "../", 3) == 0)
        input += 3;
      else if (strncmp (input, "./", 2) == 0)
        input += 2;

      /*  B.  if the input buffer begins with a prefix of "/./" or "/.",
       *      where "." is a complete path segment, then replace that
       *      prefix with "/" in the input buffer; otherwise,
       */
      else if (strncmp (input, "/./", 3) == 0)
        input += 2;
      else if (strcmp (input, "/.") == 0)
        input[1] = '\0';

      /*  C.  if the input buffer begins with a prefix of "/../" or "/..",
       *      where ".." is a complete path segment, then replace that
       *      prefix with "/" in the input buffer and remove the last
       *      segment and its preceding "/" (if any) from the output
       *      buffer; otherwise,
       */
      else if (strncmp (input, "/../", 4) == 0)
        {
          input += 3;
          if (output > path)
            {
              do
                {
                  output--;
                }
              while (*output != '/' && output > path);
            }
        }
      else if (strcmp (input, "/..") == 0)
        {
          input[1] = '\0';
          if (output > path)
            {
              do
                 {
                   output--;
                 }
              while (*output != '/' && output > path);
            }
        }

      /*  D.  if the input buffer consists only of "." or "..", then remove
       *      that from the input buffer; otherwise,
       */
      else if (strcmp (input, "..") == 0 || strcmp (input, ".") == 0)
        input[0] = '\0';

      /*  E.  move the first path segment in the input buffer to the end of
       *      the output buffer, including the initial "/" character (if
       *      any) and any subsequent characters up to, but not including,
       *      the next "/" character or the end of the input buffer.
       */
      else
        {
          *output++ = *input++;
          while (*input && *input != '/')
            *output++ = *input++;
        }
    }
  *output = '\0';
}

GUri *
flatpak_g_uri_parse (const gchar  *uri_string,
             GUriFlags     flags,
             GError      **error)
{
  g_return_val_if_fail (uri_string != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return flatpak_g_uri_parse_relative (NULL, uri_string, flags, error);
}

GUri *
flatpak_g_uri_parse_relative (GUri         *base_uri,
                      const gchar  *uri_ref,
                      GUriFlags     flags,
                      GError      **error)
{
  GUri *uri = NULL;

  g_return_val_if_fail (uri_ref != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (base_uri == NULL || base_uri->scheme != NULL, NULL);

  /* Use GUri struct to construct the return value: there is no guarantee it is
   * actually correct within the function body. */
  uri = g_new0 (GUri, 1);
  uri->ref_count = 1;
  uri->flags = flags;

  if (!flatpak_g_uri_split_internal (uri_ref, flags,
                             &uri->scheme, &uri->userinfo,
                             &uri->user, &uri->password, &uri->auth_params,
                             &uri->host, &uri->port,
                             &uri->path, &uri->query, &uri->fragment,
                             error))
    {
      flatpak_g_uri_unref (uri);
      return NULL;
    }

  if (!uri->scheme && !base_uri)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           _("URI is not absolute, and no base URI was provided"));
      flatpak_g_uri_unref (uri);
      return NULL;
    }

  if (base_uri)
    {
      /* This is section 5.2.2 of RFC 3986, except that we're doing
       * it in place in @uri rather than copying from R to T.
       *
       * See https://tools.ietf.org/html/rfc3986#section-5.2.2
       */
      if (uri->scheme)
        remove_dot_segments (uri->path);
      else
        {
          uri->scheme = g_strdup (base_uri->scheme);
          if (uri->host)
            remove_dot_segments (uri->path);
          else
            {
              if (!*uri->path)
                {
                  g_free (uri->path);
                  uri->path = g_strdup (base_uri->path);
                  if (!uri->query)
                    uri->query = g_strdup (base_uri->query);
                }
              else
                {
                  if (*uri->path == '/')
                    remove_dot_segments (uri->path);
                  else
                    {
                      gchar *newpath, *last;

                      last = strrchr (base_uri->path, '/');
                      if (last)
                        {
                          newpath = g_strdup_printf ("%.*s/%s",
                                                     (gint)(last - base_uri->path),
                                                     base_uri->path,
                                                     uri->path);
                        }
                      else
                        newpath = g_strdup_printf ("/%s", uri->path);

                      g_free (uri->path);
                      uri->path = g_steal_pointer (&newpath);

                      remove_dot_segments (uri->path);
                    }
                }

              uri->userinfo = g_strdup (base_uri->userinfo);
              uri->user = g_strdup (base_uri->user);
              uri->password = g_strdup (base_uri->password);
              uri->auth_params = g_strdup (base_uri->auth_params);
              uri->host = g_strdup (base_uri->host);
              uri->port = base_uri->port;
            }
        }

      /* Scheme normalization couldn't have been done earlier
       * as the relative URI may not have had a scheme */
      if (flags & G_URI_FLAGS_SCHEME_NORMALIZE)
        {
          if (should_normalize_empty_path (uri->scheme) && !*uri->path)
            {
              g_free (uri->path);
              uri->path = g_strdup ("/");
            }

          uri->port = normalize_port (uri->scheme, uri->port);
        }
    }
  else
    {
      remove_dot_segments (uri->path);
    }

  return g_steal_pointer (&uri);
}

/* userinfo as a whole can contain sub-delims + ":", but split-out
 * user can't contain ":" or ";", and split-out password can't contain
 * ";".
 */
#define USERINFO_ALLOWED_CHARS G_URI_RESERVED_CHARS_ALLOWED_IN_USERINFO
#define USER_ALLOWED_CHARS "!$&'()*+,="
#define PASSWORD_ALLOWED_CHARS "!$&'()*+,=:"
#define AUTH_PARAMS_ALLOWED_CHARS USERINFO_ALLOWED_CHARS
#define IP_ADDR_ALLOWED_CHARS ":"
#define HOST_ALLOWED_CHARS G_URI_RESERVED_CHARS_SUBCOMPONENT_DELIMITERS
#define PATH_ALLOWED_CHARS G_URI_RESERVED_CHARS_ALLOWED_IN_PATH
#define QUERY_ALLOWED_CHARS G_URI_RESERVED_CHARS_ALLOWED_IN_PATH "?"
#define FRAGMENT_ALLOWED_CHARS G_URI_RESERVED_CHARS_ALLOWED_IN_PATH "?"

static gchar *
flatpak_g_uri_join_internal (GUriFlags    flags,
                     const gchar *scheme,
                     gboolean     userinfo,
                     const gchar *user,
                     const gchar *password,
                     const gchar *auth_params,
                     const gchar *host,
                     gint         port,
                     const gchar *path,
                     const gchar *query,
                     const gchar *fragment)
{
  gboolean encoded = (flags & G_URI_FLAGS_ENCODED);
  GString *str;
  char *normalized_scheme = NULL;

  /* Restrictions on path prefixes. See:
   * https://tools.ietf.org/html/rfc3986#section-3
   */
  g_return_val_if_fail (path != NULL, NULL);
  g_return_val_if_fail (host == NULL || (path[0] == '\0' || path[0] == '/'), NULL);
  g_return_val_if_fail (host != NULL || (path[0] != '/' || path[1] != '/'), NULL);

  str = g_string_new (scheme);
  if (scheme)
    g_string_append_c (str, ':');

  if (flags & G_URI_FLAGS_SCHEME_NORMALIZE && scheme && ((host && port != -1) || path[0] == '\0'))
    normalized_scheme = g_ascii_strdown (scheme, -1);

  if (host)
    {
      g_string_append (str, "//");

      if (user)
        {
          if (encoded)
            g_string_append (str, user);
          else
            {
              if (userinfo)
                g_string_append_uri_escaped (str, user, USERINFO_ALLOWED_CHARS, TRUE);
              else
                /* Encode ':' and ';' regardless of whether we have a
                 * password or auth params, since it may be parsed later
                 * under the assumption that it does.
                 */
                g_string_append_uri_escaped (str, user, USER_ALLOWED_CHARS, TRUE);
            }

          if (password)
            {
              g_string_append_c (str, ':');
              if (encoded)
                g_string_append (str, password);
              else
                g_string_append_uri_escaped (str, password,
                                             PASSWORD_ALLOWED_CHARS, TRUE);
            }

          if (auth_params)
            {
              g_string_append_c (str, ';');
              if (encoded)
                g_string_append (str, auth_params);
              else
                g_string_append_uri_escaped (str, auth_params,
                                             AUTH_PARAMS_ALLOWED_CHARS, TRUE);
            }

          g_string_append_c (str, '@');
        }

      if (strchr (host, ':') && g_hostname_is_ip_address (host))
        {
          g_string_append_c (str, '[');
          if (encoded)
            g_string_append (str, host);
          else
            g_string_append_uri_escaped (str, host, IP_ADDR_ALLOWED_CHARS, TRUE);
          g_string_append_c (str, ']');
        }
      else
        {
          if (encoded)
            g_string_append (str, host);
          else
            g_string_append_uri_escaped (str, host, HOST_ALLOWED_CHARS, TRUE);
        }

      if (port != -1 && (!normalized_scheme || normalize_port (normalized_scheme, port) != -1))
        g_string_append_printf (str, ":%d", port);
    }

  if (path[0] == '\0' && normalized_scheme && should_normalize_empty_path (normalized_scheme))
    g_string_append (str, "/");
  else if (encoded || flags & G_URI_FLAGS_ENCODED_PATH)
    g_string_append (str, path);
  else
    g_string_append_uri_escaped (str, path, PATH_ALLOWED_CHARS, TRUE);

  g_free (normalized_scheme);

  if (query)
    {
      g_string_append_c (str, '?');
      if (encoded || flags & G_URI_FLAGS_ENCODED_QUERY)
        g_string_append (str, query);
      else
        g_string_append_uri_escaped (str, query, QUERY_ALLOWED_CHARS, TRUE);
    }
  if (fragment)
    {
      g_string_append_c (str, '#');
      if (encoded || flags & G_URI_FLAGS_ENCODED_FRAGMENT)
        g_string_append (str, fragment);
      else
        g_string_append_uri_escaped (str, fragment, FRAGMENT_ALLOWED_CHARS, TRUE);
    }

  return g_string_free (str, FALSE);
}

static gchar *
flatpak_g_uri_join (GUriFlags    flags,
            const gchar *scheme,
            const gchar *userinfo,
            const gchar *host,
            gint         port,
            const gchar *path,
            const gchar *query,
            const gchar *fragment)
{
  g_return_val_if_fail (port >= -1 && port <= 65535, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  return flatpak_g_uri_join_internal (flags,
                              scheme,
                              TRUE, userinfo, NULL, NULL,
                              host,
                              port,
                              path,
                              query,
                              fragment);
}

static gchar *
flatpak_g_uri_join_with_user (GUriFlags    flags,
                      const gchar *scheme,
                      const gchar *user,
                      const gchar *password,
                      const gchar *auth_params,
                      const gchar *host,
                      gint         port,
                      const gchar *path,
                      const gchar *query,
                      const gchar *fragment)
{
  g_return_val_if_fail (port >= -1 && port <= 65535, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  return flatpak_g_uri_join_internal (flags,
                              scheme,
                              FALSE, user, password, auth_params,
                              host,
                              port,
                              path,
                              query,
                              fragment);
}

GUri *
flatpak_g_uri_build (GUriFlags    flags,
             const gchar *scheme,
             const gchar *userinfo,
             const gchar *host,
             gint         port,
             const gchar *path,
             const gchar *query,
             const gchar *fragment)
{
  GUri *uri;

  g_return_val_if_fail (scheme != NULL, NULL);
  g_return_val_if_fail (port >= -1 && port <= 65535, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  uri = g_new0 (GUri, 1);
  uri->ref_count = 1;
  uri->flags = flags;
  uri->scheme = g_ascii_strdown (scheme, -1);
  uri->userinfo = g_strdup (userinfo);
  uri->host = g_strdup (host);
  uri->port = port;
  uri->path = g_strdup (path);
  uri->query = g_strdup (query);
  uri->fragment = g_strdup (fragment);

  return g_steal_pointer (&uri);
}

gchar *
flatpak_g_uri_to_string_partial (GUri          *uri,
                         GUriHideFlags  flags)
{
  gboolean hide_user = (flags & G_URI_HIDE_USERINFO);
  gboolean hide_password = (flags & (G_URI_HIDE_USERINFO | G_URI_HIDE_PASSWORD));
  gboolean hide_auth_params = (flags & (G_URI_HIDE_USERINFO | G_URI_HIDE_AUTH_PARAMS));
  gboolean hide_query = (flags & G_URI_HIDE_QUERY);
  gboolean hide_fragment = (flags & G_URI_HIDE_FRAGMENT);

  g_return_val_if_fail (uri != NULL, NULL);

  if (uri->flags & (G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_HAS_AUTH_PARAMS))
    {
      return flatpak_g_uri_join_with_user (uri->flags,
                                   uri->scheme,
                                   hide_user ? NULL : uri->user,
                                   hide_password ? NULL : uri->password,
                                   hide_auth_params ? NULL : uri->auth_params,
                                   uri->host,
                                   uri->port,
                                   uri->path,
                                   hide_query ? NULL : uri->query,
                                   hide_fragment ? NULL : uri->fragment);
    }

  return flatpak_g_uri_join (uri->flags,
                     uri->scheme,
                     hide_user ? NULL : uri->userinfo,
                     uri->host,
                     uri->port,
                     uri->path,
                     hide_query ? NULL : uri->query,
                     hide_fragment ? NULL : uri->fragment);
}

const gchar *
flatpak_g_uri_get_scheme (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);

  return uri->scheme;
}

const gchar *
flatpak_g_uri_get_userinfo (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);

  return uri->userinfo;
}

const gchar *
flatpak_g_uri_get_user (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);

  return uri->user;
}

const gchar *
flatpak_g_uri_get_password (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);

  return uri->password;
}

const gchar *
flatpak_g_uri_get_auth_params (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);

  return uri->auth_params;
}

const gchar *
flatpak_g_uri_get_host (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);

  return uri->host;
}

gint
flatpak_g_uri_get_port (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, -1);

  if (uri->port == -1 && uri->flags & G_URI_FLAGS_SCHEME_NORMALIZE)
    return default_scheme_port (uri->scheme);

  return uri->port;
}

const gchar *
flatpak_g_uri_get_path (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);

  return uri->path;
}

const gchar *
flatpak_g_uri_get_query (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);

  return uri->query;
}

const gchar *
flatpak_g_uri_get_fragment (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, NULL);

  return uri->fragment;
}

GUriFlags
flatpak_g_uri_get_flags (GUri *uri)
{
  g_return_val_if_fail (uri != NULL, G_URI_FLAGS_NONE);

  return uri->flags;
}

#endif /* GLIB_CHECK_VERSION (2, 66, 0) */


static void
append_form_encoded (GString *str, const char *in)
{
  const unsigned char *s = (const unsigned char *)in;

  while (*s)
    {
      if (*s == ' ')
        {
          g_string_append_c (str, '+');
          s++;
        }
      else if (!g_ascii_isalnum (*s) && (*s != '-') && (*s != '_')
               && (*s != '.'))
        g_string_append_printf (str, "%%%02X", (int)*s++);
      else
        g_string_append_c (str, *s++);
    }
}

void
flatpak_uri_encode_query_arg (GString *str,
                              const char *key,
                              const char *value)
{
  if (str->len)
    g_string_append_c (str, '&');
  append_form_encoded (str, key);

  g_string_append_c (str, '=');
  append_form_encoded (str, value);
}


/* This is a simplified copy of soup_header_parse_param_list() to avoid a soup dependency */

static const char *
skip_lws (const char *s)
{
  while (g_ascii_isspace (*s))
    s++;
  return s;
}

static const char *
unskip_lws (const char *s, const char *start)
{
  while (s > start && g_ascii_isspace (*(s - 1)))
    s--;
  return s;
}

static const char *
skip_delims (const char *s, char delim)
{
  /* The grammar allows for multiple delimiters */
  while (g_ascii_isspace (*s) || *s == delim)
    s++;
  return s;
}

static const char *
skip_item (const char *s, char delim)
{
  gboolean quoted = FALSE;
  const char *start = s;

  /* A list item ends at the last non-whitespace character
   * before a delimiter which is not inside a quoted-string. Or
   * at the end of the string.
   */

  while (*s)
    {
      if (*s == '"')
        quoted = !quoted;
      else if (quoted)
        {
          if (*s == '\\' && *(s + 1))
            s++;
        }
      else
        {
          if (*s == delim)
            break;
        }
      s++;
    }

  return unskip_lws (s, start);
}

static GSList *
parse_list (const char *header, char delim)
{
  GSList *list = NULL;
  const char *end;

  header = skip_delims (header, delim);
  while (*header)
    {
      end = skip_item (header, delim);
      list = g_slist_prepend (list, g_strndup (header, end - header));
      header = skip_delims (end, delim);
    }

  return g_slist_reverse (list);
}

static void
decode_quoted_string (char *quoted_string)
{
  char *src, *dst;

  src = quoted_string + 1;
  dst = quoted_string;
  while (*src && *src != '"')
    {
      if (*src == '\\' && *(src + 1))
        src++;
      *dst++ = *src++;
    }
  *dst = '\0';
}

GHashTable *
flatpak_parse_http_header_param_list (const char *header)
{
  GHashTable *params;
  GSList *list, *iter;
  char *eq, *name_end, *value;

  params = g_hash_table_new_full (g_str_hash,
                                  g_str_equal,
                                  g_free, g_free);

  list = parse_list (header, ',');
  for (iter = list; iter; iter = iter->next)
    {
      g_autofree char *item = iter->data;

      eq = strchr (item, '=');
      if (eq)
        {
          name_end = (char *)unskip_lws (eq, item);
          if (name_end == item)
            continue;

          *name_end = '\0';

          value = (char *)skip_lws (eq + 1);
          if (*value == '"')
            decode_quoted_string (value);
        }
      else
        value = NULL;

      g_autofree char *key =  g_ascii_strdown (item, -1);
      if (!g_hash_table_contains (params, key))
        g_hash_table_replace (params, g_steal_pointer (&key), g_strdup (value));
    }

  g_slist_free (list);
  return params;
}

/* Do not internationalize */
static const char *const months[] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Do not internationalize */
static const char *const days[] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

char *
flatpak_format_http_date (GDateTime *date)
{
  g_autoptr(GDateTime) utcdate = g_date_time_to_utc (date);
  g_autofree char *date_format = NULL;

  /* "Sun, 06 Nov 1994 08:49:37 GMT" */

  date_format = g_strdup_printf ("%s, %%d %s %%Y %%T GMT",
                                 days[g_date_time_get_day_of_week (utcdate) - 1],
                                 months[g_date_time_get_month (utcdate) - 1]);

  return g_date_time_format (utcdate, (const char*)date_format);
}


static inline gboolean
parse_day (int *day, const char **date_string)
{
  char *end;

  *day = strtoul (*date_string, &end, 10);
  if (end == (char *)*date_string)
    return FALSE;

  while (*end == ' ' || *end == '-')
    end++;
  *date_string = end;
  return TRUE;
}

static inline gboolean
parse_month (int *month, const char **date_string)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (months); i++)
    {
      if (!g_ascii_strncasecmp (*date_string, months[i], 3))
        {
          *month = i + 1;
          *date_string += 3;
          while (**date_string == ' ' || **date_string == '-')
            (*date_string)++;
          return TRUE;
        }
    }

  return FALSE;
}

static inline gboolean
parse_year (int *year, const char **date_string)
{
  char *end;

  *year = strtoul (*date_string, &end, 10);
  if (end == (char *)*date_string)
    return FALSE;

  if (end == (char *)*date_string + 2) {
    if (*year < 70)
      *year += 2000;
    else
      *year += 1900;
  } else if (end == (char *)*date_string + 3)
    *year += 1900;

  while (*end == ' ' || *end == '-')
    end++;
  *date_string = end;

  return TRUE;
}

static inline gboolean
parse_time (int *hour, int *minute, int *second, const char **date_string)
{
  char *p, *end;

  *hour = strtoul (*date_string, &end, 10);
  if (end == (char *)*date_string || *end++ != ':')
    return FALSE;
  p = end;
  *minute = strtoul (p, &end, 10);
  if (end == p || *end++ != ':')
    return FALSE;
  p = end;
  *second = strtoul (p, &end, 10);
  if (end == p)
    return FALSE;
  p = end;

  while (*p == ' ')
    p++;
  *date_string = p;

  return TRUE;
}

static inline gboolean
parse_timezone (GTimeZone **timezone_out, const char **date_string)
{
  gint32 offset_minutes;
  gboolean utc;

  if (!**date_string)
    {
      utc = FALSE;
      offset_minutes = 0;
    }
  else if (**date_string == '+' || **date_string == '-')
    {
      gulong val;
      int sign = (**date_string == '+') ? 1 : -1;
      val = strtoul (*date_string + 1, (char **)date_string, 10);
      if (**date_string == ':')
        val = 60 * val + strtoul (*date_string + 1, (char **)date_string, 10);
      else
        val =  60 * (val / 100) + (val % 100);
      offset_minutes = sign * val;
      utc = (sign == -1) && !val;
    }
  else if (**date_string == 'Z')
    {
      offset_minutes = 0;
      utc = TRUE;
      (*date_string)++;
    }
  else if (!strcmp (*date_string, "GMT") ||
           !strcmp (*date_string, "UTC"))
    {
      offset_minutes = 0;
      utc = TRUE;
      (*date_string) += 3;
    }
  else if (strchr ("ECMP", **date_string) &&
           ((*date_string)[1] == 'D' || (*date_string)[1] == 'S') &&
           (*date_string)[2] == 'T') {
    offset_minutes = -60 * (5 * strcspn ("ECMP", *date_string));
    if ((*date_string)[1] == 'D')
      offset_minutes += 60;
    utc = FALSE;
  }
  else
    return FALSE;

  if (utc)
    *timezone_out = g_time_zone_new_utc ();
  else
    *timezone_out = g_time_zone_new_offset (offset_minutes * 60);

  return TRUE;
}

GDateTime *
flatpak_parse_http_time (const char *date_string)
{
  int month, day, year, hour, minute, second;
  g_autoptr(GTimeZone) tz = NULL;

  g_return_val_if_fail (date_string != NULL, NULL);

  while (g_ascii_isspace (*date_string))
    date_string++;

  /* If it starts with a word, it must be a weekday, which we skip */
  if (g_ascii_isalpha (*date_string))
    {
      while (g_ascii_isalpha (*date_string))
        date_string++;
      if (*date_string == ',')
        date_string++;
      while (g_ascii_isspace (*date_string))
        date_string++;
    }

  /* If there's now another word, this must be an asctime-date */
  if (g_ascii_isalpha (*date_string))
    {
      /* (Sun) Nov  6 08:49:37 1994 */
      if (!parse_month (&month, &date_string) ||
          !parse_day (&day, &date_string) ||
          !parse_time (&hour, &minute, &second, &date_string) ||
          !parse_year (&year, &date_string))
        return NULL;

      /* There shouldn't be a timezone, but check anyway */
      parse_timezone (&tz, &date_string);
    }
  else
    {
      /* Non-asctime date, so some variation of
       * (Sun,) 06 Nov 1994 08:49:37 GMT
       */
      if (!parse_day (&day, &date_string) ||
          !parse_month (&month, &date_string) ||
          !parse_year (&year, &date_string) ||
          !parse_time (&hour, &minute, &second, &date_string))
        return NULL;

      /* This time there *should* be a timezone, but we
       * survive if there isn't.
       */
      parse_timezone (&tz, &date_string);
    }

  if (!tz)
    tz = g_time_zone_new_utc ();

  return g_date_time_new (tz, year, month, day, hour, minute, second);
}
