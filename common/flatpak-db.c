/* flatpak-db.c
 *
 * Copyright (C) 2015 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/statfs.h>

#include "flatpak-db.h"
#include "gvdb/gvdb-reader.h"
#include "gvdb/gvdb-builder.h"

struct FlatpakDb
{
  GObject    parent;

  char      *path;
  gboolean   fail_if_not_found;
  GvdbTable *gvdb;
  GBytes    *gvdb_contents;

  gboolean   dirty;

  /* Map id => GVariant (data, sorted-dict[appid->perms]) */
  GvdbTable  *main_table;
  GHashTable *main_updates;

  /* (reverse) Map app id => [ id ]*/
  GvdbTable  *app_table;
  GHashTable *app_additions;
  GHashTable *app_removals;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakDbClass;

static void initable_iface_init (GInitableIface *initable_iface);

G_DEFINE_TYPE_WITH_CODE (FlatpakDb, flatpak_db, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init));

enum {
  PROP_0,
  PROP_PATH,
  PROP_FAIL_IF_NOT_FOUND,
  LAST_PROP
};

static int
cmpstringp (const void *p1, const void *p2)
{
  return strcmp (*(char * const *) p1, *(char * const *) p2);
}

static void
sort_strv (const char **strv)
{
  qsort (strv, g_strv_length ((char **) strv), sizeof (const char *), cmpstringp);
}

static int
str_ptr_array_find (GPtrArray  *array,
                    const char *str)
{
  int i;

  for (i = 0; i < array->len; i++)
    if (strcmp (g_ptr_array_index (array, i), str) == 0)
      return i;

  return -1;
}

static gboolean
str_ptr_array_contains (GPtrArray  *array,
                        const char *str)
{
  return str_ptr_array_find (array, str) >= 0;
}

const char *
flatpak_db_get_path (FlatpakDb *self)
{
  g_return_val_if_fail (FLATPAK_IS_DB (self), NULL);

  return self->path;
}

void
flatpak_db_set_path (FlatpakDb  *self,
                     const char *path)
{
  g_return_if_fail (FLATPAK_IS_DB (self));

  g_clear_pointer (&self->path, g_free);
  self->path = g_strdup (path);
}

FlatpakDb *
flatpak_db_new (const char *path,
                gboolean    fail_if_not_found,
                GError    **error)
{
  return g_initable_new (FLATPAK_TYPE_DB,
                         NULL,
                         error,
                         "path", path,
                         "fail-if-not-found", fail_if_not_found,
                         NULL);
}

static void
flatpak_db_finalize (GObject *object)
{
  FlatpakDb *self = (FlatpakDb *) object;

  g_clear_pointer (&self->path, g_free);
  g_clear_pointer (&self->gvdb_contents, g_bytes_unref);
  g_clear_pointer (&self->gvdb, gvdb_table_free);
  g_clear_pointer (&self->main_table, gvdb_table_free);
  g_clear_pointer (&self->app_table, gvdb_table_free);
  g_clear_pointer (&self->main_updates, g_hash_table_unref);
  g_clear_pointer (&self->app_additions, g_hash_table_unref);
  g_clear_pointer (&self->app_removals, g_hash_table_unref);

  G_OBJECT_CLASS (flatpak_db_parent_class)->finalize (object);
}

