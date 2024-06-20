/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2018 Red Hat, Inc
 * Copyright © 2024 GNOME Foundation, Inc.
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
 *       Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *       Hubert Figuière <hub@figuiere.net>
 */

#include "config.h"
#include "flatpak-context-private.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include <grp.h>
#include <unistd.h>
#include <gio/gunixfdlist.h>

#include <glib/gi18n-lib.h>

#include <gio/gio.h>
#include "libglnx.h"

#include "flatpak-error.h"
#include "flatpak-metadata-private.h"
#include "flatpak-usb-private.h"
#include "flatpak-utils-private.h"

/* Same order as enum */
const char *flatpak_context_shares[] = {
  "network",
  "ipc",
  NULL
};

/* Same order as enum */
const char *flatpak_context_sockets[] = {
  "x11",
  "wayland",
  "pulseaudio",
  "session-bus",
  "system-bus",
  "fallback-x11",
  "ssh-auth",
  "pcsc",
  "cups",
  "gpg-agent",
  "inherit-wayland-socket",
  NULL
};

const char *flatpak_context_devices[] = {
  "dri",
  "all",
  "kvm",
  "shm",
  "input",
  "usb",
  NULL
};

const char *flatpak_context_features[] = {
  "devel",
  "multiarch",
  "bluetooth",
  "canbus",
  "per-app-dev-shm",
  NULL
};

const char *flatpak_context_special_filesystems[] = {
  "home",
  "host",
  "host-etc",
  "host-os",
  "host-reset",
  "host-root",
  NULL
};

const char *flatpak_context_conditions[] = {
  "true",
  "false",
  "has-input-device",
  "has-wayland",
  NULL
};

FlatpakContextConditions flatpak_context_true_conditions =
  FLATPAK_CONTEXT_CONDITION_TRUE |
  FLATPAK_CONTEXT_CONDITION_HAS_INPUT_DEV;

static const char *parse_negated (const char *option, gboolean *negated);
static guint32 flatpak_context_bitmask_from_string (const char *name, const char **names);

typedef struct FlatpakPermission FlatpakPermission;

struct FlatpakPermission {
  /* Is the permission unconditionally allowed */
  gboolean allowed;
  /* When layering, reset all permissions below */
  gboolean reset;
  /* Assumes allowed is false */
  GPtrArray *conditionals;

  /* Only used during deserialization */
  gboolean disallow_if_conditional;
  gboolean disallow_if_conditional_original_reset;
  GPtrArray *disallow_if_conditional_original_conditionals;
};


static FlatpakPermission *
flatpak_permission_new (void)
{
  FlatpakPermission *permission;

  permission = g_slice_new0 (FlatpakPermission);
  permission->conditionals = g_ptr_array_new_with_free_func (g_free);
  permission->disallow_if_conditional_original_conditionals =
    g_ptr_array_new_with_free_func (g_free);

  return permission;
};

static void
flatpak_permission_free (FlatpakPermission *permission)
{
  g_ptr_array_free (permission->conditionals, TRUE);
  g_ptr_array_free (permission->disallow_if_conditional_original_conditionals,
                    TRUE);
  g_slice_free (FlatpakPermission, permission);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakPermission, flatpak_permission_free)

static FlatpakPermission *
flatpak_permission_dup (FlatpakPermission *permission)
{
  FlatpakPermission *copy = NULL;

  copy = flatpak_permission_new ();
  copy->allowed = permission->allowed;
  copy->reset = permission->reset;

  for (size_t i = 0; i < permission->conditionals->len; i++) {
    const char *condition = permission->conditionals->pdata[i];

    g_ptr_array_add (copy->conditionals, g_strdup (condition));
  }

  return copy;
}

static void
flatpak_permission_set_not_allowed (FlatpakPermission *permission)
{
  permission->allowed = FALSE;
  permission->reset = TRUE;
  g_ptr_array_set_size (permission->conditionals, 0);
}

static void
flatpak_permission_set_allowed (FlatpakPermission *permission)
{
  permission->allowed = TRUE;
  /* We reset even when allowed, because lower layer conditionals being added
   * at merge would make this non-conditional layer conditional. */
  permission->reset = TRUE;
  g_ptr_array_set_size (permission->conditionals, 0);
}

static void
flatpak_permission_set_allowed_if (FlatpakPermission *permission,
                                   const char        *condition)
{
  /* If we are already unconditionally allowed, don't add useless conditionals */
  if (permission->allowed)
    return;

  /* Check if its already there */
  if (g_ptr_array_find_with_equal_func (permission->conditionals,
                                        condition,
                                        g_str_equal, NULL))
    return;

  g_ptr_array_add (permission->conditionals, g_strdup (condition));
  g_ptr_array_sort (permission->conditionals, flatpak_strcmp0_ptr);
}

static void
flatpak_permission_serialize (FlatpakPermission *permission,
                              const char        *name,
                              GPtrArray         *res,
                              gboolean           flatten)
{
  if (permission->allowed)
    {
      /* Completely allowed */

      g_ptr_array_add (res, g_strdup (name));
      g_assert (permission->conditionals->len == 0);
      /* A non-conditional add always implies reset, so no need to serialize that */
    }
  else if (permission->conditionals->len > 0)
    {
      /* Partially allowed */

      if (permission->reset && !flatten)
        g_ptr_array_add (res, g_strdup_printf ("!%s", name));

      /* As backwards compat for pre-conditional flatpaks we unconditionally
       * add this first. New versions will ignore this if there are
       * any conditionals.
       * Note: This may result in both "!foo" and "foo", but that
       * is fine as the "foo" is last and wins for older flatpaks.
       */
      g_ptr_array_add (res, g_strdup (name));

      for (size_t i = 0; i < permission->conditionals->len; i++)
        {
          const char *conditional = permission->conditionals->pdata[i];

          g_ptr_array_add (res, g_strdup_printf ("if:%s:%s", name, conditional));
        }
    }
  else
    {
      /* Completely disallowed */

      if (!flatten)
        g_ptr_array_add (res, g_strdup_printf ("!%s", name));
    }
}

static void
flatpak_permission_to_args (FlatpakPermission *permission,
                            const char        *argname,
                            const char        *name,
                            GPtrArray         *args)
{
  if (permission->allowed)
    {
      /* Completely allowed */

      g_ptr_array_add (args, g_strdup_printf ("--%s=%s", argname, name));
    }
  else if (permission->conditionals->len > 0)
    {
      /* Partially allowed */

      if (permission->reset)
        g_ptr_array_add (args, g_strdup_printf ("--no%s=%s", argname, name));

      for (size_t i = 0; i < permission->conditionals->len; i++)
        {
          const char *conditional = permission->conditionals->pdata[i];

          g_ptr_array_add (args, g_strdup_printf ("--%s-if=%s:%s",
                                                  argname, name, conditional));
        }
    }
  else
    {
      /* Completely disallowed */

      g_ptr_array_add (args, g_strdup_printf ("--no%s=%s", argname, name));
    }
}

static void
flatpak_permission_deserialize (FlatpakPermission *permission,
                                gboolean           negated,
                                const char        *maybe_condition)
{
  /* This can't use the flatpak_permission_set_ helpers, because we
   * have to be wary of the backward compat non-conditional permission
   * in case conditionals are used. */

  if (maybe_condition == NULL)
    {
      /* Non-conditional option, these are always before conditionals,
       * but if non-negated could be backwards compat for later conditional. */
      if (negated)
        {
          permission->allowed = FALSE;
          permission->reset = TRUE;
        }
      else
        {
          GPtrArray *tmp;

          /* Allow us to revert this if it is a backwards compat */
          permission->disallow_if_conditional = TRUE;
          permission->disallow_if_conditional_original_reset = permission->reset;
          tmp = permission->conditionals;
          permission->conditionals =
            permission->disallow_if_conditional_original_conditionals;
          permission->disallow_if_conditional_original_conditionals = tmp;

          permission->allowed = TRUE;
          permission->reset = TRUE;
        }
    }
  else
    {
      /* Conditional option */
      if (permission->disallow_if_conditional)
        {
          GPtrArray *tmp;

          /* Previous allow was a backward compat, revert it */
          permission->allowed = FALSE;
          permission->reset = permission->disallow_if_conditional_original_reset;
          permission->disallow_if_conditional = FALSE;
          tmp = permission->disallow_if_conditional_original_conditionals;
          permission->disallow_if_conditional_original_conditionals =
            permission->conditionals;
          permission->conditionals = tmp;
          g_ptr_array_set_size (
            permission->disallow_if_conditional_original_conditionals, 0);
        }

      g_ptr_array_add (permission->conditionals, g_strdup (maybe_condition));
      g_ptr_array_sort (permission->conditionals, flatpak_strcmp0_ptr);
    }
}

static void
flatpak_permission_merge (FlatpakPermission *permission,
                          FlatpakPermission *other_permission)
{
  if (other_permission->reset)
    {
      permission->reset = TRUE;
      g_ptr_array_set_size (permission->conditionals, 0);
    }

  permission->allowed = other_permission->allowed;

  for (size_t i = 0; i < other_permission->conditionals->len; i++)
    {
      const char *conditional = other_permission->conditionals->pdata[i];

      /* Check if its already there */
      if (g_ptr_array_find_with_equal_func (permission->conditionals,
                                            conditional,
                                            g_str_equal, NULL))
        return;

      g_ptr_array_add (permission->conditionals, g_strdup (conditional));
    }

  g_ptr_array_sort (permission->conditionals, flatpak_strcmp0_ptr);

  /* Internal consistency check */
  if (permission->allowed)
    g_assert (permission->conditionals->len == 0);
}

static gboolean
flatpak_permission_compute_allowed (FlatpakPermission                *permission,
                                    FlatpakContextConditionEvaluator  evaluator)
{
  if (permission->allowed)
    return TRUE;

  for (size_t i = 0; i < permission->conditionals->len; i++)
    {
      const char *conditional = permission->conditionals->pdata[i];
      gboolean negated;
      const char *condition_str;
      guint32 condition;

      condition_str = parse_negated (conditional, &negated);
      condition =
        flatpak_context_bitmask_from_string (condition_str,
                                             flatpak_context_conditions);

      /* If condition is 0 it means this version of flatpak doesn't know
       * about the condition and it cannot be satisfied. */
      if (condition == 0)
        continue;

      /* Conditions which are always true in this version of flatpak */
      if ((condition & flatpak_context_true_conditions) && !negated)
        return TRUE;

      /* Conditions which need runtime evaluation */
      if (evaluator && evaluator (condition) == !negated)
        return TRUE;
   }

  /* No condition evaluated to TRUE, so disable the thing */
  return FALSE;
}

static gboolean
flatpak_permission_adds_permissions (FlatpakPermission *old,
                                     FlatpakPermission *new)
{
  size_t i = 0, j = 0;

  if (old->allowed)
    return FALSE;

  if (new->allowed)
    return TRUE;

  if (new->conditionals->len > old->conditionals->len)
    return TRUE;

  while (TRUE)
    {
      const char *old_cond = old->conditionals->pdata[i];
      const char *new_cond = new->conditionals->pdata[j];
      int res;

      if (old_cond == NULL)
        return new_cond != NULL;

      if (new_cond == NULL)
        return FALSE;

      res = strcmp (old_cond, new_cond);
      if (res == 0) /* Same conditional */
        {
          i++;
          j++;
        }
      else if (res < 0) /* Old conditional was removed */
        {
          i++;
        }
      else /* new conditional */
        {
          return FALSE;
        }
    }

  return FALSE;
}

static GHashTable *
flatpak_permissions_new (void)
{
  return g_hash_table_new_full (g_str_hash, g_str_equal,
                                (GDestroyNotify) g_free,
                                (GDestroyNotify) flatpak_permission_free);
}

static GHashTable *
flatpak_permissions_dup (GHashTable *old)
{
  GHashTable *new;
  const char *name;
  FlatpakPermission *old_permission;
  GHashTableIter iter;

  new = flatpak_permissions_new ();

  g_hash_table_iter_init (&iter, old);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *) &name,
                                 (gpointer *) &old_permission))
    {
      g_hash_table_insert (new,
                           g_strdup (name),
                           flatpak_permission_dup (old_permission));
    }

  return new;
}

static FlatpakPermission *
flatpak_permissions_ensure (GHashTable *permissions,
                            const char *name)
{
  FlatpakPermission *permission = g_hash_table_lookup (permissions, name);

  if (permission == NULL)
    {
      permission = flatpak_permission_new ();
      g_hash_table_insert (permissions, g_strdup (name), permission);
    }

  return permission;
}

static void
flatpak_permissions_set_not_allowed (GHashTable *permissions,
                                     const char *name)
{
  flatpak_permission_set_not_allowed (flatpak_permissions_ensure (permissions,
                                                                  name));
}

static void
flatpak_permissions_set_allowed (GHashTable *permissions,
                                 const char *name)
{
  flatpak_permission_set_allowed (flatpak_permissions_ensure (permissions,
                                                              name));
}

static void
flatpak_permissions_set_allowed_if (GHashTable *permissions,
                                    const char *name,
                                    const char *condition)
{
  flatpak_permission_set_allowed_if (flatpak_permissions_ensure (permissions,
                                                                 name),
                                      condition);
}

static gboolean
flatpak_permissions_allows_unconditionally (GHashTable *permissions,
                                            const char *name)
{
  FlatpakPermission *permission = g_hash_table_lookup (permissions, name);

  if (permission)
    return permission->allowed;

  return FALSE;
}

static void
flatpak_permissions_to_args (GHashTable *permissions,
                             const char *argname,
                             GPtrArray  *args)
{
  g_autoptr(GList) ordered_keys = NULL;

  ordered_keys = g_hash_table_get_keys (permissions);
  ordered_keys = g_list_sort (ordered_keys, (GCompareFunc) strcmp);

  for (GList *l = ordered_keys; l != NULL; l = l->next)
    {
      const char *name = l->data;
      FlatpakPermission *permission = g_hash_table_lookup (permissions, name);

      flatpak_permission_to_args (permission, argname, name, args);
    }
}

static char **
flatpak_permissions_to_strv (GHashTable *permissions,
                             gboolean    flatten)
{
  g_autoptr(GList) ordered_keys = NULL;
  g_autoptr(GPtrArray) res = g_ptr_array_new ();

  ordered_keys = g_hash_table_get_keys (permissions);
  ordered_keys = g_list_sort (ordered_keys, (GCompareFunc) strcmp);

  for (GList *l = ordered_keys; l != NULL; l = l->next)
    {
      const char *name = l->data;
      FlatpakPermission *permission = g_hash_table_lookup (permissions, name);

      flatpak_permission_serialize (permission, name, res, flatten);
    }

  g_ptr_array_add (res, NULL);
  return (char **)g_ptr_array_free (g_steal_pointer (&res), FALSE);
}

static guint32
flatpak_permissions_compute_allowed (GHashTable                        *permissions,
                                     const char                       **names,
                                     FlatpakContextConditionEvaluator   evaluator)
{
  guint32 bitmask = 0;

  for (size_t i = 0; names[i] != NULL; i++)
    {
      const char *name = names[i];
      FlatpakPermission *permission = g_hash_table_lookup (permissions, name);

      if (permission &&
          flatpak_permission_compute_allowed (permission, evaluator))
        bitmask |= 1 << i;
    }

  return bitmask;
}

