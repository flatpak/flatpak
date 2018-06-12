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

typedef struct FlatpakTransactionOp FlatpakTransactionOp;

typedef enum {
  FLATPAK_TRANSACTION_OP_KIND_INSTALL,
  FLATPAK_TRANSACTION_OP_KIND_UPDATE,
  FLATPAK_TRANSACTION_OP_KIND_BUNDLE,
  FLATPAK_TRANSACTION_OP_KIND_INSTALL_OR_UPDATE,
  FLATPAK_TRANSACTION_OP_KIND_UNINSTALL,
} FlatpakTransactionOpKind;

struct FlatpakTransactionOp {
  char *remote;
  char *ref;
  /* NULL means unspecified (normally keep whatever was there before), [] means force everything */
  char **subpaths;
  char *commit;
  GFile *bundle;
  FlatpakTransactionOpKind kind;
  gboolean non_fatal;
  FlatpakTransactionOp *source_op; /* This is the main app/runtime ref for related extensions, and the runtime for apps */
  gboolean failed;
};

typedef struct _FlatpakTransactionPrivate FlatpakTransactionPrivate;

struct _FlatpakTransactionPrivate {
  GObject parent;

  FlatpakInstallation *installation;
  FlatpakDir *dir;
  GHashTable *last_op_for_ref;
  GHashTable *remote_states; /* (element-type utf8 FlatpakRemoteState) */
  GPtrArray *system_dirs;
  GList *ops;
  GPtrArray *added_origin_remotes;

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

static FlatpakTransactionOperationType
op_type_from_resolved_kind (FlatpakTransactionOpKind kind)
{
  switch (kind)
    {
    case FLATPAK_TRANSACTION_OP_KIND_INSTALL:
      return FLATPAK_TRANSACTION_OPERATION_INSTALL;
    case FLATPAK_TRANSACTION_OP_KIND_UPDATE:
      return FLATPAK_TRANSACTION_OPERATION_UPDATE;
    case FLATPAK_TRANSACTION_OP_KIND_BUNDLE:
      return FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE;
    case FLATPAK_TRANSACTION_OP_KIND_UNINSTALL:
      return FLATPAK_TRANSACTION_OPERATION_UNINSTALL;

      /* This should be resolve before converting to type */
    case FLATPAK_TRANSACTION_OP_KIND_INSTALL_OR_UPDATE:
    default:
      g_assert_not_reached ();
    }
}

static gboolean
transaction_is_local_only (FlatpakTransaction *self,
                           FlatpakTransactionOpKind kind)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  return priv->no_pull || kind == FLATPAK_TRANSACTION_OP_KIND_UNINSTALL;
}

static gboolean
remote_name_is_file (const char *remote_name)
{
  return remote_name != NULL &&
    g_str_has_prefix (remote_name, "file://");
}

/* Check if the ref is in the dir, or in the system dir, in case its a
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

  /* Don't try to fallback for the system's default directory. */
  if (!flatpak_dir_is_user (dir) && flatpak_dir_get_id (dir) == NULL)
    return FALSE;

  /* Lazy initialization of this, once per transaction */
  if (priv->system_dirs == NULL)
    {
      priv->system_dirs = flatpak_dir_get_system_list (NULL, error);
      if (priv->system_dirs == NULL)
        return FALSE;
    }

  for (i = 0; i < priv->system_dirs->len; i++)
    {
      FlatpakDir *system_dir = g_ptr_array_index (priv->system_dirs, i);

      if (g_strcmp0 (flatpak_dir_get_id (dir), flatpak_dir_get_id (system_dir)) == 0)
        continue;

      deploy_dir = flatpak_dir_get_if_deployed (system_dir, ref, NULL, NULL);
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

static FlatpakTransactionOp *
flatpak_transaction_operation_new (const char *remote,
                                   const char *ref,
                                   const char **subpaths,
                                   const char *commit,
                                   GFile *bundle,
                                   FlatpakTransactionOpKind kind)
{
  FlatpakTransactionOp *self = g_new0 (FlatpakTransactionOp, 1);

  self->remote = g_strdup (remote);
  self->ref = g_strdup (ref);
  self->subpaths = g_strdupv ((char **)subpaths);
  self->commit = g_strdup (commit);
  if (bundle)
    self->bundle = g_object_ref (bundle);
  self->kind = kind;

  return self;
}

static void
flatpak_transaction_operation_free (FlatpakTransactionOp *self)
{
  g_free (self->remote);
  g_free (self->ref);
  g_free (self->commit);
  g_strfreev (self->subpaths);
  g_clear_object (&self->bundle);
  g_free (self);
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
  g_list_free_full (priv->ops, (GDestroyNotify)flatpak_transaction_operation_free);
  g_object_unref (priv->dir);

  g_ptr_array_unref (priv->added_origin_remotes);

  if (priv->system_dirs != NULL)
    g_ptr_array_free (priv->system_dirs, TRUE);

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

static void
flatpak_transaction_class_init (FlatpakTransactionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

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
                  G_TYPE_NONE, 5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, FLATPAK_TYPE_TRANSACTION_PROGRESS);

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
                  G_TYPE_BOOLEAN, 5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_ERROR, G_TYPE_INT);

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
                  G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);

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
}

