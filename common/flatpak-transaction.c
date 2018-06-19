/*
 * Copyright © 2016 Red Hat, Inc
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

#include <stdio.h>
#include <glib/gi18n.h>

#include "flatpak-transaction-private.h"
#include "flatpak-installation-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"

/**
 * SECTION:flatpak-transaction
 * @Title: FlatpakTransaction
 * @Short_description: Transaction information
 *
 * FlatpakTransaction is an object representing an install/update
 * transaction. You create an object like this using flatpak_transaction_new_for_installation()
 * and then you add all the operations (installs, updates, etc) you wish to do. Then
 * you start the transaction with flatpak_transaction_run() which will resolve all kinds
 * of dependencies and report progress and status while downloading and installing these.
 *
 * A transaction is a blocking operation, and all signals are emitted in the same thread.
 * This means you should either handle the signals directly (say, by doing blocking console
 * interaction, or by just returning without interaction), or run the operation in a separate
 * thread and do your own forwarding to the GUI thread.
 */

/* This is an internal-only element of FlatpakTransactionOperationType */
#define FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE FLATPAK_TRANSACTION_OPERATION_LAST_TYPE + 1

struct _FlatpakTransactionOperation {
  GObject parent;

  char *remote;
  char *ref;
  /* NULL means unspecified (normally keep whatever was there before), [] means force everything */
  char **subpaths;
  char *commit;
  GFile *bundle;
  GBytes *external_metadata;
  FlatpakTransactionOperationType kind;
  gboolean non_fatal;
  gboolean failed;
  gboolean skip;

  gboolean resolved;
  char *resolved_commit;
  GBytes *resolved_metadata;
  GKeyFile *resolved_metakey;
  GBytes *resolved_old_metadata;
  GKeyFile *resolved_old_metakey;
  int run_after_count;
  int run_after_prio; /* Higher => run later (when it becomes runnable). Used to run related ops (runtime extensions) before deps (apps using the runtime) */
  GList *run_before_ops;
  FlatpakTransactionOperation *fail_if_op_fails; /* main app/runtime for related extensions, runtime for apps */
};

typedef struct _FlatpakTransactionPrivate FlatpakTransactionPrivate;

struct _FlatpakTransactionPrivate {
  GObject parent;

  FlatpakInstallation *installation;
  FlatpakDir *dir;
  GHashTable *last_op_for_ref;
  GHashTable *remote_states; /* (element-type utf8 FlatpakRemoteState) */
  GPtrArray *extra_dependency_dirs;
  GList *ops;
  GPtrArray *added_origin_remotes;

  FlatpakTransactionOperation *current_op;

  gboolean no_pull;
  gboolean no_deploy;
  gboolean disable_static_deltas;
  gboolean disable_prune;
  gboolean disable_deps;
  gboolean disable_related;
  gboolean reinstall;
  gboolean force_uninstall;
};

enum {
  NEW_OPERATION,
  OPERATION_DONE,
  OPERATION_ERROR,
  CHOOSE_REMOTE_FOR_REF,
  END_OF_LIFED,
  READY,
  LAST_SIGNAL
};

enum {
  PROP_0,
  PROP_INSTALLATION,
};

struct _FlatpakTransactionProgress {
  GObject parent;

  OstreeAsyncProgress *ostree_progress;
  char *status;
  gboolean estimating;
  int progress;

  gboolean done;
};

enum {
  CHANGED,
  LAST_PROGRESS_SIGNAL
};

static guint progress_signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (FlatpakTransactionProgress, flatpak_transaction_progress, G_TYPE_OBJECT)

void
flatpak_transaction_progress_set_update_frequency (FlatpakTransactionProgress  *self,
                                                   guint update_frequency)
{
  g_object_set_data (G_OBJECT (self->ostree_progress), "update-frequency", GUINT_TO_POINTER (update_frequency));
}


char *
flatpak_transaction_progress_get_status (FlatpakTransactionProgress  *self)
{
  return g_strdup (self->status);
}

gboolean
flatpak_transaction_progress_get_is_estimating (FlatpakTransactionProgress  *self)
{
  return self->estimating;
}

int
flatpak_transaction_progress_get_progress (FlatpakTransactionProgress  *self)
{
  return self->progress;
}

static void
flatpak_transaction_progress_finalize (GObject *object)
{
  FlatpakTransactionProgress *self = (FlatpakTransactionProgress *) object;

  g_free (self->status);
  g_object_unref (self->ostree_progress);

  G_OBJECT_CLASS (flatpak_transaction_progress_parent_class)->finalize (object);
}

static void
flatpak_transaction_progress_class_init (FlatpakTransactionProgressClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_transaction_progress_finalize;

  /**
   * FlatpakTransactionProgress::changed:
   * @object: A #FlatpakTransactionProgress
   *
   * Emitted when some detail of the progress object changes, you can call the various methods to get the current status.
   */
  progress_signals[CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
}

static void
got_progress_cb (const char *status,
                 guint       progress,
                 gboolean    estimating,
                 gpointer    user_data)
{
  FlatpakTransactionProgress *p = user_data;

  g_free (p->status);
  p->status = g_strdup (status);
  p->progress = progress;
  p->estimating = estimating;

  if (!p->done)
    g_signal_emit (p, progress_signals[CHANGED], 0);
}

static void
flatpak_transaction_progress_init (FlatpakTransactionProgress *self)
{
  self->status = g_strdup ("Initializing");
  self->estimating = TRUE;
  self->ostree_progress = flatpak_progress_new (got_progress_cb, self);
}

static void
flatpak_transaction_progress_done (FlatpakTransactionProgress *self)
{
  ostree_async_progress_finish (self->ostree_progress);
  self->done = TRUE;
}

static FlatpakTransactionProgress *
flatpak_transaction_progress_new (void)
{
  return g_object_new (FLATPAK_TYPE_TRANSACTION_PROGRESS, NULL);
}

static guint signals[LAST_SIGNAL] = { 0 };

static void initable_iface_init       (GInitableIface      *initable_iface);

G_DEFINE_TYPE_WITH_CODE (FlatpakTransaction, flatpak_transaction, G_TYPE_OBJECT,
                         G_ADD_PRIVATE(FlatpakTransaction)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init))

static gboolean
transaction_is_local_only (FlatpakTransaction *self,
                           FlatpakTransactionOperationType kind)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  return priv->no_pull || kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL;
}

static gboolean
remote_name_is_file (const char *remote_name)
{
  return remote_name != NULL &&
    g_str_has_prefix (remote_name, "file://");
}

/**
 * flatpak_transaction_add_dependency_source:
 * @self: a #FlatpakTransaction
 * @installation: a #FlatpakInstallation
 *
 * Adds an extra installation as a source for application dependencies.
 * This means that applications can be installed in this transaction relying
 * on runtimes from this additional installation (wheres it would normally
 * install required runtimes that are not installed in the installation
 * the transaction works on).
 */