static gboolean
flatpak_permissions_from_strv (GHashTable  *permissions,
                               const char **strv,
                               GError     **error)
{
  for (size_t i = 0; strv[i] != NULL; i++)
    {
      g_auto(GStrv) tokens = g_strsplit (strv[i], ":", 3);
      const char *name = NULL;
      gboolean negated = FALSE;
      const char *condition = NULL;
      FlatpakPermission *permission;

      if (strcmp (tokens[0], "if") == 0)
        {
          if (g_strv_length (tokens) != 3)
            {
              g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                           _("Invalid permission syntax: %s"), strv[i]);
              return FALSE;
            }

          name = tokens[1];
          condition = tokens[2];
        }
      else
        {
          if (g_strv_length (tokens) != 1)
            {
              g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                           _("Invalid permission syntax: %s"), strv[i]);
              return FALSE;
            }

          name = parse_negated (tokens[0], &negated);
        }

      permission = flatpak_permissions_ensure (permissions, name);
      flatpak_permission_deserialize (permission, negated, condition);
    }

  return TRUE;
}

static void
flatpak_permissions_merge (GHashTable *permissions,
                           GHashTable *other)
{
  const char *name;
  FlatpakPermission *other_permission;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, other);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *) &name,
                                 (gpointer *) &other_permission))
    {
      FlatpakPermission *permission = g_hash_table_lookup (permissions, name);

      if (permission)
        {
          flatpak_permission_merge (permission, other_permission);
        }
      else
        {
          g_hash_table_insert (permissions,
                               g_strdup (name),
                               flatpak_permission_dup (other_permission));
        }
    }
}

static gboolean
flatpak_permissions_adds_permissions (GHashTable *old,
                                      GHashTable *new)
{
  const char *name;
  FlatpakPermission *new_permission;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, new);
  while (g_hash_table_iter_next (&iter,
                                 (gpointer *) &name,
                                 (gpointer *) &new_permission))
    {
      FlatpakPermission *old_permission = g_hash_table_lookup (old, name);

      if (old_permission)
        {
          if (flatpak_permission_adds_permissions (old_permission,
                                                   new_permission))
            return TRUE;
        }
      else
        {
          if (new_permission->allowed ||
              new_permission->conditionals->len > 0)
            return TRUE; /* new is completely new permission */
        }
    }

  return FALSE;
}

#ifdef INCLUDE_INTERNAL_TESTS
static void flatpak_permissions_test_basic (void)
{
  /* This is in canonical form, so must be kept sorted by name */
  const char *perms_strv[] =
    {
      /* Regular unconditional allowed (resets) */
      "allowed",

      /* conditional allowed with two conditions (doesn't reset) */
      "cond1", /* backwards compat */
      "if:cond1:check1",
      "if:cond1:check2",

      /* conditional allowed with one conditions (doesn't reset) */
      "cond2", /* backwards compat */
      "if:cond2:check3",

      /* conditional allowed (resets) */
      "!cond3", /* reset */
      "cond3", /* backwards compat */
      "if:cond3:check3",

      /* Regular unconditional disallowed (resets) */
      "!disallowed",

      NULL,
    };

  const char *perms_args[] =
    {
      "--socket=allowed",

      "--socket-if=cond1:check1",
      "--socket-if=cond1:check2",

      "--socket-if=cond2:check3",

      /* conditional allowed (resets) */
      "--nosocket=cond3",
      "--socket-if=cond3:check3",

      /* Regular unconditional disallowed (resets) */
      "--nosocket=disallowed",

      NULL,
    };

  GError *error = NULL;

  /* Test parsing */
  g_autoptr(GHashTable) perms = flatpak_permissions_new ();
  gboolean ok = flatpak_permissions_from_strv (perms, perms_strv, &error);
  g_assert_true(ok);
  g_assert_no_error(error);
  g_assert_nonnull(perms);

  g_assert_cmpint(g_hash_table_size (perms), ==, 5);

  FlatpakPermission *allowed = g_hash_table_lookup (perms, "allowed");
  g_assert_nonnull(allowed);
  g_assert_true(allowed->allowed);
  g_assert_true(allowed->reset);
  g_assert(allowed->conditionals->len == 0);

  FlatpakPermission *disallowed = g_hash_table_lookup (perms, "disallowed");
  g_assert_nonnull(disallowed);
  g_assert_false(disallowed->allowed);
  g_assert_true(disallowed->reset);
  g_assert(disallowed->conditionals->len == 0);

  FlatpakPermission *cond1 = g_hash_table_lookup (perms, "cond1");
  g_assert_nonnull(cond1);
  g_assert_false(cond1->allowed);
  g_assert_false(cond1->reset);
  g_assert(cond1->conditionals->len == 2);
  g_assert_cmpstr(cond1->conditionals->pdata[0], ==, "check1");
  g_assert_cmpstr(cond1->conditionals->pdata[1], ==, "check2");

  FlatpakPermission *cond2 = g_hash_table_lookup (perms, "cond2");
  g_assert_nonnull(cond2);
  g_assert_false(cond2->allowed);
  g_assert_false(cond2->reset);
  g_assert(cond2->conditionals->len == 1);
  g_assert_cmpstr(cond2->conditionals->pdata[0], ==, "check3");

  FlatpakPermission *cond3 = g_hash_table_lookup (perms, "cond3");
  g_assert_nonnull(cond3);
  g_assert_false(cond3->allowed);
  g_assert_true(cond3->reset);
  g_assert(cond3->conditionals->len == 1);
  g_assert_cmpstr(cond3->conditionals->pdata[0], ==, "check3");

  /* Test roundtrip */
  g_auto(GStrv) new_strv = flatpak_permissions_to_strv (perms, FALSE);
  g_assert_cmpstrv (perms_strv, new_strv);

  g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
  flatpak_permissions_to_args (perms, "socket", args);
  g_ptr_array_add(args, NULL);
  g_assert_cmpstrv (perms_args, args->pdata);

  /* Test copy */
  g_autoptr(FlatpakPermission) cond1_copy = flatpak_permission_dup(cond1);
  g_assert_nonnull(cond1_copy);
  g_assert_false(cond1_copy->allowed);
  g_assert_false(cond1_copy->reset);
  g_assert(cond1_copy->conditionals->len == 2);
  g_assert_cmpstr(cond1_copy->conditionals->pdata[0], ==, "check1");
  g_assert_cmpstr(cond1_copy->conditionals->pdata[1], ==, "check2");

  /* Test setters: */
  {
    g_autoptr(FlatpakPermission) copy = flatpak_permission_dup(cond1);
    flatpak_permission_set_allowed (copy);
    g_assert_true (copy->allowed);
    g_assert_true (copy->reset);
    g_assert(copy->conditionals->len == 0);
  }

  {
    g_autoptr(FlatpakPermission) copy = flatpak_permission_dup(cond1);
    flatpak_permission_set_not_allowed (copy);
    g_assert_false (copy->allowed);
    g_assert_true (copy->reset);
    g_assert(copy->conditionals->len == 0);
  }

  {
    g_autoptr(FlatpakPermission) copy = flatpak_permission_dup(cond1);
    flatpak_permission_set_allowed_if (copy, "check0");
    g_assert_false (copy->allowed);
    g_assert_false (copy->reset);
    g_assert(copy->conditionals->len == 3);
    g_assert_cmpstr(copy->conditionals->pdata[0], ==, "check0");
    g_assert_cmpstr(copy->conditionals->pdata[1], ==, "check1");
    g_assert_cmpstr(copy->conditionals->pdata[2], ==, "check2");
  }

  /* Test merge */
  {
    g_autoptr(FlatpakPermission) copy = flatpak_permission_dup(cond1);
    flatpak_permission_merge (copy, allowed);
    g_assert_true (copy->allowed);
    g_assert_true (copy->reset);
    g_assert(copy->conditionals->len == 0);
  }
  {
    g_autoptr(FlatpakPermission) copy = flatpak_permission_dup(cond1);
    flatpak_permission_merge (copy, disallowed);
    g_assert_false (copy->allowed);
    g_assert_true (copy->reset);
    g_assert(copy->conditionals->len == 0);
  }
  {
    /* Merge from non-reset conditional */
    g_autoptr(FlatpakPermission) copy = flatpak_permission_dup(cond1);
    flatpak_permission_merge (copy, cond2);
    g_assert_false (copy->allowed);
    g_assert_false (copy->reset);
    g_assert(copy->conditionals->len == 3);
  }
  {
    /* Merge from reset conditional */
    g_autoptr(FlatpakPermission) copy = flatpak_permission_dup(cond1);
    flatpak_permission_merge (copy, cond3);
    g_assert_false (copy->allowed);
    g_assert_true (copy->reset);
    g_assert(copy->conditionals->len == 1);
  }
}

static void flatpak_permissions_test_backwards_compat (void)
{
  {
    /* Deserialize if:wayland:foo;wayland
     * The last wayland makes it unconditional. */
    g_autoptr(FlatpakPermission) perm = flatpak_permission_new ();

    flatpak_permission_deserialize (perm, FALSE, "foo");
    flatpak_permission_deserialize (perm, FALSE, NULL);
    g_assert_true (perm->allowed);
    g_assert_true (perm->reset);
    g_assert_cmpint (perm->conditionals->len, ==, 0);
  }

  {
    /* Deserialize wayland;if:wayland:foo;wayland
     * Should be the same as the one above. The first wayland is just for
     * backwards compat. */
    g_autoptr(FlatpakPermission) perm = flatpak_permission_new ();

    flatpak_permission_deserialize (perm, FALSE, NULL);
    flatpak_permission_deserialize (perm, FALSE, "foo");
    flatpak_permission_deserialize (perm, FALSE, NULL);
    g_assert_true (perm->allowed);
    g_assert_true (perm->reset);
    g_assert_cmpint (perm->conditionals->len, ==, 0);
  }

  {
    /* Deserialize if:wayland:foo;wayland;if:wayland:bar
     * Now the wayland is before a conditional, so it acts as backwards
     * compat. */
    g_autoptr(FlatpakPermission) perm = flatpak_permission_new ();

    flatpak_permission_deserialize (perm, FALSE, "foo");
    flatpak_permission_deserialize (perm, FALSE, NULL);
    flatpak_permission_deserialize (perm, FALSE, "bar");
    g_assert_false (perm->allowed);
    g_assert_false (perm->reset);
    g_assert_cmpint (perm->conditionals->len, ==, 2);
    g_assert_cmpstr (perm->conditionals->pdata[0], ==, "bar");
    g_assert_cmpstr (perm->conditionals->pdata[1], ==, "foo");
  }
}

FLATPAK_INTERNAL_TEST("/context/permissions/basic",
                      flatpak_permissions_test_basic);
FLATPAK_INTERNAL_TEST("/context/permissions/backwards-compat",
                      flatpak_permissions_test_backwards_compat);

#endif /* INCLUDE_INTERNAL_TESTS */

FlatpakContext *
flatpak_context_new (void)
{
  FlatpakContext *context;

  context = g_slice_new0 (FlatpakContext);
  context->env_vars = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  context->persistent = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  /* filename or special filesystem name => FlatpakFilesystemMode */
  context->filesystems = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->session_bus_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->system_bus_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->a11y_bus_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->generic_policy = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                   g_free, (GDestroyNotify) g_strfreev);
  context->enumerable_usb_devices = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                           g_free, (GDestroyNotify) flatpak_usb_query_free);
  context->hidden_usb_devices = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                       g_free, (GDestroyNotify) flatpak_usb_query_free);
  context->socket_permissions = flatpak_permissions_new ();
  context->device_permissions = flatpak_permissions_new ();

  return context;
}

void
flatpak_context_free (FlatpakContext *context)
{
  g_hash_table_destroy (context->env_vars);
  g_hash_table_destroy (context->persistent);
  g_hash_table_destroy (context->filesystems);
  g_hash_table_destroy (context->session_bus_policy);
  g_hash_table_destroy (context->system_bus_policy);
  g_hash_table_destroy (context->a11y_bus_policy);
  g_hash_table_destroy (context->generic_policy);
  g_hash_table_destroy (context->enumerable_usb_devices);
  g_hash_table_destroy (context->hidden_usb_devices);
  g_hash_table_destroy (context->device_permissions);
  g_hash_table_destroy (context->socket_permissions);
  g_slice_free (FlatpakContext, context);
}

static guint32
flatpak_context_bitmask_from_string (const char *name, const char **names)
{
  guint32 i;

  for (i = 0; names[i] != NULL; i++)
    {
      if (strcmp (names[i], name) == 0)
        return 1 << i;
    }

  return 0;
}

static void
flatpak_context_bitmask_to_string (guint32      enabled,
                                   guint32      valid,
                                   const char **names,
                                   GPtrArray   *array)
{
  guint32 i;

  for (i = 0; names[i] != NULL; i++)
    {
      guint32 bitmask = 1 << i;

      if (valid & bitmask)
        {
          if (enabled & bitmask)
            g_ptr_array_add (array, g_strdup (names[i]));
          else
            g_ptr_array_add (array, g_strdup_printf ("!%s", names[i]));
        }
    }
}

static void
flatpak_context_bitmask_to_args (guint32 enabled, guint32 valid, const char **names,
                                 const char *enable_arg, const char *disable_arg,
                                 GPtrArray *args)
{
  guint32 i;

  for (i = 0; names[i] != NULL; i++)
    {
      guint32 bitmask = 1 << i;
      if (valid & bitmask)
        {
          if (enabled & bitmask)
            g_ptr_array_add (args, g_strdup_printf ("%s=%s", enable_arg, names[i]));
          else
            g_ptr_array_add (args, g_strdup_printf ("%s=%s", disable_arg, names[i]));
        }
    }
}


static FlatpakContextShares
flatpak_context_share_from_string (const char *string, GError **error)
{
  FlatpakContextShares shares = flatpak_context_bitmask_from_string (string, flatpak_context_shares);

  if (shares == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **) flatpak_context_shares);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown share type %s, valid types are: %s"), string, values);
    }

  return shares;
}

static char **
flatpak_context_shared_to_string (FlatpakContextShares shares,
                                  FlatpakContextShares valid)
{
  g_autoptr (GPtrArray) array = g_ptr_array_new_with_free_func (g_free);

  flatpak_context_bitmask_to_string (shares, valid,
                                     flatpak_context_shares,
                                     array);

  g_ptr_array_add (array, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&array), FALSE);
}

static void
flatpak_context_shared_to_args (FlatpakContext *context,
                                GPtrArray      *args)
{
  flatpak_context_bitmask_to_args (context->shares, context->shares_valid,
                                   flatpak_context_shares,
                                   "--share", "--unshare",
                                   args);
}

static FlatpakPolicy
flatpak_policy_from_string (const char *string, GError **error)
{
  const char *policies[] = { "none", "see", "talk", "own", NULL };
  int i;
  g_autofree char *values = NULL;

  for (i = 0; policies[i]; i++)
    {
      if (strcmp (string, policies[i]) == 0)
        return i;
    }

  values = g_strjoinv (", ", (char **) policies);
  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               _("Unknown policy type %s, valid types are: %s"), string, values);

  return -1;
}

static const char *
flatpak_policy_to_string (FlatpakPolicy policy)
{
  if (policy == FLATPAK_POLICY_SEE)
    return "see";
  if (policy == FLATPAK_POLICY_TALK)
    return "talk";
  if (policy == FLATPAK_POLICY_OWN)
    return "own";

  return "none";
}

static gboolean
flatpak_verify_dbus_name (const char *name, GError **error)
{
  const char *name_part;
  g_autofree char *tmp = NULL;

  if (g_str_has_suffix (name, ".*"))
    {
      tmp = g_strndup (name, strlen (name) - 2);
      name_part = tmp;
    }
  else
    {
      name_part = name;
    }

  if (g_dbus_is_name (name_part) && !g_dbus_is_unique_name (name_part))
    return TRUE;

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               _("Invalid dbus name %s"), name);
  return FALSE;
}