static void
flatpak_transaction_init (FlatpakTransaction *self)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);

  priv->last_op_for_ref = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  priv->remote_states = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)flatpak_remote_state_free);
  priv->added_origin_remotes = g_ptr_array_new_with_free_func (g_free);
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

static FlatpakTransactionOp *
flatpak_transaction_get_last_op_for_ref (FlatpakTransaction *self,
                                         const char *ref)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  FlatpakTransactionOp *op;

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
kind_to_str (FlatpakTransactionOpKind kind)
{
  switch (kind)
    {
    case FLATPAK_TRANSACTION_OP_KIND_INSTALL:
      return "install";
    case FLATPAK_TRANSACTION_OP_KIND_UPDATE:
      return "update";
    case FLATPAK_TRANSACTION_OP_KIND_INSTALL_OR_UPDATE:
      return "install/update";
    case FLATPAK_TRANSACTION_OP_KIND_BUNDLE:
      return "install bundle";
    case FLATPAK_TRANSACTION_OP_KIND_UNINSTALL:
      return "uninstall";
    }
  return "unknown";
}

static FlatpakRemoteState *
flatpak_transaction_ensure_remote_state (FlatpakTransaction *self,
                                         FlatpakTransactionOpKind kind,
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
kind_compatible (FlatpakTransactionOpKind a,
                 FlatpakTransactionOpKind b)
{
  if (a == b)
    return TRUE;

  if (a == FLATPAK_TRANSACTION_OP_KIND_INSTALL_OR_UPDATE &&
      (b == FLATPAK_TRANSACTION_OP_KIND_INSTALL ||
       b == FLATPAK_TRANSACTION_OP_KIND_UPDATE))
    return TRUE;

  if (b == FLATPAK_TRANSACTION_OP_KIND_INSTALL_OR_UPDATE &&
      (a == FLATPAK_TRANSACTION_OP_KIND_INSTALL ||
       a == FLATPAK_TRANSACTION_OP_KIND_UPDATE))
    return TRUE;

  return FALSE;
}

static FlatpakTransactionOp *
flatpak_transaction_add_op (FlatpakTransaction *self,
                            FlatpakTransactionOp *before_op,
                            const char *remote,
                            const char *ref,
                            const char **subpaths,
                            const char *commit,
                            GFile *bundle,
                            FlatpakTransactionOpKind kind)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  FlatpakTransactionOp *op;
  g_autofree char *subpaths_str = NULL;
  GList *before_l = NULL;

  subpaths_str = subpaths_to_string (subpaths);
  g_debug ("Transaction: %s %s:%s%s%s%s%s%s",
           kind_to_str (kind), remote, ref,
           commit != NULL ? "@" : "",
           commit != NULL ? commit : "",
           subpaths_str,
           before_op ? " before op " : "",
           before_op ? before_op->ref : ""
           );

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

  if (before_op != NULL)
    before_l = g_list_find (priv->ops, before_op);

  /* Note: we build the list in reverse order, so before => after before_l == before before_l->next */
  if (before_l)
    priv->ops = g_list_insert_before (priv->ops,
                                      before_l->next,
                                      op);
  else
    priv->ops = g_list_prepend (priv->ops, op);

  return op;
}

