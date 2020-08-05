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
#include <glib/gi18n-lib.h>
#include <libsoup/soup.h>

#include "flatpak-auth-private.h"
#include "flatpak-error.h"
#include "flatpak-installation-private.h"
#include "flatpak-progress-private.h"
#include "flatpak-transaction-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-variant-impl-private.h"

/**
 * SECTION:flatpak-transaction
 * @Title: FlatpakTransaction
 * @Short_description: Transaction information
 *
 * FlatpakTransaction is an object representing an install/update/uninstall
 * transaction. You create an object like this using flatpak_transaction_new_for_installation()
 * and then you add all the operations (installs, updates, etc) you wish to do. Then
 * you start the transaction with flatpak_transaction_run() which will resolve all kinds
 * of dependencies and report progress and status while downloading and installing these.
 *
 * The dependency resolution that is the first step of executing a transaction can
 * be influenced by flatpak_transaction_set_disable_dependencies(),
 * flatpak_transaction_set_disable_related(), flatpak_transaction_add_dependency_source()
 * and flatpak_transaction_add_default_dependency_sources().
 *
 * The underlying operations that get orchestrated by a FlatpakTransaction are: pulling
 * new data from remote repositories, deploying newer applications or runtimes and pruning
 * old deployments. Which of these operations are carried out can be controlled with
 * flatpak_transaction_set_no_pull(), flatpak_transaction_set_no_deploy() and
 * flatpak_transaction_set_disable_prune().
 *
 * A transaction is a blocking operation, and all signals are emitted in the same thread.
 * This means you should either handle the signals directly (say, by doing blocking console
 * interaction, or by just returning without interaction), or run the operation in a separate
 * thread and do your own forwarding to the GUI thread.
 *
 * Despite the name, a FlatpakTransaction is more like a batch operation than a transaction
 * in the database sense. Individual operations are carried out sequentially, and are atomic.
 * They become visible to the system as they are completed. When an error occurs, already
 * completed operations are not rolled back.
 *
 * For each operation that is executed during a transaction, you first get a
 * #FlatpakTransaction::new-operation signal, followed by either a
 * #FlatpakTransaction::operation-done or #FlatpakTransaction::operation-error.

 * The FlatpakTransaction API is threadsafe in the sense that it is safe to run two
 * transactions at the same time, in different threads (or processes).
 *
 * Note: Transactions (or any other install/update operation) to a
 * system installation rely on the ability to create files that are readable
 * by other users. Some users set a umask that prohibits this. Unfortunately
 * there is no good way to work around this in a threadsafe, local way, so
 * such setups will break by default. The flatpak commandline app works
 * around this by calling umask(022) in the early setup, and it is recommended
 * that other apps using libflatpak do this too.
 */

/* This is an internal-only element of FlatpakTransactionOperationType */
#define FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE FLATPAK_TRANSACTION_OPERATION_LAST_TYPE + 1

enum {
  RUNTIME_UPDATE,
  RUNTIME_INSTALL,
  APP_UPDATE,
  APP_INSTALL
};

struct _FlatpakTransactionOperation
{
  GObject                         parent;

  char                           *remote;
  char                           *ref;
  /* NULL means unspecified (normally keep whatever was there before), [] means force everything */
  char                          **subpaths;
  char                          **previous_ids;
  char                           *commit;
  GFile                          *bundle;
  GBytes                         *external_metadata;
  FlatpakTransactionOperationType kind;
  gboolean                        non_fatal;
  gboolean                        failed;
  gboolean                        skip;
  gboolean                        update_only_deploy;

  gboolean                        resolved;
  char                           *resolved_commit;
  GFile                          *resolved_sideload_path;
  GBytes                         *resolved_metadata;
  GKeyFile                       *resolved_metakey;
  GBytes                         *resolved_old_metadata;
  GKeyFile                       *resolved_old_metakey;
  char                           *resolved_token;
  gboolean                        requested_token; /* TRUE if we requested a token. value in resolved_token, but may be NULL if token not needed. */
  guint64                         download_size;
  guint64                         installed_size;
  char                           *eol;
  char                           *eol_rebase;
  gint32                          token_type;
  GVariant                       *summary_metadata; /* Additional metadatafield for commit from summary */
  int                             run_after_count;
  int                             run_after_prio; /* Higher => run later (when it becomes runnable). Used to run related ops (runtime extensions) before deps (apps using the runtime) */
  GList                          *run_before_ops;
  FlatpakTransactionOperation    *fail_if_op_fails; /* main app/runtime for related extensions, runtime for apps */
  /* main app/runtime for related extensions, app for runtimes; could be multiple
   * related-to-ops if this op is for a runtime which is needed by multiple apps
   * in the transaction: */
  GPtrArray                      *related_to_ops;  /* (element-type FlatpakTransactionOperation) (nullable) */
};

typedef struct _FlatpakTransactionPrivate FlatpakTransactionPrivate;

typedef struct _BundleData                BundleData;

struct _BundleData
{
  GFile  *file;
  GBytes *gpg_data;
};

typedef struct {
  FlatpakTransaction *transaction;
  const char *remote;
  FlatpakAuthenticatorRequest *request;
  gboolean done;
  guint response;
  GVariant *results;
} RequestData;

struct _FlatpakTransactionPrivate
{
  GObject                      parent;

  FlatpakInstallation         *installation;
  FlatpakDir                  *dir;
  GHashTable                  *last_op_for_ref;
  GHashTable                  *remote_states; /* (element-type utf8 FlatpakRemoteState) */
  GPtrArray                   *extra_dependency_dirs;
  GPtrArray                   *extra_sideload_repos;
  GList                       *ops;
  GPtrArray                   *added_origin_remotes;
  GPtrArray                   *added_pinned_runtimes;

  GList                       *flatpakrefs; /* GKeyFiles */
  GList                       *bundles; /* BundleData */

  guint                        next_request_id;
  guint                        active_request_id;
  RequestData                 *active_request;

  FlatpakTransactionOperation *current_op;

  char                        *parent_window;
  gboolean                     no_pull;
  gboolean                     no_deploy;
  gboolean                     disable_static_deltas;
  gboolean                     disable_prune;
  gboolean                     disable_deps;
  gboolean                     disable_related;
  gboolean                     reinstall;
  gboolean                     force_uninstall;
  gboolean                     can_run;
  char                        *default_arch;
  guint                        max_op;

  gboolean                     needs_resolve;
  gboolean                     needs_tokens;
};

enum {
  NEW_OPERATION,
  OPERATION_DONE,
  OPERATION_ERROR,
  CHOOSE_REMOTE_FOR_REF,
  END_OF_LIFED,
  END_OF_LIFED_WITH_REBASE,
  READY,
  ADD_NEW_REMOTE,
  WEBFLOW_START,
  WEBFLOW_DONE,
  BASIC_AUTH_START,
  INSTALL_AUTHENTICATOR,
  LAST_SIGNAL
};

enum {
  PROP_0,
  PROP_INSTALLATION,
};

struct _FlatpakTransactionProgress
{
  GObject              parent;

  FlatpakProgress     *progress_obj;
};

enum {
  CHANGED,
  LAST_PROGRESS_SIGNAL
};

static void flatpak_transaction_normalize_ops (FlatpakTransaction *self);
static gboolean request_required_tokens (FlatpakTransaction *self,
                                         const char         *optional_remote,
                                         GCancellable       *cancellable,
                                         GError            **error);


static BundleData *
bundle_data_new (GFile  *file,
                 GBytes *gpg_data)
{
  BundleData *data = g_new0 (BundleData, 1);

  data->file = g_object_ref (file);
  if (gpg_data)
    data->gpg_data = g_bytes_ref (gpg_data);

  return data;
}

static void
bundle_data_free (BundleData *data)
{
  g_clear_object (&data->file);
  g_clear_object (&data->gpg_data);
  g_free (data);
}

static guint progress_signals[LAST_SIGNAL] = { 0 };

/**
 * SECTION:flatpak-transaction-progress
 * @Title: FlatpakTransactionProgress
 * @Short_description: Progress of an operation
 *
 * FlatpakTransactionProgress is an object that represents the progress
 * of a single operation in a transaction. You obtain a FlatpakTransactionProgress
 * with the #FlatpakTransaction::new-operation signal.
 */

G_DEFINE_TYPE (FlatpakTransactionProgress, flatpak_transaction_progress, G_TYPE_OBJECT)

/**
 * flatpak_transaction_progress_set_update_frequency:
 * @self: a #FlatpakTransactionProgress
 * @update_interval: the update interval, in milliseconds
 *
 * Sets how often progress should be updated.
 */
void
flatpak_transaction_progress_set_update_frequency (FlatpakTransactionProgress *self,
                                                   guint                       update_interval)
{
  flatpak_progress_set_update_interval (self->progress_obj, update_interval);
}

/**
 * flatpak_transaction_progress_get_status:
 * @self: a #FlatpakTransactionProgress
 *
 * Gets the current status string
 *
 * Returns: (transfer full): the current status
 */
char *
flatpak_transaction_progress_get_status (FlatpakTransactionProgress *self)
{
  return g_strdup (flatpak_progress_get_status (self->progress_obj));
}

/**
 * flatpak_transaction_progress_get_is_estimating:
 * @self: a #FlatpakTransactionProgress
 *
 * Gets whether the progress is currently estimating
 *
 * Returns: whether we're estimating
 */
gboolean
flatpak_transaction_progress_get_is_estimating (FlatpakTransactionProgress *self)
{
  return flatpak_progress_get_estimating (self->progress_obj);
}

/**
 * flatpak_transaction_progress_get_progress:
 * @self: a #FlatpakTransactionProgress
 *
 * Gets the current progress.
 *
 * Returns: the current progress, as an integer between 0 and 100
 */
int
flatpak_transaction_progress_get_progress (FlatpakTransactionProgress *self)
{
  return flatpak_progress_get_progress (self->progress_obj);
}

/**
 * flatpak_transaction_progress_get_bytes_transferred:
 * @self: a #FlatpakTransactionProgress
 *
 * Gets the number of bytes that have been transferred.
 *
 * Returns: the number of bytes transferred
 * Since: 1.1.2
 */
guint64
flatpak_transaction_progress_get_bytes_transferred (FlatpakTransactionProgress *self)
{
  guint64 bytes_transferred, transferred_extra_data_bytes;

  bytes_transferred = flatpak_progress_get_bytes_transferred (self->progress_obj);
  transferred_extra_data_bytes = flatpak_progress_get_transferred_extra_data_bytes (self->progress_obj);

  return bytes_transferred + transferred_extra_data_bytes;
}

/**
 * flatpak_transaction_progress_get_start_time:
 * @self: a #FlatpakTransactionProgress
 *
 * Gets the time at which this operation has started, as monotonic time.
 *
 * Returns: the start time
 * Since: 1.1.2
 */
guint64
flatpak_transaction_progress_get_start_time (FlatpakTransactionProgress *self)
{
  return flatpak_progress_get_start_time (self->progress_obj);
}

static void
flatpak_transaction_progress_finalize (GObject *object)
{
  FlatpakTransactionProgress *self = (FlatpakTransactionProgress *) object;

  g_object_unref (self->progress_obj);

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

  if (!flatpak_progress_is_done (p->progress_obj))
    g_signal_emit (p, progress_signals[CHANGED], 0);
}

static void
flatpak_transaction_progress_init (FlatpakTransactionProgress *self)
{
  self->progress_obj = flatpak_progress_new (got_progress_cb, self);
}

static void
flatpak_transaction_progress_done (FlatpakTransactionProgress *self)
{
  flatpak_progress_done (self->progress_obj);
}

static FlatpakTransactionProgress *
flatpak_transaction_progress_new (void)
{
  return g_object_new (FLATPAK_TYPE_TRANSACTION_PROGRESS, NULL);
}

static guint signals[LAST_SIGNAL] = { 0 };

static void initable_iface_init (GInitableIface *initable_iface);

G_DEFINE_TYPE_WITH_CODE (FlatpakTransaction, flatpak_transaction, G_TYPE_OBJECT,
                         G_ADD_PRIVATE (FlatpakTransaction)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init))

static gboolean
transaction_is_local_only (FlatpakTransaction             *self,
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
 * on runtimes from this additional installation (whereas it would normally
 * install required runtimes that are not installed in the installation
 * the transaction works on).
 *
 * Also see flatpak_transaction_add_default_dependency_sources().
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
 * flatpak_transaction_add_sideload_repo:
 * @self: a #FlatpakTransaction
 * @path: a path to a local flatpak repository
 *
 * Adds an extra local ostree repo as source for installation. This is
 * equivalent to using the sideload-repos directories (see flatpak(1)), but can
 * be done dynamically. Any path added here is used in addition to ones in
 * those directories.
 *
 * Since: 1.7.1
 */
void
flatpak_transaction_add_sideload_repo (FlatpakTransaction  *self,
                                       const char          *path)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  g_ptr_array_add (priv->extra_sideload_repos,
                   g_strdup (path));
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
flatpak_transaction_add_default_dependency_sources (FlatpakTransaction *self)
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
                  const char         *ref,
                  GError            **error)
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
dir_ref_is_installed (FlatpakDir *dir, const char *ref, char **remote_out, GBytes **deploy_data_out)
{
  g_autoptr(GBytes) deploy_data = NULL;

  deploy_data = flatpak_dir_get_deploy_data (dir, ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);
  if (deploy_data == NULL)
    return FALSE;

  if (remote_out)
    *remote_out = g_strdup (flatpak_deploy_data_get_origin (deploy_data));

  if (deploy_data_out)
    *deploy_data_out = g_bytes_ref (deploy_data);

  return TRUE;
}

/**
 * SECTION:flatpak-transaction-operation
 * @Title: FlatpakTransactionOperation
 * @Short_description: Operation in a transaction
 *
 * FlatpakTransactionOperation is an object that represents a single operation
 * in a transaction. You receive a FlatpakTransactionOperation object with the
 * #FlatpakTransaction::new-operation signal.
 */

G_DEFINE_TYPE (FlatpakTransactionOperation, flatpak_transaction_operation, G_TYPE_OBJECT)