static FlatpakContextSockets
flatpak_context_socket_from_string (const char *string, GError **error)
{
  FlatpakContextSockets sockets = flatpak_context_bitmask_from_string (string, flatpak_context_sockets);

  if (sockets == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **) flatpak_context_sockets);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown socket type %s, valid types are: %s"), string, values);
    }

  return sockets;
}

static FlatpakContextDevices
flatpak_context_device_from_string (const char *string, GError **error)
{
  FlatpakContextDevices devices = flatpak_context_bitmask_from_string (string, flatpak_context_devices);

  if (devices == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **) flatpak_context_devices);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown device type %s, valid types are: %s"), string, values);
    }
  return devices;
}

static FlatpakContextFeatures
flatpak_context_feature_from_string (const char *string, GError **error)
{
  FlatpakContextFeatures feature = flatpak_context_bitmask_from_string (string, flatpak_context_features);

  if (feature == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **) flatpak_context_features);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown feature type %s, valid types are: %s"), string, values);
    }

  return feature;
}

static char **
flatpak_context_features_to_string (FlatpakContextFeatures features, FlatpakContextFeatures valid)
{
  g_autoptr (GPtrArray) array = g_ptr_array_new_with_free_func (g_free);

  flatpak_context_bitmask_to_string (features, valid,
                                     flatpak_context_features,
                                     array);

  g_ptr_array_add (array, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&array), FALSE);
}

static void
flatpak_context_features_to_args (FlatpakContext *context,
                                  GPtrArray      *args)
{
  flatpak_context_bitmask_to_args (context->features, context->features_valid,
                                   flatpak_context_features,
                                   "--allow", "--disallow",
                                   args);
}

static void
flatpak_context_add_shares (FlatpakContext      *context,
                            FlatpakContextShares shares)
{
  context->shares_valid |= shares;
  context->shares |= shares;
}

static void
flatpak_context_remove_shares (FlatpakContext      *context,
                               FlatpakContextShares shares)
{
  context->shares_valid |= shares;
  context->shares &= ~shares;
}

static void
flatpak_context_add_features (FlatpakContext        *context,
                              FlatpakContextFeatures features)
{
  context->features_valid |= features;
  context->features |= features;
}

static void
flatpak_context_remove_features (FlatpakContext        *context,
                                 FlatpakContextFeatures features)
{
  context->features_valid |= features;
  context->features &= ~features;
}

static void
flatpak_context_set_env_var (FlatpakContext *context,
                             const char     *name,
                             const char     *value)
{
  g_hash_table_insert (context->env_vars, g_strdup (name), g_strdup (value));
}

void
flatpak_context_set_session_bus_policy (FlatpakContext *context,
                                        const char     *name,
                                        FlatpakPolicy   policy)
{
  g_hash_table_insert (context->session_bus_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

void
flatpak_context_set_a11y_bus_policy (FlatpakContext *context,
                                     const char     *name,
                                     FlatpakPolicy   policy)
{
  g_hash_table_insert (context->a11y_bus_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

GStrv
flatpak_context_get_session_bus_policy_allowed_own_names (FlatpakContext *context)
{
  GHashTableIter iter;
  gpointer key, value;
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func (g_free);

  g_hash_table_iter_init (&iter, context->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (GPOINTER_TO_INT (value) == FLATPAK_POLICY_OWN)
      g_ptr_array_add (names, g_strdup (key));

  g_ptr_array_add (names, NULL);
  return (GStrv) g_ptr_array_free (g_steal_pointer (&names), FALSE);
}

void
flatpak_context_set_system_bus_policy (FlatpakContext *context,
                                       const char     *name,
                                       FlatpakPolicy   policy)
{
  g_hash_table_insert (context->system_bus_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

static void
flatpak_context_apply_generic_policy (FlatpakContext *context,
                                      const char     *key,
                                      const char     *value)
{
  GPtrArray *new = g_ptr_array_new ();
  const char **old_v;
  int i;

  g_assert (strchr (key, '.') != NULL);

  old_v = g_hash_table_lookup (context->generic_policy, key);
  for (i = 0; old_v != NULL && old_v[i] != NULL; i++)
    {
      const char *old = old_v[i];
      const char *cmp1 = old;
      const char *cmp2 = value;
      if (*cmp1 == '!')
        cmp1++;
      if (*cmp2 == '!')
        cmp2++;
      if (strcmp (cmp1, cmp2) != 0)
        g_ptr_array_add (new, g_strdup (old));
    }

  g_ptr_array_add (new, g_strdup (value));
  g_ptr_array_add (new, NULL);

  g_hash_table_insert (context->generic_policy, g_strdup (key),
                       g_ptr_array_free (new, FALSE));
}

static void
flatpak_context_add_query_to (GHashTable            *queries,
                              const FlatpakUsbQuery *usb_query)
{
  g_autoptr(FlatpakUsbQuery) copy = NULL;
  g_autoptr(GString) string = NULL;

  g_assert (queries != NULL);
  g_assert (usb_query != NULL && usb_query->rules != NULL);

  copy = flatpak_usb_query_copy (usb_query);

  string = g_string_new (NULL);
  flatpak_usb_query_print (usb_query, string);

  g_hash_table_insert (queries,
                       g_strdup (string->str),
                       g_steal_pointer (&copy));
}

static void
flatpak_context_add_usb_query (FlatpakContext        *context,
                               const FlatpakUsbQuery *usb_query)
{
  flatpak_context_add_query_to (context->enumerable_usb_devices, usb_query);
}

static void
flatpak_context_add_nousb_query (FlatpakContext        *context,
                                 const FlatpakUsbQuery *usb_query)
{
  flatpak_context_add_query_to (context->hidden_usb_devices, usb_query);
}

static gboolean
flatpak_context_add_usb_list (FlatpakContext *context,
                              const char     *list,
                              GError        **error)
{
  return flatpak_usb_parse_usb_list (list, context->enumerable_usb_devices,
                                     context->hidden_usb_devices, error);
}

static gboolean
flatpak_context_add_usb_list_from_file (FlatpakContext *context,
                                        const char     *path,
                                        GError        **error)
{
  g_autofree char *contents = NULL;

  if (!flatpak_validate_path_characters (path, error))
    return FALSE;

  if (!g_file_get_contents (path, &contents, NULL, error))
    return FALSE;

  return flatpak_usb_parse_usb_list (contents, context->enumerable_usb_devices,
                                     context->hidden_usb_devices, error);
}

static gboolean
flatpak_context_set_persistent (FlatpakContext *context,
                                const char     *path,
                                GError        **error)
{
  if (!flatpak_validate_path_characters (path, error))
    return FALSE;

  g_hash_table_insert (context->persistent, g_strdup (path), GINT_TO_POINTER (1));
  return TRUE;
}

static gboolean
get_xdg_dir_from_prefix (const char  *prefix,
                         const char **where,
                         const char **dir)
{
  if (strcmp (prefix, "xdg-data") == 0)
    {
      if (where)
        *where = "data";
      if (dir)
        *dir = g_get_user_data_dir ();
      return TRUE;
    }
  if (strcmp (prefix, "xdg-cache") == 0)
    {
      if (where)
        *where = "cache";
      if (dir)
        *dir = g_get_user_cache_dir ();
      return TRUE;
    }
  if (strcmp (prefix, "xdg-config") == 0)
    {
      if (where)
        *where = "config";
      if (dir)
        *dir = g_get_user_config_dir ();
      return TRUE;
    }
  return FALSE;
}

/* This looks only in the xdg dirs (config, cache, data), not the user
   definable ones */
static char *
get_xdg_dir_from_string (const char  *filesystem,
                         const char **suffix,
                         const char **where)
{
  char *slash;
  const char *rest;
  g_autofree char *prefix = NULL;
  const char *dir = NULL;
  gsize len;

  slash = strchr (filesystem, '/');

  if (slash)
    len = slash - filesystem;
  else
    len = strlen (filesystem);

  rest = filesystem + len;
  while (*rest == '/')
    rest++;

  if (suffix != NULL)
    *suffix = rest;

  prefix = g_strndup (filesystem, len);

  if (get_xdg_dir_from_prefix (prefix, where, &dir))
    return g_build_filename (dir, rest, NULL);

  return NULL;
}

static gboolean
get_xdg_user_dir_from_string (const char  *filesystem,
                              const char **config_key,
                              const char **suffix,
                              char **dir)
{
  char *slash;
  const char *rest;
  g_autofree char *prefix = NULL;
  gsize len;
  const char *dir_out = NULL;

  slash = strchr (filesystem, '/');

  if (slash)
    len = slash - filesystem;
  else
    len = strlen (filesystem);

  rest = filesystem + len;
  while (*rest == '/')
    rest++;

  if (suffix)
    *suffix = rest;

  prefix = g_strndup (filesystem, len);

  if (strcmp (prefix, "xdg-desktop") == 0)
    {
      if (config_key)
        *config_key = "XDG_DESKTOP_DIR";
      if (dir)
        *dir = g_strdup (g_get_user_special_dir (G_USER_DIRECTORY_DESKTOP));
      return TRUE;
    }
  if (strcmp (prefix, "xdg-documents") == 0)
    {
      if (config_key)
        *config_key = "XDG_DOCUMENTS_DIR";
      if (dir)
        *dir = g_strdup (g_get_user_special_dir (G_USER_DIRECTORY_DOCUMENTS));
      return TRUE;
    }
  if (strcmp (prefix, "xdg-download") == 0)
    {
      if (config_key)
        *config_key = "XDG_DOWNLOAD_DIR";
      if (dir)
        *dir = g_strdup (g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD));
      return TRUE;
    }
  if (strcmp (prefix, "xdg-music") == 0)
    {
      if (config_key)
        *config_key = "XDG_MUSIC_DIR";
      if (dir)
        *dir = g_strdup (g_get_user_special_dir (G_USER_DIRECTORY_MUSIC));
      return TRUE;
    }
  if (strcmp (prefix, "xdg-pictures") == 0)
    {
      if (config_key)
        *config_key = "XDG_PICTURES_DIR";
      if (dir)
        *dir = g_strdup (g_get_user_special_dir (G_USER_DIRECTORY_PICTURES));
      return TRUE;
    }
  if (strcmp (prefix, "xdg-public-share") == 0)
    {
      if (config_key)
        *config_key = "XDG_PUBLICSHARE_DIR";
      if (dir)
        *dir = g_strdup (g_get_user_special_dir (G_USER_DIRECTORY_PUBLIC_SHARE));
      return TRUE;
    }
  if (strcmp (prefix, "xdg-templates") == 0)
    {
      if (config_key)
        *config_key = "XDG_TEMPLATES_DIR";
      if (dir)
        *dir = g_strdup (g_get_user_special_dir (G_USER_DIRECTORY_TEMPLATES));
      return TRUE;
    }
  if (strcmp (prefix, "xdg-videos") == 0)
    {
      if (config_key)
        *config_key = "XDG_VIDEOS_DIR";
      if (dir)
        *dir = g_strdup (g_get_user_special_dir (G_USER_DIRECTORY_VIDEOS));
      return TRUE;
    }
  if (get_xdg_dir_from_prefix (prefix, NULL, &dir_out))
    {
      if (config_key)
        *config_key = NULL;
      if (dir)
        *dir = g_strdup (dir_out);
      return TRUE;
    }
  /* Don't support xdg-run without suffix, because that doesn't work */
  if (strcmp (prefix, "xdg-run") == 0 &&
      *rest != 0)
    {
      if (config_key)
        *config_key = NULL;
      if (dir)
        *dir = flatpak_get_real_xdg_runtime_dir ();
      return TRUE;
    }

  return FALSE;
}

static char *
unparse_filesystem_flags (const char           *path,
                          FlatpakFilesystemMode mode)
{
  g_autoptr(GString) s = g_string_new ("");
  const char *p;

  for (p = path; *p != 0; p++)
    {
      if (*p == ':')
        g_string_append (s, "\\:");
      else if (*p == '\\')
        g_string_append (s, "\\\\");
      else
        g_string_append_c (s, *p);
    }

  switch (mode)
    {
    case FLATPAK_FILESYSTEM_MODE_READ_ONLY:
      g_string_append (s, ":ro");
      break;

    case FLATPAK_FILESYSTEM_MODE_CREATE:
      g_string_append (s, ":create");
      break;

    case FLATPAK_FILESYSTEM_MODE_READ_WRITE:
      break;

    case FLATPAK_FILESYSTEM_MODE_NONE:
      g_string_insert_c (s, 0, '!');

      if (g_str_has_suffix (s->str, "-reset"))
        {
          g_string_truncate (s, s->len - 6);
          g_string_append (s, ":reset");
        }
      break;

    default:
      g_warning ("Unexpected filesystem mode %d", mode);
      break;
    }

  return g_string_free (g_steal_pointer (&s), FALSE);
}

static char *
parse_filesystem_flags (const char            *filesystem,
                        gboolean               negated,
                        FlatpakFilesystemMode *mode_out,
                        GError               **error)
{
  g_autoptr(GString) s = g_string_new ("");
  const char *p, *suffix;
  FlatpakFilesystemMode mode;
  gboolean reset = FALSE;

  p = filesystem;
  while (*p != 0 && *p != ':')
    {
      if (*p == '\\')
        {
          p++;
          if (*p != 0)
            g_string_append_c (s, *p++);
        }
      else
        g_string_append_c (s, *p++);
    }

  if (negated)
    mode = FLATPAK_FILESYSTEM_MODE_NONE;
  else
    mode = FLATPAK_FILESYSTEM_MODE_READ_WRITE;

  if (g_str_equal (s->str, "host-reset"))
    {
      reset = TRUE;

      if (!negated)
        {
          g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                       "Filesystem token \"%s\" is only applicable for --nofilesystem",
                       s->str);
          return NULL;
        }

      if (*p != '\0')
        {
          g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                       "Filesystem token \"%s\" cannot be used with a suffix",
                       s->str);
          return NULL;
        }
    }

  if (*p == ':')
    {
      suffix = p + 1;

      if (strcmp (suffix, "ro") == 0)
        mode = FLATPAK_FILESYSTEM_MODE_READ_ONLY;
      else if (strcmp (suffix, "rw") == 0)
        mode = FLATPAK_FILESYSTEM_MODE_READ_WRITE;
      else if (strcmp (suffix, "create") == 0)
        mode = FLATPAK_FILESYSTEM_MODE_CREATE;
      else if (strcmp (suffix, "reset") == 0)
        reset = TRUE;
      else if (*suffix != 0)
        g_warning ("Unexpected filesystem suffix %s, ignoring", suffix);

      if (negated && mode != FLATPAK_FILESYSTEM_MODE_NONE)
        {
          g_warning ("Filesystem suffix \"%s\" is not applicable for --nofilesystem",
                     suffix);
          mode = FLATPAK_FILESYSTEM_MODE_NONE;
        }

      if (reset)
        {
          if (!negated)
            {
              g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                           "Filesystem suffix \"%s\" only applies to --nofilesystem",
                           suffix);
              return NULL;
            }

          if (!g_str_equal (s->str, "host"))
            {
              g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                           "Filesystem suffix \"%s\" can only be applied to "
                           "--nofilesystem=host",
                           suffix);
              return NULL;
            }

          /* We internally handle host:reset (etc) as host-reset, only exposing it as a flag in the public
             part to allow it to be ignored (with a warning) for old flatpak versions */
          g_string_append (s, "-reset");
        }
    }

  /* Postcondition check: the code above should make some results
   * impossible */
  if (negated)
    {
      g_assert (mode == FLATPAK_FILESYSTEM_MODE_NONE);
    }
  else
    {
      g_assert (mode > FLATPAK_FILESYSTEM_MODE_NONE);
      /* This flag is only applicable to --nofilesystem */
      g_assert (!reset);
    }

  /* Postcondition check: filesystem token is host-reset iff reset flag
   * was found */
  if (reset)
    g_assert (g_str_equal (s->str, "host-reset"));
  else
    g_assert (!g_str_equal (s->str, "host-reset"));

  if (mode_out)
    *mode_out = mode;

  return g_string_free (g_steal_pointer (&s), FALSE);
}

gboolean
flatpak_context_parse_filesystem (const char             *filesystem_and_mode,
                                  gboolean                negated,
                                  char                  **filesystem_out,
                                  FlatpakFilesystemMode  *mode_out,
                                  GError                **error)
{
  g_autofree char *filesystem = NULL;
  char *slash;

  if (!flatpak_validate_path_characters (filesystem_and_mode, error))
    return FALSE;

  filesystem = parse_filesystem_flags (filesystem_and_mode, negated, mode_out, error);
  if (filesystem == NULL)
    return FALSE;

  slash = strchr (filesystem, '/');

  /* Forbid /../ in paths */
  if (slash != NULL)
    {
      if (g_str_has_prefix (slash + 1, "../") ||
          g_str_has_suffix (slash + 1, "/..") ||
          strstr (slash + 1, "/../") != NULL)
        {
          g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                       _("Filesystem location \"%s\" contains \"..\""),
                       filesystem);
          return FALSE;
        }

      /* Convert "//" and "/./" to "/" */
      for (; slash != NULL; slash = strchr (slash + 1, '/'))
        {
          while (TRUE)
            {
              if (slash[1] == '/')
                memmove (slash + 1, slash + 2, strlen (slash + 2) + 1);
              else if (slash[1] == '.' && slash[2] == '/')
                memmove (slash + 1, slash + 3, strlen (slash + 3) + 1);
              else
                break;
            }
        }

      /* Eliminate trailing "/." or "/". */
      while (TRUE)
        {
          slash = strrchr (filesystem, '/');

          if (slash != NULL &&
              ((slash != filesystem && slash[1] == '\0') ||
               (slash[1] == '.' && slash[2] == '\0')))
            *slash = '\0';
          else
            break;
        }

      if (filesystem[0] == '/' && filesystem[1] == '\0')
        {
          /* We don't allow --filesystem=/ as equivalent to host, because
           * it doesn't do what you'd think: --filesystem=host mounts some
           * host directories in /run/host, not in the root. */
          g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                       _("--filesystem=/ is not available, "
                         "use --filesystem=host for a similar result"));
          return FALSE;
        }
    }

  if (g_strv_contains (flatpak_context_special_filesystems, filesystem) ||
      get_xdg_user_dir_from_string (filesystem, NULL, NULL, NULL) ||
      g_str_has_prefix (filesystem, "~/") ||
      g_str_has_prefix (filesystem, "/"))
    {
      if (filesystem_out != NULL)
        *filesystem_out = g_steal_pointer (&filesystem);

      return TRUE;
    }

  if (strcmp (filesystem, "~") == 0)
    {
      if (filesystem_out != NULL)
        *filesystem_out = g_strdup ("home");

      return TRUE;
    }

  if (g_str_has_prefix (filesystem, "home/"))
    {
      if (filesystem_out != NULL)
        *filesystem_out = g_strconcat ("~/", filesystem + 5, NULL);

      return TRUE;
    }

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               _("Unknown filesystem location %s, valid locations are: host, host-os, host-etc, host-root, home, xdg-*[/…], ~/dir, /dir"), filesystem);
  return FALSE;
}

