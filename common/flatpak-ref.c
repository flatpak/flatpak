/*
 * Copyright © 2015 Red Hat, Inc
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

#include "flatpak-utils-private.h"
#include "flatpak-ref.h"
#include "flatpak-enum-types.h"

/**
 * SECTION:flatpak-ref
 * @Title: FlatpakRef
 * @Short_description: Application reference
 *
 * Currently Flatpak manages two types of binary artifacts: applications, and
 * runtimes. Applications contain a program that desktop users can run, while
 * runtimes contain only libraries and data. An FlatpakRef object (or short: ref)
 * can refer to either of these.
 *
 * Both applications and runtimes are identified by a 4-tuple of strings: kind,
 * name, arch and branch, e.g. app/org.gnome.evince/x86_64/master. The functions
 * flatpak_ref_parse() and flatpak_ref_format_ref() can be used to convert
 * FlatpakRef objects into this string representation and back.
 *
 * FlatpakRef objects are immutable and can be passed freely between threads.
 *
 * To uniquely identify a particular version of an application or runtime, you
 * need a commit.
 *
 * The subclasses #FlatpakInstalledRef and #FlatpakRemoteRef provide more information
 * for artifacts that are locally installed or available from a remote repository.
 */
typedef struct _FlatpakRefPrivate FlatpakRefPrivate;

struct _FlatpakRefPrivate
{
  char          *name;
  char          *arch;
  char          *branch;
  char          *commit;
  FlatpakRefKind kind;
  char          *collection_id;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakRef, flatpak_ref, G_TYPE_OBJECT)

enum {
  PROP_0,

  PROP_NAME,
  PROP_ARCH,
  PROP_BRANCH,
  PROP_COMMIT,
  PROP_KIND,
  PROP_COLLECTION_ID,
};

static void
flatpak_ref_finalize (GObject *object)
{
  FlatpakRef *self = FLATPAK_REF (object);
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  g_free (priv->name);
  g_free (priv->arch);
  g_free (priv->branch);
  g_free (priv->commit);
  g_free (priv->collection_id);

  G_OBJECT_CLASS (flatpak_ref_parent_class)->finalize (object);
}