static void
flatpak_transaction_operation_finalize (GObject *object)
{
  FlatpakTransactionOperation *self = (FlatpakTransactionOperation *) object;

  g_free (self->remote);
  g_free (self->ref);
  g_free (self->commit);
  g_strfreev (self->subpaths);
  g_clear_object (&self->bundle);
  g_free (self->eol);
  g_free (self->eol_rebase);
  if (self->previous_ids)
    g_strfreev (self->previous_ids);
  if (self->external_metadata)
    g_bytes_unref (self->external_metadata);
  g_free (self->resolved_commit);
  if (self->resolved_sideload_path)
    g_object_unref (self->resolved_sideload_path);
  if (self->resolved_metadata)
    g_bytes_unref (self->resolved_metadata);
  if (self->resolved_metakey)
    g_key_file_unref (self->resolved_metakey);
  if (self->resolved_old_metadata)
    g_bytes_unref (self->resolved_old_metadata);
  if (self->resolved_old_metakey)
    g_key_file_unref (self->resolved_old_metakey);
  g_free (self->resolved_token);
  g_list_free (self->run_before_ops);
  if (self->related_to_ops)
    g_ptr_array_unref (self->related_to_ops);
  if (self->summary_metadata)
    g_variant_unref (self->summary_metadata);

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
flatpak_transaction_operation_new (const char                     *remote,
                                   const char                     *ref,
                                   const char                    **subpaths,
                                   const char                    **previous_ids,
                                   const char                     *commit,
                                   GFile                          *bundle,
                                   FlatpakTransactionOperationType kind)
{
  FlatpakTransactionOperation *self;

  self = g_object_new (FLATPAK_TYPE_TRANSACTION_OPERATION, NULL);

  self->remote = g_strdup (remote);
  self->ref = g_strdup (ref);
  self->subpaths = g_strdupv ((char **) subpaths);
  self->previous_ids = g_strdupv ((char **) previous_ids);
  self->commit = g_strdup (commit);
  if (bundle)
    self->bundle = g_object_ref (bundle);
  self->kind = kind;

  return self;
}

/**
 * flatpak_transaction_operation_get_operation_type:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets the type of the operation.
 *
 * Returns: the type of operation, as #FlatpakTransactionOperationType
 */
FlatpakTransactionOperationType
flatpak_transaction_operation_get_operation_type (FlatpakTransactionOperation *self)
{
  return self->kind;
}

/**
 * flatpak_transaction_operation_get_ref:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets the ref that the operation applies to.
 *
 * Returns: (transfer none): the ref
 */
const char *
flatpak_transaction_operation_get_ref (FlatpakTransactionOperation *self)
{
  return self->ref;
}

/**
 * flatpak_transaction_operation_get_related_to_ops:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets the operations which caused this operation to be added to the
 * transaction. In the case of a runtime, it's the apps whose runtime it is (and
 * this could be multiple apps, if they all require the same runtime). In
 * the case of a related ref such as an extension, it's the main app or
 * runtime. In the case of a main app or something added to the transaction by
 * flatpak_transaction_add_ref(), %NULL or an empty array will be returned.
 *
 * Note that an op will be returned even if it’s marked as to be skipped when
 * the transaction is run. Check that using
 * flatpak_transaction_operation_get_is_skipped().
 *
 * Elements in the returned array are only safe to access while the parent
 * #FlatpakTransaction is alive.
 *
 * Returns: (transfer none) (element-type FlatpakTransactionOperation) (nullable): the
 *   #FlatpakTransactionOperations this one is related to (may be %NULL or an
 *   empty array, which are equivalent)
 * Since: 1.7.3
 */
GPtrArray *
flatpak_transaction_operation_get_related_to_ops (FlatpakTransactionOperation *self)
{
  return self->related_to_ops;
}

static void
flatpak_transaction_operation_add_related_to_op (FlatpakTransactionOperation *op,
                                                 FlatpakTransactionOperation *related_op)
{
  if (op->related_to_ops == NULL)
    op->related_to_ops = g_ptr_array_new ();
  g_ptr_array_add (op->related_to_ops, related_op);
}

/**
 * flatpak_transaction_operation_get_is_skipped:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets whether this operation will be skipped when the transaction is run.
 * Operations are skipped in some transaction situations, for example when an
 * app has reached end of life and needs a rebase, or when it would have been
 * updated but no update is available. By default, skipped
 * operations are not returned by flatpak_transaction_get_operations() — but
 * they can be accessed by traversing the operation graph using
 * flatpak_transaction_operation_get_related_to_ops().
 *
 * Returns: %TRUE if the operation has been marked as to skip, %FALSE otherwise
 * Since: 1.7.3
 */
gboolean
flatpak_transaction_operation_get_is_skipped (FlatpakTransactionOperation *self)
{
  return self->skip;
}

/**
 * flatpak_transaction_operation_get_remote:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets the remote that the operation applies to.
 *
 * Returns: (transfer none): the remote
 */
const char *
flatpak_transaction_operation_get_remote (FlatpakTransactionOperation *self)
{
  return self->remote;
}

/**
 * flatpak_transaction_operation_type_to_string:
 * @kind: a #FlatpakTransactionOperationType
 *
 * Converts the operation type to a string.
 *
 * Returns: (transfer none): a string representing @kind
 */
const char *
flatpak_transaction_operation_type_to_string (FlatpakTransactionOperationType kind)
{
  if (kind == FLATPAK_TRANSACTION_OPERATION_INSTALL)
    return "install";
  if (kind == FLATPAK_TRANSACTION_OPERATION_UPDATE)
    return "update";
  if (kind == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE)
    return "install-bundle";
  if (kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    return "uninstall";
  return NULL;
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
flatpak_transaction_operation_get_bundle_path (FlatpakTransactionOperation *self)
{
  return self->bundle;
}

/**
 * flatpak_transaction_operation_get_commit:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets the commit ID for the operation.
 *
 * This information is available when the transaction is resolved,
 * i.e. when #FlatpakTransaction::ready is emitted.
 *
 * Returns: (transfer none): the commit ID
 */
const char *
flatpak_transaction_operation_get_commit (FlatpakTransactionOperation *self)
{
  return self->resolved_commit;
}

/**
 * flatpak_transaction_operation_get_download_size:
 * @self: a #flatpakTransactionOperation
 *
 * Gets the maximum download size for the operation.
 *
 * Note that this does not include the size of dependencies, and
 * the actual download may be smaller, if some of the data is already
 * available locally.
 *
 * For uninstall operations, this returns 0.
 *
 * This information is available when the transaction is resolved,
 * i.e. when #FlatpakTransaction::ready is emitted.
 *
 * Returns: the download size, in bytes
 * Since: 1.1.2
 */
guint64
flatpak_transaction_operation_get_download_size (FlatpakTransactionOperation *self)
{
  return self->download_size;
}

/**
 * flatpak_transaction_operation_get_installed_size:
 * @self: a #flatpakTransactionOperation
 *
 * Gets the installed size for the operation.
 *
 * Note that even for a new install, the extra space required on
 * disk may be smaller than this number, if some of the data is already
 * available locally.
 *
 * For uninstall operations, this returns 0.
 *
 * This information is available when the transaction is resolved,
 * i.e. when #FlatpakTransaction::ready is emitted.
 *
 * Returns: the installed size, in bytes
 * Since: 1.1.2
 */
guint64
flatpak_transaction_operation_get_installed_size (FlatpakTransactionOperation *self)
{
  return self->installed_size;
}

/**
 * flatpak_transaction_operation_get_metadata:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets the metadata that will be applicable when the
 * operation is done.
 *
 * This can be compared to the current metadata returned
 * by flatpak_transaction_operation_get_old_metadata()
 * to find new required permissions and similar changes.
 *
 * This information is available when the transaction is resolved,
 * i.e. when #FlatpakTransaction::ready is emitted.
 *
 * Returns: (transfer none): the metadata #GKeyFile
 */
GKeyFile *
flatpak_transaction_operation_get_metadata (FlatpakTransactionOperation *self)
{
  return self->resolved_metakey;
}

/**
 * flatpak_transaction_operation_get_old_metadata:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets the metadata current metadata for the ref that @self works on.
 * Also see flatpak_transaction_operation_get_metadata().
 *
 * This information is available when the transaction is resolved,
 * i.e. when #FlatpakTransaction::ready is emitted.
 *
 * Returns: (transfer none): the old metadata #GKeyFile
 */
GKeyFile *
flatpak_transaction_operation_get_old_metadata (FlatpakTransactionOperation *self)
{
  return self->resolved_old_metakey;
}

/**
 * flatpak_transaction_is_empty:
 * @self: a #FlatpakTransaction
 *
 * Returns whether the transaction contains any non-skipped operations.
 *
 * Returns: %TRUE if the transaction is empty
 */
gboolean
flatpak_transaction_is_empty (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l;

  for (l = priv->ops; l; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;

      if (!op->skip)
        return FALSE;
    }

  return TRUE;
}

static void
flatpak_transaction_finalize (GObject *object)
{
  FlatpakTransaction *self = (FlatpakTransaction *) object;
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  g_clear_object (&priv->installation);

  g_free (priv->parent_window);
  g_list_free_full (priv->flatpakrefs, (GDestroyNotify) g_key_file_unref);
  g_list_free_full (priv->bundles, (GDestroyNotify) bundle_data_free);
  g_free (priv->default_arch);
  g_hash_table_unref (priv->last_op_for_ref);
  g_hash_table_unref (priv->remote_states);
  g_list_free_full (priv->ops, (GDestroyNotify) g_object_unref);
  g_clear_object (&priv->dir);

  g_ptr_array_unref (priv->added_origin_remotes);
  g_ptr_array_unref (priv->added_pinned_runtimes);

  g_ptr_array_free (priv->extra_dependency_dirs, TRUE);
  g_ptr_array_free (priv->extra_sideload_repos, TRUE);

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

static gboolean
flatpak_transaction_add_new_remote (FlatpakTransaction            *transaction,
                                    FlatpakTransactionRemoteReason reason,
                                    const char                    *from_id,
                                    const char                    *suggested_remote_name,
                                    const char                    *url)
{
  return FALSE;
}

static void
flatpak_transaction_install_authenticator  (FlatpakTransaction *transaction,
                                            const char         *remote,
                                            const char         *authenticator_ref)
{
}

static gboolean flatpak_transaction_real_run (FlatpakTransaction *transaction,
                                              GCancellable       *cancellable,
                                              GError            **error);

static void
flatpak_transaction_class_init (FlatpakTransactionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  klass->ready = flatpak_transaction_ready;
  klass->add_new_remote = flatpak_transaction_add_new_remote;
  klass->install_authenticator = flatpak_transaction_install_authenticator;
  klass->run = flatpak_transaction_real_run;
  object_class->finalize = flatpak_transaction_finalize;
  object_class->get_property = flatpak_transaction_get_property;
  object_class->set_property = flatpak_transaction_set_property;

  /**
   * FlatpakTransaction:installation:
   *
   * The installation that the transaction operates on.
   */
  g_object_class_install_property (object_class,
                                   PROP_INSTALLATION,
                                   g_param_spec_object ("installation",
                                                        "Installation",
                                                        "The installation instance",
                                                        FLATPAK_TYPE_INSTALLATION,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  /**
   * FlatpakTransaction::new-operation:
   * @object: A #FlatpakTransaction
   * @operation: The new #FlatpakTransactionOperation
   * @progress: A #FlatpakTransactionProgress for @operation
   *
   * The ::new-operation signal gets emitted during the execution of
   * the transaction when a new operation is beginning.
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
   * @operation: The #FlatpakTransactionOperation which failed
   * @error: A #GError
   * @details: (type FlatpakTransactionErrorDetails): A #FlatpakTransactionErrorDetails with details about the error
   *
   * The ::operation-error signal gets emitted when an error occurs during the
   * execution of the transaction.
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
   * @operation: The #FlatpakTransactionOperation which finished
   * @commit: The commit
   * @result: (type FlatpakTransactionResult): A #FlatpakTransactionResult giving details about the result
   *
   * The ::operation-done signal gets emitted during the execution of
   * the transaction when an operation is finished.
   */
  signals[OPERATION_DONE] =
    g_signal_new ("operation-done",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, operation_done),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 3, FLATPAK_TYPE_TRANSACTION_OPERATION, G_TYPE_STRING, G_TYPE_INT);

  /**
   * FlatpakTransaction::choose-remote-for-ref:
   * @object: A #FlatpakTransaction
   * @for_ref: The ref we are installing
   * @runtime_ref: The ref we are looking for
   * @remotes: the remotes that has the ref, sorted in prio order
   *
   * The ::choose-remote-for-ref signal gets emitted when a
   * remote needs to be selected during the execution of the transaction.
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
   *
   * The ::end-of-lifed signal gets emitted when a ref is found to
   * be marked as end-of-life during the execution of the transaction.
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
   * FlatpakTransaction::end-of-lifed-with-rebase:
   * @object: A #FlatpakTransaction
   * @remote: The remote for the ref we are processing
   * @ref: The ref we are processing
   * @reason: The eol reason, or %NULL
   * @rebased_to_ref: The new name, if rebased, or %NULL
   * @previous_ids: The previous names for the rebased ref (if any), including the one from @ref
   *
   * The ::end-of-lifed-with-rebase signal gets emitted when a ref is found
   * to be marked as end-of-life before the transaction begins. Unlike
   * #FlatpakTransaction::end-of-lifed, this signal allows for the
   * transaction to be modified in order to e.g. install the rebased
   * ref.
   *
   * Returns: %TRUE if the operation on this end-of-lifed ref should
   * be skipped, %FALSE if it should remain.
   */
  signals[END_OF_LIFED_WITH_REBASE] =
    g_signal_new ("end-of-lifed-with-rebase",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, end_of_lifed_with_rebase),
                  NULL, NULL,
                  NULL,
                  G_TYPE_BOOLEAN, 5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRV);

  /**
   * FlatpakTransaction::ready:
   * @object: A #FlatpakTransaction
   *
   * The ::ready signal is emitted when all the refs involved in the operation
   * have been resolved to commits. At this point flatpak_transaction_get_operations()
   * will return all the operations that will be executed as part of the
   * transaction.
   *
   * Returns: %TRUE to carry on with the transaction, %FALSE to abort
   */
  signals[READY] =
    g_signal_new ("ready",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, ready),
                  signal_accumulator_false_abort, NULL,
                  NULL,
                  G_TYPE_BOOLEAN, 0);

  /**
   * FlatpakTransaction::add-new-remote:
   * @object: A #FlatpakTransaction
   * @reason: (type FlatpakTransactionRemoteReason): A #FlatpakTransactionRemoteReason for this suggestion
   * @from_id: The id of the app/runtime
   * @suggested_remote_name: The suggested remote name
   * @url: The repo url
   *
   * The ::add-new-remote signal gets emitted if, as part of the transaction,
   * it is required or recommended that a new remote is added, for the reason
   * described in @reason.
   *
   * Returns: %TRUE to add the remote
   */
  signals[ADD_NEW_REMOTE] =
    g_signal_new ("add-new-remote",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, add_new_remote),
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  G_TYPE_BOOLEAN, 4, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  /**
   * FlatpakTransaction::install-authenticator:
   * @object: A #FlatpakTransaction
   * @remote: The remote name
   * @authenticator_ref: The ref for the authenticator
   *
   * The ::install-authenticator signal gets emitted if, as part of
   * resolving the transaction, we need to use an authenticator, but the authentication
   * is not installed, but is available to be installed from the ref.
   *
   * The application can handle this signal, and if so create another transaction
   * to install the authenticator.
   *
   * The default handler does nothing, and if the authenticator is not installed when
   * the signal handler fails the transaction will error out.
   *
   * Since: 1.7.4
   */
  signals[INSTALL_AUTHENTICATOR] =
    g_signal_new ("install-authenticator",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, install_authenticator),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  /**
   * FlatpakTransaction::webflow-start:
   * @object: A #FlatpakTransaction
   * @remote: The remote we're authenticating with
   * @url: The url to show
   * @options: Extra options, currently unused
   * @id: The id of the operation, can be used to cancel it
   *
   * The ::webflow-start signal gets emitted when some kind of user
   * authentication is needed during the operation. If the caller handles this
   * it should show the url in a webbrowser and return %TRUE. This will
   * eventually cause the webbrowser to finish the authentication operation and
   * operation will continue, as signaled by the webflow-done being emitted.
   *
   * If the client does not support webflow then return %FALSE from this signal
   * (or don't implement it). This will abort the authentication and likely
   * result in the transaction failing (unless the authentication was somehow
   * optional).
   *
   * During the time between webflow-start and webflow-done the client can call
   * flatpak_transaction_abort_webflow() to manually abort the authentication.
   * This is useful if the user aborted the authentication operation some way,
   * like e.g. closing the browser window.
   *
   * Since: 1.5.1
   */
  signals[WEBFLOW_START] =
    g_signal_new ("webflow-start",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, webflow_start),
                  NULL, NULL,
                  NULL,
                  G_TYPE_BOOLEAN, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_VARIANT, G_TYPE_INT);
  /**
   * FlatpakTransaction::webflow-done:
   * @object: A #FlatpakTransaction
   * @options: Extra options, currently unused
   * @id: The id of the operation
   *
   * The ::webflow-done signal gets emitted when the authentication
   * finished the webflow, independent of the reason and results.  If
   * you for were showing a web-browser window it can now be closed.
   *
   * Since: 1.5.1
   */
  signals[WEBFLOW_DONE] =
    g_signal_new ("webflow-done",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, webflow_done),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2, G_TYPE_VARIANT, G_TYPE_INT);
  /**
   * FlatpakTransaction::basic-auth-start:
   * @object: A #FlatpakTransaction
   * @remote: The remote we're authenticating with
   * @realm: The url to show
   * @options: Extra options, currently unused
   * @id: The id of the operation, can be used to finish it
   *
   * The ::basic-auth-start signal gets emitted when a basic user/password
   * authentication is needed during the operation. If the caller handles this
   * it should ask the user for the user and password and return %TRUE. Once
   * the information is gathered call flatpak_transaction_complete_basic_auth()
   * with it.
   *
   * If the client does not support basic auth then return %FALSE from this signal
   * (or don't implement it). This will abort the authentication and likely
   * result in the transaction failing (unless the authentication was somehow
   * optional).
   *
   * Since: 1.5.2
   */
  signals[BASIC_AUTH_START] =
    g_signal_new ("basic-auth-start",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (FlatpakTransactionClass, basic_auth_start),
                  NULL, NULL,
                  NULL,
                  G_TYPE_BOOLEAN, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_VARIANT, G_TYPE_INT);

}

static void
flatpak_transaction_init (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->last_op_for_ref = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  priv->remote_states = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) flatpak_remote_state_unref);
  priv->added_origin_remotes = g_ptr_array_new_with_free_func (g_free);
  priv->added_pinned_runtimes = g_ptr_array_new_with_free_func (g_free);
  priv->extra_dependency_dirs = g_ptr_array_new_with_free_func (g_object_unref);
  priv->extra_sideload_repos = g_ptr_array_new_with_free_func (g_free);
  priv->can_run = TRUE;
}