static void
flatpak_context_take_filesystem (FlatpakContext        *context,
                                 char                  *fs,
                                 FlatpakFilesystemMode  mode)
{
  /* Special case: --nofilesystem=host-reset implies --nofilesystem=host.
   * --filesystem=host-reset (or host:reset) is not allowed. */
  if (g_str_equal (fs, "host-reset"))
    {
      g_return_if_fail (mode == FLATPAK_FILESYSTEM_MODE_NONE);
      g_hash_table_insert (context->filesystems, g_strdup ("host"), GINT_TO_POINTER (mode));
    }

  g_hash_table_insert (context->filesystems, fs, GINT_TO_POINTER (mode));
}

void
flatpak_context_merge (FlatpakContext *context,
                       FlatpakContext *other)
{
  GHashTableIter iter;
  gpointer key, value;

  context->shares &= ~other->shares_valid;
  context->shares |= other->shares;
  context->shares_valid |= other->shares_valid;
  context->features &= ~other->features_valid;
  context->features |= other->features;
  context->features_valid |= other->features_valid;

  flatpak_permissions_merge (context->socket_permissions,
                             other->socket_permissions);
  flatpak_permissions_merge (context->device_permissions,
                             other->device_permissions);

  g_hash_table_iter_init (&iter, other->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->env_vars, g_strdup (key), g_strdup (value));

  g_hash_table_iter_init (&iter, other->persistent);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->persistent, g_strdup (key), value);

  /* We first handle host:reset, as it overrides all other keys from the parent */
  if (g_hash_table_lookup_extended (other->filesystems, "host-reset", NULL, &value))
    {
      g_warn_if_fail (GPOINTER_TO_INT (value) == FLATPAK_FILESYSTEM_MODE_NONE);
      g_hash_table_remove_all (context->filesystems);
    }

  /* Then set the new ones, which includes propagating host:reset. */
  g_hash_table_iter_init (&iter, other->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->filesystems, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->session_bus_policy, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->system_bus_policy, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->a11y_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->a11y_bus_policy, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->generic_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char **policy_values = (const char **) value;
      int i;

      for (i = 0; policy_values[i] != NULL; i++)
        flatpak_context_apply_generic_policy (context, (char *) key, policy_values[i]);
    }

  g_hash_table_iter_init (&iter, other->enumerable_usb_devices);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    flatpak_context_add_usb_query (context, value);

  g_hash_table_iter_init (&iter, other->hidden_usb_devices);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    flatpak_context_add_nousb_query (context, value);
}

static gboolean
option_share_cb (const gchar *option_name,
                 const gchar *value,
                 gpointer     data,
                 GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextShares share;

  share = flatpak_context_share_from_string (value, error);
  if (share == 0)
    return FALSE;

  flatpak_context_add_shares (context, share);

  return TRUE;
}

static gboolean
option_unshare_cb (const gchar *option_name,
                   const gchar *value,
                   gpointer     data,
                   GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextShares share;

  share = flatpak_context_share_from_string (value, error);
  if (share == 0)
    return FALSE;

  flatpak_context_remove_shares (context, share);

  return TRUE;
}

static gboolean
option_socket_cb (const gchar *option_name,
                  const gchar *value,
                  gpointer     data,
                  GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextSockets socket;

  socket = flatpak_context_socket_from_string (value, error);
  if (socket == 0)
    return FALSE;

  flatpak_permissions_set_allowed (context->socket_permissions,
                                   value);

  return TRUE;
}

static gboolean
option_nosocket_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextSockets socket;

  socket = flatpak_context_socket_from_string (value, error);
  if (socket == 0)
    return FALSE;

  flatpak_permissions_set_not_allowed (context->socket_permissions,
                                       value);

  return TRUE;
}

static gboolean
parse_if_option (const char  *option_name,
                 const char  *value,
                 char       **name_out,
                 char       **condition_out,
                 GError     **error)
{
  g_auto(GStrv) tokens = g_strsplit (value, ":", 2);

  if (g_strv_length (tokens) != 2)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Invalid syntax for %s: %s"), option_name, value);
      return FALSE;
    }

  *name_out = g_strdup (tokens[0]);
  *condition_out = g_strdup (tokens[1]);
  return TRUE;
}

static gboolean
option_socket_if_cb (const gchar  *option_name,
                     const gchar  *value,
                     gpointer      data,
                     GError      **error)
{
  FlatpakContext *context = data;
  g_autofree char *name = NULL;
  g_autofree char *condition = NULL;
  FlatpakContextSockets socket;

  if (!parse_if_option (option_name, value, &name, &condition, error))
    return FALSE;

  socket = flatpak_context_socket_from_string (name, error);
  if (socket == 0)
    return FALSE;

  flatpak_permissions_set_allowed_if (context->socket_permissions,
                                      name, condition);
  return TRUE;
}

static gboolean
option_device_cb (const gchar *option_name,
                  const gchar *value,
                  gpointer     data,
                  GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextDevices device;

  device = flatpak_context_device_from_string (value, error);
  if (device == 0)
    return FALSE;

  flatpak_permissions_set_allowed (context->device_permissions,
                                   value);

  return TRUE;
}

static gboolean
option_nodevice_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextDevices device;

  device = flatpak_context_device_from_string (value, error);
  if (device == 0)
    return FALSE;

  flatpak_permissions_set_not_allowed (context->device_permissions,
                                       value);

  return TRUE;
}

static gboolean
option_device_if_cb (const gchar  *option_name,
                     const gchar  *value,
                     gpointer      data,
                     GError      **error)
{
  FlatpakContext *context = data;
  g_autofree char *name = NULL;
  g_autofree char *condition = NULL;
  FlatpakContextDevices device;

  if (!parse_if_option (option_name, value, &name, &condition, error))
    return FALSE;

  device = flatpak_context_device_from_string (name, error);
  if (device == 0)
    return FALSE;

  flatpak_permissions_set_allowed_if (context->device_permissions,
                                      name, condition);
  return TRUE;
}

static gboolean
option_allow_cb (const gchar *option_name,
                 const gchar *value,
                 gpointer     data,
                 GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextFeatures feature;

  feature = flatpak_context_feature_from_string (value, error);
  if (feature == 0)
    return FALSE;

  flatpak_context_add_features (context, feature);

  return TRUE;
}

static gboolean
option_disallow_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextFeatures feature;

  feature = flatpak_context_feature_from_string (value, error);
  if (feature == 0)
    return FALSE;

  flatpak_context_remove_features (context, feature);

  return TRUE;
}

static gboolean
option_filesystem_cb (const gchar *option_name,
                      const gchar *value,
                      gpointer     data,
                      GError     **error)
{
  FlatpakContext *context = data;
  g_autofree char *fs = NULL;
  FlatpakFilesystemMode mode;

  if (!flatpak_context_parse_filesystem (value, FALSE, &fs, &mode, error))
    return FALSE;

  flatpak_context_take_filesystem (context, g_steal_pointer (&fs), mode);
  return TRUE;
}

static gboolean
option_nofilesystem_cb (const gchar *option_name,
                        const gchar *value,
                        gpointer     data,
                        GError     **error)
{
  FlatpakContext *context = data;
  g_autofree char *fs = NULL;
  FlatpakFilesystemMode mode;

  if (!flatpak_context_parse_filesystem (value, TRUE, &fs, &mode, error))
    return FALSE;

  flatpak_context_take_filesystem (context, g_steal_pointer (&fs),
                                   FLATPAK_FILESYSTEM_MODE_NONE);
  return TRUE;
}

static gboolean
option_env_cb (const gchar *option_name,
               const gchar *value,
               gpointer     data,
               GError     **error)
{
  FlatpakContext *context = data;
  g_auto(GStrv) split = g_strsplit (value, "=", 2);

  if (split == NULL || split[0] == NULL || split[0][0] == 0 || split[1] == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Invalid env format %s"), value);
      return FALSE;
    }

  flatpak_context_set_env_var (context, split[0], split[1]);
  return TRUE;
}

gboolean
flatpak_context_parse_env_block (FlatpakContext *context,
                                 const char *data,
                                 gsize length,
                                 GError **error)
{
  g_auto(GStrv) env_vars = NULL;
  int i;

  env_vars = flatpak_parse_env_block (data, length, error);
  if (env_vars == NULL)
    return FALSE;

  for (i = 0; env_vars[i] != NULL; i++)
    {
      g_auto(GStrv) split = g_strsplit (env_vars[i], "=", 2);

      g_assert (g_strv_length (split) == 2);
      g_assert (split[0][0] != '\0');

      flatpak_context_set_env_var (context,
                                   split[0], split[1]);
    }

  return TRUE;
}

gboolean
flatpak_context_parse_env_fd (FlatpakContext *context,
                              int fd,
                              GError **error)
{
  g_autoptr(GBytes) env_block = NULL;
  const char *data;
  gsize len;

  env_block = glnx_fd_readall_bytes (fd, NULL, error);

  if (env_block == NULL)
    return FALSE;

  data = g_bytes_get_data (env_block, &len);
  return flatpak_context_parse_env_block (context, data, len, error);
}

static gboolean
option_env_fd_cb (const gchar *option_name,
                  const gchar *value,
                  gpointer     data,
                  GError     **error)
{
  FlatpakContext *context = data;
  guint64 fd;
  gchar *endptr;
  gboolean ret;

  fd = g_ascii_strtoull (value, &endptr, 10);

  if (endptr == NULL || *endptr != '\0' || fd > G_MAXINT)
    return glnx_throw (error, "Not a valid file descriptor: %s", value);

  ret = flatpak_context_parse_env_fd (context, (int) fd, error);

  if (fd >= 3)
    close (fd);

  return ret;
}

static gboolean
option_unset_env_cb (const gchar *option_name,
                     const gchar *value,
                     gpointer     data,
                     GError     **error)
{
  FlatpakContext *context = data;

  if (strchr (value, '=') != NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Environment variable name must not contain '=': %s"), value);
      return FALSE;
    }

  flatpak_context_set_env_var (context, value, NULL);
  return TRUE;
}

static gboolean
option_own_name_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_session_bus_policy (context, value, FLATPAK_POLICY_OWN);
  return TRUE;
}

static gboolean
option_a11y_own_name_cb (const gchar  *option_name,
                         const gchar  *value,
                         gpointer      data,
                         GError      **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_a11y_bus_policy (context, value, FLATPAK_POLICY_OWN);
  return TRUE;
}

static gboolean
option_talk_name_cb (const gchar *option_name,
                     const gchar *value,
                     gpointer     data,
                     GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_session_bus_policy (context, value, FLATPAK_POLICY_TALK);
  return TRUE;
}

static gboolean
option_no_talk_name_cb (const gchar *option_name,
                        const gchar *value,
                        gpointer     data,
                        GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_session_bus_policy (context, value, FLATPAK_POLICY_NONE);
  return TRUE;
}

static gboolean
option_system_own_name_cb (const gchar *option_name,
                           const gchar *value,
                           gpointer     data,
                           GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_system_bus_policy (context, value, FLATPAK_POLICY_OWN);
  return TRUE;
}

static gboolean
option_system_talk_name_cb (const gchar *option_name,
                            const gchar *value,
                            gpointer     data,
                            GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_system_bus_policy (context, value, FLATPAK_POLICY_TALK);
  return TRUE;
}

static gboolean
option_system_no_talk_name_cb (const gchar *option_name,
                               const gchar *value,
                               gpointer     data,
                               GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_system_bus_policy (context, value, FLATPAK_POLICY_NONE);
  return TRUE;
}

static gboolean
option_add_generic_policy_cb (const gchar *option_name,
                              const gchar *value,
                              gpointer     data,
                              GError     **error)
{
  FlatpakContext *context = data;
  char *t;
  g_autofree char *key = NULL;
  const char *policy_value;

  t = strchr (value, '=');
  if (t == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("--add-policy arguments must be in the form SUBSYSTEM.KEY=VALUE"));
      return FALSE;
    }
  policy_value = t + 1;
  key = g_strndup (value, t - value);
  if (strchr (key, '.') == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("--add-policy arguments must be in the form SUBSYSTEM.KEY=VALUE"));
      return FALSE;
    }

  if (policy_value[0] == '!')
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("--add-policy values can't start with \"!\""));
      return FALSE;
    }

  flatpak_context_apply_generic_policy (context, key, policy_value);

  return TRUE;
}