void
flatpak_transaction_add_dependency_source (FlatpakTransaction  *self,
                                           FlatpakInstallation *installation)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  g_ptr_array_add (priv->extra_dependency_dirs,
                   flatpak_installation_clone_dir_noensure (installation));
}

/**
 * flatpak_transaction_add_default_dependency_sources:
 * @self: a #FlatpakTransaction
 *
 * Similar to flatpak_transaction_add_dependency_source(), but adds
 * all the default installations, which means all the defined system-wide
 * (but not per-user) installations.
 */
void
flatpak_transaction_add_default_dependency_sources (FlatpakTransaction  *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GPtrArray) system_dirs = NULL;
  GFile *path = flatpak_dir_get_path (priv->dir);
  int i;

  system_dirs = flatpak_dir_get_system_list (NULL, NULL);
  if (system_dirs == NULL)
    return;

  for (i = 0; i < system_dirs->len; i++)
    {
      FlatpakDir *system_dir = g_ptr_array_index (system_dirs, i);
      GFile *system_path = flatpak_dir_get_path (system_dir);

      if (g_file_equal (path, system_path))
        continue;

      g_ptr_array_add (priv->extra_dependency_dirs, g_object_ref (system_dir));
    }
}

/* Check if the ref is in the dir, or in the extra dependency source dir, in case its a
 * user-dir or another system-wide installation. We want to avoid depending
 * on user-installed things when installing to the system dir.
 */
static gboolean
ref_is_installed (FlatpakTransaction *self,
                  const char *ref,
                  GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GFile) deploy_dir = NULL;
  FlatpakDir *dir = priv->dir;
  int i;

  deploy_dir = flatpak_dir_get_if_deployed (dir, ref, NULL, NULL);
  if (deploy_dir != NULL)
    return TRUE;

  for (i = 0; i < priv->extra_dependency_dirs->len; i++)
    {
      FlatpakDir *dependency_dir = g_ptr_array_index (priv->extra_dependency_dirs, i);

      deploy_dir = flatpak_dir_get_if_deployed (dependency_dir, ref, NULL, NULL);
      if (deploy_dir != NULL)
        return TRUE;
    }

  return FALSE;
}

static gboolean
dir_ref_is_installed (FlatpakDir *dir, const char *ref, char **remote_out, GVariant **deploy_data_out)
{
  g_autoptr(GVariant) deploy_data = NULL;

  deploy_data = flatpak_dir_get_deploy_data (dir, ref, NULL, NULL);
  if (deploy_data == NULL)
    return FALSE;

  if (remote_out)
    *remote_out = g_strdup (flatpak_deploy_data_get_origin (deploy_data));

  if (deploy_data_out)
    *deploy_data_out = g_variant_ref (deploy_data);

  return TRUE;
}

G_DEFINE_TYPE (FlatpakTransactionOperation, flatpak_transaction_operation, G_TYPE_OBJECT)

static void
flatpak_transaction_operation_finalize (GObject *object)
{
  FlatpakTransactionOperation *self = (FlatpakTransactionOperation *)object;

  g_free (self->remote);
  g_free (self->ref);
  g_free (self->commit);
  g_strfreev (self->subpaths);
  g_clear_object (&self->bundle);
  if (self->external_metadata)
    g_bytes_unref (self->external_metadata);
  g_free (self->resolved_commit);
  if (self->resolved_metadata)
    g_bytes_unref (self->resolved_metadata);
  if (self->resolved_metakey)
    g_key_file_unref (self->resolved_metakey);
  if (self->resolved_old_metadata)
    g_bytes_unref (self->resolved_old_metadata);
  if (self->resolved_old_metakey)
    g_key_file_unref (self->resolved_old_metakey);
  g_list_free (self->run_before_ops);

  G_OBJECT_CLASS (flatpak_transaction_operation_parent_class)->finalize (object);
}

static void
flatpak_transaction_operation_class_init (FlatpakTransactionOperationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_transaction_operation_finalize;
}

static void
flatpak_transaction_operation_init (FlatpakTransactionOperation *self)
{
}

static FlatpakTransactionOperation *
flatpak_transaction_operation_new (const char *remote,
                                   const char *ref,
                                   const char **subpaths,
                                   const char *commit,
                                   GFile *bundle,
                                   FlatpakTransactionOperationType kind)
{
  FlatpakTransactionOperation *self;

  self = g_object_new (FLATPAK_TYPE_TRANSACTION_OPERATION, NULL);

  self->remote = g_strdup (remote);
  self->ref = g_strdup (ref);
  self->subpaths = g_strdupv ((char **)subpaths);
  self->commit = g_strdup (commit);
  if (bundle)
    self->bundle = g_object_ref (bundle);
  self->kind = kind;

  return self;
}

FlatpakTransactionOperationType
flatpak_transaction_operation_get_operation_type (FlatpakTransactionOperation  *self)
{
  return self->kind;
}

const char *
flatpak_transaction_operation_get_ref (FlatpakTransactionOperation  *self)
{
  return self->ref;
}

const char *
flatpak_transaction_operation_get_remote (FlatpakTransactionOperation  *self)
{
  return self->remote;
}

/**
 * flatpak_transaction_operation_get_bundle_path:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets the path to the bundle.
 *
 * Returns: (transfer none): the bundle #GFile or %NULL
 */
GFile *
flatpak_transaction_operation_get_bundle_path    (FlatpakTransactionOperation  *self)
{
  return self->bundle;
}

const char *
flatpak_transaction_operation_get_commit (FlatpakTransactionOperation  *self)
{
  return self->resolved_commit;
}

GKeyFile *
flatpak_transaction_operation_get_metadata (FlatpakTransactionOperation  *self)
{
  return self->resolved_metakey;
}

GKeyFile *
flatpak_transaction_operation_get_old_metadata   (FlatpakTransactionOperation  *self)
{
  return self->resolved_old_metakey;
}

gboolean
flatpak_transaction_is_empty (FlatpakTransaction  *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  return priv->ops == NULL;
}

static void
flatpak_transaction_finalize (GObject *object)
{
  FlatpakTransaction *self = (FlatpakTransaction *) object;
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  g_clear_object (&priv->installation);

  g_hash_table_unref (priv->last_op_for_ref);
  g_hash_table_unref (priv->remote_states);
  g_list_free_full (priv->ops, (GDestroyNotify)g_object_unref);
  g_object_unref (priv->dir);

  g_ptr_array_unref (priv->added_origin_remotes);

  g_ptr_array_free (priv->extra_dependency_dirs, TRUE);

  G_OBJECT_CLASS (flatpak_transaction_parent_class)->finalize (object);
}

static void
flatpak_transaction_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  FlatpakTransaction *self = FLATPAK_TRANSACTION (object);
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_INSTALLATION:
      g_clear_object (&priv->installation);
      priv->installation = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static gboolean
signal_accumulator_false_abort (GSignalInvocationHint *ihint,
                                GValue                *return_accu,
                                const GValue          *handler_return,
                                gpointer               dummy)
{
  gboolean continue_emission;
  gboolean signal_continue;

  signal_continue = g_value_get_boolean (handler_return);
  g_value_set_boolean (return_accu, signal_continue);
  continue_emission = signal_continue;

  return continue_emission;
}