static gboolean
initable_init (GInitable    *initable,
               GCancellable *cancellable,
               GError      **error)
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

/**
 * flatpak_transaction_set_no_pull:
 * @self: a #FlatpakTransaction
 * @no_pull: whether to avoid pulls
 *
 * Sets whether the transaction should operate only on locally
 * available data.
 */
void
flatpak_transaction_set_no_pull (FlatpakTransaction *self,
                                 gboolean            no_pull)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->no_pull = no_pull;
}

/**
 * flatpak_transaction_get_no_pull:
 * @self: a #FlatpakTransaction
 *
 * Gets whether the transaction should operate only on locally
 * available data.
 *
 * Returns: %TRUE if no_pull is set, %FALSE otherwise
 *
 * Since: 1.5.1
 */
gboolean
flatpak_transaction_get_no_pull (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  return priv->no_pull;
}

/**
 * flatpak_transaction_set_parent_window:
 * @self: a #FlatpakTransaction
 * @parent_window: whether to avoid pulls
 *
 * Sets the parent window (if any) to use for any UI show by this transaction.
 * This is used by authenticators if they need to interact with the user during
 * authentication.
 *
 * The format of this string depends on the display system in use, and is the
 * same as used by xdg-desktop-portal.
 *
 * On X11 it should be of the form x11:$xid where $xid is the hex
 * version of the xwindows id.
 *
 * On wayland is should be wayland:$handle where handle is gotten by
 * using the export call of the xdg-foreign-unstable wayland extension.
 *
 * Since: 1.5.1
 */
void
flatpak_transaction_set_parent_window (FlatpakTransaction *self,
                                       const char *parent_window)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  g_free (priv->parent_window);
  priv->parent_window = g_strdup (parent_window);
}

/**
 * flatpak_transaction_get_parent_window:
 * @self: a #FlatpakTransaction
 *
 * Gets the parent window set for this transaction, or %NULL if unset. See
 * flatpak_transaction_get_parent_window().
 *
 * Returns: (transfer none): a window name, or %NULL
 *
 * Since: 1.5.1
 */
const char *
flatpak_transaction_get_parent_window (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  return priv->parent_window;
}

/**
 * flatpak_transaction_set_no_deploy:
 * @self: a #FlatpakTransaction
 * @no_deploy: whether to avoid deploying
 *
 * Sets whether the transaction should download updates, but
 * not deploy them.
 */
void
flatpak_transaction_set_no_deploy (FlatpakTransaction *self,
                                   gboolean            no_deploy)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->no_deploy = no_deploy;
}

/**
 * flatpak_transaction_get_no_deploy:
 * @self: a #FlatpakTransaction
 *
 * Gets whether the transaction is only downloading updates,
 * and not deploying them.
 *
 * Returns: %TRUE if no_deploy is set, %FALSE otherwise
 *
 * Since: 1.5.1
 */
gboolean
flatpak_transaction_get_no_deploy (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  return priv->no_deploy;
}

/**
 * flatpak_transaction_set_disable_static_deltas:
 * @self: a #FlatpakTransaction
 * @disable_static_deltas: whether to avoid static deltas
 *
 * Sets whether the transaction should avoid using static
 * deltas when pulling.
 */
void
flatpak_transaction_set_disable_static_deltas (FlatpakTransaction *self,
                                               gboolean            disable_static_deltas)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->disable_static_deltas = disable_static_deltas;
}

/**
 * flatpak_transaction_set_disable_prune:
 * @self: a #FlatpakTransaction
 * @disable_prune: whether to avoid pruning
 *
 * Sets whether the transaction should avoid pruning the local OSTree
 * repository after updating.
 */
void
flatpak_transaction_set_disable_prune (FlatpakTransaction *self,
                                       gboolean            disable_prune)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->disable_prune = disable_prune;
}

/**
 * flatpak_transaction_set_disable_dependencies:
 * @self: a #FlatpakTransaction
 * @disable_dependencies: whether to disable runtime dependencies
 *
 * Sets whether the transaction should ignore runtime dependencies
 * when resolving operations for applications.
 */
void
flatpak_transaction_set_disable_dependencies (FlatpakTransaction *self,
                                              gboolean            disable_dependencies)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->disable_deps = disable_dependencies;
}

/**
 * flatpak_transaction_set_disable_related:
 * @self: a #FlatpakTransaction
 * @disable_related: whether to avoid adding related refs
 *
 * Sets whether the transaction should avoid adding related refs
 * when resolving operations. Related refs are extensions that are
 * suggested by apps, such as locales.
 */
void
flatpak_transaction_set_disable_related (FlatpakTransaction *self,
                                         gboolean            disable_related)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->disable_related = disable_related;
}

/**
 * flatpak_transaction_set_reinstall:
 * @self: a #FlatpakTransaction
 * @reinstall: whether to reinstall refs
 *
 * Sets whether the transaction should uninstall first if a
 * ref is already installed.
 */
void
flatpak_transaction_set_reinstall (FlatpakTransaction *self,
                                   gboolean            reinstall)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->reinstall = reinstall;
}

/**
 * flatpak_transaction_set_no_interaction:
 * @self: a #FlatpakTransaction
 * @no_interaction: Whether to disallow interactive authorization for operations
 *
 * This method can be used to prevent interactive authorization dialogs to appear
 * for operations on @self. This is useful for background operations that are not
 * directly triggered by a user action.
 *
 * By default, the setting from the parent #FlatpakInstallation is used.
 *
 * Since: 1.7.3
 */
void
flatpak_transaction_set_no_interaction (FlatpakTransaction *self,
                                        gboolean            no_interaction)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  flatpak_dir_set_no_interaction (priv->dir, no_interaction);
}

/**
 * flatpak_transaction_set_force_uninstall:
 * @self: a #FlatpakTransaction
 * @force_uninstall: whether to force-uninstall refs
 *
 * Sets whether the transaction should uninstall files even
 * if they're used by a running application.
 */
void
flatpak_transaction_set_force_uninstall (FlatpakTransaction *self,
                                         gboolean            force_uninstall)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->force_uninstall = force_uninstall;
}

/**
 * flatpak_transaction_set_default_arch:
 * @self: a #FlatpakTransaction
 * @arch: the arch to make default
 *
 * Sets the architecture to default to where it is unspecified.
 */
void
flatpak_transaction_set_default_arch (FlatpakTransaction *self,
                                      const char         *arch)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  g_free (priv->default_arch);
  priv->default_arch = g_strdup (arch);
}

static FlatpakTransactionOperation *
flatpak_transaction_get_last_op_for_ref (FlatpakTransaction *self,
                                         const char         *ref)
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
  switch ((int) kind)
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
flatpak_transaction_ensure_remote_state (FlatpakTransaction             *self,
                                         FlatpakTransactionOperationType kind,
                                         const char                     *remote,
                                         GError                        **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  FlatpakRemoteState *state;

  /* We don't cache local-only states, as we might later need the same state with non-local state */
  if (transaction_is_local_only (self, kind))
    return flatpak_dir_get_remote_state_local_only (priv->dir, remote, NULL, error);

  state = g_hash_table_lookup (priv->remote_states, remote);
  if (state)
    return flatpak_remote_state_ref (state);

  state = flatpak_dir_get_remote_state_optional (priv->dir, remote, FALSE, NULL, error);

  if (state)
    {
      g_hash_table_insert (priv->remote_states, state->remote_name, flatpak_remote_state_ref (state));

      for (int i = 0; i < priv->extra_sideload_repos->len; i++)
        {
          const char *path = g_ptr_array_index (priv->extra_sideload_repos, i);
          g_autoptr(GFile) f = g_file_new_for_path (path);
          flatpak_remote_state_add_sideload_repo (state, f);
        }
    }

  return state;
}

static gboolean
kind_compatible (FlatpakTransactionOperationType a,
                 FlatpakTransactionOperationType b,
                 gboolean                        b_is_rebase)
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

  /* If b is a rebase, the only reason it exists is so that the ref's previous-ids can be
     updated. Therefore, it can be folded into any other install or update operation. */
  if (b_is_rebase &&
      (a == FLATPAK_TRANSACTION_OPERATION_INSTALL ||
       a == FLATPAK_TRANSACTION_OPERATION_UPDATE ||
       a == FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE))
    return TRUE;

  return FALSE;
}

static FlatpakTransactionOperation *
flatpak_transaction_add_op (FlatpakTransaction             *self,
                            const char                     *remote,
                            const char                     *ref,
                            const char                    **subpaths,
                            const char                    **previous_ids,
                            const char                     *commit,
                            GFile                          *bundle,
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
  /* If previous_ids is given, then this is a rebase operation. */
  if (op != NULL && kind_compatible (kind, op->kind, previous_ids != NULL))
    {
      g_auto(GStrv) old_subpaths = NULL;
      g_auto(GStrv) old_previous_ids = NULL;

      old_subpaths = op->subpaths;
      op->subpaths = flatpak_subpaths_merge (old_subpaths, (char **) subpaths);

      old_previous_ids = op->previous_ids;
      op->previous_ids = flatpak_strv_merge (old_previous_ids, (char **) previous_ids);

      return op;
    }

  op = flatpak_transaction_operation_new (remote, ref, subpaths, previous_ids, commit, bundle, kind);
  g_hash_table_insert (priv->last_op_for_ref, g_strdup (ref), op);

  priv->ops = g_list_prepend (priv->ops, op);

  priv->needs_resolve = TRUE;

  return op;
}