static void
flatpak_ref_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  FlatpakRef *self = FLATPAK_REF (object);
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_NAME:
      g_assert (priv->name == NULL); /* Construct-only */
      priv->name = g_value_dup_string (value);
      break;

    case PROP_ARCH:
      g_assert (priv->arch == NULL); /* Construct-only */
      priv->arch = g_value_dup_string (value);
      break;

    case PROP_BRANCH:
      g_assert (priv->branch == NULL); /* Construct-only */
      priv->branch = g_value_dup_string (value);
      break;

    case PROP_COMMIT:
      g_assert (priv->commit == NULL); /* Construct-only */
      priv->commit = g_value_dup_string (value);
      break;

    case PROP_KIND:
      priv->kind = g_value_get_enum (value);
      break;

    case PROP_COLLECTION_ID:
      g_assert (priv->collection_id == NULL); /* Construct-only */
      priv->collection_id = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_ref_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  FlatpakRef *self = FLATPAK_REF (object);
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_ARCH:
      g_value_set_string (value, priv->arch);
      break;

    case PROP_BRANCH:
      g_value_set_string (value, priv->branch);
      break;

    case PROP_COMMIT:
      g_value_set_string (value, priv->commit);
      break;

    case PROP_KIND:
      g_value_set_enum (value, priv->kind);
      break;

    case PROP_COLLECTION_ID:
      g_value_set_string (value, priv->collection_id);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_ref_class_init (FlatpakRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = flatpak_ref_get_property;
  object_class->set_property = flatpak_ref_set_property;
  object_class->finalize = flatpak_ref_finalize;

  g_object_class_install_property (object_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The name of the application or runtime",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_ARCH,
                                   g_param_spec_string ("arch",
                                                        "Architecture",
                                                        "The architecture of the application or runtime",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_BRANCH,
                                   g_param_spec_string ("branch",
                                                        "Branch",
                                                        "The branch of the application or runtime",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_COMMIT,
                                   g_param_spec_string ("commit",
                                                        "Commit",
                                                        "The commit",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_KIND,
                                   g_param_spec_enum ("kind",
                                                      "Kind",
                                                      "The kind of artifact",
                                                      FLATPAK_TYPE_REF_KIND,
                                                      FLATPAK_REF_KIND_APP,
                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_COLLECTION_ID,
                                   g_param_spec_string ("collection-id",
                                                        "Collection ID",
                                                        "The collection ID",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
flatpak_ref_init (FlatpakRef *self)
{
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  priv->kind = FLATPAK_REF_KIND_APP;
}

/**
 * flatpak_ref_get_name:
 * @self: a #FlatpakRef
 *
 * Gets the name of the ref.
 *
 * Returns: (transfer none): the name
 */
const char *
flatpak_ref_get_name (FlatpakRef *self)
{
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  return priv->name;
}

/**
 * flatpak_ref_get_arch:
 * @self: a #FlatpakRef
 *
 * Gets the arch or the ref.
 *
 * Returns: (transfer none): the arch
 */
const char *
flatpak_ref_get_arch (FlatpakRef *self)
{
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  return priv->arch;
}

/**
 * flatpak_ref_get_branch:
 * @self: a #FlatpakRef
 *
 * Gets the branch of the ref.
 *
 * Returns: (transfer none): the branch
 */
const char *
flatpak_ref_get_branch (FlatpakRef *self)
{
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  return priv->branch;
}

/**
 * flatpak_ref_get_commit:
 * @self: a #FlatpakRef
 *
 * Gets the commit of the ref.
 *
 * Returns: (transfer none): the commit
 */
const char *
flatpak_ref_get_commit (FlatpakRef *self)
{
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  return priv->commit;
}

/**
 * flatpak_ref_get_kind:
 * @self: a #FlatpakRef
 *
 * Gets the kind of artifact that this ref refers to.
 *
 * Returns: the kind of artifact
 */
FlatpakRefKind
flatpak_ref_get_kind (FlatpakRef *self)
{
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  return priv->kind;
}

/**
 * flatpak_ref_format_ref:
 * @self: a #FlatpakRef
 *
 * Convert an FlatpakRef object into a string representation that
 * can be parsed by flatpak_ref_parse().
 *
 * Returns: (transfer full): string representation
 */
char *
flatpak_ref_format_ref (FlatpakRef *self)
{
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  if (priv->kind == FLATPAK_REF_KIND_APP)
    return flatpak_build_app_ref (priv->name,
                                  priv->branch,
                                  priv->arch);
  else
    return flatpak_build_runtime_ref (priv->name,
                                      priv->branch,
                                      priv->arch);
}

/**
 * flatpak_ref_format_ref_cached:
 * @self: a #FlatpakRef
 *
 * Like flatpak_ref_format_ref() but this returns the same string each time
 * it's called rather than allocating a new one.
 *
 * Returns: (transfer none): string representation
 *
 * Since: 1.9.1
 */
const char *
flatpak_ref_format_ref_cached (FlatpakRef *self)
{
  char *full_ref;

  full_ref = g_object_get_data (G_OBJECT (self), "cached-full-ref");
  if (!full_ref)
    {
      full_ref = flatpak_ref_format_ref (self);
      g_object_set_data_full (G_OBJECT (self), "cached-full-ref", full_ref, g_free);
    }

  return (const char *) full_ref;
}

/**
 * flatpak_ref_parse:
 * @ref: A string ref name, such as "app/org.test.App/x86_64/master"
 * @error: return location for a #GError
 *
 * Tries to parse a full ref name and return a #FlatpakRef (without a
 * commit set) or fail if the ref is invalid somehow.
 *
 * Returns: (transfer full): an #FlatpakRef, or %NULL
 */
FlatpakRef *
flatpak_ref_parse (const char *ref, GError **error)
{
  g_auto(GStrv) parts = NULL;
  FlatpakRefKind kind;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  if (g_strcmp0 (parts[0], "app") == 0)
    kind = FLATPAK_REF_KIND_APP;
  else
    kind = FLATPAK_REF_KIND_RUNTIME;

  return FLATPAK_REF (g_object_new (FLATPAK_TYPE_REF,
                                    "kind", kind,
                                    "name", parts[1],
                                    "arch", parts[2],
                                    "branch", parts[3],
                                    NULL));
}

/**
 * flatpak_ref_get_collection_id:
 * @self: a #FlatpakRef
 *
 * Gets the collection ID of the ref.
 *
 * Returns: (transfer none): the collection ID
 */
const char *
flatpak_ref_get_collection_id (FlatpakRef *self)
{
  FlatpakRefPrivate *priv = flatpak_ref_get_instance_private (self);

  return priv->collection_id;
}