static void
flatpak_transaction_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  FlatpakTransaction *self = FLATPAK_TRANSACTION (object);
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_INSTALLATION:
      g_value_set_object (value, priv->installation);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static gboolean
flatpak_transaction_ready (FlatpakTransaction *transaction)
{
  return TRUE;
}

static void
flatpak_transaction_class_init (FlatpakTransactionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  klass->ready = flatpak_transaction_ready;
  object_class->finalize = flatpak_transaction_finalize;
  object_class->get_property = flatpak_transaction_get_property;
  object_class->set_property = flatpak_transaction_set_property;

  g_object_class_install_property (object_class,
                                   PROP_INSTALLATION,
                                   g_param_spec_object ("installation",
                                                        "Installation",
                                                        "The installation instance",
                                                        FLATPAK_TYPE_INSTALLATION,
                                                        G_PARAM_READWRITE|G_PARAM_CONSTRUCT_ONLY|G_PARAM_STATIC_STRINGS));

  /**
   * FlatpakTransaction::new-operation:
   * @object: A #FlatpakTransaction
   * @ref: The ref the operation will be working on
   * @remote: The ref the operation will be working on
   * @bundle: The bundle path (or %NULL)
   * @operation_type: A #FlatpakTransactionOperationType specifying operation type
   * @progress: A #FlatpakTransactionProgress
   */
  signals[NEW_OPERATION] =
    g_signal_new ("new-operation",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, new_operation),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2, FLATPAK_TYPE_TRANSACTION_OPERATION, FLATPAK_TYPE_TRANSACTION_PROGRESS);

  /**
   * FlatpakTransaction::operation-error:
   * @object: A #FlatpakTransaction
   * @ref: The ref the operation was working on
   * @remote: The remote
   * @operation_type: A #FlatpakTransactionOperationType specifying operation type
   * @error: A #GError
   * @details: A #FlatpakTransactionErrorDetails with Details about the error
   *
   * Returns: the %TRUE to contine transaction, %FALSE to stop
   */
  signals[OPERATION_ERROR] =
    g_signal_new ("operation-error",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, operation_error),
                  NULL, NULL,
                  NULL,
                  G_TYPE_BOOLEAN, 3, FLATPAK_TYPE_TRANSACTION_OPERATION, G_TYPE_ERROR, G_TYPE_INT);

  /**
   * FlatpakTransaction::operation-done:
   * @object: A #FlatpakTransaction
   * @ref: The ref the operation was working on
   * @remote: The remote
   * @operation_type: A #FlatpakTransactionOperationType specifying operation type
   * @commit: The new commit checksum
   * @result: A #FlatpakTransactionResult giving details about the result
   *
   */
  signals[OPERATION_DONE] =
    g_signal_new ("operation-done",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, operation_done),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2, FLATPAK_TYPE_TRANSACTION_OPERATION, G_TYPE_INT);

  /**
   * FlatpakTransaction::choose-remote-for-ref:
   * @object: A #FlatpakTransaction
   * @for_ref: The ref we are installing
   * @runtime_ref: The ref we are looking for
   * @remotes: the remotes that has the ref, sorted in prio order
   *
   * Returns: the index of the remote to use, or -1 to not pick one (and fail)
   */
  signals[CHOOSE_REMOTE_FOR_REF] =
    g_signal_new ("choose-remote-for-ref",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, choose_remote_for_ref),
                  NULL, NULL,
                  NULL,
                  G_TYPE_INT, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRV);
  /**
   * FlatpakTransaction::end-of-lifed:
   * @object: A #FlatpakTransaction
   * @ref: The ref we are installing
   * @reason: The eol reason, or %NULL
   * @rebase: The new name, if rebased, or %NULL
   */
  signals[END_OF_LIFED] =
    g_signal_new ("end-of-lifed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, end_of_lifed),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  /**
   * FlatpakTransaction::ready:
   * @object: A #FlatpakTransaction
   *
   * This is is emitted when all the refs involved in the operation. At this point
   * flatpak_transaction_get_operations() will return all the operations that will be
   * executed as part of the transactions. If this returns FALSE, the operation is aborted.
   */
  signals[READY] =
    g_signal_new ("ready",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, ready),
                  signal_accumulator_false_abort, NULL,
                  NULL,
                  G_TYPE_BOOLEAN, 0);
}

static void
flatpak_transaction_init (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->last_op_for_ref = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  priv->remote_states = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)flatpak_remote_state_free);
  priv->added_origin_remotes = g_ptr_array_new_with_free_func (g_free);
  priv->extra_dependency_dirs = g_ptr_array_new_with_free_func (g_object_unref);
}


static gboolean
initable_init (GInitable     *initable,
               GCancellable  *cancellable,
               GError       **error)
{
  FlatpakTransaction *self = FLATPAK_TRANSACTION (initable);
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(FlatpakDir) dir = NULL;

  if (priv->installation == NULL)
    return flatpak_fail (error, "No installation specified");

  dir = flatpak_installation_clone_dir (priv->installation, cancellable, error);
  if (dir == NULL)
    return FALSE;

  priv->dir = g_steal_pointer (&dir);

  return TRUE;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

/**
 * flatpak_transaction_new_for_installation:
 * @installation: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Creates a new #FlatpakTransaction object that can be used to do installation
 * and updates of multiple refs, as well as their dependencies, in a single
 * operation. Set the options you want on the transaction and add the
 * refs you want to install/update, then start the transaction with
 * flatpak_transaction_run ().
 *
 * Returns: (transfer full): a #FlatpakTransaction, or %NULL on failure.
 */
FlatpakTransaction *
flatpak_transaction_new_for_installation (FlatpakInstallation *installation,
                                          GCancellable        *cancellable,
                                          GError             **error)
{
  return g_initable_new (FLATPAK_TYPE_TRANSACTION,
                         cancellable, error,
                         "installation", installation,
                         NULL);
}

void
flatpak_transaction_set_no_pull (FlatpakTransaction  *self,
                                 gboolean             no_pull)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->no_pull = no_pull;
}

void
flatpak_transaction_set_no_deploy (FlatpakTransaction  *self,
                                   gboolean             no_deploy)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  priv->no_deploy = no_deploy;
}

void
flatpak_transaction_set_disable_static_deltas (FlatpakTransaction  *self,
                                               gboolean             disable_static_deltas)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  priv->disable_static_deltas = disable_static_deltas;
}

void
flatpak_transaction_set_disable_prune (FlatpakTransaction  *self,
                                       gboolean             disable_prune)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  priv->disable_prune = disable_prune;
}

void
flatpak_transaction_set_disable_dependencies  (FlatpakTransaction  *self,
                                               gboolean             disable_dependencies)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  priv->disable_deps = disable_dependencies;
}