static void
run_operation_before (FlatpakTransactionOperation *op,
                      FlatpakTransactionOperation *before_this,
                      int                          prio)
{
  if (op == before_this)
    return; /* Don't cause unnecessary loops */
  op->run_before_ops = g_list_prepend (op->run_before_ops, before_this);
  before_this->run_after_count++;
  before_this->run_after_prio = MAX (before_this->run_after_prio, prio);
}

static gboolean
add_related (FlatpakTransaction          *self,
             FlatpakTransactionOperation *op,
             GError                     **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autoptr(GPtrArray) related = NULL;
  g_autoptr(GError) local_error = NULL;
  int i;

  if (priv->disable_related)
    return TRUE;

  if (op->kind != FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    {
      state = flatpak_transaction_ensure_remote_state (self, op->kind, op->remote, error);
      if (state == NULL)
        return FALSE;
    }

  if (op->resolved_metakey == NULL)
    {
      g_debug ("no resolved metadata for related to %s", op->ref);
      return TRUE;
    }

  if (transaction_is_local_only (self, op->kind))
    related = flatpak_dir_find_local_related_for_metadata (priv->dir, op->ref, op->resolved_commit, op->remote, op->resolved_metakey,
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
                                                   NULL, NULL, NULL, NULL,
                                                   FLATPAK_TRANSACTION_OPERATION_UNINSTALL);
          related_op->non_fatal = TRUE;
          related_op->fail_if_op_fails = op;
          flatpak_transaction_operation_add_related_to_op (related_op, op);
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
                                                   (const char **) rel->subpaths,
                                                   NULL, NULL, NULL,
                                                   FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE);
          related_op->non_fatal = TRUE;
          related_op->fail_if_op_fails = op;
          flatpak_transaction_operation_add_related_to_op (related_op, op);
          run_operation_before (related_op, op, 1);
        }
    }

  return TRUE;
}

static char *
find_runtime_remote (FlatpakTransaction             *self,
                     const char                     *app_ref,
                     const char                     *app_remote,
                     const char                     *runtime_ref,
                     FlatpakTransactionOperationType source_kind,
                     GError                        **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_auto(GStrv) remotes = NULL;
  const char *app_pref;
  const char *runtime_pref;
  int res = -1;

  app_pref = strchr (app_ref, '/') + 1;
  runtime_pref = strchr (runtime_ref, '/') + 1;

  /* Here we are passing along app_remote so it gets priority */
  if (transaction_is_local_only (self, source_kind))
    remotes = flatpak_dir_search_for_local_dependency (priv->dir, app_remote, runtime_ref, NULL, NULL);
  else
    remotes = flatpak_dir_search_for_dependency (priv->dir, app_remote, runtime_ref, NULL, NULL);

  if (remotes == NULL || *remotes == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_RUNTIME_NOT_FOUND,
                          _("The application %s requires the runtime %s which was not found"),
                          app_pref, runtime_pref);
      return NULL;
    }

  /* In the no-pull case, if only one local ref is available, assume that is the one because
     the user chose it interactively when pulling */
  if (priv->no_pull && g_strv_length (remotes) == 1)
    res = 0;
  else
    g_signal_emit (self, signals[CHOOSE_REMOTE_FOR_REF], 0, app_ref, runtime_ref, remotes, &res);

  if (res >= 0 && res < g_strv_length (remotes))
    return g_strdup (remotes[res]);

  flatpak_fail_error (error, FLATPAK_ERROR_RUNTIME_NOT_FOUND,
                      _("The application %s requires the runtime %s which is not installed"),
                      app_pref, runtime_pref);
  return NULL;
}


static gboolean
add_deps (FlatpakTransaction          *self,
          FlatpakTransactionOperation *op,
          GError                     **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *runtime_ref = NULL;
  g_autofree char *full_runtime_ref = NULL;
  g_autofree char *runtime_remote = NULL;
  FlatpakTransactionOperation *runtime_op = NULL;

  if (!op->resolved_metakey)
    return TRUE;

  /* Generally only app needs runtimes dependencies, not dependencies because you don't run extensions directly.
     However if the extension has extra data (and doesn't define NoRuntime) its also needed so we can run the
     apply-extra script. */
  if (g_str_has_prefix (op->ref, "app/"))
    runtime_ref = g_key_file_get_string (op->resolved_metakey, "Application", "runtime", NULL);
  else if (g_key_file_has_group (op->resolved_metakey, "Extra Data") &&
           !g_key_file_get_boolean (op->resolved_metakey, "Extra Data", "NoRuntime", NULL))
    runtime_ref = g_key_file_get_string (op->resolved_metakey, "ExtensionOf", "runtime", NULL);

  if (runtime_ref == NULL)
    return TRUE;

  full_runtime_ref = g_strconcat ("runtime/", runtime_ref, NULL);

  runtime_op = flatpak_transaction_get_last_op_for_ref (self, full_runtime_ref);

  if (op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    {
      /* If the runtime this app uses is already to be uninstalled, then this uninstall must happen before
         the runtime is uninstalled */
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

          runtime_remote = find_runtime_remote (self, op->ref, op->remote, full_runtime_ref, op->kind, error);
          if (runtime_remote == NULL)
            return FALSE;

          runtime_op = flatpak_transaction_add_op (self, runtime_remote, full_runtime_ref, NULL, NULL, NULL, NULL,
                                                   FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE);
        }
      else
        {
          /* Update if in same dir */
          if (dir_ref_is_installed (priv->dir, full_runtime_ref, &runtime_remote, NULL))
            {
              g_debug ("Updating dependent runtime %s", full_runtime_ref);
              runtime_op = flatpak_transaction_add_op (self, runtime_remote, full_runtime_ref, NULL, NULL, NULL, NULL,
                                                       FLATPAK_TRANSACTION_OPERATION_UPDATE);
              runtime_op->non_fatal = TRUE;
            }
        }
    }

  /* Install/Update the runtime before the app */
  if (runtime_op)
    {
      if (runtime_op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
        return flatpak_fail_error (error, FLATPAK_ERROR_RUNTIME_USED,
                                   _("Can't uninstall %s which is needed by %s"),
                                   runtime_op->ref, op->ref);

      op->fail_if_op_fails = runtime_op;
      flatpak_transaction_operation_add_related_to_op (runtime_op, op);
      run_operation_before (runtime_op, op, 2);
    }

  return TRUE;
}

static gboolean
flatpak_transaction_add_ref (FlatpakTransaction             *self,
                             const char                     *remote,
                             const char                     *ref,
                             const char                    **subpaths,
                             const char                    **previous_ids,
                             const char                     *commit,
                             FlatpakTransactionOperationType kind,
                             GFile                          *bundle,
                             const char                     *external_metadata,
                             GError                        **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *origin = NULL;
  g_auto(GStrv) parts = NULL;
  g_auto(GStrv) merged_subpaths = NULL;
  const char *pref;
  g_autofree char *origin_remote = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  FlatpakTransactionOperation *op;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;

  if (remote_name_is_file (remote))
    {
      gboolean changed_config;
      origin_remote = flatpak_dir_create_origin_remote (priv->dir,
                                                        remote, /* uri */
                                                        parts[1],
                                                        "Local repo",
                                                        ref,
                                                        NULL,
                                                        NULL,
                                                        &changed_config,
                                                        NULL, error);
      if (origin_remote == NULL)
        return FALSE;

      /* Reload changed configuration */
      if (changed_config)
        flatpak_installation_drop_caches (priv->installation, NULL, NULL);

      g_ptr_array_add (priv->added_origin_remotes, g_strdup (origin_remote));

      remote = origin_remote;
    }

  /* safe because flatpak_decompose_ref() has validated ref */
  pref = strchr (ref, '/') + 1;

  /* install or update */
  if (kind == FLATPAK_TRANSACTION_OPERATION_UPDATE)
    {
      g_autoptr(GBytes) deploy_data = NULL;

      if (!dir_ref_is_installed (priv->dir, ref, &origin, &deploy_data))
        return flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED,
                                   _("%s not installed"), pref);

      if (flatpak_dir_get_remote_disabled (priv->dir, origin))
        {
          g_debug (_("Remote %s disabled, ignoring %s update"), origin, pref);
          return TRUE;
        }
      remote = origin;

      /* As stated in the documentation for flatpak_transaction_add_update(),
       * for locale extensions we merge existing subpaths with the set of
       * configured languages, to match the behavior of add_related().
       */
      if (subpaths == NULL && g_str_has_suffix (parts[1], ".Locale"))
        {
          g_autofree const char **old_subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
          g_auto(GStrv) extra_subpaths = flatpak_dir_get_locale_subpaths (priv->dir);
          merged_subpaths = flatpak_subpaths_merge ((char **)old_subpaths, extra_subpaths);
          subpaths = (const char **)merged_subpaths;
        }
    }
  else if (kind == FLATPAK_TRANSACTION_OPERATION_INSTALL)
    {
      if (!priv->reinstall &&
          dir_ref_is_installed (priv->dir, ref, &origin, NULL))
        {
          if (g_strcmp0 (remote, origin) == 0)
            return flatpak_fail_error (error, FLATPAK_ERROR_ALREADY_INSTALLED,
                                       _("%s is already installed"), pref);
          else
            return flatpak_fail_error (error, FLATPAK_ERROR_DIFFERENT_REMOTE,
                                       _("%s is already installed from remote %s"),
                                       pref, origin);
        }
    }
  else if (kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    {
      if (!dir_ref_is_installed (priv->dir, ref, &origin, NULL))
        return flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED,
                                   _("%s not installed"), pref);

      remote = origin;
    }

  /* This should have been passed in or found out above */
  g_assert (remote != NULL);

  /* We don't need remote state for an uninstall, and we don't want a missing
   * remote to be fatal */
  if (kind != FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    {
      state = flatpak_transaction_ensure_remote_state (self, kind, remote, error);
      if (state == NULL)
        return FALSE;
    }

  op = flatpak_transaction_add_op (self, remote, ref, subpaths, previous_ids, commit, bundle, kind);

  if (external_metadata)
    op->external_metadata = g_bytes_new (external_metadata, strlen (external_metadata) + 1);

  return TRUE;
}

/**
 * flatpak_transaction_add_install:
 * @self: a #FlatpakTransaction
 * @remote: the name of the remote
 * @ref: the ref
 * @subpaths: (nullable) (array zero-terminated=1): subpaths to install, or the
 *  empty list or %NULL to pull all subpaths
 * @error: return location for a #GError
 *
 * Adds installing the given ref to this transaction.
 *
 * The @remote can either be a configured remote of the installation,
 * or a file:// uri pointing at a local repository to install from,
 * in which case an origin remote is created.
 *
 * Returns: %TRUE on success; %FALSE with @error set on failure.
 */
gboolean
flatpak_transaction_add_install (FlatpakTransaction *self,
                                 const char         *remote,
                                 const char         *ref,
                                 const char        **subpaths,
                                 GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  const char *all_paths[] = { NULL };

  g_return_val_if_fail (ref != NULL, FALSE);
  g_return_val_if_fail (remote != NULL, FALSE);

  /* If we install with no special args pull all subpaths */
  if (subpaths == NULL)
    subpaths = all_paths;

  if (!flatpak_transaction_add_ref (self, remote, ref, subpaths, NULL, NULL, FLATPAK_TRANSACTION_OPERATION_INSTALL, NULL, NULL, error))
    return FALSE;

  /* Pin runtimes that are installed explicitly rather than pulled as
   * dependencies so they are not automatically removed. */
  if (g_str_has_prefix (ref, "runtime/"))
    {
      gboolean already_pinned;

      if (!flatpak_dir_config_append_pattern (priv->dir, "pinned", ref, TRUE, &already_pinned, error))
        return FALSE;

      if (!already_pinned)
        {
          g_ptr_array_add (priv->added_pinned_runtimes, g_strdup (ref));
          flatpak_installation_drop_caches (priv->installation, NULL, NULL);
        }
    }

  return TRUE;
}

/**
 * flatpak_transaction_add_rebase:
 * @self: a #FlatpakTransaction
 * @remote: the name of the remote
 * @ref: the ref
 * @subpaths: (nullable): the subpaths to include, or %NULL to install the complete ref
 * @previous_ids: (nullable) (array zero-terminated=1): Previous ids to add to the
 *     given ref. These should simply be the ids, not the full ref names (e.g. org.foo.Bar,
 *     not org.foo.Bar/x86_64/master).
 * @error: return location for a #GError
 *
 * Adds updating the @previous_ids of the given ref to this transaction, via either
 * installing the @ref if it was not already present. The will treat @ref as the
 * result of following an eol-rebase, and data migration from the refs in
 * @previous_ids will be set up.
 *
 * See flatpak_transaction_add_install() for a description of @remote.
 *
 * Returns: %TRUE on success; %FALSE with @error set on failure.
 * Since: 1.3.3.
 */
gboolean
flatpak_transaction_add_rebase (FlatpakTransaction *self,
                                const char         *remote,
                                const char         *ref,
                                const char        **subpaths,
                                const char        **previous_ids,
                                GError            **error)
{
  const char *all_paths[] = { NULL };

  g_return_val_if_fail (ref != NULL, FALSE);
  g_return_val_if_fail (remote != NULL, FALSE);
  /* flatpak_transaction_add_rebase without previous_ids doesn't make sense */
  g_return_val_if_fail (previous_ids != NULL, FALSE);

  /* If we install with no special args pull all subpaths */
  if (subpaths == NULL)
    subpaths = all_paths;

  return flatpak_transaction_add_ref (self, remote, ref, subpaths, previous_ids, NULL, FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE, NULL, NULL, error);
}

/**
 * flatpak_transaction_add_install_bundle:
 * @self: a #FlatpakTransaction
 * @file: a #GFile that is an flatpak bundle
 * @gpg_data: (nullable): GPG key with which to check bundle signatures, or
 *  %NULL to use the key embedded in the bundle (if any)
 * @error: return location for a #GError
 *
 * Adds installing the given bundle to this transaction.
 *
 * Returns: %TRUE on success; %FALSE with @error set on failure.
 */
gboolean
flatpak_transaction_add_install_bundle (FlatpakTransaction *self,
                                        GFile              *file,
                                        GBytes             *gpg_data,
                                        GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->bundles = g_list_append (priv->bundles, bundle_data_new (file, gpg_data));

  return TRUE;
}

/**
 * flatpak_transaction_add_install_flatpakref:
 * @self: a #FlatpakTransaction
 * @flatpakref_data: data from a flatpakref file
 * @error: return location for a #GError
 *
 * Adds installing the given flatpakref to this transaction.
 *
 * Returns: %TRUE on success; %FALSE with @error set on failure.
 */
gboolean
flatpak_transaction_add_install_flatpakref (FlatpakTransaction *self,
                                            GBytes             *flatpakref_data,
                                            GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autoptr(GError) local_error = NULL;

  g_return_val_if_fail (flatpakref_data != NULL, FALSE);

  if (!g_key_file_load_from_data (keyfile, g_bytes_get_data (flatpakref_data, NULL),
                                  g_bytes_get_size (flatpakref_data),
                                  0, &local_error))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid .flatpakref: %s"), local_error->message);

  priv->flatpakrefs = g_list_append (priv->flatpakrefs, g_steal_pointer (&keyfile));

  return TRUE;
}

/**
 * flatpak_transaction_add_update:
 * @self: a #FlatpakTransaction
 * @ref: the ref
 * @subpaths: (nullable) (array zero-terminated=1): subpaths to install; %NULL
 *  to use the current set plus the set of configured languages, or
 *  `{ "", NULL }` to pull all subpaths.
 * @commit: (nullable): the commit to update to, or %NULL to use the latest
 * @error: return location for a #GError
 *
 * Adds updating the given ref to this transaction.
 *
 * Returns: %TRUE on success; %FALSE with @error set on failure.
 */
gboolean
flatpak_transaction_add_update (FlatpakTransaction *self,
                                const char         *ref,
                                const char        **subpaths,
                                const char         *commit,
                                GError            **error)
{
  const char *all_paths[] = { NULL };

  g_return_val_if_fail (ref != NULL, FALSE);

  /* If specify an empty subpath, that means all subpaths */
  if (subpaths != NULL && subpaths[0] != NULL && subpaths[0][0] == 0)
    subpaths = all_paths;

  /* Note: we implement the merge when subpaths == NULL in flatpak_transaction_add_ref() */
  return flatpak_transaction_add_ref (self, NULL, ref, subpaths, NULL, commit, FLATPAK_TRANSACTION_OPERATION_UPDATE, NULL, NULL, error);
}

/**
 * flatpak_transaction_add_uninstall:
 * @self: a #FlatpakTransaction
 * @ref: the ref
 * @error: return location for a #GError
 *
 * Adds uninstalling the given ref to this transaction.
 *
 * Returns: %TRUE on success; %FALSE with @error set on failure.
 */
gboolean
flatpak_transaction_add_uninstall (FlatpakTransaction *self,
                                   const char         *ref,
                                   GError            **error)
{
  g_return_val_if_fail (ref != NULL, FALSE);

  return flatpak_transaction_add_ref (self, NULL, ref, NULL, NULL, NULL, FLATPAK_TRANSACTION_OPERATION_UNINSTALL, NULL, NULL, error);
}

static gboolean
flatpak_transaction_update_metadata (FlatpakTransaction *self,
                                     GCancellable       *cancellable,
                                     GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_auto(GStrv) remotes = NULL;
  int i;
  GList *l;
  gboolean some_updated = FALSE;
  g_autoptr(GHashTable) ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gboolean local_only = TRUE;

  /* Collect all dir+remotes used in this transaction */

  if (!flatpak_dir_migrate_config (priv->dir, &some_updated, cancellable, error))
    return FALSE;

  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      g_hash_table_add (ht, g_strdup (op->remote));
      local_only = local_only && transaction_is_local_only (self, op->kind);
    }
  remotes = (char **) g_hash_table_get_keys_as_array (ht, NULL);
  g_hash_table_steal_all (ht); /* Move ownership to remotes */

  /* Bail early if the entire transaction is local-only, as in that case we
   * don’t need updated metadata. */
  if (local_only)
    return TRUE;

  /* Update metadata for said remotes */
  for (i = 0; remotes[i] != NULL; i++)
    {
      char *remote = remotes[i];
      gboolean updated = FALSE;
      g_autoptr(GError) my_error = NULL;
      g_autoptr(FlatpakRemoteState) state = flatpak_transaction_ensure_remote_state (self, FLATPAK_TRANSACTION_OPERATION_UPDATE, remote, NULL);

      g_debug ("Looking for remote metadata updates for %s", remote);
      if (!flatpak_dir_update_remote_configuration (priv->dir, remote, state, &updated, cancellable, &my_error))
        g_debug (_("Error updating remote metadata for '%s': %s"), remote, my_error->message);

      if (updated)
        {
          g_debug ("Got updated metadata for %s", remote);
          some_updated = TRUE;
        }
    }

  if (some_updated)
    {
      /* Reload changed configuration */
      if (!flatpak_dir_recreate_repo (priv->dir, cancellable, error))
        return FALSE;

      flatpak_installation_drop_caches (priv->installation, NULL, NULL);

      /* These are potentially out of date now */
      g_hash_table_remove_all (priv->remote_states);
    }

  return TRUE;
}