static void
flatpak_db_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  FlatpakDb *self = FLATPAK_DB (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_string (value, self->path);
      break;

    case PROP_FAIL_IF_NOT_FOUND:
      g_value_set_boolean (value, self->fail_if_not_found);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
flatpak_db_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  FlatpakDb *self = FLATPAK_DB (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_clear_pointer (&self->path, g_free);
      self->path = g_value_dup_string (value);
      break;

    case PROP_FAIL_IF_NOT_FOUND:
      self->fail_if_not_found = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
flatpak_db_class_init (FlatpakDbClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_db_finalize;
  object_class->get_property = flatpak_db_get_property;
  object_class->set_property = flatpak_db_set_property;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_FAIL_IF_NOT_FOUND,
                                   g_param_spec_boolean ("fail-if-not-found",
                                                         "",
                                                         "",
                                                         TRUE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
flatpak_db_init (FlatpakDb *self)
{
  self->fail_if_not_found = TRUE;

  self->main_updates =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free, (GDestroyNotify) g_variant_unref);
  self->app_additions =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free, (GDestroyNotify) g_ptr_array_unref);
  self->app_removals =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free, (GDestroyNotify) g_ptr_array_unref);
}

static gboolean
is_on_nfs (const char *path)
{
  struct statfs statfs_buffer;
  int statfs_result;
  g_autofree char *dirname = NULL;

  dirname = g_path_get_dirname (path);

  statfs_result = statfs (dirname, &statfs_buffer);
  if (statfs_result != 0)
    return FALSE;

  return statfs_buffer.f_type == 0x6969;
}

static gboolean
initable_init (GInitable    *initable,
               GCancellable *cancellable,
               GError      **error)
{
  FlatpakDb *self = (FlatpakDb *) initable;
  GError *my_error = NULL;

  if (self->path == NULL)
    return TRUE;

  if (is_on_nfs (self->path))
    {
      g_autoptr(GFile) file = g_file_new_for_path (self->path);
      char *contents;
      gsize length;

      /* We avoid using mmap on NFS, because its prone to give us SIGBUS at semi-random
         times (nfs down, file removed, etc). Instead we just load the file */
      if (g_file_load_contents (file, cancellable, &contents, &length, NULL, &my_error))
        self->gvdb_contents = g_bytes_new_take (contents, length);
    }
  else
    {
      GMappedFile *mapped = g_mapped_file_new (self->path, FALSE, &my_error);
      if (mapped)
        {
          self->gvdb_contents = g_mapped_file_get_bytes (mapped);
          g_mapped_file_unref (mapped);
        }
    }

  if (self->gvdb_contents == NULL)
    {
      if (!self->fail_if_not_found &&
          g_error_matches (my_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_error_free (my_error);
        }
      else
        {
          g_propagate_error (error, my_error);
          return FALSE;
        }
    }
  else
    {
      self->gvdb = gvdb_table_new_from_bytes (self->gvdb_contents, TRUE, error);
      if (self->gvdb == NULL)
        return FALSE;

      self->main_table = gvdb_table_get_table (self->gvdb, "main");
      if (self->main_table == NULL)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                       "No main table in db");
          return FALSE;
        }

      self->app_table = gvdb_table_get_table (self->gvdb, "apps");
      if (self->app_table == NULL)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                       "No app table in db");
          return FALSE;
        }
    }

  return TRUE;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

/* Transfer: full */
char **
flatpak_db_list_ids (FlatpakDb *self)
{
  GPtrArray *res;
  GHashTableIter iter;
  gpointer key, value;
  int i;

  g_return_val_if_fail (FLATPAK_IS_DB (self), NULL);

  res = g_ptr_array_new ();

  g_hash_table_iter_init (&iter, self->main_updates);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (value != NULL)
        g_ptr_array_add (res, g_strdup (key));
    }

  if (self->main_table)
    {
      // TODO: can we use gvdb_table_list here???
      g_autofree char **main_ids = gvdb_table_get_names (self->main_table, NULL);

      for (i = 0; main_ids[i] != NULL; i++)
        {
          char *id = main_ids[i];

          if (g_hash_table_lookup_extended (self->main_updates, id, NULL, NULL))
            g_free (id);
          else
            g_ptr_array_add (res, id);
        }
    }

  g_ptr_array_add (res, NULL);
  return (char **) g_ptr_array_free (res, FALSE);
}

static gboolean
app_update_empty (GHashTable *ht, const char *app)
{
  GPtrArray *array;

  array = g_hash_table_lookup (ht, app);
  if (array == NULL)
    return TRUE;

  return array->len == 0;
}