void
flatpak_transaction_set_disable_related (FlatpakTransaction  *self,
                                         gboolean             disable_related)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  priv->disable_related = disable_related;
}

void
flatpak_transaction_set_reinstall (FlatpakTransaction   *self,
                                   gboolean             reinstall)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  priv->reinstall = reinstall;
}

void
flatpak_transaction_set_force_uninstall (FlatpakTransaction  *self,
                                         gboolean             force_uninstall)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  priv->force_uninstall = force_uninstall;
}

static FlatpakTransactionOperation *
flatpak_transaction_get_last_op_for_ref (FlatpakTransaction *self,
                                         const char *ref)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  FlatpakTransactionOperation *op;

  op = g_hash_table_lookup (priv->last_op_for_ref, ref);

  return op;
}

static char *
subpaths_to_string (const char **subpaths)
{
  GString *s = NULL;
  int i;

  if (subpaths == NULL)
    return g_strdup ("[$old]");

  if (*subpaths == 0)
    return g_strdup ("[*]");

  s = g_string_new ("[");
  for (i = 0; subpaths[i] != NULL; i++)
    {
      if (i != 0)
        g_string_append (s, ", ");
      g_string_append (s, subpaths[i]);
    }
  g_string_append (s, "]");

  return g_string_free (s, FALSE);
}

static const char *
kind_to_str (FlatpakTransactionOperationType kind)
{
  switch ((int)kind)
    {
    case FLATPAK_TRANSACTION_OPERATION_INSTALL:
      return "install";
    case FLATPAK_TRANSACTION_OPERATION_UPDATE:
      return "update";
    case FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE:
      return "install/update";
    case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
      return "install bundle";
    case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
      return "uninstall";
    case FLATPAK_TRANSACTION_OPERATION_LAST_TYPE:
    default:
      return "unknown";
    }
}

static FlatpakRemoteState *
flatpak_transaction_ensure_remote_state (FlatpakTransaction *self,
                                         FlatpakTransactionOperationType kind,
                                         const char *remote,
                                         GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  FlatpakRemoteState *state;

  /* We don't cache local-only states, as we might later need the same state with non-local state */
  if (transaction_is_local_only (self, kind))
    return flatpak_dir_get_remote_state_local_only (priv->dir, remote, NULL, error);

  state = g_hash_table_lookup (priv->remote_states, remote);
  if (state)
    return state;

  state = flatpak_dir_get_remote_state_optional (priv->dir, remote, NULL, error);

  if (state)
    g_hash_table_insert (priv->remote_states, state->remote_name, state);

  return state;
}

static gboolean
kind_compatible (FlatpakTransactionOperationType a,
                 FlatpakTransactionOperationType b)
{
  if (a == b)
    return TRUE;

  if (a == FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE &&
      (b == FLATPAK_TRANSACTION_OPERATION_INSTALL ||
       b == FLATPAK_TRANSACTION_OPERATION_UPDATE))
    return TRUE;

  if (b == FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE &&
      (a == FLATPAK_TRANSACTION_OPERATION_INSTALL ||
       a == FLATPAK_TRANSACTION_OPERATION_UPDATE))
    return TRUE;

  return FALSE;
}

static FlatpakTransactionOperation *
flatpak_transaction_add_op (FlatpakTransaction *self,
                            const char *remote,
                            const char *ref,
                            const char **subpaths,
                            const char *commit,
                            GFile *bundle,
                            FlatpakTransactionOperationType kind)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  FlatpakTransactionOperation *op;
  g_autofree char *subpaths_str = NULL;

  subpaths_str = subpaths_to_string (subpaths);
  g_debug ("Transaction: %s %s:%s%s%s%s",
           kind_to_str (kind), remote, ref,
           commit != NULL ? "@" : "",
           commit != NULL ? commit : "",
           subpaths_str);

  op = flatpak_transaction_get_last_op_for_ref (self, ref);
  if (op != NULL && kind_compatible (kind, op->kind))
    {
      g_auto(GStrv) old_subpaths = NULL;

      old_subpaths = op->subpaths;
      op->subpaths = flatpak_subpaths_merge (old_subpaths, (char **)subpaths);

      return op;
    }

  op = flatpak_transaction_operation_new (remote, ref, subpaths, commit, bundle, kind);
  g_hash_table_insert (priv->last_op_for_ref, g_strdup (ref), op);

  priv->ops = g_list_prepend (priv->ops, op);

  return op;
}

static void
run_operation_before (FlatpakTransactionOperation *op,
                      FlatpakTransactionOperation *before_this,
                      int prio)
{
  if (op == before_this)
    return; /* Don't cause unnecessary loops */
  op->run_before_ops = g_list_prepend (op->run_before_ops, before_this);
  before_this->run_after_count++;
  before_this->run_after_prio = MAX (before_this->run_after_prio, prio);
}