static gboolean
option_remove_generic_policy_cb (const gchar *option_name,
                                 const gchar *value,
                                 gpointer     data,
                                 GError     **error)
{
  FlatpakContext *context = data;
  char *t;
  g_autofree char *key = NULL;
  const char *policy_value;
  g_autofree char *extended_value = NULL;

  t = strchr (value, '=');
  if (t == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("--remove-policy arguments must be in the form SUBSYSTEM.KEY=VALUE"));
      return FALSE;
    }
  policy_value = t + 1;
  key = g_strndup (value, t - value);
  if (strchr (key, '.') == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("--remove-policy arguments must be in the form SUBSYSTEM.KEY=VALUE"));
      return FALSE;
    }

  if (policy_value[0] == '!')
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("--remove-policy values can't start with \"!\""));
      return FALSE;
    }

  extended_value = g_strconcat ("!", policy_value, NULL);

  flatpak_context_apply_generic_policy (context, key, extended_value);

  return TRUE;
}

static gboolean
option_usb_cb (const char  *option_name,
               const char  *value,
               gpointer     data,
               GError     **error)
{
  g_autoptr(FlatpakUsbQuery) usb_query = NULL;
  FlatpakContext *context = data;

  if (!flatpak_usb_parse_usb (value, &usb_query, error))
    return FALSE;

  flatpak_context_add_usb_query (context, usb_query);
  return TRUE;
}

static gboolean
option_nousb_cb (const char  *option_name,
		 const char  *value,
		 gpointer     data,
		 GError     **error)
{
  g_autoptr(FlatpakUsbQuery) usb_query = NULL;
  FlatpakContext *context = data;

  if (!flatpak_usb_parse_usb (value, &usb_query, error))
    return FALSE;

  flatpak_context_add_nousb_query (context, usb_query);
  return TRUE;
}

static gboolean
option_usb_list_file_cb (const char  *option_name,
                         const char  *value,
                         gpointer     data,
                         GError     **error)
{
  return flatpak_context_add_usb_list_from_file (data, value, error);
}

static gboolean
option_usb_list_cb (const char  *option_name,
                    const char  *value,
                    gpointer     data,
                    GError     **error)
{
  return flatpak_context_add_usb_list (data, value, error);
}

static gboolean
option_persist_cb (const char *option_name,
                   const char *value,
                   gpointer     data,
                   GError     **error)
{
  FlatpakContext *context = data;

  return flatpak_context_set_persistent (context, value, error);
}

static gboolean option_no_desktop_deprecated;

static GOptionEntry context_options[] = {
  { "share", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_share_cb, N_("Share with host"), N_("SHARE") },
  { "unshare", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_unshare_cb, N_("Unshare with host"), N_("SHARE") },
  { "socket", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_socket_cb, N_("Expose socket to app"), N_("SOCKET") },
  { "nosocket", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nosocket_cb, N_("Don't expose socket to app"), N_("SOCKET") },
  { "socket-if", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_socket_if_cb, N_("Require conditions to be met for a socket to get exposed"), N_("DEVICE:CONDITION") },
  { "device", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_device_cb, N_("Expose device to app"), N_("DEVICE") },
  { "nodevice", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nodevice_cb, N_("Don't expose device to app"), N_("DEVICE") },
  { "device-if", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_device_if_cb, N_("Require conditions to be met for a device to get exposed"), N_("DEVICE:CONDITION") },
  { "allow", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_allow_cb, N_("Allow feature"), N_("FEATURE") },
  { "disallow", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_disallow_cb, N_("Don't allow feature"), N_("FEATURE") },
  { "filesystem", 0, G_OPTION_FLAG_IN_MAIN | G_OPTION_FLAG_FILENAME, G_OPTION_ARG_CALLBACK, &option_filesystem_cb, N_("Expose filesystem to app (:ro for read-only)"), N_("FILESYSTEM[:ro]") },
  { "nofilesystem", 0, G_OPTION_FLAG_IN_MAIN | G_OPTION_FLAG_FILENAME, G_OPTION_ARG_CALLBACK, &option_nofilesystem_cb, N_("Don't expose filesystem to app"), N_("FILESYSTEM") },
  { "env", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_env_cb, N_("Set environment variable"), N_("VAR=VALUE") },
  { "env-fd", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_env_fd_cb, N_("Read environment variables in env -0 format from FD"), N_("FD") },
  { "unset-env", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_unset_env_cb, N_("Remove variable from environment"), N_("VAR") },
  { "own-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_own_name_cb, N_("Allow app to own name on the session bus"), N_("DBUS_NAME") },
  { "talk-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_talk_name_cb, N_("Allow app to talk to name on the session bus"), N_("DBUS_NAME") },
  { "no-talk-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_no_talk_name_cb, N_("Don't allow app to talk to name on the session bus"), N_("DBUS_NAME") },
  { "system-own-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_system_own_name_cb, N_("Allow app to own name on the system bus"), N_("DBUS_NAME") },
  { "system-talk-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_system_talk_name_cb, N_("Allow app to talk to name on the system bus"), N_("DBUS_NAME") },
  { "system-no-talk-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_system_no_talk_name_cb, N_("Don't allow app to talk to name on the system bus"), N_("DBUS_NAME") },
  { "a11y-own-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_a11y_own_name_cb, N_("Allow app to own name on the a11y bus"), N_("DBUS_NAME") },
  { "add-policy", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_add_generic_policy_cb, N_("Add generic policy option"), N_("SUBSYSTEM.KEY=VALUE") },
  { "remove-policy", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_remove_generic_policy_cb, N_("Remove generic policy option"), N_("SUBSYSTEM.KEY=VALUE") },
  { "usb", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_usb_cb, N_("Add USB device to enumerables"), N_("VENDOR_ID:PRODUCT_ID") },
  { "nousb", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nousb_cb, N_("Add USB device to hidden list"), N_("VENDOR_ID:PRODUCT_ID") },
  { "usb-list", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_usb_list_cb, N_("A list of USB devices that are enumerable"), N_("LIST") },
  { "usb-list-file", 0, G_OPTION_FLAG_IN_MAIN | G_OPTION_FLAG_FILENAME, G_OPTION_ARG_CALLBACK, &option_usb_list_file_cb, N_("File containing a list of USB devices to make enumerable"), N_("FILENAME") },
  { "persist", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_persist_cb, N_("Persist home directory subpath"), N_("FILENAME") },
  /* This is not needed/used anymore, so hidden, but we accept it for backwards compat */
  { "no-desktop", 0, G_OPTION_FLAG_IN_MAIN |  G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &option_no_desktop_deprecated, N_("Don't require a running session (no cgroups creation)"), NULL },
  { NULL }
};

GOptionEntry *
flatpak_context_get_option_entries (void)
{
  return context_options;
}

GOptionGroup  *
flatpak_context_get_options (FlatpakContext *context)
{
  GOptionGroup *group;

  group = g_option_group_new ("environment",
                              "Runtime Environment",
                              "Runtime Environment",
                              context,
                              NULL);
  g_option_group_set_translation_domain (group, GETTEXT_PACKAGE);

  g_option_group_add_entries (group, context_options);

  return group;
}

static const char *
parse_negated (const char *option, gboolean *negated)
{
  if (option[0] == '!')
    {
      option++;
      *negated = TRUE;
    }
  else
    {
      *negated = FALSE;
    }
  return option;
}

static void
flatpak_context_load_share (FlatpakContext *context,
                            const char     *share_str)
{
  FlatpakContextShares share;
  gboolean remove;

  share =
    flatpak_context_share_from_string (parse_negated (share_str, &remove),
                                       NULL);

  if (share == 0)
    {
      g_info ("Unknown share type %s", share_str);
      return;
    }

  if (remove)
    flatpak_context_remove_shares (context, share);
  else
    flatpak_context_add_shares (context, share);
}

static void
flatpak_context_load_feature (FlatpakContext *context,
                              const char     *feature_str)
{
  FlatpakContextFeatures feature;
  gboolean remove;

  feature =
    flatpak_context_feature_from_string (parse_negated (feature_str, &remove),
                                         NULL);

  if (feature == 0)
    {
      g_info ("Unknown feature type %s", feature_str);
      return;
    }

  if (remove)
    flatpak_context_remove_features (context, feature);
  else
    flatpak_context_add_features (context, feature);
}

/*
 * Merge the FLATPAK_METADATA_GROUP_CONTEXT,
 * FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
 * FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY and
 * FLATPAK_METADATA_GROUP_ENVIRONMENT groups, and all groups starting
 * with FLATPAK_METADATA_GROUP_PREFIX_POLICY, from metakey into context.
 *
 * This is a merge, not a replace!
 */
gboolean
flatpak_context_load_metadata (FlatpakContext *context,
                               GKeyFile       *metakey,
                               GError        **error)
{
  gboolean remove;
  g_auto(GStrv) groups = NULL;
  gsize i;

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_SHARED, NULL))
    {
      g_auto(GStrv) shares = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                         FLATPAK_METADATA_KEY_SHARED, NULL, error);
      if (shares == NULL)
        return FALSE;

      for (i = 0; shares[i] != NULL; i++)
        flatpak_context_load_share (context, shares[i]);
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_SOCKETS, NULL))
    {
      g_auto(GStrv) sockets = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                          FLATPAK_METADATA_KEY_SOCKETS, NULL, error);
      if (sockets == NULL)
        return FALSE;

      if (!flatpak_permissions_from_strv (context->socket_permissions, (const char **)sockets, error))
        return FALSE;
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_DEVICES, NULL))
    {
      g_auto(GStrv) devices = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                          FLATPAK_METADATA_KEY_DEVICES, NULL, error);
      if (devices == NULL)
        return FALSE;

      if (!flatpak_permissions_from_strv (context->device_permissions, (const char **)devices, error))
        return FALSE;
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_FEATURES, NULL))
    {
      g_auto(GStrv) features = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                           FLATPAK_METADATA_KEY_FEATURES, NULL, error);
      if (features == NULL)
        return FALSE;

      for (i = 0; features[i] != NULL; i++)
        flatpak_context_load_feature (context, features[i]);
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_FILESYSTEMS, NULL))
    {
      g_auto(GStrv) filesystems = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                              FLATPAK_METADATA_KEY_FILESYSTEMS, NULL, error);
      if (filesystems == NULL)
        return FALSE;

      for (i = 0; filesystems[i] != NULL; i++)
        {
          const char *fs = parse_negated (filesystems[i], &remove);
          g_autofree char *filesystem = NULL;
          g_autoptr(GError) local_error = NULL;
          FlatpakFilesystemMode mode;

          if (!flatpak_context_parse_filesystem (fs, remove,
                                                 &filesystem, &mode, &local_error))
            {
              if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA))
                {
                  /* Invalid characters, so just hard-fail. */
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }
              else
                {
                  g_info ("Unknown filesystem type %s", filesystems[i]);
                  g_clear_error (&local_error);
                }
            }
          else
            {
              g_assert (mode == FLATPAK_FILESYSTEM_MODE_NONE || !remove);
              flatpak_context_take_filesystem (context, g_steal_pointer (&filesystem), mode);
            }
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_PERSISTENT, NULL))
    {
      g_auto(GStrv) persistent = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                             FLATPAK_METADATA_KEY_PERSISTENT, NULL, error);
      if (persistent == NULL)
        return FALSE;

      for (i = 0; persistent[i] != NULL; i++)
        if (!flatpak_context_set_persistent (context, persistent[i], error))
          return FALSE;
    }

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY))
    {
      g_auto(GStrv) keys = NULL;
      gsize keys_count;

      keys = g_key_file_get_keys (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY, key, NULL);
          FlatpakPolicy policy;

          if (!flatpak_verify_dbus_name (key, error))
            return FALSE;

          policy = flatpak_policy_from_string (value, NULL);
          if ((int) policy != -1)
            flatpak_context_set_session_bus_policy (context, key, policy);
        }
    }

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY))
    {
      g_auto(GStrv) keys = NULL;
      gsize keys_count;

      keys = g_key_file_get_keys (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY, key, NULL);
          FlatpakPolicy policy;

          if (!flatpak_verify_dbus_name (key, error))
            return FALSE;

          policy = flatpak_policy_from_string (value, NULL);
          if ((int) policy != -1)
            flatpak_context_set_system_bus_policy (context, key, policy);
        }
    }

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT))
    {
      g_auto(GStrv) keys = NULL;
      gsize keys_count;

      keys = g_key_file_get_keys (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT, key, NULL);

          flatpak_context_set_env_var (context, key, value);
        }
    }

  /* unset-environment is higher precedence than Environment, so that
   * we can put unset keys in both places. Old versions of Flatpak will
   * interpret the empty string as unset; new versions will obey
   * unset-environment. */
  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_UNSET_ENVIRONMENT, NULL))
    {
      g_auto(GStrv) vars = NULL;
      gsize vars_count;

      vars = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                         FLATPAK_METADATA_KEY_UNSET_ENVIRONMENT,
                                         &vars_count, error);

      if (vars == NULL)
        return FALSE;

      for (i = 0; i < vars_count; i++)
        {
          const char *var = vars[i];

          flatpak_context_set_env_var (context, var, NULL);
        }
    }

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      const char *group = groups[i];
      const char *subsystem;
      int j;

      if (g_str_has_prefix (group, FLATPAK_METADATA_GROUP_PREFIX_POLICY))
        {
          g_auto(GStrv) keys = NULL;
          subsystem = group + strlen (FLATPAK_METADATA_GROUP_PREFIX_POLICY);
          keys = g_key_file_get_keys (metakey, group, NULL, NULL);
          for (j = 0; keys != NULL && keys[j] != NULL; j++)
            {
              const char *key = keys[j];
              g_autofree char *policy_key = g_strdup_printf ("%s.%s", subsystem, key);
              g_auto(GStrv) values = NULL;
              int k;

              values = g_key_file_get_string_list (metakey, group, key, NULL, NULL);
              for (k = 0; values != NULL && values[k] != NULL; k++)
                flatpak_context_apply_generic_policy (context, policy_key,
                                                      values[k]);
            }
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_USB_DEVICES, FLATPAK_METADATA_KEY_USB_ENUMERABLE_DEVICES, NULL))
    {
      g_auto(GStrv) values = NULL;
      size_t count;

      values = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_USB_DEVICES,
                                           FLATPAK_METADATA_KEY_USB_ENUMERABLE_DEVICES,
                                           &count, error);

      if (!values)
        return FALSE;

      for (i = 0; i < count; i++)
        {
          g_autoptr(FlatpakUsbQuery) usb_query = NULL;

          if (!flatpak_usb_parse_usb (values[i], &usb_query, error))
            return FALSE;

          flatpak_context_add_usb_query (context, usb_query);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_USB_DEVICES, FLATPAK_METADATA_KEY_USB_HIDDEN_DEVICES, NULL))
    {
      g_auto(GStrv) values = NULL;
      size_t count;

      values = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_USB_DEVICES,
                                           FLATPAK_METADATA_KEY_USB_HIDDEN_DEVICES,
                                           &count, error);

      if (!values)
        return FALSE;

      for (i = 0; i < count; i++)
        {
          g_autoptr(FlatpakUsbQuery) usb_query = NULL;

          if (!flatpak_usb_parse_usb (values[i], &usb_query, error))
            return FALSE;

          flatpak_context_add_nousb_query (context, usb_query);
        }
    }

  return TRUE;
}