static gboolean
flatpak_transaction_add_auto_install (FlatpakTransaction *self,
                                      GCancellable       *cancellable,
                                      GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_auto(GStrv) remotes = NULL;

  remotes = flatpak_dir_list_remotes (priv->dir, cancellable, error);
  if (remotes == NULL)
    return FALSE;

  /* Auto-add auto-download apps that are not already installed.
   * Try to avoid doing network i/o until we know its needed, as this
   * iterates over all configured remotes.
   */
  for (int i = 0; remotes[i] != NULL; i++)
    {
      char *remote = remotes[i];
      g_autofree char *auto_install_ref = NULL;

      if (flatpak_dir_get_remote_disabled (priv->dir, remote))
        continue;

      auto_install_ref = flatpak_dir_get_remote_auto_install_authenticator_ref (priv->dir, remote);
      if (auto_install_ref != NULL)
        {
          g_autoptr(GError) local_error = NULL;
          g_autoptr(GFile) deploy = NULL;

          deploy = flatpak_dir_get_if_deployed (priv->dir, auto_install_ref, NULL, cancellable);
          if (deploy == NULL)
            {
              g_autoptr(FlatpakRemoteState) state = flatpak_transaction_ensure_remote_state (self, FLATPAK_TRANSACTION_OPERATION_UPDATE, remote, NULL);

              if (state != NULL &&
                  flatpak_remote_state_lookup_ref (state, auto_install_ref, NULL, NULL, NULL, NULL, NULL))
                {
                  g_debug ("Auto adding install of %s from remote %s", auto_install_ref, remote);
                  if (!flatpak_transaction_add_ref (self, remote, auto_install_ref, NULL, NULL, NULL,
                                                    FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE,
                                                    NULL, NULL,
                                                    &local_error))
                    g_debug ("Failed to add auto-install ref %s: %s", auto_install_ref, local_error->message);
                }
            }
        }
    }

  return TRUE;
}

static void
emit_new_op (FlatpakTransaction *self, FlatpakTransactionOperation *op, FlatpakTransactionProgress *progress)
{
  g_signal_emit (self, signals[NEW_OPERATION], 0, op, progress);
}

static void
emit_op_done (FlatpakTransaction          *self,
              FlatpakTransactionOperation *op,
              FlatpakTransactionResult     details)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *commit = NULL;

  if (priv->no_deploy)
    commit = flatpak_dir_read_latest (priv->dir, op->remote, op->ref, NULL, NULL, NULL);
  else
    {
      g_autoptr(GBytes) deploy_data = flatpak_dir_get_deploy_data (priv->dir, op->ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);
      if (deploy_data)
        commit = g_strdup (flatpak_deploy_data_get_commit (deploy_data));
    }

  g_signal_emit (self, signals[OPERATION_DONE], 0, op, commit, details);
}

static GBytes *
load_deployed_metadata (FlatpakTransaction *self, const char *ref, char **out_commit)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autofree char *metadata_contents = NULL;
  gsize metadata_contents_length;

  deploy_dir = flatpak_dir_get_if_deployed (priv->dir, ref, NULL, NULL);
  if (deploy_dir == NULL)
    return NULL;

  if (out_commit)
    {
      g_autoptr(GBytes) deploy_data = NULL;
      deploy_data = flatpak_load_deploy_data (deploy_dir, ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);
      if (deploy_data == NULL)
        return NULL;

      *out_commit = g_strdup (flatpak_deploy_data_get_commit (deploy_data));
    }

  metadata_file = g_file_get_child (deploy_dir, "metadata");

  if (!g_file_load_contents (metadata_file, NULL, &metadata_contents, &metadata_contents_length, NULL, NULL))
    {
      g_debug ("No metadata in local deploy of %s", ref);
      return NULL;
    }

  return g_bytes_new_take (g_steal_pointer (&metadata_contents), metadata_contents_length + 1);
}

static void
emit_eol_and_maybe_skip (FlatpakTransaction          *self,
                         FlatpakTransactionOperation *op)
{
  g_auto(GStrv) parts = NULL;
  const char *previous_ids[] = { NULL, NULL };

  if (op->skip || (!op->eol && !op->eol_rebase) ||
      op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    return;

  parts = flatpak_decompose_ref (op->ref, NULL);
  if (parts != NULL)
    previous_ids[0] = parts[1];

  g_signal_emit (self, signals[END_OF_LIFED_WITH_REBASE], 0, op->remote, op->ref, op->eol, op->eol_rebase, previous_ids, &op->skip);
}

static void
mark_op_resolved (FlatpakTransactionOperation *op,
                  const char                  *commit,
                  GFile                       *sideload_path,
                  GBytes                      *metadata,
                  GBytes                      *old_metadata)
{
  g_debug ("marking op %s:%s resolved to %s", kind_to_str (op->kind), op->ref, commit ? commit : "-");

  g_assert (op != NULL);

  g_assert (commit != NULL);

  op->resolved = TRUE;
  g_free (op->resolved_commit); /* This is already set if we retry resolving to get a token, so free first */
  op->resolved_commit = g_strdup (commit);

  if (sideload_path)
    op->resolved_sideload_path = g_object_ref (sideload_path);

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

static void
resolve_op_end (FlatpakTransaction *self,
                FlatpakTransactionOperation *op,
                const char *checksum,
                GFile *sideload_path,
                GBytes *metadata_bytes)
{
  g_autoptr(GBytes) old_metadata_bytes = NULL;

  old_metadata_bytes = load_deployed_metadata (self, op->ref, NULL);
  mark_op_resolved (op, checksum, sideload_path, metadata_bytes, old_metadata_bytes);
  emit_eol_and_maybe_skip (self, op);
 }


static void
resolve_op_from_commit (FlatpakTransaction *self,
                        FlatpakTransactionOperation *op,
                        const char *checksum,
                        GFile *sideload_path,
                        GVariant *commit_data)
{
  g_autoptr(GBytes) metadata_bytes = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;
  const char *xa_metadata = NULL;
  guint64 download_size = 0;
  guint64 installed_size = 0;

  commit_metadata = g_variant_get_child_value (commit_data, 0);
  g_variant_lookup (commit_metadata, "xa.metadata", "&s", &xa_metadata);
  if (xa_metadata == NULL)
    g_message ("Warning: No xa.metadata in local commit %s ref %s", checksum, op->ref);
  else
    metadata_bytes = g_bytes_new (xa_metadata, strlen (xa_metadata) + 1);

  if (g_variant_lookup (commit_metadata, "xa.download-size", "t", &download_size))
    op->download_size = GUINT64_FROM_BE (download_size);
  if (g_variant_lookup (commit_metadata, "xa.installed-size", "t", &installed_size))
    op->installed_size = GUINT64_FROM_BE (installed_size);

  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "s", &op->eol);
  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, "s", &op->eol_rebase);

  resolve_op_end (self, op, checksum, sideload_path, metadata_bytes);
}

static gboolean
try_resolve_op_from_metadata (FlatpakTransaction *self,
                              FlatpakTransactionOperation *op,
                              const char *checksum,
                              GFile *sideload_path,
                              FlatpakRemoteState *state)
{
  g_autoptr(GBytes) metadata_bytes = NULL;
  guint64 download_size = 0;
  guint64 installed_size = 0;
  const char *metadata = NULL;
  VarMetadataRef sparse_cache;
  VarRefInfoRef info;
  g_autofree char *summary_checksum = NULL;

  /* Ref has to match the actual commit in the summary */
  if (state->summary == NULL ||
      !flatpak_summary_lookup_ref (state->summary, NULL, op->ref, &summary_checksum, NULL) ||
      strcmp (summary_checksum, checksum) != 0)
    return FALSE;

  /* And, we must have the actual cached data in the summary */
  if (!flatpak_remote_state_lookup_cache (state, op->ref,
                                          &download_size, &installed_size, &metadata, NULL))
      return FALSE;

  metadata_bytes = g_bytes_new (metadata, strlen (metadata) + 1);

  if (flatpak_remote_state_lookup_ref (state, op->ref, NULL, NULL, &info, NULL, NULL))
    op->summary_metadata = var_metadata_dup_to_gvariant (var_ref_info_get_metadata (info));

  op->installed_size = installed_size;
  op->download_size = download_size;

  op->token_type = state->default_token_type;

  if (flatpak_remote_state_lookup_sparse_cache (state, op->ref, &sparse_cache, NULL))
    {
      op->eol = g_strdup (var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE, NULL));
      op->eol_rebase = g_strdup (var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE_REBASE, NULL));
      op->token_type = GINT32_FROM_LE (var_metadata_lookup_int32 (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_TOKEN_TYPE, op->token_type));
    }

  resolve_op_end (self, op, checksum, sideload_path, metadata_bytes);
  return TRUE;
}

static gboolean
op_may_need_token (FlatpakTransactionOperation *op)
{
  return
    !op->skip &&
    !op->update_only_deploy &&
    (op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL ||
     op->kind == FLATPAK_TRANSACTION_OPERATION_UPDATE  ||
     op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE);
}

/* Resolving an operation means figuring out the target commit
   checksum and the metadata for that commit, so that we can handle
   dependencies from it, and verify versions. */
