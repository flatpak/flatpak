/*
 * Copyright © 2017 Endless Mobile, Inc.
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
 *       Sam Spilsbury <sam@endlessm.com>
 */

#include "config.h"

#include <json-glib/json-glib.h>

#include "flatpak-transaction-log.h"

struct _FlatpakTransactionLog {
  GObject    parent;

  GFile      *path;
};

G_DEFINE_TYPE (FlatpakTransactionLog, flatpak_transaction_log, G_TYPE_OBJECT)

typedef enum {
  PROP_PATH = 1,
  LAST_PROP
} FlatpakTransactionLogProperty;

/**
 * flatpak_transaction_log_new:
 * @path: a #GFile indicating where the transaction log will be created
 *
 * Creates a new #FlatpakTransactionLog object. This class encapsulates
 * a special append-only JSON-like log file indicating the transactions
 * that have been performed by flatpak over time. The log file created or
 * appended to at @path does not conform to RFC7159, instead it is a
 * newline delimited list of JSON-formatted object definitions which should
 * be parsed one line at a time. This enables #FlatpakTransactionLog to write
 * new entries in O(1) time, as opposed to having to read the entire log
 * into memory so that it can be re-written again.
 *
 * Since: 0.10.0
 * Returns: A new #FlatpakTransactionLog
 */
FlatpakTransactionLog *
flatpak_transaction_log_new (GFile *path)
{
  g_return_val_if_fail (G_IS_FILE (path), NULL);

  return g_object_new (FLATPAK_TYPE_TRANSACTION_LOG,
                       "path", path,
                       NULL);
}

static void
flatpak_transaction_log_finalize (GObject *object)
{
  FlatpakTransactionLog *self = FLATPAK_TRANSACTION_LOG (object);

  g_clear_object (&self->path);

  G_OBJECT_CLASS (flatpak_transaction_log_parent_class)->finalize (object);
}

static void
flatpak_transaction_log_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  FlatpakTransactionLog *self = FLATPAK_TRANSACTION_LOG (object);

  switch ((FlatpakTransactionLogProperty) prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->path);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
flatpak_transaction_log_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  FlatpakTransactionLog *self = FLATPAK_TRANSACTION_LOG (object);

  switch ((FlatpakTransactionLogProperty) prop_id)
    {
    case PROP_PATH:
      g_assert (self->path == NULL);
      self->path = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* For efficiency's sake, we have a special "append only" log format here. This
 * is a subset of JSON, but is not valid JSON in its own right - rather, each
 * line is a new JSON object. Applications wishing to parse this file should
 * read and parse each line independently. */
static gboolean
append_node_to_log_file (GFile        *file,
                         JsonNode     *node,
                         GCancellable *cancellable,
                         GError      **error)
{
  g_autoptr(GFile) parent = g_file_get_parent (file);
  g_autoptr(GFileOutputStream) stream = NULL;
  g_autofree gchar *serialized = NULL;
  gsize serialized_len = 0;
  g_autoptr(JsonGenerator) generator = NULL;
  g_autoptr(GError) my_error = NULL;

  if (!g_file_make_directory_with_parents (parent, cancellable, &my_error))
    {
      if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }

      g_clear_error (&my_error);
    }

  generator = json_generator_new ();
  json_generator_set_root (generator, node);
  json_generator_set_pretty (generator, FALSE);

  /* Resize the buffer to accomodate one extra newline and write that
   * in place at serialized_len */
  serialized = json_generator_to_data (generator, &serialized_len);
  serialized = g_realloc (serialized, (serialized_len + 1) * sizeof (gchar));
  serialized[serialized_len++] = '\n';
  serialized[serialized_len] = '\0';

  stream = g_file_append_to (file,
                             G_FILE_CREATE_NONE,
                             cancellable,
                             error);

  return g_output_stream_write_all (G_OUTPUT_STREAM (stream),
                                    (gconstpointer) serialized,
                                    serialized_len,
                                    NULL,
                                    cancellable,
                                    error);
}

static JsonNode *
build_base_event (const gchar *event)
{
  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "event");
  json_builder_add_string_value (builder, event);

  json_builder_end_object (builder);

  return json_builder_get_root (builder);
}

static JsonNode *
deploy_event (const gchar *ref,
              const gchar *origin,
              const gchar *commit)
{
  g_autoptr(JsonNode) base = build_base_event ("deploy");
  JsonObject *object = json_node_get_object (base);

  json_object_set_string_member (object, "ref", ref);
  json_object_set_string_member (object, "origin", origin);
  json_object_set_string_member (object, "commit", commit);

  return g_steal_pointer (&base);
}

/**
 * flatpak_transaction_log_commit_deploy_event:
 * @log: a #FlatpakTransactionLog
 * @ref: a decomposable Flatpak ref
 * @origin: the origin of the flatpak to be installed, whether it is from
 *          a bundle or from a remote.
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Write a "deploy-install" event to the transaction log. The "ref" and
 * "origin" arguments will be included in the event description as properties.
 *
 * Since: 0.10.0
 * Returns: %TRUE if writing the event succeeded.
 */
gboolean
flatpak_transaction_log_commit_deploy_event (FlatpakTransactionLog *self,
                                             const gchar           *ref,
                                             const gchar           *origin,
                                             const gchar           *commit,
                                             GCancellable          *cancellable,
                                             GError               **error)
{
  g_autoptr(JsonNode) event = deploy_event (ref, origin, commit);

  return append_node_to_log_file (self->path, event, cancellable, error);
}

static JsonNode *
uninstall_event (const gchar *ref)
{
  g_autoptr(JsonNode) base = build_base_event ("uninstall");
  JsonObject *object = json_node_get_object (base);

  json_object_set_string_member (object, "ref", ref);

  return g_steal_pointer (&base);
}


/**
 * flatpak_transaction_log_commit_uninstall_event:
 * @log: a #FlatpakTransactionLog
 * @ref: a decomposable Flatpak ref
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Write an "uninstall" event to the transaction log. The "ref" arguments will be
 * included in the event description as properties.
 *
 * Since: 0.10.0
 * Returns: %TRUE if writing the event succeeded.
 */
gboolean
flatpak_transaction_log_commit_uninstall_event (FlatpakTransactionLog *self,
                                                const gchar           *ref,
                                                GCancellable          *cancellable,
                                                GError               **error)
{
  g_autoptr(JsonNode) event = uninstall_event (ref);

  return append_node_to_log_file (self->path, event, cancellable, error);
}

static void
flatpak_transaction_log_class_init (FlatpakTransactionLogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_transaction_log_finalize;
  object_class->get_property = flatpak_transaction_log_get_property;
  object_class->set_property = flatpak_transaction_log_set_property;

  /**
   * FlatpakTransactionLog:path:
   *
   * The absolute path, on disk, where the transaction log will be written to.
   * If the file indicated by the path, or any of its parent directories do
   * not exist, they will be created.
   *
   * Since: 0.10.0
   */
  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
flatpak_transaction_log_init (FlatpakTransactionLog *self)
{
}