static void
flatpak_context_save_usb_devices (GHashTable *devices, GKeyFile *keyfile, const char *key)
{
  GHashTableIter iter;
  gpointer value;

  if (g_hash_table_size (devices) > 0)
    {
      g_autoptr(GPtrArray) usb_devices = g_ptr_array_new ();

      g_hash_table_iter_init (&iter, devices);
      while (g_hash_table_iter_next (&iter, &value, NULL))
        g_ptr_array_add (usb_devices, (char *) value);

      if (usb_devices->len > 0)
        {
          g_key_file_set_string_list (keyfile,
                                      FLATPAK_METADATA_GROUP_USB_DEVICES,
                                      key,
                                      (const char * const *) usb_devices->pdata,
                                      usb_devices->len);
        }
    }
}

/*
 * Save the FLATPAK_METADATA_GROUP_CONTEXT,
 * FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
 * FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY and
 * FLATPAK_METADATA_GROUP_ENVIRONMENT groups, and all groups starting
 * with FLATPAK_METADATA_GROUP_PREFIX_POLICY, into metakey
 */
void
flatpak_context_save_metadata (FlatpakContext *context,
                               gboolean        flatten,
                               GKeyFile       *metakey)
{
  g_auto(GStrv) shared = NULL;
  g_auto(GStrv) sockets = NULL;
  g_auto(GStrv) devices = NULL;
  g_auto(GStrv) features = NULL;
  g_autoptr(GPtrArray) unset_env = NULL;
  GHashTableIter iter;
  gpointer key, value;
  FlatpakContextShares shares_mask = context->shares;
  FlatpakContextShares shares_valid = context->shares_valid;
  FlatpakContextFeatures features_mask = context->features;
  FlatpakContextFeatures features_valid = context->features_valid;
  g_auto(GStrv) groups = NULL;
  int i;

  if (flatten)
    {
      /* A flattened format means we don't expect this to be merged on top of
         another context. In that case we never need to negate any flags.
         We calculate this by removing the zero parts of the mask from the valid set.
       */
      /* First we make sure only the valid parts of the mask are set, in case we
         got some leftover */
      shares_mask &= shares_valid;
      features_mask &= features_valid;

      /* Then just set the valid set to be the mask set */
      shares_valid = shares_mask;
      features_valid = features_mask;
    }

  shared = flatpak_context_shared_to_string (shares_mask, shares_valid);
  sockets = flatpak_permissions_to_strv (context->socket_permissions, flatten);
  devices = flatpak_permissions_to_strv (context->device_permissions, flatten);
  features = flatpak_context_features_to_string (features_mask, features_valid);

  if (shared[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_SHARED,
                                  (const char * const *) shared, g_strv_length (shared));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_SHARED,
                             NULL);
    }

  if (sockets[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_SOCKETS,
                                  (const char * const *) sockets, g_strv_length (sockets));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_SOCKETS,
                             NULL);
    }

  if (devices[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_DEVICES,
                                  (const char * const *) devices, g_strv_length (devices));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_DEVICES,
                             NULL);
    }

  if (features[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_FEATURES,
                                  (const char * const *) features, g_strv_length (features));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_FEATURES,
                             NULL);
    }

  if (g_hash_table_size (context->filesystems) > 0)
    {
      g_autoptr(GPtrArray) array = g_ptr_array_new_with_free_func (g_free);

      /* Serialize host-reset first, because order can matter in
       * corner cases. */
      if (g_hash_table_lookup_extended (context->filesystems, "host-reset",
                                        NULL, &value))
        {
          g_warn_if_fail (GPOINTER_TO_INT (value) == FLATPAK_FILESYSTEM_MODE_NONE);
          if (!flatten)
            g_ptr_array_add (array, g_strdup ("!host:reset"));
        }

      g_hash_table_iter_init (&iter, context->filesystems);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);

          if (flatten && mode == FLATPAK_FILESYSTEM_MODE_NONE)
            continue;

          /* We already did this */
          if (g_str_equal (key, "host-reset"))
            continue;

          g_ptr_array_add (array, unparse_filesystem_flags (key, mode));
        }

      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_FILESYSTEMS,
                                  (const char * const *) array->pdata, array->len);
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_FILESYSTEMS,
                             NULL);
    }

  if (g_hash_table_size (context->persistent) > 0)
    {
      g_autofree char **keys = (char **) g_hash_table_get_keys_as_array (context->persistent, NULL);

      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_PERSISTENT,
                                  (const char * const *) keys, g_strv_length (keys));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_PERSISTENT,
                             NULL);
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY, NULL);
  g_hash_table_iter_init (&iter, context->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      if (flatten && (policy == 0))
        continue;

      g_key_file_set_string (metakey,
                             FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
                             (char *) key, flatpak_policy_to_string (policy));
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY, NULL);
  g_hash_table_iter_init (&iter, context->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      if (flatten && (policy == 0))
        continue;

      g_key_file_set_string (metakey,
                             FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY,
                             (char *) key, flatpak_policy_to_string (policy));
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_A11Y_BUS_POLICY, NULL);
  g_hash_table_iter_init (&iter, context->a11y_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      if (flatten && (policy == 0))
        continue;

      g_key_file_set_string (metakey,
                             FLATPAK_METADATA_GROUP_A11Y_BUS_POLICY,
                             (char *) key, flatpak_policy_to_string (policy));
    }

  /* Elements are borrowed from context->env_vars */
  unset_env = g_ptr_array_new ();

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT, NULL);
  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (value != NULL)
        {
          g_key_file_set_string (metakey,
                                 FLATPAK_METADATA_GROUP_ENVIRONMENT,
                                 (char *) key, (char *) value);
        }
      else
        {
          /* In older versions of Flatpak, [Environment] FOO=
           * was interpreted as unsetting - so let's do both. */
          g_key_file_set_string (metakey,
                                 FLATPAK_METADATA_GROUP_ENVIRONMENT,
                                 (char *) key, "");
          g_ptr_array_add (unset_env, key);
        }
    }

  if (unset_env->len > 0)
    {
      g_ptr_array_add (unset_env, NULL);
      g_key_file_set_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_UNSET_ENVIRONMENT,
                                  (const char * const *) unset_env->pdata,
                                  unset_env->len - 1);
    }
  else
    {
      g_key_file_remove_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_UNSET_ENVIRONMENT, NULL);
    }

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      const char *group = groups[i];
      if (g_str_has_prefix (group, FLATPAK_METADATA_GROUP_PREFIX_POLICY))
        g_key_file_remove_group (metakey, group, NULL);
    }

  g_hash_table_iter_init (&iter, context->generic_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_auto(GStrv) parts = g_strsplit ((const char *) key, ".", 2);
      g_autofree char *group = NULL;
      g_assert (parts[1] != NULL);
      const char **policy_values = (const char **) value;
      g_autoptr(GPtrArray) new = g_ptr_array_new ();

      for (i = 0; policy_values[i] != NULL; i++)
        {
          const char *policy_value = policy_values[i];

          if (!flatten || policy_value[0] != '!')
            g_ptr_array_add (new, (char *) policy_value);
        }

      if (new->len > 0)
        {
          group = g_strconcat (FLATPAK_METADATA_GROUP_PREFIX_POLICY,
                               parts[0], NULL);
          g_key_file_set_string_list (metakey, group, parts[1],
                                      (const char * const *) new->pdata,
                                      new->len);
        }
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_USB_DEVICES, NULL);
  flatpak_context_save_usb_devices (context->enumerable_usb_devices, metakey,
                                    FLATPAK_METADATA_KEY_USB_ENUMERABLE_DEVICES);
  flatpak_context_save_usb_devices (context->hidden_usb_devices, metakey,
                                    FLATPAK_METADATA_KEY_USB_HIDDEN_DEVICES);
}

void
flatpak_context_allow_host_fs (FlatpakContext *context)
{
  flatpak_context_take_filesystem (context, g_strdup ("host"),
                                   FLATPAK_FILESYSTEM_MODE_READ_WRITE);
}

gboolean
flatpak_context_get_needs_session_bus_proxy (FlatpakContext *context)
{
  return g_hash_table_size (context->session_bus_policy) > 0;
}

gboolean
flatpak_context_get_needs_system_bus_proxy (FlatpakContext *context)
{
  return g_hash_table_size (context->system_bus_policy) > 0;
}

static gboolean
adds_flags (guint32 old_flags, guint32 new_flags)
{
  return (new_flags & ~old_flags) != 0;
}

static gboolean
adds_bus_policy (GHashTable *old, GHashTable *new)
{
  GLNX_HASH_TABLE_FOREACH_KV (new, const char *, name, gpointer, _new_policy)
    {
      int new_policy = GPOINTER_TO_INT (_new_policy);
      int old_policy = GPOINTER_TO_INT (g_hash_table_lookup (old, name));
      if (new_policy > old_policy)
        return TRUE;
    }

  return FALSE;
}

static gboolean
adds_generic_policy (GHashTable *old, GHashTable *new)
{
  GLNX_HASH_TABLE_FOREACH_KV (new, const char *, key, GPtrArray *, new_values)
    {
      GPtrArray *old_values = g_hash_table_lookup (old, key);
      int i;

      if (new_values == NULL || new_values->len == 0)
        continue;

      if (old_values == NULL || old_values->len == 0)
        return TRUE;

      for (i = 0; i < new_values->len; i++)
        {
          const char *new_value = g_ptr_array_index (new_values, i);

          if (!flatpak_g_ptr_array_contains_string (old_values, new_value))
            return TRUE;
        }
    }

  return FALSE;
}

static gboolean
adds_filesystem_access (GHashTable *old, GHashTable *new)
{
  FlatpakFilesystemMode old_host_mode = GPOINTER_TO_INT (g_hash_table_lookup (old, "host"));

  GLNX_HASH_TABLE_FOREACH_KV (new, const char *, location, gpointer, _new_mode)
    {
      FlatpakFilesystemMode new_mode = GPOINTER_TO_INT (_new_mode);
      FlatpakFilesystemMode old_mode = GPOINTER_TO_INT (g_hash_table_lookup (old, location));

      /* Allow more limited access to the same thing */
      if (new_mode <= old_mode)
        continue;

      /* Allow more limited access if we used to have access to everything */
     if (new_mode <= old_host_mode)
        continue;

     /* For the remainder we have to be pessimistic, for instance even
        if we have home access we can't allow adding access to ~/foo,
        because foo might be a symlink outside home which didn't work
        before but would work with an explicit access to that
        particular file. */

      return TRUE;
    }

  return FALSE;
}

static gboolean
adds_usb_device (FlatpakContext *old, FlatpakContext *new)
{
  GHashTableIter iter;
  gpointer value;

  /* Does it add new devices to the allowlist? */
  g_hash_table_iter_init (&iter, new->enumerable_usb_devices);
  while (g_hash_table_iter_next (&iter, &value, NULL))
    {
      if (!g_hash_table_contains (old->enumerable_usb_devices, value))
        return TRUE;
    }

  /* Does it remove devices from the blocklist? */
  g_hash_table_iter_init (&iter, old->hidden_usb_devices);
  while (g_hash_table_iter_next (&iter, &value, NULL))
    {
      if (!g_hash_table_contains (new->hidden_usb_devices, value))
        return TRUE;
    }

  return FALSE;
}

gboolean
flatpak_context_adds_permissions (FlatpakContext *old,
                                  FlatpakContext *new)
{
  g_autoptr(GHashTable) old_socket_permissions = NULL;
  guint32 harmless_features;

  /* We allow upgrade to multiarch, that is really not a huge problem.
   * Similarly, having sensible semantics for /dev/shm is
   * not a security concern. */
  harmless_features = (FLATPAK_CONTEXT_FEATURE_MULTIARCH |
                       FLATPAK_CONTEXT_FEATURE_PER_APP_DEV_SHM);

  if (adds_flags (old->shares & old->shares_valid,
                  new->shares & new->shares_valid))
    return TRUE;

  old_socket_permissions = flatpak_permissions_dup (old->socket_permissions);
  /* If we used to allow X11, also allow new fallback X11,
     as that is actually less permissions */
  if (flatpak_permissions_allows_unconditionally (old_socket_permissions, "x11"))
    flatpak_permissions_set_allowed (old_socket_permissions, "fallback-x11");

  if (flatpak_permissions_adds_permissions (old_socket_permissions,
                                            new->socket_permissions))
      return TRUE;

  if (flatpak_permissions_adds_permissions (old->device_permissions,
                                            new->device_permissions))
    return TRUE;

  if (adds_flags ((old->features & old->features_valid) | harmless_features,
                  new->features & new->features_valid))
    return TRUE;

  if (adds_bus_policy (old->session_bus_policy, new->session_bus_policy))
    return TRUE;

  if (adds_bus_policy (old->system_bus_policy, new->system_bus_policy))
    return TRUE;

  if (adds_bus_policy (old->a11y_bus_policy, new->a11y_bus_policy))
    return TRUE;

  if (adds_generic_policy (old->generic_policy, new->generic_policy))
    return TRUE;

  if (adds_filesystem_access (old->filesystems, new->filesystems))
    return TRUE;

  if (adds_usb_device (old, new))
    return TRUE;

  return FALSE;
}

gboolean
flatpak_context_allows_features (FlatpakContext        *context,
                                 FlatpakContextFeatures features)
{
  return (context->features & features) == features;
}

char *
flatpak_context_devices_to_usb_list (GHashTable *devices,
                                     gboolean    hidden)
{
  GString *list = g_string_new (NULL);
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, devices);
  while (g_hash_table_iter_next (&iter, &value, NULL))
    {
      if (hidden)
        g_string_append_printf (list, "!%s;", (const char *) value);
      else
        g_string_append_printf (list, "%s;", (const char *) value);
    }

  return g_string_free (list, FALSE);
}

void
flatpak_context_to_args (FlatpakContext *context,
                         GPtrArray      *args)
{
  GHashTableIter iter;
  gpointer key, value;
  char *usb_list = NULL;

  flatpak_context_shared_to_args (context, args);
  flatpak_context_features_to_args (context, args);

  flatpak_permissions_to_args (context->device_permissions, "device", args);
  flatpak_permissions_to_args (context->socket_permissions, "socket", args);

  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (value != NULL)
        g_ptr_array_add (args, g_strdup_printf ("--env=%s=%s", (char *) key, (char *) value));
      else
        g_ptr_array_add (args, g_strdup_printf ("--unset-env=%s", (char *) key));
    }

  g_hash_table_iter_init (&iter, context->persistent);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_ptr_array_add (args, g_strdup_printf ("--persist=%s", (char *) key));

  g_hash_table_iter_init (&iter, context->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key;
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      g_ptr_array_add (args, g_strdup_printf ("--%s-name=%s", flatpak_policy_to_string (policy), name));
    }

  g_hash_table_iter_init (&iter, context->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key;
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      g_ptr_array_add (args, g_strdup_printf ("--system-%s-name=%s", flatpak_policy_to_string (policy), name));
    }

  /* Serialize host-reset first, because order can matter in
   * corner cases. */
  if (g_hash_table_lookup_extended (context->filesystems, "host-reset",
                                    NULL, &value))
    {
      g_warn_if_fail (GPOINTER_TO_INT (value) == FLATPAK_FILESYSTEM_MODE_NONE);
      g_ptr_array_add (args, g_strdup ("--nofilesystem=host:reset"));
    }

  g_hash_table_iter_init (&iter, context->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_autofree char *fs = NULL;
      FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);

      /* We already did this */
      if (g_str_equal (key, "host-reset"))
        continue;

      fs = unparse_filesystem_flags (key, mode);

      if (mode != FLATPAK_FILESYSTEM_MODE_NONE)
        {
          g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s", fs));
        }
      else
        {
          g_assert (fs[0] == '!');
          g_ptr_array_add (args, g_strdup_printf ("--nofilesystem=%s", &fs[1]));
        }
    }

  usb_list = flatpak_context_devices_to_usb_list (context->enumerable_usb_devices, FALSE);
  g_ptr_array_add (args, g_strdup_printf ("--usb-list=%s", usb_list));
  g_free (usb_list);

  usb_list = flatpak_context_devices_to_usb_list (context->hidden_usb_devices, TRUE);
  g_ptr_array_add (args, g_strdup_printf ("--usb-list=%s", usb_list));
  g_free (usb_list);
}