static gboolean
add_related (FlatpakTransaction *self,
             FlatpakTransactionOperation *op,
             GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  FlatpakRemoteState *state = NULL;
  g_autoptr(GPtrArray) related = NULL;
  g_autoptr(GError) local_error = NULL;
  int i;

  if (priv->disable_related)
    return TRUE;

  state = flatpak_transaction_ensure_remote_state (self, op->kind, op->remote, error);
  if (state == NULL)
    return FALSE;

  if (op->resolved_metakey == NULL)
    {
      g_debug ("no resolved metadata for related to %s", op->ref);
      return TRUE;
    }

  if (transaction_is_local_only (self, op->kind))
    related = flatpak_dir_find_local_related_for_metadata (priv->dir, op->ref, op->remote, op->resolved_metakey,
                                                           NULL, &local_error);
  else
    related = flatpak_dir_find_remote_related_for_metadata (priv->dir, state, op->ref, op->resolved_metakey,
                                                            NULL, &local_error);
  if (related == NULL)
    {
      g_message (_("Warning: Problem looking for related refs: %s"), local_error->message);
      return TRUE;
    }

  if (op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    {
      for (i = 0; i < related->len; i++)
        {
          FlatpakRelated *rel = g_ptr_array_index (related, i);
          FlatpakTransactionOperation *related_op;

          if (!rel->delete)
            continue;

          related_op = flatpak_transaction_add_op (self, op->remote, rel->ref,
                                                   NULL, NULL, NULL,
                                                   FLATPAK_TRANSACTION_OPERATION_UNINSTALL);
          related_op->non_fatal = TRUE;
          related_op->fail_if_op_fails = op;
          run_operation_before (op, related_op, 1);
        }
    }
  else /* install or update */
    {
      for (i = 0; i < related->len; i++)
        {
          FlatpakRelated *rel = g_ptr_array_index (related, i);
          FlatpakTransactionOperation *related_op;

          if (!rel->download)
            continue;

          related_op = flatpak_transaction_add_op (self, op->remote, rel->ref,
                                                   (const char **)rel->subpaths,
                                                   NULL, NULL,
                                                   FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE);
          op->non_fatal = TRUE;
          op->fail_if_op_fails = op;
          run_operation_before (op, related_op, 1);
        }
    }

  return TRUE;
}

static char *
find_runtime_remote (FlatpakTransaction *self,
                     const char *app_ref,
                     const char *runtime_ref,
                     FlatpakTransactionOperationType source_kind,
                     GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_auto(GStrv) remotes = NULL;
  const char *app_pref;
  const char *runtime_pref;
  int res = -1;

  app_pref = strchr (app_ref, '/') + 1;
  runtime_pref = strchr (runtime_ref, '/') + 1;

  if (transaction_is_local_only (self, source_kind))
    remotes = flatpak_dir_search_for_local_dependency (priv->dir, runtime_ref, NULL, NULL);
  else
    remotes = flatpak_dir_search_for_dependency (priv->dir, runtime_ref, NULL, NULL);

  if (remotes == NULL || *remotes == NULL)
    {
      flatpak_fail (error, _("The Application %s requires the %s which was not found"),
                    app_pref, runtime_pref);
      return NULL;
    }

  /* In the no-puil case, if only one local ref is available, assume that is the one becasue
     the user chosed it interactively when pulling */
  if (priv->no_pull && g_strv_length (remotes) == 1)
    res = 0;
  else
    g_signal_emit (self, signals[CHOOSE_REMOTE_FOR_REF], 0, app_ref, runtime_ref, remotes, &res);

  if (res >= 0 && res < g_strv_length (remotes))
    return g_strdup (remotes[res]);

  flatpak_fail (error, _("The Application %s requires the %s which is not installed"),
                app_pref, runtime_pref);
  return NULL;
}


static gboolean
add_deps (FlatpakTransaction *self,
          FlatpakTransactionOperation *op,
          GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *runtime_ref = NULL;
  g_autofree char *full_runtime_ref = NULL;
  g_autofree char *runtime_remote = NULL;
  FlatpakTransactionOperation *runtime_op = NULL;

  if (!g_str_has_prefix (op->ref, "app/"))
    return TRUE;

  if (op->resolved_metakey)
    runtime_ref = g_key_file_get_string (op->resolved_metakey, "Application", "runtime", NULL);

  if (runtime_ref == NULL)
    return TRUE;

  full_runtime_ref = g_strconcat ("runtime/", runtime_ref, NULL);

  runtime_op = flatpak_transaction_get_last_op_for_ref (self, full_runtime_ref);

  if (op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    {
      /* If the runtime this app uses is already to be uninstalled, then this uninstall must happen before
         the runtime is installed */
      if (runtime_op && op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
        run_operation_before (op, runtime_op, 1);

      return TRUE;
    }

  if (priv->disable_deps)
    return TRUE;

  if (runtime_op == NULL)
    {
      g_autoptr(GError) local_error = NULL;

      if (!ref_is_installed (self, full_runtime_ref, &local_error))
        {
          if (local_error != NULL)
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          runtime_remote = find_runtime_remote (self, op->ref, full_runtime_ref, op->kind, error);
          if (runtime_remote == NULL)
            return FALSE;

          runtime_op = flatpak_transaction_add_op (self, runtime_remote, full_runtime_ref, NULL, NULL, NULL,
                                                   FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE);
        }
      else
        {
          /* Update if in same dir */
          if (dir_ref_is_installed (priv->dir, full_runtime_ref, &runtime_remote, NULL))
            {
              g_debug ("Updating dependent runtime %s", full_runtime_ref);
              runtime_op = flatpak_transaction_add_op (self, runtime_remote, full_runtime_ref, NULL, NULL, NULL,
                                                       FLATPAK_TRANSACTION_OPERATION_UPDATE);
              runtime_op->non_fatal = TRUE;
            }
        }
    }

  /* Install/Update the runtime before the app */
  if (runtime_op)
    {
      op->fail_if_op_fails = runtime_op;
      run_operation_before (runtime_op, op, 2);
    }

  return TRUE;
}

static gboolean
flatpak_transaction_add_ref (FlatpakTransaction *self,
                             const char *remote,
                             const char *ref,
                             const char **subpaths,
                             const char *commit,
                             FlatpakTransactionOperationType kind,
                             GFile *bundle,
                             const char *external_metadata,
                             GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *origin = NULL;
  const char *pref;
  g_autofree char *origin_remote = NULL;
  FlatpakTransactionOperation *op;

  if (remote_name_is_file (remote))
    {
      g_auto(GStrv) parts = NULL;
      parts = g_strsplit (ref, "/", -1);

      origin_remote = flatpak_dir_create_origin_remote (priv->dir,
                                                        remote, /* uri */
                                                        parts[1],
                                                        "Local repo",
                                                        ref,
                                                        NULL,
                                                        NULL,
                                                        NULL, error);
      if (origin_remote == NULL)
        return FALSE;

      g_ptr_array_add (priv->added_origin_remotes, g_strdup (origin_remote));

      remote = origin_remote;
    }

  pref = strchr (ref, '/') + 1;

  /* install or update */
  if (kind == FLATPAK_TRANSACTION_OPERATION_UPDATE)
    {
      if (!dir_ref_is_installed (priv->dir, ref, &origin, NULL))
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                       _("%s not installed"), pref);
          return FALSE;
        }

      if (flatpak_dir_get_remote_disabled (priv->dir, origin))
        {
          g_debug (_("Remote %s disabled, ignoring %s update"), origin, pref);
          return TRUE;
        }
      remote = origin;
    }
  else if (kind == FLATPAK_TRANSACTION_OPERATION_INSTALL)
    {
      if (!priv->reinstall &&
          dir_ref_is_installed (priv->dir, ref, &origin, NULL))
        {
          if (g_strcmp0 (remote, origin) == 0)
            {
              g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                           _("%s is already installed"), pref);
              return FALSE;
            }
          else
            {
              g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_DIFFERENT_REMOTE,
                           _("%s is already installed from remote %s"), pref, origin);
              return FALSE;
            }
        }
    }
  else if (kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    {
      if (!dir_ref_is_installed (priv->dir, ref, &origin, NULL))
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                       _("%s not installed"), pref);
          return FALSE;
        }

      remote = origin;
    }

  /* This should have been passed int or found out above */
  g_assert (remote != NULL);

  if (flatpak_transaction_ensure_remote_state (self, kind, remote, error) == NULL)
    return FALSE;

  op = flatpak_transaction_add_op (self, remote, ref, subpaths, commit, bundle, kind);
  if (external_metadata)
    op->external_metadata = g_bytes_new (external_metadata, strlen (external_metadata) + 1);

  return TRUE;
}

gboolean
flatpak_transaction_add_install (FlatpakTransaction *self,
                                 const char *remote,
                                 const char *ref,
                                 const char **subpaths,
                                 GError **error)
{
  const char *all_paths[] = { NULL };

  /* If we install with no special args pull all subpaths */
  if (subpaths == NULL)
    subpaths = all_paths;

  return flatpak_transaction_add_ref (self, remote, ref, subpaths, NULL, FLATPAK_TRANSACTION_OPERATION_INSTALL, NULL, NULL, error);
}