static gboolean
add_related (FlatpakTransaction *self,
             FlatpakTransactionOpKind source_kind,
             FlatpakRemoteState *state,
             const char *remote,
             const char *ref,
             FlatpakTransactionOp *source_op,
             FlatpakTransactionOp *before_op,
             GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autoptr(GPtrArray) related = NULL;
  g_autoptr(GError) local_error = NULL;
  int i;

  if (priv->disable_related)
    return TRUE;

  if (transaction_is_local_only (self, source_kind))
    related = flatpak_dir_find_local_related (priv->dir, ref, remote,
                                              /* Look for deployed if uninstalling, in repo otherwise */
                                              source_kind == FLATPAK_TRANSACTION_OP_KIND_UNINSTALL,
                                              NULL, &local_error);
  else
    related = flatpak_dir_find_remote_related (priv->dir, state, ref, NULL, &local_error);

  if (related == NULL)
    {
      g_message (_("Warning: Problem looking for related refs: %s"), local_error->message);
      g_clear_error (&local_error);
    }
  else if (source_kind == FLATPAK_TRANSACTION_OP_KIND_UNINSTALL)
    {
      for (i = 0; i < related->len; i++)
        {
          FlatpakRelated *rel = g_ptr_array_index (related, i);
          FlatpakTransactionOp *op;

          if (!rel->delete)
            continue;

          op = flatpak_transaction_add_op (self, before_op, remote, rel->ref,
                                           NULL, NULL, NULL,
                                           FLATPAK_TRANSACTION_OP_KIND_UNINSTALL);
          op->non_fatal = TRUE;
          op->source_op = source_op;
        }
    }
  else /* install or update */
    {
      for (i = 0; i < related->len; i++)
        {
          FlatpakRelated *rel = g_ptr_array_index (related, i);
          FlatpakTransactionOp *op;

          if (!rel->download)
            continue;

          op = flatpak_transaction_add_op (self, before_op, remote, rel->ref,
                                           (const char **)rel->subpaths,
                                           NULL, NULL,
                                           FLATPAK_TRANSACTION_OP_KIND_INSTALL_OR_UPDATE);
          op->non_fatal = TRUE;
          op->source_op = source_op;
        }
    }

  return TRUE;
}

static char *
find_runtime_remote (FlatpakTransaction *self,
                     const char *app_ref,
                     const char *runtime_ref,
                     FlatpakTransactionOpKind source_kind,
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
          FlatpakTransactionOpKind source_kind,
          GKeyFile *metakey,
          FlatpakRemoteState *state,
          const char *remote,
          const char *ref,
          FlatpakTransactionOp **dep_op_out,
          FlatpakTransactionOp **before_op_out,
          GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *runtime_ref = NULL;
  g_autofree char *full_runtime_ref = NULL;
  g_autofree char *runtime_remote = NULL;
  FlatpakTransactionOp *op = NULL;

  if (dep_op_out)
    *dep_op_out = NULL;
  if (before_op_out)
    *before_op_out = NULL;

  if (!g_str_has_prefix (ref, "app/"))
    return TRUE;

  if (metakey)
    runtime_ref = g_key_file_get_string (metakey, "Application", "runtime", NULL);

  if (runtime_ref == NULL)
    return TRUE;

  full_runtime_ref = g_strconcat ("runtime/", runtime_ref, NULL);

  op = flatpak_transaction_get_last_op_for_ref (self, full_runtime_ref);

  if (source_kind == FLATPAK_TRANSACTION_OP_KIND_UNINSTALL)
    {
      /* If the runtime this app uses is already to be uninstalled, then this uninstall must happen before
         the runtime is installed */
      if (op && op->kind == FLATPAK_TRANSACTION_OP_KIND_UNINSTALL &&
          before_op_out != NULL)
        {
          *before_op_out = op;
        }

      return TRUE;
    }

  if (priv->disable_deps)
    return TRUE;

  if (op == NULL)
    {
      g_autoptr(GError) local_error = NULL;

      if (!ref_is_installed (self, full_runtime_ref, &local_error))
        {
          if (local_error != NULL)
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          runtime_remote = find_runtime_remote (self, ref, full_runtime_ref, source_kind, error);
          if (runtime_remote == NULL)
            return FALSE;

          op = flatpak_transaction_add_op (self, NULL, runtime_remote, full_runtime_ref, NULL, NULL, NULL,
                                           FLATPAK_TRANSACTION_OP_KIND_INSTALL_OR_UPDATE);
        }
      else
        {
          /* Update if in same dir */
          if (dir_ref_is_installed (priv->dir, full_runtime_ref, &runtime_remote, NULL))
            {
              g_debug ("Updating dependent runtime %s", full_runtime_ref);
              op = flatpak_transaction_add_op (self, NULL, runtime_remote, full_runtime_ref, NULL, NULL, NULL,
                                               FLATPAK_TRANSACTION_OP_KIND_UPDATE);
              op->non_fatal = TRUE;
            }
        }
    }

  if (runtime_remote != NULL &&
      !add_related (self, source_kind, state, runtime_remote, full_runtime_ref, op, NULL, error))
    return FALSE;

  if (dep_op_out)
    *dep_op_out = op;

  return TRUE;
}