void
flatpak_context_add_bus_filters (FlatpakContext *context,
                                 const char     *app_id,
                                 FlatpakBus      bus,
                                 gboolean        sandboxed,
                                 FlatpakBwrap   *bwrap)
{
  GHashTable *ht;
  GHashTableIter iter;
  gpointer key, value;

  flatpak_bwrap_add_arg (bwrap, "--filter");

  switch (bus)
    {
    case FLATPAK_SESSION_BUS:
      if (app_id)
        {
          if (!sandboxed)
            {
              flatpak_bwrap_add_arg_printf (bwrap, "--own=%s.*", app_id);
              flatpak_bwrap_add_arg_printf (bwrap, "--own=org.mpris.MediaPlayer2.%s.*", app_id);
            }
          else
            {
              flatpak_bwrap_add_arg_printf (bwrap, "--own=%s.Sandboxed.*", app_id);
              flatpak_bwrap_add_arg_printf (bwrap, "--own=org.mpris.MediaPlayer2.%s.Sandboxed.*", app_id);
            }
        }
      ht = context->session_bus_policy;
      break;

    case FLATPAK_SYSTEM_BUS:
      ht = context->system_bus_policy;
      break;

    case FLATPAK_A11Y_BUS:
      ht = context->a11y_bus_policy;
      break;

    default:
      g_assert_not_reached ();
   }

  g_hash_table_iter_init (&iter, ht);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      if (policy > 0)
        flatpak_bwrap_add_arg_printf (bwrap, "--%s=%s",
                                      flatpak_policy_to_string (policy),
                                      (char *) key);
    }
}

void
flatpak_context_reset_non_permissions (FlatpakContext *context)
{
  g_hash_table_remove_all (context->env_vars);
}

void
flatpak_context_reset_permissions (FlatpakContext *context)
{
  context->shares_valid = 0;
  context->features_valid = 0;

  context->shares = 0;
  context->features = 0;

  g_hash_table_remove_all (context->socket_permissions);
  g_hash_table_remove_all (context->device_permissions);
  g_hash_table_remove_all (context->persistent);
  g_hash_table_remove_all (context->filesystems);
  g_hash_table_remove_all (context->session_bus_policy);
  g_hash_table_remove_all (context->system_bus_policy);
  g_hash_table_remove_all (context->a11y_bus_policy);
  g_hash_table_remove_all (context->generic_policy);
}

void
flatpak_context_make_sandboxed (FlatpakContext *context)
{
  /* We drop almost everything from the app permission, except
   * multiarch which is inherited, to make sure app code keeps
   * running. */
  context->shares_valid &= 0;
  context->features_valid &= FLATPAK_CONTEXT_FEATURE_MULTIARCH;

  context->shares &= context->shares_valid;
  context->features &= context->features_valid;

  g_hash_table_remove_all (context->socket_permissions);
  g_hash_table_remove_all (context->device_permissions);

  g_hash_table_remove_all (context->persistent);
  g_hash_table_remove_all (context->filesystems);
  g_hash_table_remove_all (context->session_bus_policy);
  g_hash_table_remove_all (context->system_bus_policy);
  g_hash_table_remove_all (context->a11y_bus_policy);
  g_hash_table_remove_all (context->generic_policy);
}

const char *dont_mount_in_root[] = {
  ".",
  "..",
  "app",
  "bin",
  "boot",
  "dev",
  "efi",
  "etc",
  "lib",
  "lib32",
  "lib64",
  "proc",
  "root",
  "run",
  "sbin",
  "sys",
  "tmp",
  "usr",
  "var",
  NULL
};

static void
log_cannot_export_error (FlatpakFilesystemMode  mode,
                         const char            *path,
                         const GError          *error)
{
  GLogLevelFlags level = G_LOG_LEVEL_MESSAGE;

  /* By default we don't show a log message if the reason we are not sharing
   * something with the sandbox is simply "it doesn't exist" (or something
   * very close): otherwise it would be very noisy to launch apps that
   * opportunistically share things they might benefit from, like Steam
   * having access to $XDG_RUNTIME_DIR/app/com.discordapp.Discord if it
   * happens to exist. */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    level = G_LOG_LEVEL_INFO;
  /* Some callers specifically suppress warnings for particular errors
   * by setting this code. */
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
    level = G_LOG_LEVEL_INFO;

  switch (mode)
    {
      case FLATPAK_FILESYSTEM_MODE_NONE:
        g_log (G_LOG_DOMAIN, level, _("Not replacing \"%s\" with tmpfs: %s"),
               path, error->message);
        break;

      case FLATPAK_FILESYSTEM_MODE_CREATE:
      case FLATPAK_FILESYSTEM_MODE_READ_ONLY:
      case FLATPAK_FILESYSTEM_MODE_READ_WRITE:
        g_log (G_LOG_DOMAIN, level,
               _("Not sharing \"%s\" with sandbox: %s"),
               path, error->message);
        break;
    }
}

static void
flatpak_context_export (FlatpakContext *context,
                        FlatpakExports *exports,
                        GFile          *app_id_dir,
                        GPtrArray       *extra_app_id_dirs,
                        gboolean        do_create,
                        gchar         **xdg_dirs_conf_out,
                        gboolean       *home_access_out)
{
  gboolean home_access = FALSE;
  g_autoptr(GString) xdg_dirs_conf = NULL;
  FlatpakFilesystemMode fs_mode, os_mode, etc_mode, root_mode, home_mode;
  GHashTableIter iter;
  gpointer key, value;
  g_autoptr(GError) local_error = NULL;

  if (xdg_dirs_conf_out != NULL)
    xdg_dirs_conf = g_string_new ("");

  fs_mode = GPOINTER_TO_INT (g_hash_table_lookup (context->filesystems, "host"));
  if (fs_mode != FLATPAK_FILESYSTEM_MODE_NONE)
    {
      DIR *dir;
      struct dirent *dirent;

      g_info ("Allowing host-fs access");
      home_access = TRUE;

      /* Bind mount most dirs in / into the new root */
      dir = opendir ("/");
      if (dir != NULL)
        {
          while ((dirent = readdir (dir)))
            {
              g_autofree char *path = NULL;

              if (g_strv_contains (dont_mount_in_root, dirent->d_name))
                continue;

              path = g_build_filename ("/", dirent->d_name, NULL);

              if (!flatpak_exports_add_path_expose (exports, fs_mode, path, &local_error))
                {
                  /* Failure to share something like /lib32 because it's
                   * actually a symlink to /usr/lib32 is less of a problem
                   * here than it would be for an explicit
                   * --filesystem=/lib32, so the warning that would normally
                   * be produced in that situation is downgraded to a
                   * debug message. */
                  if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTABLE_FILE))
                    local_error->code = G_IO_ERROR_FAILED_HANDLED;

                  log_cannot_export_error (fs_mode, path, local_error);
                  g_clear_error (&local_error);
                }
            }
          closedir (dir);
        }

      if (!flatpak_exports_add_path_expose (exports, fs_mode, "/run/media", &local_error))
        {
          log_cannot_export_error (fs_mode, "/run/media", local_error);
          g_clear_error (&local_error);
        }
    }

  os_mode = MAX (GPOINTER_TO_INT (g_hash_table_lookup (context->filesystems, "host-os")),
                 fs_mode);

  if (os_mode != FLATPAK_FILESYSTEM_MODE_NONE)
    flatpak_exports_add_host_os_expose (exports, os_mode);

  etc_mode = MAX (GPOINTER_TO_INT (g_hash_table_lookup (context->filesystems, "host-etc")),
                  fs_mode);

  if (etc_mode != FLATPAK_FILESYSTEM_MODE_NONE)
    flatpak_exports_add_host_etc_expose (exports, etc_mode);

  root_mode = MAX (GPOINTER_TO_INT (g_hash_table_lookup (context->filesystems, "host-root")),
                   fs_mode);

  if (root_mode != FLATPAK_FILESYSTEM_MODE_NONE)
    flatpak_exports_add_host_root_expose (exports, root_mode);

  home_mode = GPOINTER_TO_INT (g_hash_table_lookup (context->filesystems, "home"));
  if (home_mode != FLATPAK_FILESYSTEM_MODE_NONE)
    {
      g_info ("Allowing homedir access");
      home_access = TRUE;

      if (!flatpak_exports_add_path_expose (exports, MAX (home_mode, fs_mode), g_get_home_dir (), &local_error))
        {
          /* Even if the error is one that we would normally silence, like
           * the path not existing, it seems reasonable to make more of a fuss
           * about the home directory not existing or otherwise being unusable,
           * so this is intentionally not using cannot_export() */
          g_warning (_("Not allowing home directory access: %s"),
                     local_error->message);
          g_clear_error (&local_error);
        }
    }

  g_hash_table_iter_init (&iter, context->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *filesystem = key;
      FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);

      if (g_strv_contains (flatpak_context_special_filesystems, filesystem))
        continue;

      if (g_str_has_prefix (filesystem, "xdg-"))
        {
          g_autofree char *path = NULL;
          const char *rest = NULL;
          const char *config_key = NULL;
          g_autofree char *subpath = NULL;

          if (!get_xdg_user_dir_from_string (filesystem, &config_key, &rest, &path))
            {
              g_warning ("Unsupported xdg dir %s", filesystem);
              continue;
            }

          if (path == NULL)
            continue; /* Unconfigured, ignore */

          if (strcmp (path, g_get_home_dir ()) == 0)
            {
              /* xdg-user-dirs sets disabled dirs to $HOME, and its in general not a good
                 idea to set full access to $HOME other than explicitly, so we ignore
                 these */
              g_info ("Xdg dir %s is $HOME (i.e. disabled), ignoring", filesystem);
              continue;
            }

          subpath = g_build_filename (path, rest, NULL);

          if (mode == FLATPAK_FILESYSTEM_MODE_CREATE && do_create)
            {
              if (g_mkdir_with_parents (subpath, 0755) != 0)
                g_info ("Unable to create directory %s", subpath);
            }

          if (g_file_test (subpath, G_FILE_TEST_EXISTS))
            {
              if (config_key && xdg_dirs_conf)
                g_string_append_printf (xdg_dirs_conf, "%s=\"%s\"\n",
                                        config_key, path);

              if (!flatpak_exports_add_path_expose_or_hide (exports, mode, subpath, &local_error))
                {
                  log_cannot_export_error (mode, subpath, local_error);
                  g_clear_error (&local_error);
                }
            }
        }
      else if (g_str_has_prefix (filesystem, "~/"))
        {
          g_autofree char *path = NULL;

          path = g_build_filename (g_get_home_dir (), filesystem + 2, NULL);

          if (mode == FLATPAK_FILESYSTEM_MODE_CREATE && do_create)
            {
              if (g_mkdir_with_parents (path, 0755) != 0)
                g_info ("Unable to create directory %s", path);
            }

          if (!flatpak_exports_add_path_expose_or_hide (exports, mode, path, &local_error))
            {
              log_cannot_export_error (mode, path, local_error);
              g_clear_error (&local_error);
            }
        }
      else if (g_str_has_prefix (filesystem, "/"))
        {
          if (mode == FLATPAK_FILESYSTEM_MODE_CREATE && do_create)
            {
              if (g_mkdir_with_parents (filesystem, 0755) != 0)
                g_info ("Unable to create directory %s", filesystem);
            }

          if (!flatpak_exports_add_path_expose_or_hide (exports, mode, filesystem, &local_error))
            {
              log_cannot_export_error (mode, filesystem, local_error);
              g_clear_error (&local_error);
            }
        }
      else
        {
          g_warning ("Unexpected filesystem arg %s", filesystem);
        }
    }

  if (app_id_dir)
    {
      g_autoptr(GFile) apps_dir = g_file_get_parent (app_id_dir);
      int i;
      /* Hide the .var/app dir by default (unless explicitly made visible) */
      if (!flatpak_exports_add_path_tmpfs (exports,
                                           flatpak_file_get_path_cached (apps_dir),
                                           &local_error))
        {
          log_cannot_export_error (FLATPAK_FILESYSTEM_MODE_NONE,
                                   flatpak_file_get_path_cached (apps_dir),
                                   local_error);
          g_clear_error (&local_error);
        }

      /* But let the app write to the per-app dir in it */
      if (!flatpak_exports_add_path_expose (exports, FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                            flatpak_file_get_path_cached (app_id_dir),
                                            &local_error))
        {
          log_cannot_export_error (FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                   flatpak_file_get_path_cached (apps_dir),
                                   local_error);
          g_clear_error (&local_error);
        }

      if (extra_app_id_dirs != NULL)
        {
          for (i = 0; i < extra_app_id_dirs->len; i++)
            {
              GFile *extra_app_id_dir = g_ptr_array_index (extra_app_id_dirs, i);
              if (!flatpak_exports_add_path_expose (exports,
                                                    FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                                    flatpak_file_get_path_cached (extra_app_id_dir),
                                                    &local_error))
                {
                  log_cannot_export_error (FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                           flatpak_file_get_path_cached (extra_app_id_dir),
                                           local_error);
                  g_clear_error (&local_error);
                }
            }
        }
    }

  if (home_access_out != NULL)
    *home_access_out = home_access;

  if (xdg_dirs_conf_out != NULL)
    {
      g_assert (xdg_dirs_conf != NULL);
      *xdg_dirs_conf_out = g_string_free (g_steal_pointer (&xdg_dirs_conf), FALSE);
    }
}

GFile *
flatpak_get_data_dir (const char *app_id)
{
  g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
  g_autoptr(GFile) var_app = g_file_resolve_relative_path (home, ".var/app");

  return g_file_get_child (var_app, app_id);
}

FlatpakExports *
flatpak_context_get_exports (FlatpakContext *context,
                             const char     *app_id)
{
  g_autoptr(GFile) app_id_dir = flatpak_get_data_dir (app_id);

  return flatpak_context_get_exports_full (context,
                                           app_id_dir, NULL,
                                           FALSE, FALSE, NULL, NULL);
}

FlatpakRunFlags
flatpak_context_get_run_flags (FlatpakContext *context)
{
  FlatpakRunFlags flags = 0;

  if (flatpak_context_allows_features (context, FLATPAK_CONTEXT_FEATURE_DEVEL))
    flags |= FLATPAK_RUN_FLAG_DEVEL;

  if (flatpak_context_allows_features (context, FLATPAK_CONTEXT_FEATURE_MULTIARCH))
    flags |= FLATPAK_RUN_FLAG_MULTIARCH;

  if (flatpak_context_allows_features (context, FLATPAK_CONTEXT_FEATURE_BLUETOOTH))
    flags |= FLATPAK_RUN_FLAG_BLUETOOTH;

  if (flatpak_context_allows_features (context, FLATPAK_CONTEXT_FEATURE_CANBUS))
    flags |= FLATPAK_RUN_FLAG_CANBUS;

  return flags;
}