gboolean
flatpak_transaction_add_install_bundle (FlatpakTransaction *self,
                                        GFile               *file,
                                        GBytes              *gpg_data,
                                        GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *remote = NULL;
  g_autofree char *ref = NULL;
  g_autofree char *commit = NULL;
  g_autofree char *metadata = NULL;
  gboolean created_remote;

  remote = flatpak_dir_ensure_bundle_remote (priv->dir, file, gpg_data,
                                             &ref, &commit, &metadata, &created_remote,
                                             NULL, error);
  if (remote == NULL)
    return FALSE;

  if (!flatpak_dir_recreate_repo (priv->dir, NULL, error))
    return FALSE;

  return flatpak_transaction_add_ref (self, remote, ref, NULL, commit, FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE, file, metadata, error);
}

gboolean
flatpak_transaction_add_update (FlatpakTransaction *self,
                                const char *ref,
                                const char **subpaths,
                                const char *commit,
                                GError **error)
{
  const char *all_paths[] = { NULL };

  /* If specify an empty subpath, that means all subpaths */
  if (subpaths != NULL && subpaths[0] != NULL && subpaths[0][0] == 0)
    subpaths = all_paths;

  return flatpak_transaction_add_ref (self, NULL, ref, subpaths, commit, FLATPAK_TRANSACTION_OPERATION_UPDATE, NULL, NULL, error);
}

gboolean
flatpak_transaction_add_uninstall (FlatpakTransaction  *self,
                                   const char          *ref,
                                   GError             **error)
{
  return flatpak_transaction_add_ref (self, NULL, ref, NULL, NULL, FLATPAK_TRANSACTION_OPERATION_UNINSTALL, NULL, NULL, error);
}

static gboolean
flatpak_transaction_update_metadata (FlatpakTransaction  *self,
                                     GCancellable        *cancellable,
                                     GError             **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_auto(GStrv) remotes = NULL;
  int i;
  GList *l;
  g_autoptr(GHashTable) ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* Collect all dir+remotes used in this transaction */

  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      g_hash_table_add (ht, g_strdup (op->remote));
    }
  remotes = (char **)g_hash_table_get_keys_as_array (ht, NULL);
  g_hash_table_steal_all (ht); /* Move ownership to remotes */

  /* Update metadata for said remotes */
  for (i = 0; remotes[i] != NULL; i++)
    {
      char *remote = remotes[i];
      g_autoptr(GError) my_error = NULL;

      g_debug ("Updating remote metadata for %s", remote);
      if (!flatpak_dir_update_remote_configuration (priv->dir, remote, cancellable, &my_error))
        g_message (_("Error updating remote metadata for '%s': %s"), remote, my_error->message);
    }

  /* Reload changed configuration */
  if (!flatpak_dir_recreate_repo (priv->dir, cancellable, error))
    return FALSE;

  return TRUE;
}

static void
emit_new_op (FlatpakTransaction *self, FlatpakTransactionOperation *op, FlatpakTransactionProgress *progress)
{
  g_signal_emit (self, signals[NEW_OPERATION], 0, op, progress);
}

static void
emit_op_done (FlatpakTransaction *self,
              FlatpakTransactionOperation *op,
              FlatpakTransactionResult details)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *commit = NULL;

  if (priv->no_deploy)
    commit = flatpak_dir_read_latest (priv->dir, op->remote, op->ref, NULL, NULL, NULL);
  else
    {
      g_autoptr(GVariant) deploy_data = flatpak_dir_get_deploy_data (priv->dir, op->ref, NULL, NULL);
      if (deploy_data)
        commit = g_strdup (flatpak_deploy_data_get_commit (deploy_data));
    }

  g_signal_emit (self, signals[OPERATION_DONE], 0, op, commit, details);
}

static GBytes *
load_deployed_metadata (FlatpakTransaction *self, const char *ref)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autofree char *metadata_contents = NULL;
  gsize metadata_contents_length;

  deploy_dir = flatpak_dir_get_if_deployed (priv->dir, ref, NULL, NULL);
  if (deploy_dir == NULL)
    return NULL;

  metadata_file = g_file_get_child (deploy_dir, "metadata");

  if (!g_file_load_contents (metadata_file, NULL, &metadata_contents, &metadata_contents_length, NULL, NULL))
    {
      g_debug ("No metadata in local deploy of %s", ref);
      return NULL;
    }

  return g_bytes_new_take (g_steal_pointer (&metadata_contents), metadata_contents_length + 1);
}

