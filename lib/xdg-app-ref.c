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

#include "xdg-app-utils.h"
#include "xdg-app-ref.h"
#include "xdg-app-enum-types.h"

/**
 * SECTION:xdg-app-ref
 * @Title: XdgAppRef
 * @Short_description: Application reference
 *
 * Currently xdg-app manages two types of binary artifacts: applications, and
 * runtimes. Applications contain a program that desktop users can run, while
 * runtimes contain only libraries and data. An XdgAppRef object (or short: ref)
 * can refer to either of these.
 *
 * Both applications and runtimes are identified by a 4-tuple of strings: kind,
 * name, arch and branch, e.g. app/org.gnome.evince/x86_64/master. The functions
 * xdg_app_ref_parse() and xdg_app_ref_format_ref() can be used to convert
 * XdgAppRef objects into this string representation and back.
 *
 * To uniquely identify a particular version of an application or runtime, you
 * need a commit.
 *
 * The subclasses #XdgAppInstalledRef and #XdgAppRemoteRef provide more information
 * for artifacts that are locally installed or available from a remote repository.
 */
typedef struct _XdgAppRefPrivate XdgAppRefPrivate;

struct _XdgAppRefPrivate
{
  char *name;
  char *arch;
  char *branch;
  char *commit;
  XdgAppRefKind kind;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppRef, xdg_app_ref, G_TYPE_OBJECT)

enum {
  PROP_0,

  PROP_NAME,
  PROP_ARCH,
  PROP_BRANCH,
  PROP_COMMIT,
  PROP_KIND
};

static void
xdg_app_ref_finalize (GObject *object)
{
  XdgAppRef *self = XDG_APP_REF (object);
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  g_free (priv->name);
  g_free (priv->arch);
  g_free (priv->branch);
  g_free (priv->commit);

  G_OBJECT_CLASS (xdg_app_ref_parent_class)->finalize (object);
}

static void
xdg_app_ref_set_property (GObject         *object,
                          guint            prop_id,
                          const GValue    *value,
                          GParamSpec      *pspec)
{
  XdgAppRef *self = XDG_APP_REF (object);
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_NAME:
      g_clear_pointer (&priv->name, g_free);
      priv->name = g_value_dup_string (value);
      break;

    case PROP_ARCH:
      g_clear_pointer (&priv->arch, g_free);
      priv->arch = g_value_dup_string (value);
      break;

    case PROP_BRANCH:
      g_clear_pointer (&priv->branch, g_free);
      priv->branch = g_value_dup_string (value);
      break;

    case PROP_COMMIT:
      g_clear_pointer (&priv->commit, g_free);
      priv->commit = g_value_dup_string (value);
      break;

    case PROP_KIND:
      priv->kind = g_value_get_enum (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_ref_get_property (GObject         *object,
                          guint            prop_id,
                          GValue          *value,
                          GParamSpec      *pspec)
{
  XdgAppRef *self = XDG_APP_REF (object);
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

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

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_ref_class_init (XdgAppRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_ref_get_property;
  object_class->set_property = xdg_app_ref_set_property;
  object_class->finalize = xdg_app_ref_finalize;

  g_object_class_install_property (object_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The name of the application or runtime",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ARCH,
                                   g_param_spec_string ("arch",
                                                        "Architecture",
                                                        "The architecture of the application or runtime",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BRANCH,
                                   g_param_spec_string ("branch",
                                                        "Branch",
                                                        "The branch of the application or runtime",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_COMMIT,
                                   g_param_spec_string ("commit",
                                                        "Commit",
                                                        "The commit",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_KIND,
                                   g_param_spec_enum ("kind",
                                                      "Kind",
                                                      "The kind of artifact",
                                                      XDG_TYPE_APP_REF_KIND,
                                                      XDG_APP_REF_KIND_APP,
                                                      G_PARAM_READWRITE));
}

static void
xdg_app_ref_init (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  priv->kind = XDG_APP_REF_KIND_APP;
}

/**
 * xdg_app_ref_get_name:
 * @self: a #XdgAppRef
 *
 * Gets the name of the ref.
 *
 * Returns: (transfer none): the name
 */
const char *
xdg_app_ref_get_name (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->name;
}

/**
 * xdg_app_ref_get_arch:
 * @self: a #XdgAppRef
 *
 * Gets the arch or the ref.
 *
 * Returns: (transfer none): the arch
 */
const char *
xdg_app_ref_get_arch (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->arch;
}

/**
 * xdg_app_ref_get_branch:
 * @self: a #XdgAppRef
 *
 * Gets the branch of the ref.
 *
 * Returns: (transfer none): the branch
 */
const char *
xdg_app_ref_get_branch (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->branch;
}

/**
 * xdg_app_ref_get_commit:
 * @self: a #XdgAppRef
 *
 * Gets the commit of the ref.
 *
 * Returns: (transfer none): the commit
 */
const char *
xdg_app_ref_get_commit (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->commit;
}

/**
 * xdg_app_ref_get_kind:
 * @self: a #XdgAppRef
 *
 * Gets the kind of artifact that this ref refers to.
 *
 * Returns: the kind of artifact
 */
XdgAppRefKind
xdg_app_ref_get_kind (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->kind;
}

/**
 * xdg_app_ref_format_ref:
 * @self: a #XdgAppRef
 *
 * Convert an XdgAppRef object into a string representation that
 * can be parsed by xdg_app_ref_parse().
 *
 * Returns: (transfer full): string representation
 */
char *
xdg_app_ref_format_ref  (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  if (priv->kind == XDG_APP_REF_KIND_APP)
    return xdg_app_build_app_ref (priv->name,
                                  priv->branch,
                                  priv->arch);
  else
    return xdg_app_build_runtime_ref (priv->name,
                                      priv->branch,
                                      priv->arch);
}

/**
 * xdg_app_ref_parse:
 * @ref: A string ref name, such as "app/org.test.App/86_64/master"
 * @error: return location for a #GError
 *
 * Tries to parse a full ref name and return a #XdgAppRef (without a
 * commit set) or fail if the ref is invalid somehow.
 *
 * Returns: (transfer full): an #XdgAppRef, or %NULL
 */
XdgAppRef *
xdg_app_ref_parse (const char *ref, GError **error)
{
  g_auto(GStrv) parts = NULL;

  parts = xdg_app_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  XdgAppRefKind kind;
  if (g_strcmp0 (parts[0], "app") == 0)
    kind = XDG_APP_REF_KIND_APP;
  else if (g_strcmp0 (parts[0], "runtime") == 0)
    kind = XDG_APP_REF_KIND_RUNTIME;
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Invalid kind: %s", parts[0]);
      return NULL;
    }

  return XDG_APP_REF (g_object_new (XDG_APP_TYPE_REF,
                                    "kind", kind,
                                    "name", parts[1],
                                    "arch", parts[2],
                                    "branch", parts[3],
                                    NULL));
}