static gboolean
resolve_ops (FlatpakTransaction *self,
             GCancellable       *cancellable,
             GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l;

  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      g_autoptr(FlatpakRemoteState) state = NULL;
      g_autofree char *checksum = NULL;
      g_autoptr(GBytes) metadata_bytes = NULL;

      if (op->resolved)
        continue;

      if (op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
        {
          /* We resolve to the deployed metadata, because we need it to uninstall related ops */

          metadata_bytes = load_deployed_metadata (self, op->ref, &checksum);
          mark_op_resolved (op, checksum, NULL, metadata_bytes, NULL);
          continue;
        }

      if (op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE)
        {
          g_assert (op->commit != NULL);
          mark_op_resolved (op, op->commit, NULL, op->external_metadata, NULL);
          continue;
        }

      /* op->kind is INSTALL or UPDATE */

      if (g_str_has_prefix (op->ref, "app/"))
        {
          if (op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL)
            priv->max_op = APP_INSTALL;
          else
            priv->max_op = MAX (priv->max_op, APP_UPDATE);
        }
      else if (g_str_has_prefix (op->ref, "runtime/"))
        {
          if (op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL)
            priv->max_op = MAX (priv->max_op, RUNTIME_INSTALL);
        }

      state = flatpak_transaction_ensure_remote_state (self, op->kind, op->remote, error);
      if (state == NULL)
        return FALSE;

      /* Should we use local state */
      if (transaction_is_local_only (self, op->kind))
        {
          g_autoptr(GVariant) commit_data = flatpak_dir_read_latest_commit (priv->dir, op->remote, op->ref, &checksum, NULL, error);
          if (commit_data == NULL)
            return FALSE;

          resolve_op_from_commit (self, op, checksum, NULL, commit_data);
        }
      else
        {
          g_autoptr(GError) local_error = NULL;
          g_autoptr(GFile) sideload_path = NULL;

          if (op->commit != NULL)
            {
              checksum = g_strdup (op->commit);
              /* Check if this is available offline and if so, use that */
              sideload_path = flatpak_remote_state_lookup_sideload_checksum (state, op->commit);
            }
          else
            {
              g_autofree char *latest_checksum = NULL;
              g_autoptr(GFile) latest_sideload_path = NULL;
              g_autofree char *local_checksum = NULL;
              guint64 latest_timestamp;
              g_autoptr(GVariant) local_commit_data = flatpak_dir_read_latest_commit (priv->dir, op->remote, op->ref, &local_checksum, NULL, NULL);

              if (flatpak_dir_find_latest_rev (priv->dir, state, op->ref, op->commit,
                                               &latest_checksum, &latest_timestamp, &latest_sideload_path,
                                               cancellable, &local_error))
                {
                  /* If we found the latest in a sideload repo, it may be older that what is locally available, check timestamps.
                   * Note: If the timestamps are equal (timestamp granularity issue), assume we want to update */
                  if (latest_sideload_path != NULL && local_commit_data &&
                      ostree_commit_get_timestamp (local_commit_data) > latest_timestamp)
                    {
                      g_debug ("Installed commit %s newer than sideloaded %s, ignoring", local_checksum, latest_checksum);
                      checksum = g_steal_pointer (&local_checksum);
                    }
                  else
                    {
                      /* Otherwise, use whatever we found */
                      checksum = g_steal_pointer (&latest_checksum);
                      sideload_path = g_steal_pointer (&latest_sideload_path);
                    }
                }
              else
                {
                  /* Ref not available in the remote (maybe offline), resolve to local version if installed */
                  if (local_commit_data == NULL)
                    {
                      g_propagate_error (error, g_steal_pointer (&local_error));
                      return FALSE;
                    }

                  g_message (_("Warning: Treating remote fetch error as non-fatal since %s is already installed: %s"),
                             op->ref, local_error->message);
                  g_clear_error (&local_error);

                  checksum = g_steal_pointer (&local_checksum);
                }
            }

          /* First try to resolve via metadata (if remote is available and its metadata matches the commit version) */
          if (!try_resolve_op_from_metadata (self, op, checksum, sideload_path, state))
            {
              /* Else try to load the commit object.
               * Note, we don't have a token here, so this will not work for authenticated apps.
               * We handle this by catching the 401 http status and retrying. */
              g_autoptr(GVariant) commit_data = NULL;
              VarRefInfoRef ref_info;

              /* OCI needs this to get the oci repository for the ref to request the token, so lets always set it here */
              if (op->summary_metadata == NULL &&
                  flatpak_remote_state_lookup_ref (state, op->ref, NULL, NULL, &ref_info, NULL, NULL))
                op->summary_metadata = var_metadata_dup_to_gvariant (var_ref_info_get_metadata (ref_info));

              commit_data = flatpak_remote_state_load_ref_commit (state, priv->dir,
                                                                  op->ref, checksum, /* initially NULL */ op->resolved_token,
                                                                  NULL, NULL, &local_error);
              if (commit_data == NULL)
                {
                  if (g_error_matches (local_error, FLATPAK_HTTP_ERROR, FLATPAK_HTTP_ERROR_UNAUTHORIZED) && !op->requested_token)
                    {

                      g_debug ("Unauthorized access during resolve by commit of %s, retrying with token", op->ref);
                      priv->needs_resolve = TRUE;
                      priv->needs_tokens = TRUE;

                      /* Token type maxint32 means we don't know the type */
                      op->token_type = G_MAXINT32;
                      op->resolved_commit = g_strdup (checksum);

                      g_clear_error (&local_error);
                      continue;
                    }
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }

              resolve_op_from_commit (self, op, checksum, sideload_path, commit_data);
            }
        }
    }

  return TRUE;
}

static gboolean
resolve_all_ops (FlatpakTransaction *self,
                 GCancellable       *cancellable,
                 GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  while (priv->needs_resolve)
    {
      priv->needs_resolve = FALSE;
      priv->needs_tokens = FALSE;
      if (!resolve_ops (self, cancellable, error))
        return FALSE;

      /* We might need tokens early, if reading individual commits needs it,
       * otherwise we try to delay to bunch the requests */
      if (priv->needs_tokens)
        {
          if (!request_required_tokens (self, NULL, cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

static void
request_tokens_response (FlatpakAuthenticatorRequest *object,
                         guint response,
                         GVariant *results,
                         RequestData *data)
{
  FlatpakTransaction *transaction = data->transaction;
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (transaction);

  if (data->done)
    return; /* Don't respond twice */

  g_assert (priv->active_request_id == 0); /* It should have reported done */

  data->response = response;
  data->results = g_variant_ref (results);
  data->done = TRUE;
  g_main_context_wakeup (g_main_context_get_thread_default ());
}

static void
request_tokens_webflow (FlatpakAuthenticatorRequest *object,
                        const gchar *arg_uri,
                        GVariant *options,
                        RequestData *data)
{
  g_autoptr(FlatpakTransaction) transaction = g_object_ref (data->transaction);
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (transaction);
  gboolean retval = FALSE;

  if (data->done)
    return; /* Don't respond twice */

  g_assert (priv->active_request_id == 0);
  priv->active_request_id = ++priv->next_request_id;

  g_debug ("Webflow start %s", arg_uri);
  g_signal_emit (transaction, signals[WEBFLOW_START], 0, data->remote, arg_uri, options, priv->active_request_id, &retval);
  if (!retval)
    {
      g_autoptr(GError) local_error = NULL;

      priv->active_request_id = 0;

      /* We didn't handle the uri, cancel the auth op. */
      if (!flatpak_authenticator_request_call_close_sync (data->request, NULL, &local_error))
        g_debug ("Failed to close auth request: %s", local_error->message);
    }
}

static void
request_tokens_webflow_done (FlatpakAuthenticatorRequest *object,
                             GVariant *options,
                             RequestData *data)
{
  g_autoptr(FlatpakTransaction) transaction = g_object_ref (data->transaction);
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (transaction);
  guint id;

  if (data->done)
    return; /* Don't respond twice */

  g_assert (priv->active_request_id != 0);
  id = priv->active_request_id;
  priv->active_request_id = 0;

  g_debug ("Webflow done");
  g_signal_emit (transaction, signals[WEBFLOW_DONE], 0, options, id);
}

static void
request_tokens_basic_auth (FlatpakAuthenticatorRequest *object,
                           const gchar *arg_realm,
                           GVariant *options,
                           RequestData *data)
{
  g_autoptr(FlatpakTransaction) transaction = g_object_ref (data->transaction);
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (transaction);
  gboolean retval = FALSE;

  if (data->done)
    return; /* Don't respond twice */

  g_assert (priv->active_request_id == 0);
  priv->active_request_id = ++priv->next_request_id;

  g_debug ("BasicAuth start %s", arg_realm);
  g_signal_emit (transaction, signals[BASIC_AUTH_START], 0, data->remote, arg_realm, options, priv->active_request_id, &retval);
  if (!retval)
    {
      g_autoptr(GError) local_error = NULL;

      priv->active_request_id = 0;

      /* We didn't handle the request, cancel the auth op. */
      if (!flatpak_authenticator_request_call_close_sync (data->request, NULL, &local_error))
        g_debug ("Failed to close auth request: %s", local_error->message);
    }

}

/**
 * flatpak_transaction_abort_webflow:
 * @self: a #FlatpakTransaction
 * @id: The webflow id, as passed into the webflow-start signal
 *
 * Cancel an ongoing webflow authentication request. This can be call
 * in the time between #FlatpakTransaction::webflow-start returned
 * %TRUE, and #FlatpakTransaction::webflow-done is emitted. It will
 * cancel the ongoing authentication operation.
 *
 * This is useful for example if you're showing an authenticaion
 * window with a browser, but the user closed it before it was finished.
 *
 * Since: 1.5.1
 */
void
flatpak_transaction_abort_webflow (FlatpakTransaction *self,
                                   guint id)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GError) local_error = NULL;

  if (priv->active_request_id == id)
    {
      RequestData *data = priv->active_request;

      g_assert (data != NULL);
      priv->active_request_id = 0;

      if (!data->done)
        {
          if (!flatpak_authenticator_request_call_close_sync (data->request, NULL, &local_error))
            g_debug ("Failed to close auth request: %s", local_error->message);
        }
    }
}

/**
 * flatpak_transaction_complete_basic_auth:
 * @self: a #FlatpakTransaction
 * @id: The webflow id, as passed into the webflow-start signal
 * @user: The user name, or %NULL if aborting request
 * @password: The password
 * @options: Extra a{sv] variant with options (or %NULL), currently unused.
 *
 * Finishes (or aborts) an ongoing basic auth request.
 *
 * Since: 1.5.2
 */
void
flatpak_transaction_complete_basic_auth (FlatpakTransaction *self,
                                         guint id,
                                         const char *user,
                                         const char *password,
                                         GVariant *options)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GVariant) default_options = NULL;

  if (options == NULL)
    {
      default_options = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));
      options = default_options;
    }

  if (priv->active_request_id == id)
    {
      RequestData *data = priv->active_request;

      g_assert (data != NULL);
      priv->active_request_id = 0;

      if (user == NULL)
        {
          if (!flatpak_authenticator_request_call_close_sync (data->request, NULL, &local_error))
            g_debug ("Failed to abort basic auth request: %s", local_error->message);
        }
      else
        {
          if (!flatpak_authenticator_request_call_basic_auth_reply_sync (data->request,
                                                                         user, password,
                                                                         options,
                                                                         NULL, &local_error))
            g_debug ("Failed to reply to basic auth request: %s", local_error->message);
        }
    }
}

static void
copy_summary_data (GVariantBuilder *builder, GVariant *summary, const char *key)
{
  g_autoptr(GVariant) extensions = g_variant_get_child_value (summary, 1);
  g_autoptr(GVariant) value = NULL;

  value = g_variant_lookup_value (extensions, key, NULL);
  if (value)
    g_variant_builder_add (builder, "{s@v}", key, g_variant_new_variant (value));
}