static void
mark_op_resolved (FlatpakTransactionOperation *op,
                  const char *commit,
                  GBytes *metadata,
                  GBytes *old_metadata)
{
  g_assert (op != NULL);

  if (op->kind != FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    g_assert (commit != NULL);

  op->resolved = TRUE;
  op->resolved_commit = g_strdup (commit);
  if (metadata)
    {
      g_autoptr(GKeyFile) metakey = g_key_file_new ();
      if (g_key_file_load_from_bytes (metakey, metadata, G_KEY_FILE_NONE, NULL))
        {
          op->resolved_metadata = g_bytes_ref (metadata);
          op->resolved_metakey = g_steal_pointer (&metakey);
        }
      else
        g_message ("Warning: Failed to parse metadata for %s\n", op->ref);
    }
  if (old_metadata)
    {
      g_autoptr(GKeyFile) metakey = g_key_file_new ();
      if (g_key_file_load_from_bytes (metakey, old_metadata, G_KEY_FILE_NONE, NULL))
        {
          op->resolved_old_metadata = g_bytes_ref (old_metadata);
          op->resolved_old_metakey = g_steal_pointer (&metakey);
        }
      else
        g_message ("Warning: Failed to parse old metadata for %s\n", op->ref);
    }
}

/* Resolving an operation means figuring out the target commit
   checksum and the metadata for that commit, so that we can handle
   dependencies from it, and verify versions. */
static gboolean
resolve_ops (FlatpakTransaction *self,
             GCancellable *cancellable,
             GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l;

  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      FlatpakRemoteState *state = NULL;
      g_autofree char *checksum = NULL;
      g_autoptr(GVariant) commit_data = NULL;
      g_autoptr(GVariant) commit_metadata = NULL;
      g_autoptr(GBytes) metadata_bytes = NULL;
      g_autoptr(GBytes) old_metadata_bytes = NULL;

      if (op->resolved)
        continue;

      if (op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
        {
          /* We resolve to the deployed metadata, becasue we need it to uninstall related ops */

          metadata_bytes = load_deployed_metadata (self, op->ref);
          mark_op_resolved (op, NULL, metadata_bytes, NULL);
          continue;
        }

      if (op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE)
        {
          g_assert (op->commit != NULL);
          mark_op_resolved (op, op->commit, op->external_metadata, NULL);
          continue;
        }

      /* op->kind is INSTALL or UPDATE */

      state = flatpak_transaction_ensure_remote_state (self, op->kind, op->remote, error);
      if (state == NULL)
        return FALSE;

      /* Should we use local state */
      if (transaction_is_local_only (self, op->kind))
        {
          const char *xa_metadata = NULL;
          commit_data = flatpak_dir_read_latest_commit (priv->dir, op->remote, op->ref, &checksum, NULL, error);
          if (commit_data == NULL)
            return FALSE;

          commit_metadata = g_variant_get_child_value (commit_data, 0);
          g_variant_lookup (commit_metadata, "xa.metadata", "&s", &xa_metadata);
          if (xa_metadata == NULL)
            g_message ("Warning: No xa.metadata in local commit");
          else
            metadata_bytes = g_bytes_new (xa_metadata, strlen (xa_metadata) + 1);
        }
      else
        {
          const char *metadata = NULL;
          g_autoptr(GError) local_error = NULL;

          if (op->commit != NULL)
            checksum = g_strdup (op->commit);
          else
            {
              /* TODO: For the p2p case, calling this for each ref separately is very inefficient */
              if (!flatpak_dir_find_latest_rev (priv->dir, state, op->ref, op->commit, &checksum,
                                                NULL, cancellable, error))
                return FALSE;
            }

          /* TODO: This only gets the metadata for the latest only, we need to handle the case
             where the user specified a commit, or p2p doesn't have the latest commit available */
          if (!flatpak_remote_state_lookup_cache (state, op->ref, NULL, NULL, &metadata, &local_error))
            {
              g_message (_("Warning: Can't find %s metadata for dependencies: %s"), op->ref, local_error->message);
              g_clear_error (&local_error);
            }
          else
            metadata_bytes = g_bytes_new (metadata, strlen (metadata) + 1);
        }

      old_metadata_bytes = load_deployed_metadata (self, op->ref);

      mark_op_resolved (op, checksum, metadata_bytes, old_metadata_bytes);
    }

  return TRUE;
}

static int
compare_op_ref (FlatpakTransactionOperation *a, FlatpakTransactionOperation *b)
{
  char *aa = strchr (a->ref, '/');
  char *bb = strchr (b->ref, '/');
  return g_strcmp0 (aa, bb);
}

static int
compare_op_prio (FlatpakTransactionOperation *a, FlatpakTransactionOperation *b)
{
  return b->run_after_prio - a->run_after_prio;
}

static void
sort_ops (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *sorted = NULL;
  GList *remaining;
  GList *runnable = NULL;
  GList *l, *next;

  remaining = priv->ops;
  priv->ops = NULL;

  /* First mark runnable all jobs that depend on nothing.
     Note that this seesntially reverses the original list, so these
     are in the same order as specified */
  for (l = remaining; l != NULL; l = next)
    {
      FlatpakTransactionOperation *op = l->data;
      next = l->next;

      if (op->run_after_count == 0)
        {
          remaining = g_list_remove_link (remaining, l);
          runnable = g_list_concat (l, runnable);
        }
    }

  /* If no other order, start in alphabetical ref-order */
  runnable = g_list_sort (runnable, (GCompareFunc)compare_op_ref);

  while (runnable)
    {
      GList *run = runnable;
      FlatpakTransactionOperation *run_op = run->data;

      /* Put the first runnable on the sorted list */
      runnable = g_list_remove_link (runnable, run);
      sorted = g_list_concat (run, sorted); /* prepends, so reverse at the end */

      /* Then greedily run ops that become runnable, in run_after_prio order, so that
         related ops are run before depdendencies */
      run_op->run_before_ops = g_list_sort (run_op->run_before_ops, (GCompareFunc)compare_op_prio);
      for (l = run_op->run_before_ops; l != NULL; l = l->next)
        {
          FlatpakTransactionOperation *after_op = l->data;
          after_op->run_after_count--;
          if (after_op->run_after_count == 0)
            {
              GList *after_l = g_list_find (remaining, after_op);
              g_assert (after_l != NULL);
              remaining = g_list_remove_link (remaining, after_l);
              runnable = g_list_concat (after_l, runnable);
            }
        }
    }

  if (remaining != NULL)
    {
      g_warning ("ops remaining after sort, maybe there is a dependency loop?");
      sorted = g_list_concat (remaining, sorted);
    }

  priv->ops = g_list_reverse (sorted);
}

/**
 * flatpak_transaction_get_operations:
 * @self: a #FlatpakTransaction
 *
 * Gets the list of operations.
 *
 * Returns: (transfer full) (element-type FlatpakTransactionOperation): a #GList of operations
 */
GList *
flatpak_transaction_get_operations (FlatpakTransaction  *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l;
  GList *non_skipped = NULL;

  non_skipped = NULL;
  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      if (!op->skip)
        non_skipped = g_list_prepend (non_skipped, g_object_ref (op));
    }
  return g_list_reverse (non_skipped);
}

/**
 * flatpak_transaction_get_current_operation:
 * @self: a #FlatpakTransaction
 *
 * Gets the current operation.
 *
 * Returns: (transfer full): the current #FlatpakTransactionOperation
 */
FlatpakTransactionOperation *
flatpak_transaction_get_current_operation (FlatpakTransaction  *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  return g_object_ref (priv->current_op);
}