/* Transfer: full */
char **
flatpak_db_list_apps (FlatpakDb *self)
{
  gpointer key, _value;
  GHashTableIter iter;
  GPtrArray *res;
  int i;

  g_return_val_if_fail (FLATPAK_IS_DB (self), NULL);

  res = g_ptr_array_new ();

  g_hash_table_iter_init (&iter, self->app_additions);
  while (g_hash_table_iter_next (&iter, &key, &_value))
    {
      GPtrArray *value = _value;
      if (value->len > 0)
        g_ptr_array_add (res, g_strdup (key));
    }

  if (self->app_table)
    {
      // TODO: can we use gvdb_table_list here???
      g_autofree char **apps = gvdb_table_get_names (self->app_table, NULL);

      for (i = 0; apps[i] != NULL; i++)
        {
          char *app = apps[i];
          gboolean empty = TRUE;
          GPtrArray *removals;
          int j;

          /* Don't use if we already added above */
          if (app_update_empty (self->app_additions, app))
            {
              g_autoptr(GVariant) ids_v = NULL;

              removals = g_hash_table_lookup (self->app_removals, app);

              /* Add unless all items are removed */
              ids_v = gvdb_table_get_value (self->app_table, app);

              if (ids_v)
                {
                  g_autofree const char **ids = g_variant_get_strv (ids_v, NULL);

                  for (j = 0; ids[j] != NULL; j++)
                    {
                      if (removals == NULL ||
                          !str_ptr_array_contains (removals, ids[j]))
                        {
                          empty = FALSE;
                          break;
                        }
                    }
                }
            }

          if (empty)
            g_free (app);
          else
            g_ptr_array_add (res, app);
        }
    }

  g_ptr_array_add (res, NULL);
  return (char **) g_ptr_array_free (res, FALSE);
}

/* Transfer: full */
char **
flatpak_db_list_ids_by_app (FlatpakDb  *self,
                            const char *app)
{
  GPtrArray *res;
  GPtrArray *additions;
  GPtrArray *removals;
  int i;

  g_return_val_if_fail (FLATPAK_IS_DB (self), NULL);

  res = g_ptr_array_new ();

  additions = g_hash_table_lookup (self->app_additions, app);
  removals = g_hash_table_lookup (self->app_removals, app);

  if (additions)
    {
      for (i = 0; i < additions->len; i++)
        g_ptr_array_add (res,
                         g_strdup (g_ptr_array_index (additions, i)));
    }

  if (self->app_table)
    {
      g_autoptr(GVariant) ids_v = gvdb_table_get_value (self->app_table, app);
      if (ids_v)
        {
          g_autofree const char **ids = g_variant_get_strv (ids_v, NULL);

          for (i = 0; ids[i] != NULL; i++)
            {
              if (removals == NULL ||
                  !str_ptr_array_contains (removals, ids[i]))
                g_ptr_array_add (res, g_strdup (ids[i]));
            }
        }
    }

  g_ptr_array_add (res, NULL);
  return (char **) g_ptr_array_free (res, FALSE);
}

/* Transfer: full */
FlatpakDbEntry *
flatpak_db_lookup (FlatpakDb  *self,
                   const char *id)
{
  GVariant *res = NULL;
  gpointer value;

  g_return_val_if_fail (FLATPAK_IS_DB (self), NULL);
  g_return_val_if_fail (id != NULL, NULL);

  if (g_hash_table_lookup_extended (self->main_updates, id, NULL, &value))
    {
      if (value != NULL)
        res = g_variant_ref ((GVariant *) value);
    }
  else if (self->main_table)
    {
      res = gvdb_table_get_value (self->main_table, id);
    }

  return (FlatpakDbEntry *) res;
}

/* Transfer: full */
char **
flatpak_db_list_ids_by_value (FlatpakDb *self,
                              GVariant  *data)
{
  g_autofree char **ids = flatpak_db_list_ids (self);
  int i;
  GPtrArray *res;

  g_return_val_if_fail (FLATPAK_IS_DB (self), NULL);
  g_return_val_if_fail (data != NULL, NULL);

  res = g_ptr_array_new ();

  for (i = 0; ids[i] != NULL; i++)
    {
      char *id = ids[i];

      g_autoptr(FlatpakDbEntry) entry = NULL;
      g_autoptr(GVariant) entry_data = NULL;

      entry = flatpak_db_lookup (self, id);
      if (entry)
        {
          entry_data = flatpak_db_entry_get_data (entry);
          if (g_variant_equal (data, entry_data))
            {
              g_ptr_array_add (res, id);
              id = NULL; /* Don't free, as we return this */
            }
        }
      g_free (id);
    }

  g_ptr_array_add (res, NULL);
  return (char **) g_ptr_array_free (res, FALSE);
}