static gboolean
flatpak_transaction_add_ref (FlatpakTransaction *self,
                             const char *remote,
                             const char *ref,
                             const char **subpaths,
                             const char *commit,
                             FlatpakTransactionOpKind kind,
                             GFile *bundle,
                             const char *metadata,
                             GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  g_autofree char *origin = NULL;
  const char *pref;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autofree char *origin_remote = NULL;
  FlatpakRemoteState *state = NULL;
  FlatpakTransactionOp *dep_op = NULL;
  FlatpakTransactionOp *before_op = NULL;
  FlatpakTransactionOp *main_op;
  g_autoptr(GVariant) commit_metadata = NULL;

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
  if (kind == FLATPAK_TRANSACTION_OP_KIND_UPDATE)
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
  else if (kind == FLATPAK_TRANSACTION_OP_KIND_INSTALL)
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
  else if (kind == FLATPAK_TRANSACTION_OP_KIND_UNINSTALL)
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

  state = flatpak_transaction_ensure_remote_state (self, kind, remote, error);
  if (state == NULL)
    return FALSE;

  if (metadata == NULL)
    {
      /* Should we use local state */
      if (transaction_is_local_only (self, kind))
        {
          g_autoptr(GVariant) commit_data = flatpak_dir_read_latest_commit (priv->dir, remote, ref,
                                                                            NULL, NULL);
          if (commit_data)
            {
              commit_metadata = g_variant_get_child_value (commit_data, 0);
              g_variant_lookup (commit_metadata, "xa.metadata", "&s", &metadata);
              if (metadata == NULL)
                g_debug ("No xa.metadata in local commit");
            }
        }
      else if (!flatpak_remote_state_lookup_cache (state, ref, NULL, NULL, &metadata, &local_error))
        {
          g_message (_("Warning: Can't find dependencies: %s"), local_error->message);
          g_clear_error (&local_error);
        }
    }

  if (metadata)
    {
      metakey = g_key_file_new ();
      if (!g_key_file_load_from_data (metakey, metadata, -1, 0, NULL))
        g_clear_object (&metakey);
    }

  if (metakey && kind != FLATPAK_TRANSACTION_OP_KIND_UNINSTALL &&
      !flatpak_check_required_version (ref, metakey, error))
    return FALSE;

  if (!add_deps (self, kind, metakey, state, remote, ref, &dep_op, &before_op, error))
    return FALSE;

  main_op = flatpak_transaction_add_op (self, before_op, remote, ref, subpaths, commit, bundle, kind);
  main_op->source_op = dep_op;

  if (!add_related (self, kind, state, remote, ref, main_op, before_op, error))
    return FALSE;

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

  return flatpak_transaction_add_ref (self, remote, ref, subpaths, NULL, FLATPAK_TRANSACTION_OP_KIND_INSTALL, NULL, NULL, error);
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
  g_autofree char *metadata = NULL;
  gboolean created_remote;

  remote = flatpak_dir_ensure_bundle_remote (priv->dir, file, gpg_data,
                                             &ref, &metadata, &created_remote,
                                             NULL, error);
  if (remote == NULL)
    return FALSE;

  if (!flatpak_dir_recreate_repo (priv->dir, NULL, error))
    return FALSE;

  return flatpak_transaction_add_ref (self, remote, ref, NULL, NULL, FLATPAK_TRANSACTION_OP_KIND_BUNDLE, file, metadata, error);
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

  return flatpak_transaction_add_ref (self, NULL, ref, subpaths, commit, FLATPAK_TRANSACTION_OP_KIND_UPDATE, NULL, NULL, error);
}