gboolean
flatpak_transaction_run (FlatpakTransaction *self,
                         GCancellable *cancellable,
                         GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l, *next;
  gboolean succeeded = TRUE;
  gboolean needs_prune = FALSE;
  gboolean needs_triggers = FALSE;
  g_autoptr(GMainContextPopDefault) main_context = NULL;
  gboolean ready_res = FALSE;
  int i;

  priv->current_op = NULL;

  if (!priv->no_pull &&
      !flatpak_transaction_update_metadata (self, cancellable, error))
    return FALSE;

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = flatpak_main_context_new_default ();

  /* Resolve initial ops */
  if (!resolve_ops (self, cancellable, error))
    return FALSE;

  /* Add all app -> runtime dependencies */
  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;

      if (!add_deps (self, op, error))
        return FALSE;
    }

  /* Resolve new ops */
  if (!resolve_ops (self, cancellable, error))
    return FALSE;

  /* Add all related extensions */
  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;

      if (!add_related (self, op, error))
        return FALSE;
    }

  /* Resolve new ops */
  if (!resolve_ops (self, cancellable, error))
    return FALSE;

  sort_ops (self);

  /* Ensure the operation kind is normalized and not no-op */
  for (l = priv->ops; l != NULL; l = next)
    {
      FlatpakTransactionOperation *op = l->data;
      next = l->next;

      if (op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE)
        {
          g_autoptr(GVariant) deploy_data = NULL;

          if (dir_ref_is_installed (priv->dir, op->ref, NULL, &deploy_data))
            {
              /* Don't use the remote from related ref on update, always use
                 the current remote. */
              g_free (op->remote);
              op->remote = g_strdup (flatpak_deploy_data_get_origin (deploy_data));

              op->kind = FLATPAK_TRANSACTION_OPERATION_UPDATE;
            }
          else
            op->kind = FLATPAK_TRANSACTION_OPERATION_INSTALL;
        }

      /* Skip no-op updates */
      if (op->kind == FLATPAK_TRANSACTION_OPERATION_UPDATE &&
          !flatpak_dir_needs_update_for_commit_and_subpaths (priv->dir, op->remote, op->ref, op->resolved_commit,
                                                             (const char **)op->subpaths))
        op->skip = TRUE;
    }


  g_signal_emit (self, signals[READY], 0, &ready_res);
  if (!ready_res)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED, _("Aborted by user"));
      return FALSE;
    }

  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      g_autoptr(GError) local_error = NULL;
      gboolean res = TRUE;
      const char *pref;
      FlatpakTransactionOperationType kind;
      FlatpakRemoteState *state;

      if (op->skip)
        continue;

      priv->current_op = op;

      kind = op->kind;
      pref = strchr (op->ref, '/') + 1;

      if (op->fail_if_op_fails && (op->fail_if_op_fails->failed) &&
          /* Allow installing an app if the runtime failed to update (i.e. is installed) because
           * the app should still run, and otherwise you could never install the app until the runtime
           * remote is fixed. */
          !(op->fail_if_op_fails->kind == FLATPAK_TRANSACTION_OPERATION_UPDATE && g_str_has_prefix (op->ref, "app/")))
        {
          g_set_error (&local_error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED,
                       _("Skipping %s due to previous error"), pref);
          res = FALSE;
        }
      else if ((state = flatpak_transaction_ensure_remote_state (self, op->kind, op->remote, &local_error)) == NULL)
        {
          res = FALSE;
        }
      else if (kind == FLATPAK_TRANSACTION_OPERATION_INSTALL)
        {
          g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();

          emit_new_op (self, op, progress);

          g_assert (op->resolved_commit != NULL); /* We resolved this before */

          if (op->resolved_metakey && !flatpak_check_required_version (op->ref, op->resolved_metakey, error))
            res = FALSE;
          else
            res = flatpak_dir_install (priv->dir,
                                       priv->no_pull,
                                       priv->no_deploy,
                                       priv->disable_static_deltas,
                                       priv->reinstall,
                                       state, op->ref, op->resolved_commit,
                                       (const char **)op->subpaths,
                                       progress->ostree_progress,
                                       cancellable, &local_error);

          flatpak_transaction_progress_done (progress);
          if (res)
            {
              emit_op_done (self, op, 0);

              /* Normally we don't need to prune after install, because it makes no old objects
                 stale. However if we reinstall, that is not true. */
              if (!priv->no_pull && priv->reinstall)
                needs_prune = TRUE;

              if (g_str_has_prefix (op->ref, "app"))
                needs_triggers = TRUE;
            }
        }
      else if (kind == FLATPAK_TRANSACTION_OPERATION_UPDATE)
        {
          g_assert (op->resolved_commit != NULL); /* We resolved this before */

          if (flatpak_dir_needs_update_for_commit_and_subpaths (priv->dir, op->remote, op->ref, op->resolved_commit,
                                                                (const char **)op->subpaths))
            {
              g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();
              FlatpakTransactionResult result_details = 0;

              emit_new_op (self, op, progress);

              if (op->resolved_metakey && !flatpak_check_required_version (op->ref, op->resolved_metakey, error))
                res = FALSE;
              else
                res = flatpak_dir_update (priv->dir,
                                          priv->no_pull,
                                          priv->no_deploy,
                                          priv->disable_static_deltas,
                                          op->commit != NULL, /* Allow downgrade if we specify commit */
                                          state, op->ref, op->resolved_commit,
                                          NULL,
                                          (const char **)op->subpaths,
                                          progress->ostree_progress,
                                          cancellable, &local_error);
              flatpak_transaction_progress_done (progress);

              /* Handle noop-updates */
              if (!res && g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
                {
                  res = TRUE;
                  g_clear_error (&local_error);

                  result_details |= FLATPAK_TRANSACTION_RESULT_NO_CHANGE;
                }

              if (res)
                {
                  emit_op_done (self, op, result_details);

                  if (!priv->no_pull)
                    needs_prune = TRUE;

                  if (g_str_has_prefix (op->ref, "app"))
                    needs_triggers = TRUE;
                }
            }
          else
            g_debug ("%s need no update", op->ref);
        }
      else if (kind == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE)
        {
          g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();
          emit_new_op (self, op, progress);
          if (op->resolved_metakey && !flatpak_check_required_version (op->ref, op->resolved_metakey, error))
            res = FALSE;
          else
            res = flatpak_dir_install_bundle (priv->dir, op->bundle,
                                              op->remote, NULL,
                                              cancellable, &local_error);
          flatpak_transaction_progress_done (progress);

          if (res)
            {
              emit_op_done (self, op, 0);
              needs_prune = TRUE;
              needs_triggers = TRUE;
            }
        }
      else if (kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
        {
          g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();
          FlatpakHelperUninstallFlags flags = 0;

          if (priv->disable_prune)
            flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF;

          if (priv->force_uninstall)
            flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE;

          emit_new_op (self, op, progress);

          res = flatpak_dir_uninstall (priv->dir, op->ref, flags,
                                       cancellable, &local_error);

          flatpak_transaction_progress_done (progress);

          if (res)
            {
              emit_op_done (self, op, 0);
              needs_prune = TRUE;

              if (g_str_has_prefix (op->ref, "app"))
                needs_triggers = TRUE;
            }
        }
      else
        g_assert_not_reached ();

      if (res)
        {
          g_autoptr(GVariant) deploy_data = NULL;
          deploy_data = flatpak_dir_get_deploy_data (priv->dir, op->ref, NULL, NULL);

          if (deploy_data)
            {
              const char *eol =  flatpak_deploy_data_get_eol (deploy_data);
              const char *eol_rebase = flatpak_deploy_data_get_eol_rebase (deploy_data);

              if (eol || eol_rebase)
                g_signal_emit (self, signals[END_OF_LIFED], 0,
                               op->ref, eol, eol_rebase);
            }
        }

      if (!res)
        {
          gboolean do_cont = FALSE;
          FlatpakTransactionErrorDetails error_details = 0;

          op->failed = TRUE;

          if (op->non_fatal)
            error_details |= FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL;

          g_signal_emit (self, signals[OPERATION_ERROR], 0, op,
                         local_error, error_details,
                         &do_cont);

          if (!do_cont)
            {
              g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED,
                           _("Aborted due to failure"));
              succeeded = FALSE;
              break;
            }
        }
    }
  priv->current_op = NULL;

  if (needs_triggers)
    flatpak_dir_run_triggers (priv->dir, cancellable, NULL);

  if (needs_prune && !priv->disable_prune)
    flatpak_dir_prune (priv->dir, cancellable, NULL);

  for (i = 0; i < priv->added_origin_remotes->len; i++)
    flatpak_dir_prune_origin_remote (priv->dir, g_ptr_array_index (priv->added_origin_remotes, i));

  return succeeded;
}