FlatpakExports *
flatpak_context_get_exports_full (FlatpakContext *context,
                                  GFile          *app_id_dir,
                                  GPtrArray      *extra_app_id_dirs,
                                  gboolean        do_create,
                                  gboolean        include_default_dirs,
                                  gchar         **xdg_dirs_conf_out,
                                  gboolean       *home_access_out)
{
  g_autoptr(FlatpakExports) exports = flatpak_exports_new ();

  flatpak_context_export (context, exports,
                          app_id_dir, extra_app_id_dirs,
                          do_create, xdg_dirs_conf_out, home_access_out);

  if (include_default_dirs)
    {
      g_autoptr(GFile) user_flatpak_dir = NULL;
      g_autoptr(GError) local_error = NULL;

      /* Hide the flatpak dir by default (unless explicitly made visible) */
      user_flatpak_dir = flatpak_get_user_base_dir_location ();
      if (!flatpak_exports_add_path_tmpfs (exports,
                                           flatpak_file_get_path_cached (user_flatpak_dir),
                                           &local_error))
        {
          log_cannot_export_error (FLATPAK_FILESYSTEM_MODE_NONE,
                                   flatpak_file_get_path_cached (user_flatpak_dir),
                                   local_error);
          g_clear_error (&local_error);
        }

      /* Ensure we always have a homedir */
      if (!flatpak_exports_add_path_dir (exports, g_get_home_dir (), &local_error))
        {
          g_warning (_("Unable to provide a temporary home directory in the sandbox: %s"),
                     local_error->message);
          g_clear_error (&local_error);
        }
    }

  return g_steal_pointer (&exports);
}

static void
flatpak_context_apply_env_appid (FlatpakBwrap *bwrap,
                                 GFile        *app_dir)
{
  g_autoptr(GFile) app_dir_data = NULL;
  g_autoptr(GFile) app_dir_config = NULL;
  g_autoptr(GFile) app_dir_cache = NULL;
  g_autoptr(GFile) app_dir_state = NULL;

  app_dir_data = g_file_get_child (app_dir, "data");
  app_dir_config = g_file_get_child (app_dir, "config");
  app_dir_cache = g_file_get_child (app_dir, "cache");
  /* Yes, this is inconsistent with data, config and cache. However, using
   * this path lets apps provide backwards-compatibility with older Flatpak
   * versions by using `--persist=.local/state --unset-env=XDG_STATE_DIR`. */
  app_dir_state = g_file_get_child (app_dir, ".local/state");
  flatpak_bwrap_set_env (bwrap, "XDG_DATA_HOME", flatpak_file_get_path_cached (app_dir_data), TRUE);
  flatpak_bwrap_set_env (bwrap, "XDG_CONFIG_HOME", flatpak_file_get_path_cached (app_dir_config), TRUE);
  flatpak_bwrap_set_env (bwrap, "XDG_CACHE_HOME", flatpak_file_get_path_cached (app_dir_cache), TRUE);
  flatpak_bwrap_set_env (bwrap, "XDG_STATE_HOME", flatpak_file_get_path_cached (app_dir_state), TRUE);

  if (g_getenv ("XDG_DATA_HOME"))
    flatpak_bwrap_set_env (bwrap, "HOST_XDG_DATA_HOME", g_getenv ("XDG_DATA_HOME"), TRUE);
  if (g_getenv ("XDG_CONFIG_HOME"))
    flatpak_bwrap_set_env (bwrap, "HOST_XDG_CONFIG_HOME", g_getenv ("XDG_CONFIG_HOME"), TRUE);
  if (g_getenv ("XDG_CACHE_HOME"))
    flatpak_bwrap_set_env (bwrap, "HOST_XDG_CACHE_HOME", g_getenv ("XDG_CACHE_HOME"), TRUE);
  if (g_getenv ("XDG_STATE_HOME"))
    flatpak_bwrap_set_env (bwrap, "HOST_XDG_STATE_HOME", g_getenv ("XDG_STATE_HOME"), TRUE);
}

/* This creates zero or more directories unders base_fd+basedir, each
 * being guaranteed to either exist and be a directory (no symlinks)
 * or be created as a directory. The last directory is opened
 * and the fd is returned.
 */
static gboolean
mkdir_p_open_nofollow_at (int          base_fd,
                          const char  *basedir,
                          int          mode,
                          const char  *subdir,
                          int         *out_fd,
                          GError     **error)
{
  glnx_autofd int parent_fd = -1;

  if (g_path_is_absolute (subdir))
    {
      const char *skipped_prefix = subdir;

      while (*skipped_prefix == '/')
        skipped_prefix++;

      g_warning ("--persist=\"%s\" is deprecated, treating it as --persist=\"%s\"", subdir, skipped_prefix);
      subdir = skipped_prefix;
    }

  g_autofree char *subdir_dirname = g_path_get_dirname (subdir);

  if (strcmp (subdir_dirname, ".") == 0)
    {
      /* It is ok to open basedir with follow=true */
      if (!glnx_opendirat (base_fd, basedir, TRUE, &parent_fd, error))
        return FALSE;
    }
  else if (strcmp (subdir_dirname, "..") == 0)
    {
      return glnx_throw (error, "'..' not supported in --persist paths");
    }
  else
    {
      if (!mkdir_p_open_nofollow_at (base_fd, basedir, mode,
                                     subdir_dirname, &parent_fd, error))
        return FALSE;
    }

  g_autofree char *subdir_basename = g_path_get_basename (subdir);

  if (strcmp (subdir_basename, ".") == 0)
    {
      *out_fd = glnx_steal_fd (&parent_fd);
      return TRUE;
    }
  else if (strcmp (subdir_basename, "..") == 0)
    {
      return glnx_throw (error, "'..' not supported in --persist paths");
    }

  if (!glnx_shutil_mkdir_p_at (parent_fd, subdir_basename, mode, NULL, error))
    return FALSE;

  int fd = openat (parent_fd, subdir_basename, O_PATH | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW);
  if (fd == -1)
    {
      int saved_errno = errno;
      struct stat stat_buf;

      /* If it's a symbolic link, that could be a user trying to offload
       * large data to another filesystem, but it could equally well be
       * a malicious or compromised app trying to exploit GHSA-7hgv-f2j8-xw87.
       * Produce a clearer error message in this case.
       * Unfortunately the errno we get in this case is ENOTDIR, so we have
       * to ask again to find out whether it's really a symlink. */
      if (saved_errno == ENOTDIR &&
          fstatat (parent_fd, subdir_basename, &stat_buf, AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISLNK (stat_buf.st_mode))
        return glnx_throw (error, "Symbolic link \"%s\" not allowed to avoid sandbox escape", subdir_basename);

      return glnx_throw_errno_prefix (error, "openat(%s)", subdir_basename);
    }

  *out_fd = fd;
  return TRUE;
}

void
flatpak_context_append_bwrap_filesystem (FlatpakContext  *context,
                                         FlatpakBwrap    *bwrap,
                                         const char      *app_id,
                                         GFile           *app_id_dir,
                                         FlatpakExports  *exports,
                                         const char      *xdg_dirs_conf,
                                         gboolean         home_access)
{
  GHashTableIter iter;
  gpointer key, value;

  if (app_id_dir != NULL)
    flatpak_context_apply_env_appid (bwrap, app_id_dir);

  if (!home_access)
    {
      /* Enable persistent mapping only if no access to real home dir */

      g_hash_table_iter_init (&iter, context->persistent);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          const char *persist = key;
          g_autofree char *appdir = g_build_filename (g_get_home_dir (), ".var/app", app_id, NULL);
          g_autofree char *dest = g_build_filename (g_get_home_dir (), persist, NULL);
          g_autoptr(GError) local_error = NULL;

          if (g_mkdir_with_parents (appdir, 0755) != 0)
            {
              g_warning ("Unable to create directory %s", appdir);
              continue;
            }

          /* Don't follow symlinks from the persist directory, as it is under user control */
          glnx_autofd int src_fd = -1;
          if (!mkdir_p_open_nofollow_at (AT_FDCWD, appdir, 0755,
                                         persist, &src_fd,
                                         &local_error))
            {
              g_warning ("Failed to create persist path %s: %s", persist, local_error->message);
              continue;
            }

          g_autofree char *src_via_proc = g_strdup_printf ("%d", src_fd);

          flatpak_bwrap_add_fd (bwrap, g_steal_fd (&src_fd));
          flatpak_bwrap_add_bind_arg (bwrap, "--bind-fd", src_via_proc, dest);
        }
    }

  if (app_id_dir != NULL)
    {
      g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
      g_autofree char *run_user_app_dst = g_strdup_printf ("/run/flatpak/app/%s", app_id);
      g_autofree char *run_user_app_src = g_build_filename (user_runtime_dir, "app", app_id, NULL);

      if (glnx_shutil_mkdir_p_at (AT_FDCWD,
                                  run_user_app_src,
                                  0700,
                                  NULL,
                                  NULL))
        flatpak_bwrap_add_args (bwrap,
                                "--bind", run_user_app_src, run_user_app_dst,
                                NULL);

      /* Later, we'll make $XDG_RUNTIME_DIR/app a symlink to /run/flatpak/app */
      flatpak_bwrap_add_runtime_dir_member (bwrap, "app");
    }

  /* This actually outputs the args for the hide/expose operations
   * in the exports */
  flatpak_exports_append_bwrap_args (exports, bwrap);

  /* Special case subdirectories of the cache, config and data xdg
   * dirs.  If these are accessible explicitly, then we bind-mount
   * these in the app-id dir. This allows applications to explicitly
   * opt out of keeping some config/cache/data in the app-specific
   * directory.
   */
  if (app_id_dir)
    {
      g_hash_table_iter_init (&iter, context->filesystems);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          const char *filesystem = key;
          FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);
          g_autofree char *xdg_path = NULL;
          const char *rest, *where;

          xdg_path = get_xdg_dir_from_string (filesystem, &rest, &where);

          if (xdg_path != NULL && *rest != 0 &&
              mode >= FLATPAK_FILESYSTEM_MODE_READ_ONLY)
            {
              g_autoptr(GFile) app_version = g_file_get_child (app_id_dir, where);
              g_autoptr(GFile) app_version_subdir = g_file_resolve_relative_path (app_version, rest);

              if (g_file_test (xdg_path, G_FILE_TEST_IS_DIR) ||
                  g_file_test (xdg_path, G_FILE_TEST_IS_REGULAR))
                {
                  g_autofree char *xdg_path_in_app = g_file_get_path (app_version_subdir);
                  flatpak_bwrap_add_bind_arg (bwrap,
                                              mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY ? "--ro-bind" : "--bind",
                                              xdg_path, xdg_path_in_app);
                }
            }
        }
    }

  if (home_access && app_id_dir != NULL)
    {
      g_autofree char *src_path = g_build_filename (g_get_user_config_dir (),
                                                    "user-dirs.dirs",
                                                    NULL);
      g_autofree char *path = g_build_filename (flatpak_file_get_path_cached (app_id_dir),
                                                "config/user-dirs.dirs", NULL);
      if (g_file_test (src_path, G_FILE_TEST_EXISTS))
        flatpak_bwrap_add_bind_arg (bwrap, "--ro-bind", src_path, path);
    }
  else if (xdg_dirs_conf != NULL && xdg_dirs_conf[0] != '\0' && app_id_dir != NULL)
    {
      g_autofree char *path =
        g_build_filename (flatpak_file_get_path_cached (app_id_dir),
                          "config/user-dirs.dirs", NULL);

      flatpak_bwrap_add_args_data (bwrap, "xdg-config-dirs",
                                   xdg_dirs_conf, strlen (xdg_dirs_conf), path, NULL);
    }
}

gboolean
flatpak_context_get_allowed_exports (FlatpakContext *context,
                                     const char     *source_path,
                                     const char     *app_id,
                                     char         ***allowed_extensions_out,
                                     char         ***allowed_prefixes_out,
                                     gboolean       *require_exact_match_out)
{
  g_autoptr(GPtrArray) allowed_extensions = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) allowed_prefixes = g_ptr_array_new_with_free_func (g_free);
  gboolean require_exact_match = FALSE;

  g_ptr_array_add (allowed_prefixes, g_strdup_printf ("%s.*", app_id));

  if (strcmp (source_path, "share/applications") == 0)
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".desktop"));
    }
  else if (flatpak_has_path_prefix (source_path, "share/icons"))
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".svgz"));
      g_ptr_array_add (allowed_extensions, g_strdup (".png"));
      g_ptr_array_add (allowed_extensions, g_strdup (".svg"));
      g_ptr_array_add (allowed_extensions, g_strdup (".ico"));
    }
  else if (strcmp (source_path, "share/dbus-1/services") == 0)
    {
      g_auto(GStrv) owned_dbus_names =  flatpak_context_get_session_bus_policy_allowed_own_names (context);

      g_ptr_array_add (allowed_extensions, g_strdup (".service"));

      for (GStrv iter = owned_dbus_names; *iter != NULL; ++iter)
        g_ptr_array_add (allowed_prefixes, g_strdup (*iter));

      /* We need an exact match with no extra garbage, because the filename refers to busnames
       * and we can *only* match exactly these */
      require_exact_match = TRUE;
    }
  else if (strcmp (source_path, "share/gnome-shell/search-providers") == 0)
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".ini"));
    }
  else if (strcmp (source_path, "share/krunner/dbusplugins") == 0)
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".desktop"));
    }
  else if (strcmp (source_path, "share/mime/packages") == 0)
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".xml"));
    }
  else if (strcmp (source_path, "share/metainfo") == 0 ||
           strcmp (source_path, "share/appdata") == 0)
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".xml"));
    }
  else
    return FALSE;

  g_ptr_array_add (allowed_extensions, NULL);
  g_ptr_array_add (allowed_prefixes, NULL);

  if (allowed_extensions_out)
    *allowed_extensions_out = (char **) g_ptr_array_free (g_steal_pointer (&allowed_extensions), FALSE);

  if (allowed_prefixes_out)
    *allowed_prefixes_out = (char **) g_ptr_array_free (g_steal_pointer (&allowed_prefixes), FALSE);

  if (require_exact_match_out)
    *require_exact_match_out = require_exact_match;

  return TRUE;
}

void
flatpak_context_dump (FlatpakContext *context,
                      const char     *title)
{
  if (flatpak_is_debugging ())
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GKeyFile) metakey = NULL;
      g_autofree char *data = NULL;
      char *saveptr = NULL;
      const char *line;

      metakey = g_key_file_new ();
      flatpak_context_save_metadata (context, FALSE, metakey);

      data = g_key_file_to_data (metakey, NULL, &local_error);

      if (data == NULL)
        {
          g_debug ("%s: (unable to serialize: %s)",
                   title, local_error->message);
          return;
        }

      g_debug ("%s:", title);

      for (line = strtok_r (data, "\n", &saveptr);
           line != NULL;
           line = strtok_r (NULL, "\n", &saveptr))
        g_debug ("\t%s", line);

      g_debug ("\t#");
    }
}

FlatpakContextSockets
flatpak_context_compute_allowed_sockets (FlatpakContext                   *context,
                                         FlatpakContextConditionEvaluator  evaluator)
{
  return flatpak_permissions_compute_allowed (context->socket_permissions,
                                              flatpak_context_sockets,
                                              evaluator);
}

FlatpakContextDevices
flatpak_context_compute_allowed_devices (FlatpakContext                   *context,
                                         FlatpakContextConditionEvaluator  evaluator)
{
  return flatpak_permissions_compute_allowed (context->device_permissions,
                                              flatpak_context_devices,
                                              evaluator);
}