gboolean
flatpak_transaction_add_uninstall (FlatpakTransaction  *self,
                                   const char          *ref,
                                   GError             **error)
{
  return flatpak_transaction_add_ref (self, NULL, ref, NULL, NULL, FLATPAK_TRANSACTION_OP_KIND_UNINSTALL, NULL, NULL, error);
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
      FlatpakTransactionOp *op = l->data;
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
emit_new_op (FlatpakTransaction *self, FlatpakTransactionOp *op, FlatpakTransactionProgress *progress)
{
  g_signal_emit (self, signals[NEW_OPERATION], 0, op->ref, op->remote,
                 op->bundle ? flatpak_file_get_path_cached (op->bundle) : NULL,
                 op_type_from_resolved_kind (op->kind), progress);
}

static void
emit_op_done (FlatpakTransaction *self,
              FlatpakTransactionOp *op,
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

  g_signal_emit (self, signals[OPERATION_DONE], 0, op->ref, op->remote,
                 op_type_from_resolved_kind (op->kind),
                 commit, details);
}

gboolean
flatpak_transaction_run (FlatpakTransaction *self,
                         GCancellable *cancellable,
                         GError **error)
{
  FlatpakTransactionPrivate *priv = flatpak_transaction_get_instance_private (self);
  GList *l;
  gboolean succeeded = TRUE;
  gboolean needs_prune = FALSE;
  gboolean needs_triggers = FALSE;
  g_autoptr(GMainContextPopDefault) main_context = NULL;
  int i;

  if (!priv->no_pull &&
      !flatpak_transaction_update_metadata (self, cancellable, error))
    return FALSE;

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = flatpak_main_context_new_default ();

  priv->ops = g_list_reverse (priv->ops);

  for (l = priv->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOp *op = l->data;
      g_autoptr(GError) local_error = NULL;
      gboolean res = TRUE;
      const char *pref;
      FlatpakTransactionOpKind kind;
      FlatpakRemoteState *state;

      kind = op->kind;
      if (kind == FLATPAK_TRANSACTION_OP_KIND_INSTALL_OR_UPDATE)
        {
          g_autoptr(GVariant) deploy_data = NULL;

          if (dir_ref_is_installed (priv->dir, op->ref, NULL, &deploy_data))
            {
              /* Don't use the remote from related ref on update, always use
                 the current remote. */
              g_free (op->remote);
              op->remote = g_strdup (flatpak_deploy_data_get_origin (deploy_data));

              kind = FLATPAK_TRANSACTION_OP_KIND_UPDATE;
            }
          else
            kind = FLATPAK_TRANSACTION_OP_KIND_INSTALL;

          op->kind = kind;
        }

      pref = strchr (op->ref, '/') + 1;

      if (op->source_op && (op->source_op->failed) &&
          /* Allow installing an app if the runtime failed to update (i.e. is installed) because
           * the app should still run, and otherwise you could never install the app until the runtime
           * remote is fixed. */
          !(op->source_op->kind == FLATPAK_TRANSACTION_OP_KIND_UPDATE && g_str_has_prefix (op->ref, "app/")))
        {
          g_set_error (&local_error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED,
                       _("Skipping %s due to previous error"), pref);
          res = FALSE;
        }
      else if ((state = flatpak_transaction_ensure_remote_state (self, op->kind, op->remote, &local_error)) == NULL)
        {
          res = FALSE;
        }
      else if (kind == FLATPAK_TRANSACTION_OP_KIND_INSTALL)
        {
          g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();

          emit_new_op (self, op, progress);

          res = flatpak_dir_install (priv->dir ,
                                     priv->no_pull,
                                     priv->no_deploy,
                                     priv->disable_static_deltas,
                                     priv->reinstall,
                                     state, op->ref,
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
      else if (kind == FLATPAK_TRANSACTION_OP_KIND_UPDATE)
        {
          g_auto(OstreeRepoFinderResultv) check_results = NULL;

          g_autofree char *target_commit = flatpak_dir_check_for_update (priv->dir, state, op->ref, op->commit,
                                                                         (const char **)op->subpaths,
                                                                         priv->no_pull,
                                                                         &check_results,
                                                                         cancellable, &local_error);
          if (target_commit != NULL)
            {
              g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();
              FlatpakTransactionResult result_details = 0;

              emit_new_op (self, op, progress);

              res = flatpak_dir_update (priv->dir,
                                        priv->no_pull,
                                        priv->no_deploy,
                                        priv->disable_static_deltas,
                                        op->commit != NULL, /* Allow downgrade if we specify commit */
                                        state, op->ref, target_commit,
                                        (const OstreeRepoFinderResult * const *) check_results,
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
            {
              res = FALSE;
              if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
                {
                  res = TRUE;
                  g_clear_error (&local_error);
                }
            }
        }
      else if (kind == FLATPAK_TRANSACTION_OP_KIND_BUNDLE)
        {
          g_autoptr(FlatpakTransactionProgress) progress = flatpak_transaction_progress_new ();
          emit_new_op (self, op, progress);
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
      else if (kind == FLATPAK_TRANSACTION_OP_KIND_UNINSTALL)
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

          g_signal_emit (self, signals[OPERATION_ERROR], 0,
                         op->ref,
                         op->remote,
                         op_type_from_resolved_kind (kind),
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

  if (needs_triggers)
    flatpak_dir_run_triggers (priv->dir, cancellable, NULL);

  if (needs_prune && !priv->disable_prune)
    flatpak_dir_prune (priv->dir, cancellable, NULL);

  for (i = 0; i < priv->added_origin_remotes->len; i++)
    flatpak_dir_prune_origin_remote (priv->dir, g_ptr_array_index (priv->added_origin_remotes, i));

  return succeeded;
}