static void
add_app_id (FlatpakDb  *self,
            const char *app,
            const char *id)
{
  GPtrArray *additions;
  GPtrArray *removals;
  int i;

  additions = g_hash_table_lookup (self->app_additions, app);
  removals = g_hash_table_lookup (self->app_removals, app);

  if (removals)
    {
      i = str_ptr_array_find (removals, id);
      if (i >= 0)
        g_ptr_array_remove_index_fast (removals, i);
    }

  if (additions)
    {
      if (!str_ptr_array_contains (additions, id))
        g_ptr_array_add (additions, g_strdup (id));
    }
  else
    {
      additions = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (additions, g_strdup (id));
      g_hash_table_insert (self->app_additions,
                           g_strdup (app), additions);
    }
}

static void
remove_app_id (FlatpakDb  *self,
               const char *app,
               const char *id)
{
  GPtrArray *additions;
  GPtrArray *removals;
  int i;

  additions = g_hash_table_lookup (self->app_additions, app);
  removals = g_hash_table_lookup (self->app_removals, app);

  if (additions)
    {
      i = str_ptr_array_find (additions, id);
      if (i >= 0)
        g_ptr_array_remove_index_fast (additions, i);
    }

  if (removals)
    {
      if (!str_ptr_array_contains (removals, id))
        g_ptr_array_add (removals, g_strdup (id));
    }
  else
    {
      removals = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (removals, g_strdup (id));
      g_hash_table_insert (self->app_removals,
                           g_strdup (app), removals);
    }
}

gboolean
flatpak_db_is_dirty (FlatpakDb *self)
{
  g_return_val_if_fail (FLATPAK_IS_DB (self), FALSE);

  return self->dirty;
}

/* add, replace, or NULL entry to remove */
void
flatpak_db_set_entry (FlatpakDb      *self,
                      const char     *id,
                      FlatpakDbEntry *entry)
{
  g_autoptr(FlatpakDbEntry) old_entry = NULL;
  g_autofree const char **old = NULL;
  g_autofree const char **new = NULL;
  static const char *empty[] = { NULL };
  const char **a, **b;
  int ia, ib;

  g_return_if_fail (FLATPAK_IS_DB (self));
  g_return_if_fail (id != NULL);

  self->dirty = TRUE;

  old_entry = flatpak_db_lookup (self, id);

  g_hash_table_insert (self->main_updates,
                       g_strdup (id),
                       flatpak_db_entry_ref (entry));

  a = empty;
  b = empty;

  if (old_entry)
    {
      old = flatpak_db_entry_list_apps (old_entry);
      sort_strv (old);
      a = old;
    }

  if (entry)
    {
      new = flatpak_db_entry_list_apps (entry);
      sort_strv (new);
      b = new;
    }

  ia = 0;
  ib = 0;
  while (a[ia] != NULL || b[ib] != NULL)
    {
      if (a[ia] == NULL)
        {
          /* Not in old, but in new => added */
          add_app_id (self, b[ib], id);
          ib++;
        }
      else if (b[ib] == NULL)
        {
          /* Not in new, but in old => removed */
          remove_app_id (self, a[ia], id);
          ia++;
        }
      else
        {
          int cmp = strcmp (a[ia], b[ib]);

          if (cmp == 0)
            {
              /* In both, no change */
              ia++;
              ib++;
            }
          else if (cmp < 0)
            {
              /* Not in new, but in old => removed */
              remove_app_id (self, a[ia], id);
              ia++;
            }
          else /* cmp > 0 */
            {
              /* Not in old, but in new => added */
              add_app_id (self, b[ib], id);
              ib++;
            }
        }
    }
}