static gboolean
request_tokens_for_remote (FlatpakTransaction *self,
                           const char         *remote,
                           GList              *ops,
                           GCancellable       *cancellable,
                           GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GString) refs_as_str = g_string_new ("");
  GList *l;
  g_autoptr(AutoFlatpakAuthenticatorRequest) request = NULL;
  g_autoptr(AutoFlatpakAuthenticator) authenticator = NULL;
  g_autoptr(GMainContextPopDefault) context = NULL;
  RequestData data = { self, remote };
  g_autoptr(GVariant) tokens = NULL;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GVariant) refs = NULL;
  GVariantBuilder refs_builder;
  g_autofree char *remote_url = NULL;
  g_autoptr(GVariantBuilder) extra_builder = NULL;
  FlatpakRemoteState *state;
  g_autofree char *auto_install_ref = NULL;


  auto_install_ref = flatpak_dir_get_remote_auto_install_authenticator_ref (priv->dir, remote);
  if (auto_install_ref != NULL)
    {
      g_autoptr(GFile) deploy = NULL;
      deploy = flatpak_dir_get_if_deployed (priv->dir, auto_install_ref, NULL, cancellable);
      if (deploy == NULL)
        g_signal_emit (self, signals[INSTALL_AUTHENTICATOR], 0,
                       remote, auto_install_ref);
      deploy = flatpak_dir_get_if_deployed (priv->dir, auto_install_ref, NULL, cancellable);
      if (deploy == NULL)
        return flatpak_fail (error, _("No authenticator installed for remote '%s'"), remote);
    }

  if (!ostree_repo_remote_get_url (flatpak_dir_get_repo (priv->dir), remote, &remote_url, error))
    return FALSE;

  g_variant_builder_init (&refs_builder, G_VARIANT_TYPE ("a(ssia{sv})"));

  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

      if (op->summary_metadata)
        {
          const int n = g_variant_n_children (op->summary_metadata);
          for (int i = 0; i < n; i++)
            {
              const char *key;
              g_autofree char *new_key = NULL;
              g_autoptr(GVariant) value = NULL;

              g_variant_get_child (op->summary_metadata, i, "{&s@v}", &key, &value);

              new_key = g_strconcat ("summary.", key, NULL);
              g_variant_builder_add (metadata_builder, "{s@v}", new_key, value);
            }
        }

      g_variant_builder_add (&refs_builder, "(ssi@a{sv})", op->ref, op->resolved_commit ? op->resolved_commit : "", (gint32)op->token_type, g_variant_builder_end (metadata_builder));
      g_string_append_printf (refs_as_str, "(%s, %s %d)", op->ref, op->resolved_commit ? op->resolved_commit : "", op->token_type);
      if (l->next != NULL)
        g_string_append (refs_as_str, ", ");
    }

  g_debug ("Requesting tokens for remote %s: %s", remote, refs_as_str->str);
  refs = g_variant_ref_sink (g_variant_builder_end (&refs_builder));

  extra_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  state = g_hash_table_lookup (priv->remote_states, remote);
  if (state && state->summary)
    {
      copy_summary_data (extra_builder, state->summary, "xa.oci-registry-uri");
    }

  if (flatpak_dir_get_no_interaction (priv->dir))
    g_variant_builder_add (extra_builder, "{sv}", "no-interaction", g_variant_new_boolean (TRUE));

  context = flatpak_main_context_new_default ();

  authenticator = flatpak_auth_new_for_remote (priv->dir, remote, cancellable, error);
  if (authenticator == NULL)
    return FALSE;

  request = flatpak_auth_create_request (authenticator, cancellable, error);
  if (request == NULL)
    return FALSE;

  g_signal_connect (request, "webflow", (GCallback)request_tokens_webflow, &data);
  g_signal_connect (request, "webflow-done", (GCallback)request_tokens_webflow_done, &data);
  g_signal_connect (request, "response", (GCallback)request_tokens_response, &data);
  g_signal_connect (request, "basic-auth", (GCallback)request_tokens_basic_auth, &data);

  priv->active_request = &data;

  data.request = request;
  if (!flatpak_auth_request_ref_tokens (authenticator, request, remote, remote_url, refs, g_variant_builder_end (extra_builder),
                                        priv->parent_window, cancellable, error))
    return FALSE;

  while (!data.done)
    g_main_context_iteration (context, TRUE);

  g_assert (priv->active_request_id == 0); /* No outstanding requests */
  priv->active_request = NULL;

  results = data.results; /* Make sure its freed as needed */

  {
    g_autofree char *results_str = results != NULL ? g_variant_print (results, FALSE) : g_strdup ("NULL");
    g_debug ("Response from request_tokens: %d - %s\n", data.response, results_str);
  }

  if (data.response == FLATPAK_AUTH_RESPONSE_CANCELLED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                   "User cancelled authentication request");
      return FALSE;
    }

  if (data.response != FLATPAK_AUTH_RESPONSE_OK)
    {
      const char *error_message;
      gint32 error_code;

      if (!g_variant_lookup (results, "error-message", "&s", &error_message))
        error_message = NULL;

      if (g_variant_lookup (results, "error-code", "i", &error_code) && error_code != -1)
        {
          if (error_message)
            flatpak_fail_error (error, error_code, _("Failed to get tokens for ref: %s"), error_message);
          else
            return flatpak_fail_error (error, error_code, _("Failed to get tokens for ref"));
        }
      else
        {
          if (error_message)
            return flatpak_fail (error, _("Failed to get tokens for ref: %s"), error_message);
          else
            return flatpak_fail (error, _("Failed to get tokens for ref"));
        }
    }

  tokens = g_variant_lookup_value (results, "tokens", G_VARIANT_TYPE ("a{sas}"));
  if (tokens == NULL)
    return flatpak_fail (error, "Authenticator didn't send requested tokens");

  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      GVariantIter iter;
      const char *token = NULL;
      const char *token_for_refs;
      g_autofree const char **refs;

      g_variant_iter_init (&iter, tokens);
      while (g_variant_iter_next (&iter, "{&s^a&s}", &token_for_refs, &refs))
        {
          if (g_strv_contains (refs, op->ref))
            {
              token = token_for_refs;
              break;
            }
        }

      if (token == NULL)
        return flatpak_fail (error, "Authenticator didn't send tokens for ref");

      /* Allow sending empty tokens to mean no token needed */

      op->resolved_token = *token == 0 ? NULL : g_strdup (token);
      op->requested_token = TRUE;
    }

  return TRUE;
}

static gboolean
request_required_tokens (FlatpakTransaction *self,
                         const char         *optional_remote, /* else all remotes */
                         GCancellable       *cancellable,
                         GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l;
  g_autoptr(GHashTable) need_token_ht = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) g_list_free); /* remote name -> list of op */

  /* Ensure all ops so far ar normalized so we don't request authentication for no-op updates */
  flatpak_transaction_normalize_ops (self);

  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      GList *old;

      if (!op_may_need_token (op) || op->token_type == 0 || op->requested_token)
        continue;

      if (optional_remote != NULL && g_strcmp0 (op->remote, optional_remote) != 0)
        continue;

      old = g_hash_table_lookup (need_token_ht, op->remote);
      if (old == NULL)
        g_hash_table_insert (need_token_ht, op->remote, g_list_append (NULL, op));
      else
        old = g_list_append (old, op);
    }

  GLNX_HASH_TABLE_FOREACH_KV(need_token_ht, const char *, remote, GList *, remote_ops)
    {
      if (!request_tokens_for_remote (self, remote, remote_ops, cancellable, error))
        return FALSE;
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
     Note that this essentially reverses the original list, so these
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
  runnable = g_list_sort (runnable, (GCompareFunc) compare_op_ref);

  while (runnable)
    {
      GList *run = runnable;
      FlatpakTransactionOperation *run_op = run->data;

      /* Put the first runnable on the sorted list */
      runnable = g_list_remove_link (runnable, run);
      sorted = g_list_concat (run, sorted); /* prepends, so reverse at the end */

      /* Then greedily run ops that become runnable, in run_after_prio order, so that
         related ops are run before dependencies */
      run_op->run_before_ops = g_list_sort (run_op->run_before_ops, (GCompareFunc) compare_op_prio);
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
 * Gets the list of operations. Skipped operations are not included. The order
 * of the list is the order in which the operations are executed.
 *
 * Returns: (transfer full) (element-type FlatpakTransactionOperation): a #GList of operations
 */
GList *
flatpak_transaction_get_operations (FlatpakTransaction *self)
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
flatpak_transaction_get_current_operation (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  if (priv->current_op)
    return g_object_ref (priv->current_op);

  return NULL;
}

/**
 * flatpak_transaction_get_installation:
 * @self: a #FlatpakTransactionOperation
 *
 * Gets the installation this transaction was created for.
 *
 * Returns: (transfer full): a #FlatpakInstallation
 */
FlatpakInstallation *
flatpak_transaction_get_installation (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  return g_object_ref (priv->installation);
}

static gboolean
remote_is_already_configured (FlatpakTransaction *self,
                              const char         *url)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *old_remote = NULL;

  old_remote = flatpak_dir_find_remote_by_uri (priv->dir, url);

  /* Note: we don't check priv->extra_dependency_dirs because the transaction
   * can only operate on one installation so any install/update ops need to
   * have a remote there. */

  return old_remote != NULL;
}

static gboolean
handle_suggested_remote_name (FlatpakTransaction *self, GKeyFile *keyfile, GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *suggested_name = NULL;
  g_autofree char *name = NULL;
  g_autofree char *url = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GBytes) gpg_key = NULL;
  gboolean res;

  suggested_name = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                          FLATPAK_REF_SUGGEST_REMOTE_NAME_KEY, NULL);
  if (suggested_name == NULL)
    return TRUE;

  name = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP, FLATPAK_REF_NAME_KEY, NULL);
  if (name == NULL)
    return TRUE;

  url = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP, FLATPAK_REF_URL_KEY, NULL);
  if (url == NULL)
    return TRUE;

  if (remote_is_already_configured (self, url))
    return TRUE;

  /* The name is already used, ignore */
  if (ostree_repo_remote_get_url (flatpak_dir_get_repo (priv->dir), suggested_name, NULL, NULL))
    return TRUE;

  res = FALSE;
  g_signal_emit (self, signals[ADD_NEW_REMOTE], 0, FLATPAK_TRANSACTION_REMOTE_GENERIC_REPO,
                 name, suggested_name, url, &res);
  if (res)
    {
      config = flatpak_parse_repofile (suggested_name, TRUE, keyfile, &gpg_key, NULL, error);
      if (config == NULL)
        return FALSE;

      if (!flatpak_dir_modify_remote (priv->dir, suggested_name, config, gpg_key, NULL, error))
        return FALSE;

      if (!flatpak_dir_recreate_repo (priv->dir, NULL, error))
        return FALSE;

      flatpak_installation_drop_caches (priv->installation, NULL, NULL);
    }

  return TRUE;
}

static gboolean
handle_runtime_repo_deps (FlatpakTransaction *self,
                          const char         *id,
                          const char         *dep_url,
                          GCancellable       *cancellable,
                          GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GBytes) dep_data = NULL;
  g_autofree char *runtime_url = NULL;
  g_autofree char *new_remote = NULL;
  g_autofree char *basename = NULL;
  g_autoptr(SoupURI) uri = NULL;
  g_auto(GStrv) remotes = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GKeyFile) dep_keyfile = g_key_file_new ();
  g_autoptr(GBytes) gpg_key = NULL;
  g_autofree char *group = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(SoupSession) soup_session = NULL;
  char *t;
  int i;
  gboolean res;

  if (priv->disable_deps)
    return TRUE;

  if (!g_str_has_prefix (dep_url, "http:") &&
      !g_str_has_prefix (dep_url, "https:") &&
      !g_str_has_prefix (dep_url, "file:"))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Flatpakrepo URL %s not file, HTTP or HTTPS"), dep_url);

  soup_session = flatpak_create_soup_session (PACKAGE_STRING);
  dep_data = flatpak_load_uri (soup_session, dep_url, 0, NULL, NULL, NULL, NULL, cancellable, error);
  if (dep_data == NULL)
    {
      g_prefix_error (error, _("Can't load dependent file %s: "), dep_url);
      return FALSE;
    }

  if (!g_key_file_load_from_data (dep_keyfile,
                                  g_bytes_get_data (dep_data, NULL),
                                  g_bytes_get_size (dep_data),
                                  0, &local_error))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid .flatpakrepo: %s"), local_error->message);

  uri = soup_uri_new (dep_url);
  basename = g_path_get_basename (soup_uri_get_path (uri));
  /* Strip suffix */
  t = strchr (basename, '.');
  if (t != NULL)
    *t = 0;

  /* Find a free remote name */
  remotes = flatpak_dir_list_remotes (priv->dir, NULL, NULL);
  i = 0;
  do
    {
      g_clear_pointer (&new_remote, g_free);

      if (i == 0)
        new_remote = g_strdup (basename);
      else
        new_remote = g_strdup_printf ("%s-%d", basename, i);
      i++;
    }
  while (remotes != NULL && g_strv_contains ((const char * const *) remotes, new_remote));

  config = flatpak_parse_repofile (new_remote, FALSE, dep_keyfile, &gpg_key, NULL, error);
  if (config == NULL)
    {
      g_prefix_error (error, "Can't parse dependent file %s: ", dep_url);
      return FALSE;
    }

  /* See if it already exists */
  group = g_strdup_printf ("remote \"%s\"", new_remote);
  runtime_url = g_key_file_get_string (config, group, "url", NULL);
  g_assert (runtime_url != NULL);

  if (remote_is_already_configured (self, runtime_url))
    return TRUE;

  res = FALSE;
  g_signal_emit (self, signals[ADD_NEW_REMOTE], 0, FLATPAK_TRANSACTION_REMOTE_RUNTIME_DEPS,
                 id, new_remote, runtime_url, &res);
  if (res)
    {
      if (!flatpak_dir_modify_remote (priv->dir, new_remote, config, gpg_key, NULL, error))
        return FALSE;

      if (!flatpak_dir_recreate_repo (priv->dir, NULL, error))
        return FALSE;

      flatpak_installation_drop_caches (priv->installation, NULL, NULL);
    }

  return TRUE;
}

static gboolean
handle_runtime_repo_deps_from_keyfile (FlatpakTransaction *self,
                                       GKeyFile           *keyfile,
                                       GCancellable       *cancellable,
                                       GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *dep_url = NULL;
  g_autofree char *name = NULL;

  if (priv->disable_deps)
    return TRUE;

  dep_url = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                   FLATPAK_REF_RUNTIME_REPO_KEY, NULL);
  if (dep_url == NULL)
    {
      g_warning ("Flatpakref file does not contain a %s", FLATPAK_REF_RUNTIME_REPO_KEY);
      return TRUE;
    }

  name = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP, FLATPAK_REF_NAME_KEY, NULL);
  if (name == NULL)
    return TRUE;

  return handle_runtime_repo_deps (self, name, dep_url, cancellable, error);
}

