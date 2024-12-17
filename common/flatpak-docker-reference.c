/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2024 Red Hat, Inc
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
 *       Owen Taylor <otaylor@redhat.com>
 */

#include "flatpak-docker-reference-private.h"
#include "flatpak-utils-private.h"

struct _FlatpakDockerReference
{
    char *uri;
    char *repository;
    char *tag;
    char *digest;
};

/*
 * Parsing here is loosely based off:
 *
 *   https://github.com/containers/image/tree/main/docker/reference
 *
 * The major simplification is that we require a domain component, and
 * don't have any default domain. This removes ambiguity between domains and paths
 * and makes parsing much simpler. We also don't normalize single component
 * paths (e.g. ubuntu => library/ubuntu.)
 */

#define TAG "[0-9A-Za-z_][0-9A-Za-z_-]{0,127}"
#define DIGEST "[A-Za-z][A-Za-z0-9]*(?:[-_+.][A-Za-z][A-Za-z0-9]*)*[:][[:xdigit:]]{32,}"
#define REMAINDER_TAG_AND_DIGEST_RE "^(.*?)(:" TAG ")?" "(@" DIGEST ")?$"

static GRegex *
get_remainder_tag_and_digest_regex (void)
{
  static gsize regex = 0;

  if (g_once_init_enter (&regex))
    {
      g_autoptr(GRegex) compiled = g_regex_new (REMAINDER_TAG_AND_DIGEST_RE,
                                                G_REGEX_DEFAULT,
                                                G_REGEX_MATCH_DEFAULT,
                                                NULL);
      g_once_init_leave (&regex, (gsize)g_steal_pointer (&compiled));
    }

  return (GRegex *)regex;
}

FlatpakDockerReference *
flatpak_docker_reference_parse (const char *reference_str,
                                GError    **error)
{
  g_autoptr(FlatpakDockerReference) reference = g_new0 (FlatpakDockerReference, 1);
  GRegex *regex = get_remainder_tag_and_digest_regex ();
  g_autoptr(GMatchInfo) match_info = NULL;
  g_autofree char *tag_match = NULL;
  g_autofree char *digest_match = NULL;
  g_autofree char *remainder = NULL;
  g_autofree char *domain = NULL;
  gboolean matched;
  const char *slash;

  matched = g_regex_match (regex, reference_str, G_REGEX_MATCH_DEFAULT, &match_info);
  g_assert (matched);

  tag_match = g_match_info_fetch (match_info, 2);
  if (tag_match[0] == '\0')
    reference->tag = NULL;
  else
    reference->tag = g_strdup (tag_match + 1);

  digest_match = g_match_info_fetch (match_info, 3);
  if (digest_match[0] == '\0')
    reference->digest = NULL;
  else
    reference->digest = g_strdup (digest_match + 1);

  remainder = g_match_info_fetch (match_info, 1);
  slash = strchr (remainder, '/');
  if (slash == NULL || slash == reference_str || *slash == '\0')
    {
      flatpak_fail(error, "Can't parse %s into <domain>/<repository>", remainder);
      return NULL;
    }

  domain = g_strndup (remainder, slash - remainder);
  reference->uri = g_strconcat ("https://", domain, NULL);
  reference->repository = g_strdup (slash + 1);

  return g_steal_pointer (&reference);
}

const char *
flatpak_docker_reference_get_uri (FlatpakDockerReference *reference)
{
  return reference->uri;
}

const char *
flatpak_docker_reference_get_repository (FlatpakDockerReference *reference)
{
  return reference->repository;
}

const char *
flatpak_docker_reference_get_tag (FlatpakDockerReference *reference)
{
  return reference->tag;
}

const char *
flatpak_docker_reference_get_digest (FlatpakDockerReference *reference)
{
  return reference->digest;
}

void
flatpak_docker_reference_free (FlatpakDockerReference *reference)
{
  g_clear_pointer (&reference->uri, g_free);
  g_clear_pointer (&reference->repository, g_free);
  g_clear_pointer (&reference->tag, g_free);
  g_clear_pointer (&reference->digest, g_free);
  g_free (reference);
}