void
flatpak_db_update (FlatpakDb *self)
{
  GHashTable *root, *main_h, *apps_h;
  GBytes *new_contents;
  GvdbTable *new_gvdb;
  int i;

  g_auto(GStrv) ids = NULL;
  g_auto(GStrv) apps = NULL;

  g_return_if_fail (FLATPAK_IS_DB (self));

  root = gvdb_hash_table_new (NULL, NULL);
  main_h = gvdb_hash_table_new (root, "main");
  apps_h = gvdb_hash_table_new (root, "apps");
  g_hash_table_unref (main_h);
  g_hash_table_unref (apps_h);

  ids = flatpak_db_list_ids (self);
  for (i = 0; ids[i] != 0; i++)
    {
      g_autoptr(FlatpakDbEntry) entry = flatpak_db_lookup (self, ids[i]);
      if (entry != NULL)
        {
          GvdbItem *item;

          item = gvdb_hash_table_insert (main_h, ids[i]);
          gvdb_item_set_value (item, (GVariant *) entry);
        }
    }

  apps = flatpak_db_list_apps (self);
  for (i = 0; apps[i] != 0; i++)
    {
      g_auto(GStrv) app_ids = flatpak_db_list_ids_by_app (self, apps[i]);
      GVariantBuilder builder;
      GvdbItem *item;
      int j;

      /* May as well ensure that on-disk arrays are sorted, even if we don't use it yet */
      sort_strv ((const char **) app_ids);

      /* We should never list an app that has empty id lists */
      g_assert (app_ids[0] != NULL);

      g_variant_builder_init (&builder, G_VARIANT_TYPE_ARRAY);
      for (j = 0; app_ids[j] != NULL; j++)
        g_variant_builder_add (&builder, "s", app_ids[j]);

      item = gvdb_hash_table_insert (apps_h, apps[i]);
      gvdb_item_set_value (item, g_variant_builder_end (&builder));
    }

  new_contents = gvdb_table_get_content (root, FALSE);
  new_gvdb = gvdb_table_new_from_bytes (new_contents, TRUE, NULL);

  /* This was just created, any failure to parse it is purely an internal error */
  g_assert (new_gvdb != NULL);

  g_clear_pointer (&self->gvdb_contents, g_bytes_unref);
  g_clear_pointer (&self->gvdb, gvdb_table_free);
  self->gvdb_contents = new_contents;
  self->gvdb = new_gvdb;
  self->dirty = FALSE;
}

GBytes *
flatpak_db_get_content (FlatpakDb *self)
{
  g_return_val_if_fail (FLATPAK_IS_DB (self), NULL);

  return self->gvdb_contents;
}

/* Note: You must first call update to serialize, this only saves serialied data */
gboolean
flatpak_db_save_content (FlatpakDb *self,
                         GError   **error)
{
  GBytes *content = NULL;

  if (self->gvdb_contents == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "No content to save");
      return FALSE;
    }

  if (self->path == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "No path set");
      return FALSE;
    }

  content = self->gvdb_contents;
  return g_file_set_contents (self->path, g_bytes_get_data (content, NULL), g_bytes_get_size (content), error);
}