static gboolean
flatpak_transaction_resolve_flatpakrefs (FlatpakTransaction *self,
                                         GCancellable       *cancellable,
                                         GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l;

  for (l = priv->flatpakrefs; l != NULL; l = l->next)
    {
      GKeyFile *flatpakref = l->data;
      g_autofree char *remote = NULL;
      g_autofree char *ref = NULL;

      /* Handle this before the runtime deps, because they might be the same */
      if (!handle_suggested_remote_name (self, flatpakref, error))
        return FALSE;

      if (!handle_runtime_repo_deps_from_keyfile (self, flatpakref, cancellable, error))
        return FALSE;

      if (!flatpak_dir_create_remote_for_ref_file (priv->dir, flatpakref, priv->default_arch,
                                                   &remote, NULL, &ref, error))
        return FALSE;

      /* Need to pick up the new config, in case it was applied in the system helper. */
      if (!flatpak_dir_recreate_repo (priv->dir, NULL, error))
        return FALSE;

      flatpak_installation_drop_caches (priv->installation, NULL, NULL);

      if (!flatpak_transaction_add_install (self, remote, ref, NULL, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
handle_runtime_repo_deps_from_bundle (FlatpakTransaction *self,
                                      GFile              *file,
                                      GCancellable       *cancellable,
                                      GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *dep_url = NULL;
  g_autofree char *ref = NULL;
  g_auto(GStrv) ref_parts = NULL;
  g_autoptr(GVariant) metadata = NULL;

  if (priv->disable_deps)
    return TRUE;

  metadata = flatpak_bundle_load (file,
                                  NULL,
                                  &ref,
                                  NULL,
                                  &dep_url,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL);

  if (metadata == NULL || dep_url == NULL || ref == NULL)
    return TRUE;

  ref_parts = g_strsplit (ref, "/", -1);

  return handle_runtime_repo_deps (self, ref_parts[1], dep_url, cancellable, error);
}

static gboolean
flatpak_transaction_resolve_bundles (FlatpakTransaction *self,
                                     GCancellable       *cancellable,
                                     GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l;

  for (l = priv->bundles; l != NULL; l = l->next)
    {
      BundleData *data = l->data;
      g_autofree char *remote = NULL;
      g_autofree char *ref = NULL;
      g_autofree char *commit = NULL;
      g_autofree char *metadata = NULL;
      gboolean created_remote;

      if (!handle_runtime_repo_deps_from_bundle (self, data->file, cancellable, error))
        return FALSE;

      if (!flatpak_dir_ensure_repo (priv->dir, cancellable, error))
        return FALSE;

      remote = flatpak_dir_ensure_bundle_remote (priv->dir, data->file, data->gpg_data,
                                                 &ref, &commit, &metadata, &created_remote,
                                                 NULL, error);
      if (remote == NULL)
        return FALSE;

      if (created_remote)
        flatpak_installation_drop_caches (priv->installation, NULL, NULL);

      if (!flatpak_transaction_add_ref (self, remote, ref, NULL, NULL, commit,
                                        FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE,
                                        data->file, metadata, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * flatpak_transaction_run:
 * @transaction: a #FlatpakTransaction
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for an error
 *
 * Executes the transaction.
 *
 * During the course of the execution, various signals will get emitted.
 * The FlatpakTransaction::choose-remote-for-ref  and
 * #FlatpakTransaction::add-new-remote signals may get emitted while
 * resolving operations. #FlatpakTransaction::ready is emitted when
 * the transaction has been fully resolved, and #FlatpakTransaction::new-operation
 * and #FlatpakTransaction::operation-done are emitted while the operations
 * are carried out. If an error occurs at any point during the execution,
 * #FlatpakTransaction::operation-error is emitted.
 *
 * Note that this call blocks until the transaction is done.
 *
 * Returns: %TRUE on success, %FALSE if an error occurred
 */
gboolean
flatpak_transaction_run (FlatpakTransaction *transaction,
                         GCancellable       *cancellable,
                         GError            **error)
{
  return FLATPAK_TRANSACTION_GET_CLASS (transaction)->run (transaction, cancellable, error);
}

static gboolean
_run_op_kind (FlatpakTransaction           *self,
              FlatpakTransactionOperation  *op,
              FlatpakRemoteState           *remote_state, /* nullable */
              gboolean                     *out_needs_prune,
              gboolean                     *out_needs_triggers,
              GCancellable                 *cancellable,
              GError                      **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  gboolean res = TRUE;

  g_return_val_if_fail (remote_state != NULL || op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL, FALSE);

  if (op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL)
    {
      g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();
      FlatpakTransactionResult result_details = 0;
      g_autoptr(GError) local_error = NULL;

      emit_new_op (self, op, progress);

      g_assert (op->resolved_commit != NULL); /* We resolved this before */

      if (op->resolved_metakey && !flatpak_check_required_version (op->ref, op->resolved_metakey, &local_error))
        res = FALSE;
      else
        res = flatpak_dir_install (priv->dir,
                                   priv->no_pull,
                                   priv->no_deploy,
                                   priv->disable_static_deltas,
                                   priv->reinstall,
                                   priv->max_op >= APP_UPDATE,
                                   remote_state, op->ref, op->resolved_commit,
                                   (const char **) op->subpaths,
                                   (const char **) op->previous_ids,
                                   op->resolved_sideload_path,
                                   op->resolved_metadata,
                                   op->resolved_token,
                                   progress->progress_obj,
                                   cancellable, &local_error);

      flatpak_transaction_progress_done (progress);

      /* Handle noop-installs (maybe we raced, or this was installed in install-authenticator)
       * We do initial checks and fail with already installed in add_ref() for other cases. */
      if (!res && g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
        {
          res = TRUE;
          g_clear_error (&local_error);

          result_details |= FLATPAK_TRANSACTION_RESULT_NO_CHANGE;
        }
      else if (!res)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
        }

      if (res)
        {
          emit_op_done (self, op, result_details);

          /* Normally we don't need to prune after install, because it makes no old objects
             stale. However if we reinstall, that is not true. */
          if (!priv->no_pull && priv->reinstall)
            *out_needs_prune = TRUE;

          if (g_str_has_prefix (op->ref, "app"))
            *out_needs_triggers = TRUE;
        }
    }
  else if (op->kind == FLATPAK_TRANSACTION_OPERATION_UPDATE)
    {
      g_assert (op->resolved_commit != NULL); /* We resolved this before */

      if (flatpak_dir_needs_update_for_commit_and_subpaths (priv->dir, op->remote, op->ref, op->resolved_commit,
                                                            (const char **) op->subpaths))
        {
          g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();
          FlatpakTransactionResult result_details = 0;
          g_autoptr(GError) local_error = NULL;

          emit_new_op (self, op, progress);

          if (op->resolved_metakey && !flatpak_check_required_version (op->ref, op->resolved_metakey, &local_error))
            res = FALSE;
          else if (op->update_only_deploy)
            res = flatpak_dir_deploy_update (priv->dir, op->ref, op->resolved_commit,
                                             (const char **) op->subpaths,
                                             (const char **) op->previous_ids,
                                             cancellable, &local_error);
          else
            res = flatpak_dir_update (priv->dir,
                                      priv->no_pull,
                                      priv->no_deploy,
                                      priv->disable_static_deltas,
                                      op->commit != NULL, /* Allow downgrade if we specify commit */
                                      priv->max_op >= APP_UPDATE,
                                      priv->max_op == APP_INSTALL || priv->max_op == RUNTIME_INSTALL,
                                      remote_state, op->ref, op->resolved_commit,
                                      (const char **) op->subpaths,
                                      (const char **) op->previous_ids,
                                      op->resolved_sideload_path,
                                      op->resolved_metadata,
                                      op->resolved_token,
                                      progress->progress_obj,
                                      cancellable, &local_error);
          flatpak_transaction_progress_done (progress);

          /* Handle noop-updates */
          if (!res && g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
            {
              res = TRUE;
              g_clear_error (&local_error);

              result_details |= FLATPAK_TRANSACTION_RESULT_NO_CHANGE;
            }
          else if (!res)
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
            }

          if (res)
            {
              emit_op_done (self, op, result_details);

              if (!priv->no_pull)
                *out_needs_prune = TRUE;

              if (g_str_has_prefix (op->ref, "app"))
                *out_needs_triggers = TRUE;
            }
        }
      else
        g_debug ("%s need no update", op->ref);
    }
  else if (op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE)
    {
      g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();
      emit_new_op (self, op, progress);
      if (op->resolved_metakey && !flatpak_check_required_version (op->ref, op->resolved_metakey, error))
        res = FALSE;
      else
        res = flatpak_dir_install_bundle (priv->dir, op->bundle,
                                          op->remote, NULL,
                                          cancellable, error);
      flatpak_transaction_progress_done (progress);

      if (res)
        {
          emit_op_done (self, op, 0);
          *out_needs_prune = TRUE;
          *out_needs_triggers = TRUE;
        }
    }
  else if (op->kind == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    {
      g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();
      FlatpakHelperUninstallFlags flags = 0;

      if (priv->disable_prune)
        flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF;

      if (priv->force_uninstall)
        flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE;

      emit_new_op (self, op, progress);

      res = flatpak_dir_uninstall (priv->dir, op->ref, flags,
                                   cancellable, error);

      flatpak_transaction_progress_done (progress);

      if (res)
        {
          emit_op_done (self, op, 0);
          *out_needs_prune = TRUE;

          if (g_str_has_prefix (op->ref, "app"))
            *out_needs_triggers = TRUE;
        }
    }
  else
    g_assert_not_reached ();

  return res;
}

/* Ensure the operation kind is normalized and not no-op */
static void
flatpak_transaction_normalize_ops (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l, *next;

  for (l = priv->ops; l != NULL; l = next)
    {
      FlatpakTransactionOperation *op = l->data;
      next = l->next;

      if (op->kind == FLATPAK_TRANSACTION_OPERATION_INSTALL_OR_UPDATE)
        {
          g_autoptr(GBytes) deploy_data = NULL;

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

      if (op->kind == FLATPAK_TRANSACTION_OPERATION_UPDATE &&
          !flatpak_dir_needs_update_for_commit_and_subpaths (priv->dir, op->remote, op->ref, op->resolved_commit,
                                                             (const char **) op->subpaths))
        {
          /* If this is a rebase, then at minimum a redeploy needs to happen */
          if (op->previous_ids)
            op->update_only_deploy = TRUE;
          else
            op->skip = TRUE;
        }
    }
}

static gboolean
flatpak_transaction_real_run (FlatpakTransaction *self,
                              GCancellable       *cancellable,
                              GError            **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l;
  gboolean succeeded = TRUE;
  gboolean needs_prune = FALSE;
  gboolean needs_triggers = FALSE;
  gboolean ready_res = FALSE;
  int i;

  if (!priv->can_run)
    return flatpak_fail (error, _("Transaction already executed"));

  priv->can_run = FALSE;

  priv->current_op = NULL;

  if (flatpak_dir_is_user (priv->dir) && getuid () == 0)
    {
      struct stat st_buf;
      g_autofree char *dir_path = NULL;

      /* Check that it's not root's own user installation */
      dir_path = g_file_get_path (flatpak_dir_get_path (priv->dir));
      if (stat (dir_path, &st_buf) == 0 && st_buf.st_uid != 0)
        return flatpak_fail_error (error, FLATPAK_ERROR_WRONG_USER,
                                   _("Refusing to operate on a user installation as root! "
                                     "This can lead to incorrect file ownership and permission errors."));
    }

  if (!priv->no_pull &&
      !flatpak_transaction_update_metadata (self, cancellable, error))
    {
      g_assert (error == NULL || *error != NULL);
      return FALSE;
    }

  if (!flatpak_transaction_add_auto_install (self, cancellable, error))
    {
      g_assert (error == NULL || *error != NULL);
      return FALSE;
    }

  if (!flatpak_transaction_resolve_flatpakrefs (self, cancellable, error))
    {
      g_assert (error == NULL || *error != NULL);
      return FALSE;
    }

  if (!flatpak_transaction_resolve_bundles (self, cancellable, error))
    {
      g_assert (error == NULL || *error != NULL);
      return FALSE;
    }

  /* Resolve initial ops */
  if (!resolve_all_ops (self, cancellable, error))
    {
      g_assert (error == NULL || *error != NULL);
      return FALSE;
    }

  /* Add all app -> runtime dependencies */
  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;

      if (!op->skip && !add_deps (self, op, error))
        {
          g_assert (error == NULL || *error != NULL);
          return FALSE;
        }
    }

  /* Resolve new ops */
  if (!resolve_all_ops (self, cancellable, error))
    {
      g_assert (error == NULL || *error != NULL);
      return FALSE;
    }

  /* Add all related extensions */
  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;

      if (!op->skip && !add_related (self, op, error))
        {
          g_assert (error == NULL || *error != NULL);
          return FALSE;
        }
    }

  /* Resolve new ops */
  if (!resolve_all_ops (self, cancellable, error))
    {
      g_assert (error == NULL || *error != NULL);
      return FALSE;
    }

  /* Ensure we have all required tokens, we do this after all resolves if possible to bunch requests */
  if (!request_required_tokens (self, NULL, cancellable, error))
    {
      g_assert (error == NULL || *error != NULL);
      return FALSE;
    }

  sort_ops (self);

  /* Ensure the operation kind is normalized and not no-op */
  flatpak_transaction_normalize_ops (self);

  g_signal_emit (self, signals[READY], 0, &ready_res);
  if (!ready_res)
    return flatpak_fail_error (error, FLATPAK_ERROR_ABORTED, _("Aborted by user"));

  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      g_autoptr(GError) local_error = NULL;
      gboolean res = TRUE;
      const char *pref;
      g_autoptr(FlatpakRemoteState) state = NULL;

      if (op->skip)
        continue;

      priv->current_op = op;

      pref = strchr (op->ref, '/') + 1;

      if (op->fail_if_op_fails && (op->fail_if_op_fails->failed) &&
          /* Allow installing an app if the runtime failed to update (i.e. is installed) because
           * the app should still run, and otherwise you could never install the app until the runtime
           * remote is fixed. */
          !(op->fail_if_op_fails->kind == FLATPAK_TRANSACTION_OPERATION_UPDATE && g_str_has_prefix (op->ref, "app/")))
        {
          flatpak_fail_error (&local_error, FLATPAK_ERROR_SKIPPED,
                              _("Skipping %s due to previous error"), pref);
          res = FALSE;
        }
      else if (op->kind != FLATPAK_TRANSACTION_OPERATION_UNINSTALL &&
               (state = flatpak_transaction_ensure_remote_state (self, op->kind, op->remote, &local_error)) == NULL)
        {
          res = FALSE;
        }

      /* Here we execute the operation in a helper function */
      if (res && !_run_op_kind (self, op, state, &needs_prune, &needs_triggers, cancellable, &local_error))
        res = FALSE;

      if (res)
        {
          g_autoptr(GBytes) deploy_data = NULL;
          deploy_data = flatpak_dir_get_deploy_data (priv->dir, op->ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);

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
              if (g_cancellable_set_error_if_cancelled (cancellable, error))
                {
                  succeeded = FALSE;
                  break;
                }

              flatpak_fail_error (error, FLATPAK_ERROR_ABORTED, _("Aborted due to failure"));
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

  for (i = 0; i < priv->added_pinned_runtimes->len; i++)
    {
      const char *pinned_runtime = g_ptr_array_index (priv->added_pinned_runtimes, i);
      if (!dir_ref_is_installed (priv->dir, pinned_runtime, NULL, NULL))
        {
          flatpak_dir_config_remove_pattern (priv->dir, "pinned", pinned_runtime, NULL);
          flatpak_installation_drop_caches (priv->installation, NULL, NULL);
        }
    }

  return succeeded;
}