static void
save_content_callback (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GFile *file = G_FILE (source_object);
  gboolean ok;
  g_autoptr(GError) error = NULL;

  ok = g_file_replace_contents_finish (file,
                                       res,
                                       NULL, &error);
  if (ok)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

void
flatpak_db_save_content_async (FlatpakDb          *self,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
  GBytes *content = NULL;

  g_autoptr(GTask) task = NULL;
  g_autoptr(GFile) file = NULL;

  task = g_task_new (self, cancellable, callback, user_data);

  if (self->gvdb_contents == NULL)
    {
      g_task_return_new_error (task, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                               "No content to save");
      return;
    }

  if (self->path == NULL)
    {
      g_task_return_new_error (task, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                               "No path set");
      return;
    }

  content = g_bytes_ref (self->gvdb_contents);
  g_task_set_task_data (task, content, (GDestroyNotify) g_bytes_unref);

  file = g_file_new_for_path (self->path);
  g_file_replace_contents_bytes_async (file, content,
                                       NULL, FALSE, 0,
                                       cancellable,
                                       save_content_callback,
                                       g_object_ref (task));
}

gboolean
flatpak_db_save_content_finish (FlatpakDb    *self,
                                GAsyncResult *res,
                                GError      **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}


GString *
flatpak_db_print_string (FlatpakDb *self,
                         GString   *string)
{
  g_auto(GStrv) ids = NULL;
  g_auto(GStrv) apps = NULL;
  int i;

  g_return_val_if_fail (FLATPAK_IS_DB (self), NULL);

  if G_UNLIKELY (string == NULL)
    string = g_string_new (NULL);

  g_string_append_printf (string, "main {\n");

  ids = flatpak_db_list_ids (self);
  sort_strv ((const char **) ids);
  for (i = 0; ids[i] != 0; i++)
    {
      g_autoptr(FlatpakDbEntry) entry = flatpak_db_lookup (self, ids[i]);
      g_string_append_printf (string, " %s: ", ids[i]);
      if (entry != NULL)
        flatpak_db_entry_print_string (entry, string);
      g_string_append_printf (string, "\n");
    }

  g_string_append_printf (string, "}\napps {\n");

  apps = flatpak_db_list_apps (self);
  sort_strv ((const char **) apps);
  for (i = 0; apps[i] != 0; i++)
    {
      int j;
      g_auto(GStrv) app_ids = NULL;

      app_ids = flatpak_db_list_ids_by_app (self, apps[i]);
      sort_strv ((const char **) app_ids);

      g_string_append_printf (string, " %s: ", apps[i]);
      for (j = 0; app_ids[j] != NULL; j++)
        g_string_append_printf (string, "%s%s", j == 0 ? "" : ", ", app_ids[j]);
      g_string_append_printf (string, "\n");
    }

  g_string_append_printf (string, "}\n");

  return string;
}

char *
flatpak_db_print (FlatpakDb *self)
{
  return g_string_free (flatpak_db_print_string (self, NULL), FALSE);
}

FlatpakDbEntry  *
flatpak_db_entry_ref (FlatpakDbEntry *entry)
{
  if (entry != NULL)
    g_variant_ref ((GVariant *) entry);
  return entry;
}

void
flatpak_db_entry_unref (FlatpakDbEntry *entry)
{
  g_variant_unref ((GVariant *) entry);
}

/* Transfer: full */
GVariant *
flatpak_db_entry_get_data (FlatpakDbEntry *entry)
{
  g_autoptr(GVariant) variant = g_variant_get_child_value ((GVariant *) entry, 0);

  return g_variant_get_child_value (variant, 0);
}

/* Transfer: container */
const char **
flatpak_db_entry_list_apps (FlatpakDbEntry *entry)
{
  GVariant *v = (GVariant *) entry;

  g_autoptr(GVariant) app_array = NULL;
  GVariantIter iter;
  GVariant *child;
  GPtrArray *res;

  res = g_ptr_array_new ();

  app_array = g_variant_get_child_value (v, 1);

  g_variant_iter_init (&iter, app_array);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      const char *child_app_id;
      g_autoptr(GVariant) permissions = g_variant_get_child_value (child, 1);

      if (g_variant_n_children (permissions) > 0)
        {
          g_variant_get_child (child, 0, "&s", &child_app_id);
          g_ptr_array_add (res, (char *) child_app_id);
        }

      g_variant_unref (child);
    }

  g_ptr_array_add (res, NULL);
  return (const char **) g_ptr_array_free (res, FALSE);
}

static GVariant *
flatpak_db_entry_get_permissions_variant (FlatpakDbEntry *entry,
                                          const char     *app_id)
{
  GVariant *v = (GVariant *) entry;

  g_autoptr(GVariant) app_array = NULL;
  GVariant *child;
  GVariant *res = NULL;
  gsize n_children, start, end, m;
  const char *child_app_id;
  int cmp;

  app_array = g_variant_get_child_value (v, 1);

  n_children = g_variant_n_children (app_array);

  start = 0;
  end = n_children;
  while (start < end)
    {
      m = (start + end) / 2;

      child = g_variant_get_child_value (app_array, m);
      g_variant_get_child (child, 0, "&s", &child_app_id);

      cmp = strcmp (app_id, child_app_id);
      if (cmp == 0)
        {
          res = g_variant_get_child_value (child, 1);
          break;
        }
      else if (cmp < 0)
        {
          end = m;
        }
      else /* cmp > 0 */
        {
          start = m + 1;
        }
    }

  return res;
}


/* Transfer: container */
const char **
flatpak_db_entry_list_permissions (FlatpakDbEntry *entry,
                                   const char     *app)
{
  g_autoptr(GVariant) permissions = NULL;

  permissions = flatpak_db_entry_get_permissions_variant (entry, app);
  if (permissions)
    return g_variant_get_strv (permissions, NULL);
  else
    return g_new0 (const char *, 1);
}

gboolean
flatpak_db_entry_has_permission (FlatpakDbEntry *entry,
                                 const char     *app,
                                 const char     *permission)
{
  g_autofree const char **app_permissions = NULL;

  app_permissions = flatpak_db_entry_list_permissions (entry, app);

  return g_strv_contains (app_permissions, permission);
}

gboolean
flatpak_db_entry_has_permissions (FlatpakDbEntry *entry,
                                  const char     *app,
                                  const char    **permissions)
{
  g_autofree const char **app_permissions = NULL;
  int i;

  app_permissions = flatpak_db_entry_list_permissions (entry, app);

  for (i = 0; permissions[i] != NULL; i++)
    {
      if (!g_strv_contains (app_permissions, permissions[i]))
        return FALSE;
    }

  return TRUE;
}

static GVariant *
make_entry (GVariant *data,
            GVariant *app_permissions)
{
  return g_variant_new ("(v@a{sas})", data, app_permissions);
}

static GVariant *
make_empty_app_permissions (void)
{
  return g_variant_new_array (G_VARIANT_TYPE ("{sas}"), NULL, 0);
}

static GVariant *
make_permissions (const char *app, const char **permissions)
{
  static const char **empty = { NULL };

  if (permissions == NULL)
    permissions = empty;

  return g_variant_new ("{s@as}",
                        app,
                        g_variant_new_strv (permissions, -1));
}

static GVariant *
add_permissions (GVariant *app_permissions,
                 GVariant *permissions)
{
  GVariantBuilder builder;
  GVariantIter iter;
  GVariant *child;
  gboolean added = FALSE;
  int cmp;
  const char *new_app_id;
  const char *child_app_id;

  g_autoptr(GVariant) new_perms_array = NULL;

  g_variant_get (permissions, "{&s@as}", &new_app_id, &new_perms_array);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_ARRAY);

  /* Insert or replace permissions in sorted order */

  g_variant_iter_init (&iter, app_permissions);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      g_autoptr(GVariant) old_perms_array = NULL;

      g_variant_get (child, "{&s@as}", &child_app_id, &old_perms_array);

      cmp = strcmp (new_app_id, child_app_id);
      if (cmp == 0)
        {
          added = TRUE;
          /* Replace old permissions */
          g_variant_builder_add_value (&builder, permissions);
        }
      else if (cmp < 0)
        {
          if (!added)
            {
              added = TRUE;
              g_variant_builder_add_value (&builder, permissions);
            }
          g_variant_builder_add_value (&builder, child);
        }
      else /* cmp > 0 */
        {
          g_variant_builder_add_value (&builder, child);
        }

      g_variant_unref (child);
    }

  if (!added)
    g_variant_builder_add_value (&builder, permissions);

  return g_variant_builder_end (&builder);
}

FlatpakDbEntry  *
flatpak_db_entry_new (GVariant *data)
{
  GVariant *res;

  if (data == NULL)
    data = g_variant_new_byte (0);

  res = make_entry (data,
                    make_empty_app_permissions ());

  return (FlatpakDbEntry  *) g_variant_ref_sink (res);
}

FlatpakDbEntry  *
flatpak_db_entry_modify_data (FlatpakDbEntry *entry,
                              GVariant       *data)
{
  GVariant *v = (GVariant *) entry;
  GVariant *res;

  if (data == NULL)
    data = g_variant_new_byte (0);

  res = make_entry (data,
                    g_variant_get_child_value (v, 1));
  return (FlatpakDbEntry  *) g_variant_ref_sink (res);
}

/* NULL (or empty) permissions to remove permissions */
FlatpakDbEntry  *
flatpak_db_entry_set_app_permissions (FlatpakDbEntry *entry,
                                      const char     *app,
                                      const char    **permissions)
{
  GVariant *v = (GVariant *) entry;
  GVariant *res;

  g_autoptr(GVariant) old_data_v = g_variant_get_child_value (v, 0);
  g_autoptr(GVariant) old_data = g_variant_get_child_value (old_data_v, 0);
  g_autoptr(GVariant) old_permissions = g_variant_get_child_value (v, 1);

  res = make_entry (old_data,
                    add_permissions (old_permissions,
                                     make_permissions (app,
                                                       permissions)));
  return (FlatpakDbEntry  *) g_variant_ref_sink (res);
}

GString *
flatpak_db_entry_print_string (FlatpakDbEntry *entry,
                               GString        *string)
{
  return g_variant_print_string ((GVariant *) entry, string, FALSE);
}
