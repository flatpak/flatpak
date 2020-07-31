/*
 * Copyright © 2014 Red Hat, Inc
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

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include <glib/gprintf.h>
#include <polkit/polkit.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pwd.h>
#include <gio/gunixfdlist.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>

#include "flatpak-dbus-generated.h"
#include "flatpak-dir-private.h"
#include "flatpak-error.h"
#include "flatpak-oci-registry-private.h"
#include "flatpak-progress-private.h"
#include "flatpak-utils-base-private.h"
#include "flatpak-utils-private.h"

static PolkitAuthority *authority = NULL;
static FlatpakSystemHelper *helper = NULL;
static GMainLoop *main_loop = NULL;
static guint name_owner_id = 0;

G_LOCK_DEFINE (cache_dirs_in_use);
static GHashTable *cache_dirs_in_use = NULL;

static gboolean on_session_bus = FALSE;
static gboolean disable_revokefs = FALSE;
static gboolean no_idle_exit = FALSE;

static int opt_verbose;
static gboolean opt_ostree_verbose;

#define IDLE_TIMEOUT_SECS 10 * 60

/* This uses a weird Auto prefix to avoid conflicts with later added polkit types.
 */
typedef PolkitAuthorizationResult AutoPolkitAuthorizationResult;
typedef PolkitDetails             AutoPolkitDetails;
typedef PolkitSubject             AutoPolkitSubject;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitAuthorizationResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitDetails, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitSubject, g_object_unref)

typedef struct
{
  FlatpakSystemHelper *object;
  GDBusMethodInvocation *invocation;
  GCancellable *cancellable;
  gboolean preserve_pull;     /* Whether to preserve partially pulled repo on pull failure */

  guint watch_id;
  uid_t uid;                  /* uid of the client initiating the pull */

  gint client_socket;         /* fd that is send back to the client for spawning revoke-fuse */
  gint backend_exit_socket;   /* write end of a pipe which helps terminating revokefs backend if
                                 system helper exits abruptly */

  gchar *src_dir;             /* source directory containing the actual child repo */
  gchar *unique_name;

  GSubprocess *revokefs_backend;
} OngoingPull;

static void
terminate_revokefs_backend (OngoingPull *pull)
{
  /* Terminating will guarantee that all access to write operations are revoked. */
  if (shutdown (pull->client_socket, SHUT_RDWR) == -1 ||
      !g_subprocess_wait (pull->revokefs_backend, NULL, NULL))
    {
      g_warning ("Failed to shutdown client socket, killing backend writer process");
      g_subprocess_force_exit (pull->revokefs_backend);
    }

  g_clear_object (&pull->revokefs_backend);
}

static gboolean
remove_dir_from_cache_dirs_in_use (const char *src_dir)
{
  gboolean res;

  G_LOCK (cache_dirs_in_use);
  res = g_hash_table_remove (cache_dirs_in_use, src_dir);
  G_UNLOCK (cache_dirs_in_use);

  return res;
}

static void
ongoing_pull_free (OngoingPull *pull)
{
  g_autoptr(GFile) src_dir_file = NULL;
  g_autoptr(GError) local_error = NULL;

  g_clear_handle_id (&pull->watch_id, g_bus_unwatch_name);

  src_dir_file = g_file_new_for_path (pull->src_dir);

  if (pull->revokefs_backend)
    terminate_revokefs_backend (pull);

  if (!pull->preserve_pull &&
      !flatpak_rm_rf (src_dir_file, NULL, &local_error))
    {
      g_warning ("Unable to remove ongoing pull's src dir at %s: %s",
                 pull->src_dir, local_error->message);
      g_clear_error (&local_error);
    }

  remove_dir_from_cache_dirs_in_use (pull->src_dir);

  g_clear_pointer (&pull->src_dir, g_free);
  g_clear_pointer (&pull->unique_name, g_free);
  close (pull->client_socket);
  close (pull->backend_exit_socket);

  g_slice_free (OngoingPull, pull);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OngoingPull, ongoing_pull_free);

static void
skeleton_died_cb (gpointer data)
{
  g_debug ("skeleton finalized, exiting");
  g_main_loop_quit (main_loop);
}

static gboolean
unref_skeleton_in_timeout_cb (gpointer user_data)
{
  static gboolean unreffed = FALSE;

  g_debug ("unreffing helper main ref");
  if (!unreffed)
    {
      g_object_unref (helper);
      unreffed = TRUE;
    }

  return G_SOURCE_REMOVE;
}

static void
unref_skeleton_in_timeout (void)
{
  if (name_owner_id)
    g_bus_unown_name (name_owner_id);
  name_owner_id = 0;

  /* After we've lost the name or idled we drop the main ref on the helper
     so that we'll exit when it drops to zero. However, if there are
     outstanding calls these will keep the refcount up during the
     execution of them. We do the unref on a timeout to make sure
     we're completely draining the queue of (stale) requests. */
  g_timeout_add (500, unref_skeleton_in_timeout_cb, NULL);
}

static gboolean
idle_timeout_cb (gpointer user_data)
{
  G_LOCK (cache_dirs_in_use);
  guint ongoing_pulls_len = g_hash_table_size (cache_dirs_in_use);
  G_UNLOCK (cache_dirs_in_use);
  if (ongoing_pulls_len != 0)
    return G_SOURCE_CONTINUE;

  if (name_owner_id)
    {
      g_debug ("Idle - unowning name");
      unref_skeleton_in_timeout ();
    }
  return G_SOURCE_REMOVE;
}

G_LOCK_DEFINE_STATIC (idle);
static void
schedule_idle_callback (void)
{
  static guint idle_timeout_id = 0;

  G_LOCK (idle);

  if (!no_idle_exit)
    {
      if (idle_timeout_id != 0)
        g_source_remove (idle_timeout_id);

      idle_timeout_id = g_timeout_add_seconds (IDLE_TIMEOUT_SECS, idle_timeout_cb, NULL);
    }

  G_UNLOCK (idle);
}

static FlatpakDir *
dir_get_system (const char *installation,
                pid_t       source_pid,
                GError    **error)
{
  FlatpakDir *system = NULL;

  if (installation != NULL && *installation != '\0')
    system = flatpak_dir_get_system_by_id (installation, NULL, error);
  else
    system = flatpak_dir_get_system_default ();

  /* This can happen in case of error with flatpak_dir_get_system_by_id(). */
  if (system == NULL)
    return NULL;

  flatpak_dir_set_source_pid (system, source_pid);
  flatpak_dir_set_no_system_helper (system, TRUE);

  return system;
}

#define DBUS_NAME_DBUS "org.freedesktop.DBus"
#define DBUS_INTERFACE_DBUS DBUS_NAME_DBUS
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"

static int
get_sender_pid (GDBusMethodInvocation *invocation)
{
  g_autoptr(GDBusMessage) msg = NULL;
  g_autoptr(GDBusMessage) reply = NULL;
  GDBusConnection *connection;
  const char *sender;
  GVariant *body;
  g_autoptr(GVariantIter) iter = NULL;
  const char *key;
  g_autoptr(GVariant) value = NULL;

  connection = g_dbus_method_invocation_get_connection (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  msg = g_dbus_message_new_method_call (DBUS_NAME_DBUS,
                                        DBUS_PATH_DBUS,
                                        DBUS_INTERFACE_DBUS,
                                        "GetConnectionCredentials");
  g_dbus_message_set_body (msg, g_variant_new ("(s)", sender));

  reply = g_dbus_connection_send_message_with_reply_sync (connection, msg,
                                                          G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                          30000,
                                                          NULL,
                                                          NULL,
                                                          NULL);
  if (reply == NULL)
    return 0;

  if (g_dbus_message_get_message_type (reply) == G_DBUS_MESSAGE_TYPE_ERROR)
    return 0;

  body = g_dbus_message_get_body (reply);

  g_variant_get (body, "(a{sv})", &iter);
  while (g_variant_iter_loop (iter, "{&sv}", &key, &value))
    {
      if (strcmp (key, "ProcessID") == 0)
        return g_variant_get_uint32 (value);
    }
  value = NULL; /* g_variant_iter_loop freed it */

  return 0;
}

static void
flatpak_invocation_return_error (GDBusMethodInvocation *invocation,
                                 GError                *error,
                                 const char            *fmt,
                                 ...)
{
  if (error->domain == FLATPAK_ERROR)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    {
      va_list args;
      g_autofree char *prefix = NULL;
      va_start (args, fmt);
      g_vasprintf (&prefix, fmt, args);
      va_end (args);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "%s: %s", prefix, error->message);
    }
}

static gboolean
get_connection_uid (GDBusMethodInvocation *invocation, uid_t *out_uid, GError **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GVariant) dict = NULL;
  g_autoptr(GVariant) credentials = NULL;

  credentials = g_dbus_connection_call_sync (connection,
                                             "org.freedesktop.DBus",
                                             "/org/freedesktop/DBus",
                                             "org.freedesktop.DBus",
                                             "GetConnectionCredentials",
                                             g_variant_new ("(s)", sender),
                                             G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
                                             G_MAXINT, NULL, error);
  if (credentials == NULL)
    return FALSE;

  dict = g_variant_get_child_value (credentials, 0);

   if (!g_variant_lookup (dict, "UnixUserID", "u", out_uid))
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "Failed to query UnixUserID for the bus name: %s", sender);
      return FALSE;
    }

  return TRUE;
}

static OngoingPull *
take_ongoing_pull_by_dir (const gchar *src_dir)
{
  OngoingPull *pull = NULL;
  gpointer key, value;

  G_LOCK (cache_dirs_in_use);
  /* Keep src_dir key inside hashtable but remove its OngoingPull
   * value and set it to NULL. This way src_dir is still marked
   * as in-use (as Deploy or CancelPull might be executing on it,
   * whereas OngoingPull ownership is transferred to respective
   * callers. */
  if (g_hash_table_steal_extended (cache_dirs_in_use, src_dir, &key, &value))
    {
      if (value)
        {
          g_hash_table_insert (cache_dirs_in_use, key, NULL);
          pull = value;
        }
    }
  G_UNLOCK (cache_dirs_in_use);

  return pull;
}

static gboolean
handle_deploy (FlatpakSystemHelper   *object,
               GDBusMethodInvocation *invocation,
               const gchar           *arg_repo_path,
               guint32                arg_flags,
               const gchar           *arg_ref,
               const gchar           *arg_origin,
               const gchar *const    *arg_subpaths,
               const gchar *const    *arg_previous_ids,
               const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GFile) repo_file = g_file_new_for_path (arg_repo_path);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  gboolean is_oci;
  gboolean is_update;
  gboolean no_deploy;
  gboolean local_pull;
  gboolean reinstall;
  g_autofree char *url = NULL;
  g_autoptr(OngoingPull) ongoing_pull = NULL;
  g_autofree gchar *src_dir = NULL;

  g_debug ("Deploy %s %u %s %s %s", arg_repo_path, arg_flags, arg_ref, arg_origin, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  src_dir = g_path_get_dirname (arg_repo_path);
  ongoing_pull = take_ongoing_pull_by_dir (src_dir);
  if (ongoing_pull != NULL)
    {
      g_autoptr(GError) local_error = NULL;
      uid_t uid;

      /* Ensure that pull's uid is same as the caller's uid */
      if (!get_connection_uid (invocation, &uid, &local_error))
        {
          g_dbus_method_invocation_return_gerror (invocation, local_error);
          return TRUE;
        }
      else
        {
          if (ongoing_pull->uid != uid)
            {
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                     "Ongoing pull's uid(%d) does not match with peer uid(%d)",
                                                     ongoing_pull->uid, uid);
              return TRUE;
            }
        }

      terminate_revokefs_backend (ongoing_pull);

      if (!flatpak_canonicalize_permissions (AT_FDCWD,
                                             arg_repo_path,
                                             getuid() == 0 ? 0 : -1,
                                             getuid() == 0 ? 0 : -1,
                                             &local_error))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Failed to canonicalize permissions of repo %s: %s",
                                                 arg_repo_path, local_error->message);
          return TRUE;
        }

      /* At this point, the cache-dir's repo is owned by root. Hence, any failure
       * from here on, should always cleanup the cache-dir and not preserve it to be re-used. */
      ongoing_pull->preserve_pull = FALSE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_DEPLOY_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_DEPLOY_FLAGS_ALL));
      return TRUE;
    }

  if (!g_file_query_exists (repo_file, NULL))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Path does not exist");
      return TRUE;
    }

  no_deploy = (arg_flags & FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY) != 0;
  local_pull = (arg_flags & FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL) != 0;
  reinstall = (arg_flags & FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL) != 0;

  deploy_dir = flatpak_dir_get_if_deployed (system, arg_ref, NULL, NULL);

  is_update = (deploy_dir && !reinstall);
  if (is_update)
    {
      g_autofree char *real_origin = NULL;
      real_origin = flatpak_dir_get_origin (system, arg_ref, NULL, NULL);
      if (g_strcmp0 (real_origin, arg_origin) != 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "Wrong origin %s for update", arg_origin);
          return TRUE;
        }
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Can't open system repo %s", arg_installation);
      return TRUE;
    }

  is_oci = flatpak_dir_get_remote_oci (system, arg_origin);

  if (is_update && !is_oci)
    {
      /* Take this opportunity to clean up refs/mirrors/ since a prune will happen
       * after this update operation. See
       * https://github.com/flatpak/flatpak/issues/3222
       */
      if (!flatpak_dir_delete_mirror_refs (system, FALSE, NULL, &error))
        {
          flatpak_invocation_return_error (invocation, error, "Can't delete mirror refs");
          return TRUE;
        }
    }

  if (strlen (arg_repo_path) > 0 && is_oci)
    {
      g_autoptr(GFile) registry_file = g_file_new_for_path (arg_repo_path);
      g_autofree char *registry_uri = g_file_get_uri (registry_file);
      g_autoptr(FlatpakOciRegistry) registry = NULL;
      g_autoptr(FlatpakOciIndex) index = NULL;
      const FlatpakOciManifestDescriptor *desc;
      g_autoptr(FlatpakOciVersioned) versioned = NULL;
      g_autoptr(FlatpakOciImage) image_config = NULL;
      g_autoptr(FlatpakRemoteState) state = NULL;
      g_autoptr(GHashTable) remote_refs = NULL;
      g_autofree char *checksum = NULL;
      const char *verified_digest;
      g_autofree char *upstream_url = NULL;

      ostree_repo_remote_get_url (flatpak_dir_get_repo (system),
                                  arg_origin,
                                  &upstream_url,
                                  NULL);

      if (upstream_url == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Remote %s is disabled", arg_origin);
          return TRUE;
        }

      registry = flatpak_oci_registry_new (registry_uri, FALSE, -1, NULL, &error);
      if (registry == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't open child OCI registry: %s", error->message);
          return TRUE;
        }

      index = flatpak_oci_registry_load_index (registry, NULL, &error);
      if (index == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't open child OCI registry index: %s", error->message);
          return TRUE;
        }

      desc = flatpak_oci_index_get_manifest (index, arg_ref);
      if (desc == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't find ref %s in child OCI registry index", arg_ref);
          return TRUE;
        }

      versioned = flatpak_oci_registry_load_versioned (registry, NULL, desc->parent.digest, (const char **)desc->parent.urls, NULL,
                                                       NULL, &error);
      if (versioned == NULL || !FLATPAK_IS_OCI_MANIFEST (versioned))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't open child manifest");
          return TRUE;
        }

      image_config = flatpak_oci_registry_load_image_config (registry, NULL,
                                                             FLATPAK_OCI_MANIFEST (versioned)->config.digest,
                                                             (const char **)FLATPAK_OCI_MANIFEST (versioned)->config.urls,
                                                             NULL, NULL, &error);
      if (image_config == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't open child image config");
          return TRUE;
        }

      state = flatpak_dir_get_remote_state (system, arg_origin, FALSE, NULL, &error);
      if (state == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "%s: Can't get remote state: %s", arg_origin, error->message);
          return TRUE;
        }

      /* We need to use list_all_remote_refs because we don't care about
       * enumerate vs. noenumerate.
       */
      if (!flatpak_dir_list_all_remote_refs (system, state, &remote_refs, NULL, &error))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "%s: Can't list refs: %s", arg_origin, error->message);
          return TRUE;
        }

      verified_digest = g_hash_table_lookup (remote_refs, arg_ref);
      if (!verified_digest)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "%s: ref %s not found", arg_origin, arg_ref);
          return TRUE;
        }

      if (!g_str_has_prefix (desc->parent.digest, "sha256:") ||
          strcmp (desc->parent.digest + strlen ("sha256:"), verified_digest) != 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "%s: manifest hash in downloaded content does not match ref %s", arg_origin, arg_ref);
          return TRUE;
        }

      checksum = flatpak_pull_from_oci (flatpak_dir_get_repo (system), registry, NULL, desc->parent.digest, NULL, FLATPAK_OCI_MANIFEST (versioned), image_config,
                                        arg_origin, arg_ref, FLATPAK_PULL_FLAGS_NONE, NULL, NULL, NULL, &error);
      if (checksum == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't pull ref %s from child OCI registry index: %s", arg_ref, error->message);
          return TRUE;
        }
    }
  else if (strlen (arg_repo_path) > 0)
    {
      if (!flatpak_dir_pull_untrusted_local (system, arg_repo_path,
                                             arg_origin,
                                             arg_ref,
                                             (const char **) arg_subpaths,
                                             NULL, NULL, &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error pulling from repo");
          return TRUE;
        }
    }
  else if (local_pull)
    {
      g_autoptr(FlatpakRemoteState) state = NULL;
      if (!ostree_repo_remote_get_url (flatpak_dir_get_repo (system),
                                       arg_origin,
                                       &url,
                                       &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error getting remote url");
          return TRUE;
        }

      if (!g_str_has_prefix (url, "file:"))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Local pull url doesn't start with file://");
          return TRUE;
        }

      state = flatpak_dir_get_remote_state_optional (system, arg_origin, FALSE, NULL, &error);
      if (state == NULL)
        {
          flatpak_invocation_return_error (invocation, error, "Error getting remote state");
          return TRUE;
        }

      if (!flatpak_dir_pull (system, state, arg_ref, NULL, (const char **) arg_subpaths, NULL, NULL, NULL, NULL,
                             FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_UNTRUSTED, NULL,
                             NULL, &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error pulling from repo");
          return TRUE;
        }
    }

  if (!no_deploy)
    {
      if (deploy_dir && !reinstall)
        {
          if (!flatpak_dir_deploy_update (system, arg_ref, NULL,
                                          (const char **) arg_subpaths,
                                          (const char **) arg_previous_ids,
                                          NULL, &error))
            {
              flatpak_invocation_return_error (invocation, error, "Error deploying");
              return TRUE;
            }
        }
      else
        {
          if (!flatpak_dir_deploy_install (system, arg_ref, arg_origin,
                                           (const char **) arg_subpaths,
                                           (const char **) arg_previous_ids,
                                           reinstall, NULL, &error))
            {
              flatpak_invocation_return_error (invocation, error, "Error deploying");
              return TRUE;
            }
        }
    }

  flatpak_system_helper_complete_deploy (object, invocation);

  return TRUE;
}

static gboolean
handle_cancel_pull (FlatpakSystemHelper   *object,
                    GDBusMethodInvocation *invocation,
                    guint                  arg_flags,
                    const gchar           *arg_installation,
                    const gchar           *arg_src_dir)
{
  OngoingPull *ongoing_pull;
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  uid_t uid;

  g_debug ("CancelPull %s %u %s", arg_installation, arg_flags, arg_src_dir);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  ongoing_pull = take_ongoing_pull_by_dir (arg_src_dir);
  if (ongoing_pull == NULL)
    {
      g_set_error (&error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "Cannot find ongoing pull to cancel at %s", arg_src_dir);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  /* Ensure that pull's uid is same as the caller's uid */
  if (!get_connection_uid (invocation, &uid, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  else
    {
      if (ongoing_pull->uid != uid)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Ongoing pull's uid(%d) does not match with peer uid(%d)",
                                                 ongoing_pull->uid, uid);
          return TRUE;
        }
    }

  ongoing_pull->preserve_pull = (arg_flags & FLATPAK_HELPER_CANCEL_PULL_FLAGS_PRESERVE_PULL) != 0;
  ongoing_pull_free (ongoing_pull);

  flatpak_system_helper_complete_cancel_pull (object, invocation);
  return TRUE;
}

static gboolean
handle_deploy_appstream (FlatpakSystemHelper   *object,
                         GDBusMethodInvocation *invocation,
                         const gchar           *arg_repo_path,
                         guint                  arg_flags,
                         const gchar           *arg_origin,
                         const gchar           *arg_arch,
                         const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *new_branch = NULL;
  g_autofree char *old_branch = NULL;
  gboolean is_oci;

  g_debug ("DeployAppstream %s %u %s %s %s", arg_repo_path, arg_flags, arg_origin, arg_arch, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (strlen (arg_repo_path) > 0)
    {
      g_autoptr(GFile) repo_file = g_file_new_for_path (arg_repo_path);
      if (!g_file_query_exists (repo_file, NULL))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "Path does not exist");
          return TRUE;
        }
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Can't open system repo %s", error->message);
      return TRUE;
    }

  is_oci = flatpak_dir_get_remote_oci (system, arg_origin);

  new_branch = g_strdup_printf ("appstream2/%s", arg_arch);
  old_branch = g_strdup_printf ("appstream/%s", arg_arch);

  if (is_oci)
    {
      g_auto(FlatpakMainContext) context = FLATKPAK_MAIN_CONTEXT_INIT;

      /* This does soup http requests spinning the current mainloop, so we need one
         for this thread. */
      flatpak_progress_init_main_context (NULL, &context);
      /* In the OCI case, we just do the full update, including network i/o, in the
       * system helper, see comment in flatpak_dir_update_appstream()
       */
      if (!flatpak_dir_update_appstream (system,
                                         arg_origin,
                                         arg_arch,
                                         NULL,
                                         NULL,
                                         NULL,
                                         &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error updating appstream");
          return TRUE;
        }

      flatpak_system_helper_complete_deploy_appstream (object, invocation);
      return TRUE;
    }
  else if (strlen (arg_repo_path) > 0)
    {
      g_autoptr(GError) first_error = NULL;
      g_autoptr(GError) second_error = NULL;

      if (!flatpak_dir_pull_untrusted_local (system, arg_repo_path,
                                             arg_origin,
                                             new_branch,
                                             NULL,
                                             NULL,
                                             NULL, &first_error))
        {
          if (!flatpak_dir_pull_untrusted_local (system, arg_repo_path,
                                                 arg_origin,
                                                 old_branch,
                                                 NULL,
                                                 NULL,
                                                 NULL, &second_error))
            {
              g_prefix_error (&first_error, "Error updating appstream2: ");
              g_prefix_error (&second_error, "%s; Error updating appstream: ", first_error->message);
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                     "Error pulling from repo: %s", second_error->message);
              return TRUE;
            }
        }
    }
  else /* empty path == local pull */
    {
      g_autoptr(FlatpakRemoteState) state = NULL;
      g_autoptr(GError) first_error = NULL;
      g_autoptr(GError) second_error = NULL;
      g_autofree char *url = NULL;

      if (!ostree_repo_remote_get_url (flatpak_dir_get_repo (system),
                                       arg_origin,
                                       &url,
                                       &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error getting remote url");
          return TRUE;
        }

      if (!g_str_has_prefix (url, "file:"))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Local pull url doesn't start with file://");
          return TRUE;
        }

      state = flatpak_dir_get_remote_state_optional (system, arg_origin, FALSE, NULL, &error);
      if (state == NULL)
        {
          flatpak_invocation_return_error (invocation, error, "Error getting remote state");
          return TRUE;
        }

      if (!flatpak_dir_pull (system, state, new_branch, NULL, NULL, NULL, NULL, NULL, NULL,
                             FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_UNTRUSTED, NULL,
                             NULL, &first_error))
        {
          if (!flatpak_dir_pull (system, state, old_branch, NULL, NULL, NULL, NULL, NULL, NULL,
                                 FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_UNTRUSTED, NULL,
                                 NULL, &second_error))
            {
              g_prefix_error (&first_error, "Error updating appstream2: ");
              g_prefix_error (&second_error, "%s; Error updating appstream: ", first_error->message);
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                     "Error pulling from repo: %s", second_error->message);
              return TRUE;
            }
        }
    }

  if (!flatpak_dir_deploy_appstream (system,
                                     arg_origin,
                                     arg_arch,
                                     NULL,
                                     NULL,
                                     &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error deploying appstream");
      return TRUE;
    }

  flatpak_system_helper_complete_deploy_appstream (object, invocation);

  return TRUE;
}

static gboolean
handle_uninstall (FlatpakSystemHelper   *object,
                  GDBusMethodInvocation *invocation,
                  guint                  arg_flags,
                  const gchar           *arg_ref,
                  const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("Uninstall %u %s %s", arg_flags, arg_ref, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_UNINSTALL_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_UNINSTALL_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_uninstall (system, arg_ref, arg_flags, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error uninstalling");
      return TRUE;
    }

  flatpak_system_helper_complete_uninstall (object, invocation);

  return TRUE;
}

static gboolean
handle_install_bundle (FlatpakSystemHelper   *object,
                       GDBusMethodInvocation *invocation,
                       const gchar           *arg_bundle_path,
                       guint32                arg_flags,
                       const gchar           *arg_remote,
                       const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GFile) bundle_file = g_file_new_for_path (arg_bundle_path);
  g_autoptr(GError) error = NULL;
  g_autofree char *ref = NULL;

  g_debug ("InstallBundle %s %u %s %s", arg_bundle_path, arg_flags, arg_remote, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_ALL));
      return TRUE;
    }

  if (!g_file_query_exists (bundle_file, NULL))
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                             "Bundle %s does not exist", arg_bundle_path);
      return TRUE;
    }

  if (!flatpak_dir_install_bundle (system, bundle_file, arg_remote, &ref, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error installing bundle");
      return TRUE;
    }

  flatpak_system_helper_complete_install_bundle (object, invocation, ref);

  return TRUE;
}


static gboolean
handle_configure_remote (FlatpakSystemHelper   *object,
                         GDBusMethodInvocation *invocation,
                         guint                  arg_flags,
                         const gchar           *arg_remote,
                         const gchar           *arg_config,
                         GVariant              *arg_gpg_key,
                         const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) config = g_key_file_new ();
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", arg_remote);
  g_autoptr(GBytes) gpg_data = NULL;
  gboolean force_remove;

  g_debug ("ConfigureRemote %u %s %s", arg_flags, arg_remote, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (*arg_remote == 0 || strchr (arg_remote, '/') != NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid remote name: %s", arg_remote);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_ALL));
      return TRUE;
    }

  if (!g_key_file_load_from_data (config, arg_config, strlen (arg_config),
                                  G_KEY_FILE_NONE, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid config: %s\n", error->message);
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (g_variant_get_size (arg_gpg_key) > 0)
    gpg_data = g_variant_get_data_as_bytes (arg_gpg_key);

  force_remove = (arg_flags & FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE) != 0;

  if (g_key_file_has_group (config, group))
    {
      if (!flatpak_dir_modify_remote (system, arg_remote, config, gpg_data, NULL, &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error modifying remote");
          return TRUE;
        }
    }
  else
    {
      if (!flatpak_dir_remove_remote (system, force_remove, arg_remote, NULL, &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error removing remote");
          return TRUE;
        }
    }

  flatpak_system_helper_complete_configure_remote (object, invocation);

  return TRUE;
}

static gboolean
handle_configure (FlatpakSystemHelper   *object,
                  GDBusMethodInvocation *invocation,
                  guint                  arg_flags,
                  const gchar           *arg_key,
                  const gchar           *arg_value,
                  const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("Configure %u %s=%s %s", arg_flags, arg_key, arg_value, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_CONFIGURE_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_CONFIGURE_FLAGS_ALL));
      return TRUE;
    }

  if ((strcmp (arg_key, "languages") != 0) &&
      (strcmp (arg_key, "extra-languages") != 0) &&
      (strcmp (arg_key, "masked") != 0) &&
      (strcmp (arg_key, "pinned") != 0))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported key: %s", arg_key);
      return TRUE;
    }

  if ((arg_flags & FLATPAK_HELPER_CONFIGURE_FLAGS_UNSET) != 0)
    arg_value = NULL;

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_set_config (system, arg_key, arg_value, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error setting config");
      return TRUE;
    }

  flatpak_system_helper_complete_configure (object, invocation);

  return TRUE;
}

static gboolean
handle_update_remote (FlatpakSystemHelper   *object,
                      GDBusMethodInvocation *invocation,
                      guint                  arg_flags,
                      const gchar           *arg_remote,
                      const gchar           *arg_installation,
                      const gchar           *arg_summary_path,
                      const gchar           *arg_summary_sig_path)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  char *summary_data = NULL;
  gsize summary_size;
  g_autoptr(GBytes) summary_bytes = NULL;
  char *summary_sig_data = NULL;
  gsize summary_sig_size;
  g_autoptr(GBytes) summary_sig_bytes = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;

  g_debug ("UpdateRemote %u %s %s %s %s", arg_flags, arg_remote, arg_installation, arg_summary_path, arg_summary_sig_path);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (*arg_remote == 0 || strchr (arg_remote, '/') != NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid remote name: %s", arg_remote);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_ALL));
      return TRUE;
    }

  if (!g_file_get_contents (arg_summary_path, &summary_data, &summary_size, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  summary_bytes = g_bytes_new_take (summary_data, summary_size);

  if (*arg_summary_sig_path != 0)
    {
      if (!g_file_get_contents (arg_summary_sig_path, &summary_sig_data, &summary_sig_size, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }
      summary_sig_bytes = g_bytes_new_take (summary_sig_data, summary_sig_size);
    }

  state = flatpak_dir_get_remote_state_for_summary (system, arg_remote, summary_bytes, summary_sig_bytes, NULL, &error);
  if (state == NULL)
    {
      flatpak_invocation_return_error (invocation, error, "Error getting remote state");
      return TRUE;
    }

  if (summary_sig_bytes == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "UpdateRemote requires a summary signature");
      return TRUE;
    }

  if (!flatpak_dir_update_remote_configuration_for_state (system, state, FALSE, NULL, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error updating remote config");
      return TRUE;
    }

  flatpak_system_helper_complete_update_remote (object, invocation);

  return TRUE;
}

static gboolean
handle_remove_local_ref (FlatpakSystemHelper   *object,
                         GDBusMethodInvocation *invocation,
                         guint                  arg_flags,
                         const gchar           *arg_remote,
                         const gchar           *arg_ref,
                         const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("RemoveLocalRef %u %s %s %s", arg_flags, arg_remote, arg_ref, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_ALL));
      return TRUE;
    }

  if (*arg_remote == 0 || strchr (arg_remote, '/') != NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid remote name: %s", arg_remote);
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_remove_ref (system, arg_remote, arg_ref, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error removing ref");
      return TRUE;
    }

  flatpak_system_helper_complete_remove_local_ref (object, invocation);

  return TRUE;
}

static gboolean
handle_prune_local_repo (FlatpakSystemHelper   *object,
                         GDBusMethodInvocation *invocation,
                         guint                  arg_flags,
                         const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("PruneLocalRepo %u %s", arg_flags, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_prune (system, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error pruning repo");
      return TRUE;
    }

  flatpak_system_helper_complete_prune_local_repo (object, invocation);

  return TRUE;
}


static gboolean
handle_ensure_repo (FlatpakSystemHelper   *object,
                    GDBusMethodInvocation *invocation,
                    guint                  arg_flags,
                    const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GError) local_error = NULL;

  g_debug ("EnsureRepo %u %s", arg_flags, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_ENSURE_REPO_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_ENSURE_REPO_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_migrate_config (system, NULL, NULL, &local_error))
    g_warning ("Failed to migrate configuration for installation %s: %s", arg_installation, local_error->message);

  flatpak_system_helper_complete_ensure_repo (object, invocation);

  return TRUE;
}

static gboolean
handle_run_triggers (FlatpakSystemHelper   *object,
                     GDBusMethodInvocation *invocation,
                     guint                  arg_flags,
                     const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("RunTriggers %u %s", arg_flags, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_run_triggers (system, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error running triggers");
      return TRUE;
    }

  flatpak_system_helper_complete_run_triggers (object, invocation);

  return TRUE;
}

static gboolean
check_for_system_helper_user (struct passwd  *passwd,
                              gchar         **passwd_buf,
                              GError        **error)
{
  struct passwd *result = NULL;
  g_autofree gchar *buf = NULL;
  size_t bufsize;
  int err;

  bufsize = sysconf (_SC_GETPW_R_SIZE_MAX);
  if (bufsize == -1)          /* Value was indeterminate */
     bufsize = 16384;        /* Should be more than enough */

  while (!result)
    {
      buf = g_malloc0 (bufsize);
      err = getpwnam_r (SYSTEM_HELPER_USER, passwd, buf, bufsize, &result);
      if (result == NULL)
        {
          if (err == ERANGE)     /* Insufficient buffer space */
            {
              g_free (buf);
              bufsize *= 2;
              continue;
            }
          else if (err == 0)     /* User SYSTEM_HELPER_USER 's record was not found*/
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "User %s does not exist in password file entry", SYSTEM_HELPER_USER);
              return FALSE;
            }
          else
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                           "Failed to query user %s from password file entry", SYSTEM_HELPER_USER);
              return FALSE;
            }
        }
    }

  *passwd_buf = g_steal_pointer (&buf);

  return TRUE;
}

static void
revokefs_fuse_backend_child_setup (gpointer user_data)
{
  struct passwd *passwd = user_data;

  /* We use 5 instead of 3 here, because fd 3 is the inherited SOCK_SEQPACKET
   * socket and fd 4 is the --close-with-fd pipe; both were dup2()'d into place
   * before this by GSubprocess */
  flatpak_close_fds_workaround (5);

  if (setgid (passwd->pw_gid) == -1)
    {
      g_warning ("Failed to setgid(%d) for revokefs backend: %s",
                 passwd->pw_gid, g_strerror (errno));
      exit (1);
    }

  if (setuid (passwd->pw_uid) == -1)
    {
      g_warning ("Failed to setuid(%d) for revokefs backend: %s",
                 passwd->pw_uid, g_strerror (errno));
      exit (1);
    }
}

static void
name_vanished_cb (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  const gchar *unique_name = (const gchar *) user_data;
  g_autoptr(GPtrArray) cleanup_pulls = NULL;
  GHashTableIter iter;
  gpointer value;

  cleanup_pulls = g_ptr_array_new_with_free_func ((GDestroyNotify) ongoing_pull_free);

  G_LOCK (cache_dirs_in_use);
  g_hash_table_iter_init (&iter, cache_dirs_in_use);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      OngoingPull *pull = (OngoingPull *) value;
      if (g_strcmp0 (pull->unique_name, unique_name) == 0)
        {
          g_ptr_array_add (cleanup_pulls, pull);
          g_hash_table_iter_remove (&iter);
        }
    }
  G_UNLOCK (cache_dirs_in_use);
}

static OngoingPull *
ongoing_pull_new (FlatpakSystemHelper   *object,
                  GDBusMethodInvocation *invocation,
                  struct passwd         *passwd,
                  uid_t                  uid,
                  const gchar           *src,
                  GError               **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  g_autoptr(OngoingPull) pull = NULL;
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  int sockets[2], exit_sockets[2];
  const char *revokefs_fuse_bin = LIBEXECDIR "/revokefs-fuse";

  pull = g_slice_new0 (OngoingPull);
  pull->object = object;
  pull->invocation = invocation;
  pull->src_dir = g_strdup (src);
  pull->cancellable = g_cancellable_new ();
  pull->uid = uid;
  pull->preserve_pull = FALSE;
  pull->unique_name = g_strdup (g_dbus_connection_get_unique_name (connection));

  pull->watch_id = g_bus_watch_name_on_connection (connection,
                                                   pull->unique_name,
                                                   G_BUS_NAME_WATCHER_FLAGS_NONE, NULL,
                                                   name_vanished_cb,
                                                   g_strdup (g_dbus_connection_get_unique_name (connection)),
                                                   g_free);

  if (socketpair (AF_UNIX, SOCK_SEQPACKET, 0, sockets) == -1)
    {
      glnx_throw_errno_prefix (error, "Failed to get a socketpair");
      return NULL;
    }

  if (pipe2 (exit_sockets, O_CLOEXEC) == -1)
    {
      glnx_throw_errno_prefix (error, "Failed to create a pipe");
      close (sockets[0]);
      close (sockets[1]);
      return NULL;
    }

  /* We use INHERIT_FDS to work around dead-lock, see flatpak_close_fds_workaround */
  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_INHERIT_FDS);
  g_subprocess_launcher_set_child_setup (launcher, revokefs_fuse_backend_child_setup, passwd, NULL);
  g_subprocess_launcher_take_fd (launcher, sockets[0], 3);
  fcntl (sockets[1], F_SETFD, FD_CLOEXEC);
  pull->client_socket = sockets[1];

  g_subprocess_launcher_take_fd (launcher, exit_sockets[0], 4);
  pull->backend_exit_socket = exit_sockets[1];

  if (g_getenv ("FLATPAK_REVOKEFS_FUSE"))
    revokefs_fuse_bin = g_getenv ("FLATPAK_REVOKEFS_FUSE");

  pull->revokefs_backend = g_subprocess_launcher_spawn (launcher,
                                                        error,
                                                        revokefs_fuse_bin,
                                                        "--backend",
                                                        "--socket=3",
                                                        "--exit-with-fd=4",
                                                        src, NULL);
  if (pull->revokefs_backend == NULL)
    return NULL;

  return g_steal_pointer (&pull);
}

static gboolean
reuse_cache_dir_if_available (const gchar    *repo_tmp,
                              gchar         **out_src_dir,
                              struct passwd  *passwd)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;
  g_autoptr(GFile) repo_tmpfile = NULL;
  GFileInfo *file_info = NULL;
  g_autoptr(GError) error = NULL;
  const gchar *name;
  gboolean res = FALSE;

  g_debug ("Checking for any temporary cache directory available to reuse");

  repo_tmpfile = g_file_new_for_path (repo_tmp);
  enumerator = g_file_enumerate_children (repo_tmpfile,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                          G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                          G_FILE_QUERY_INFO_NONE, NULL, &error);
  if (enumerator == NULL)
    {
      g_warning ("Failed to enumerate %s: %s", repo_tmp, error->message);
      return FALSE;
    }

  while (TRUE)
    {
      if (!g_file_enumerator_iterate (enumerator, &file_info, NULL, NULL, &error))
        {
          g_warning ("Error while iterating %s: %s", repo_tmp, error->message);
          break;
        }

      if (file_info == NULL || res == TRUE)
        break;

      name = g_file_info_get_name (file_info);
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY &&
          g_str_has_prefix (name, "flatpak-cache-"))
        {
          g_autoptr(GFile) cache_dir_file = g_file_get_child (repo_tmpfile, name);
          g_autofree gchar *cache_dir_name = g_file_get_path (cache_dir_file);

          G_LOCK (cache_dirs_in_use);
          if (!g_hash_table_contains (cache_dirs_in_use, cache_dir_name))
            {
              struct stat st_buf;

              /* We are able to find a cache dir which is not in use. */
              if (stat (cache_dir_name, &st_buf) == 0 &&
                  st_buf.st_uid == passwd->pw_uid &&        /* should be owned by SYSTEM_HELPER_USER */
                  (st_buf.st_mode & 0022) == 0)             /* should not be world-writeable */
                {
                  gboolean did_not_exist = g_hash_table_insert (cache_dirs_in_use,
                                                                g_strdup (cache_dir_name),
                                                                NULL);
                  g_assert (did_not_exist);
                  *out_src_dir = g_steal_pointer (&cache_dir_name);
                  res = TRUE;
                }
            }
          G_UNLOCK (cache_dirs_in_use);
        }
    }

  return res;
}

static gboolean
handle_get_revokefs_fd (FlatpakSystemHelper   *object,
                        GDBusMethodInvocation *invocation,
                        GUnixFDList           *arg_fdlist,
                        guint                  arg_flags,
                        const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *src_dir = NULL;
  g_autofree gchar *flatpak_dir = NULL;
  g_autofree gchar *repo_tmp = NULL;
  g_autofree gchar *passwd_buf = NULL;
  struct passwd passwd = { NULL };
  OngoingPull *new_pull;
  uid_t uid;
  int fd_index = -1;

  g_debug ("GetRevokefsFd %u %s", arg_flags, arg_installation);

  if (disable_revokefs)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "RevokeFS disabled");
      return TRUE;
    }

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_GET_REVOKEFS_FD_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_GET_REVOKEFS_FD_FLAGS_ALL));
      return TRUE;
    }

  if (on_session_bus)
    {
      passwd.pw_uid = getuid();
      passwd.pw_gid = getgid();
    }
  else if (!check_for_system_helper_user (&passwd, &passwd_buf, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!get_connection_uid (invocation, &uid, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  flatpak_dir = g_file_get_path (flatpak_dir_get_path (system));
  repo_tmp = g_build_filename (flatpak_dir, "repo", "tmp", NULL);

   if (reuse_cache_dir_if_available (repo_tmp, &src_dir, &passwd))
     g_debug ("Cache dir %s can be reused", src_dir);
  else
    {
      /* Create a new cache dir and add it to cache_dirs_in_use. Do all this under
       * a lock, so that a different pull does not snatch this directory up using
       * reuse_cache_dir_if_available. */
      G_LOCK (cache_dirs_in_use);
      src_dir = g_mkdtemp_full (g_build_filename (repo_tmp, "flatpak-cache-XXXXXX", NULL), 0755);
      if (src_dir == NULL)
        {
          G_UNLOCK (cache_dirs_in_use);
          glnx_throw_errno_prefix (&error, "Failed to create new cache-dir at %s", repo_tmp);
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }
      g_hash_table_insert (cache_dirs_in_use, g_strdup (src_dir), NULL);
      G_UNLOCK (cache_dirs_in_use);

      if (chown (src_dir, passwd.pw_uid, passwd.pw_gid) == -1)
        {
          remove_dir_from_cache_dirs_in_use (src_dir);
          glnx_throw_errno_prefix (&error, "Failed to chown %s to user %s",
                                   src_dir, passwd.pw_name);
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }
    }

  new_pull = ongoing_pull_new (object, invocation, &passwd, uid, src_dir, &error);
  if (error != NULL)
    {
      remove_dir_from_cache_dirs_in_use (src_dir);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  G_LOCK (cache_dirs_in_use);
  g_hash_table_insert (cache_dirs_in_use, g_strdup (src_dir), new_pull);
  G_UNLOCK (cache_dirs_in_use);

  fd_list = g_unix_fd_list_new ();
  fd_index = g_unix_fd_list_append (fd_list, new_pull->client_socket, NULL);

  flatpak_system_helper_complete_get_revokefs_fd (object, invocation,
                                                  fd_list, g_variant_new_handle (fd_index),
                                                  new_pull->src_dir);

  return TRUE;
}

static gboolean
handle_update_summary (FlatpakSystemHelper   *object,
                       GDBusMethodInvocation *invocation,
                       guint                  arg_flags,
                       const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  gboolean delete_summary;

  g_debug ("UpdateSummary %u %s", arg_flags, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  delete_summary = (arg_flags & FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_DELETE) != 0;
  if (!flatpak_dir_update_summary (system, delete_summary, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error %s summary",
                                       delete_summary ? "deleting" : "updating");
      return TRUE;
    }

  flatpak_system_helper_complete_update_summary (object, invocation);

  return TRUE;
}

static gboolean
handle_generate_oci_summary (FlatpakSystemHelper   *object,
                             GDBusMethodInvocation *invocation,
                             guint                  arg_flags,
                             const gchar           *arg_origin,
                             const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  gboolean only_cached;
  gboolean is_oci;

  g_debug ("GenerateOciSummary %u %s %s", arg_flags, arg_origin, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ALL));
      return TRUE;
    }

  only_cached = (arg_flags & FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ONLY_CACHED) != 0;

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Can't open system repo %s", error->message);
      return TRUE;
    }

  is_oci = flatpak_dir_get_remote_oci (system, arg_origin);
  if (!is_oci)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "%s is not a OCI remote", arg_origin);
      return TRUE;
    }

  if (!flatpak_dir_remote_make_oci_summary (system, arg_origin, only_cached, NULL, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Failed to update OCI summary");
      return TRUE;
    }


  flatpak_system_helper_complete_generate_oci_summary (object, invocation);

  return TRUE;
}

static gboolean
dir_ref_is_installed (FlatpakDir *dir,
                      const char *ref)
{
  g_autoptr(GBytes) deploy_data = NULL;

  deploy_data = flatpak_dir_get_deploy_data (dir, ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);

  return deploy_data != NULL;
}

static gboolean
flatpak_authorize_method_handler (GDBusInterfaceSkeleton *interface,
                                  GDBusMethodInvocation  *invocation,
                                  gpointer                user_data)
{
  const gchar *method_name = g_dbus_method_invocation_get_method_name (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  GVariant *parameters = g_dbus_method_invocation_get_parameters (invocation);
  g_autoptr(AutoPolkitSubject) subject = polkit_system_bus_name_new (sender);
  g_autoptr(AutoPolkitDetails) details = polkit_details_new ();
  const gchar *action = NULL;
  gboolean authorized = FALSE;
  gboolean no_interaction = FALSE;

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  if (on_session_bus)
    {
      /* This is test code, make sure it never runs with privileges */
      g_assert (geteuid () != 0);
      g_assert (getuid () != 0);
      g_assert (getegid () != 0);
      g_assert (getgid () != 0);
      authorized = TRUE;
    }
  else if (g_strcmp0 (method_name, "Deploy") == 0)
    {
      const char *installation;
      const char *ref, *origin;
      guint32 flags;

      g_variant_get_child (parameters, 1, "u", &flags);
      g_variant_get_child (parameters, 2, "&s", &ref);
      g_variant_get_child (parameters, 3, "&s", &origin);
      g_variant_get_child (parameters, 6, "&s", &installation);

      /* For metadata updates, redirect to the metadata-update action which
       * should basically always be allowed */
      if (ref != NULL && g_strcmp0 (ref, OSTREE_REPO_METADATA_REF) == 0)
        {
          action = "org.freedesktop.Flatpak.metadata-update";
        }
      else
        {
          gboolean is_app, is_install;

          /* These flags allow clients to "upgrade" the permission,
           * avoiding the need for multiple polkit dialogs when we first
           * update a runtime, then install the app that needs it.
           *
           * Note that our policy has implications:
           * app-install > app-update > runtime-install > runtime-update
           * which means that these hints only ever select a stronger
           * permission, and are safe in that sense.
           */

          if ((flags & FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT) != 0)
            is_app = TRUE;
          else
            is_app = g_str_has_prefix (ref, "app/");

          if ((flags & FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT) != 0 ||
              (flags & FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL) != 0)
            is_install = TRUE;
          else
            {
              g_autoptr(FlatpakDir) system = dir_get_system (installation, 0, NULL);

              is_install = !dir_ref_is_installed (system, ref);
            }

          if (is_install)
            {
              if (is_app)
                action = "org.freedesktop.Flatpak.app-install";
              else
                action = "org.freedesktop.Flatpak.runtime-install";
            }
          else
            {
              if (is_app)
                action = "org.freedesktop.Flatpak.app-update";
              else
                action = "org.freedesktop.Flatpak.runtime-update";
            }

          no_interaction = (flags & FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION) != 0;
        }

      polkit_details_insert (details, "origin", origin);
      polkit_details_insert (details, "ref", ref);
    }
  else if (g_strcmp0 (method_name, "DeployAppstream") == 0)
    {
      guint32 flags;
      const char *arch, *origin;

      g_variant_get_child (parameters, 1, "u", &flags);
      g_variant_get_child (parameters, 2, "&s", &origin);
      g_variant_get_child (parameters, 3, "&s", &arch);

      action = "org.freedesktop.Flatpak.appstream-update";
      no_interaction = (flags & FLATPAK_HELPER_DEPLOY_APPSTREAM_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "origin", origin);
      polkit_details_insert (details, "arch", arch);
    }
  else if (g_strcmp0 (method_name, "InstallBundle") == 0)
    {
      const char *path;
      guint32 flags;

      g_variant_get_child (parameters, 0, "^&ay", &path);
      g_variant_get_child (parameters, 1, "u", &flags);

      action = "org.freedesktop.Flatpak.install-bundle";
      no_interaction = (flags & FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "path", path);
    }
  else if (g_strcmp0 (method_name, "Uninstall") == 0)
    {
      const char *ref;
      gboolean is_app;
      guint32 flags;

      g_variant_get_child (parameters, 0, "u", &flags);
      g_variant_get_child (parameters, 1, "&s", &ref);

      is_app = g_str_has_prefix (ref, "app/");
      if (is_app)
        action = "org.freedesktop.Flatpak.app-uninstall";
      else
        action = "org.freedesktop.Flatpak.runtime-uninstall";
      no_interaction = (flags & FLATPAK_HELPER_UNINSTALL_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "ref", ref);
    }
  else if (g_strcmp0 (method_name, "ConfigureRemote") == 0)
    {
      const char *remote;
      guint32 flags;

      g_variant_get_child (parameters, 0, "u", &flags);
      g_variant_get_child (parameters, 1, "&s", &remote);

      action = "org.freedesktop.Flatpak.configure-remote";
      no_interaction = (flags & FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "remote", remote);
    }
  else if (g_strcmp0 (method_name, "Configure") == 0)
    {
      const char *key;
      guint32 flags;

      g_variant_get_child (parameters, 0, "u", &flags);
      g_variant_get_child (parameters, 1, "&s", &key);

      action = "org.freedesktop.Flatpak.configure";
      no_interaction = (flags & FLATPAK_HELPER_CONFIGURE_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "key", key);
    }
  else if (g_strcmp0 (method_name, "UpdateRemote") == 0)
    {
      const char *remote;
      guint32 flags;

      g_variant_get_child (parameters, 0, "u", &flags);
      g_variant_get_child (parameters, 1, "&s", &remote);

      action = "org.freedesktop.Flatpak.update-remote";
      no_interaction = (flags & FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "remote", remote);
    }
  else if (g_strcmp0 (method_name, "RemoveLocalRef") == 0 ||
           g_strcmp0 (method_name, "PruneLocalRepo") == 0 ||
           g_strcmp0 (method_name, "EnsureRepo") == 0 ||
           g_strcmp0 (method_name, "RunTriggers") == 0 ||
           g_strcmp0 (method_name, "GetRevokefsFd") == 0 ||
           g_strcmp0 (method_name, "CancelPull") == 0)
    {
      guint32 flags;

      action = "org.freedesktop.Flatpak.modify-repo";

      /* all of these methods have flags as first argument, and 1 << 0 as 'no-interaction' */
      g_variant_get_child (parameters, 0, "u", &flags);
      no_interaction = (flags & (1 << 0)) != 0;
    }
  else if (g_strcmp0 (method_name, "UpdateSummary") == 0 ||
           g_strcmp0 (method_name, "GenerateOciSummary") == 0)
    {
      guint32 flags;
      action = "org.freedesktop.Flatpak.metadata-update";

      /* all of these methods have flags as first argument, and 1 << 0 as 'no-interaction' */
      g_variant_get_child (parameters, 0, "u", &flags);
      no_interaction = (flags & (1 << 0)) != 0;
    }

  if (action)
    {
      g_autoptr(AutoPolkitAuthorizationResult) result = NULL;
      g_autoptr(GError) error = NULL;
      PolkitCheckAuthorizationFlags auth_flags;

      if (no_interaction)
        auth_flags = POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE;
      else
        auth_flags = POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION;

      result = polkit_authority_check_authorization_sync (authority, subject,
                                                          action, details,
                                                          auth_flags,
                                                          NULL, &error);
      if (result == NULL)
        {
          g_dbus_error_strip_remote_error (error);
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Authorization error: %s", error->message);
          return FALSE;
        }

      authorized = polkit_authorization_result_get_is_authorized (result);
    }

  if (!authorized)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Flatpak system operation %s not allowed for user", method_name);
    }

  return authorized;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;

  g_debug ("Bus acquired, creating skeleton");

  g_dbus_connection_set_exit_on_close (connection, FALSE);

  helper = flatpak_system_helper_skeleton_new ();

  flatpak_system_helper_set_version (FLATPAK_SYSTEM_HELPER (helper), 2);

  g_object_set_data_full (G_OBJECT (helper), "track-alive", GINT_TO_POINTER (42), skeleton_died_cb);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (helper),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  g_signal_connect (helper, "handle-deploy", G_CALLBACK (handle_deploy), NULL);
  g_signal_connect (helper, "handle-deploy-appstream", G_CALLBACK (handle_deploy_appstream), NULL);
  g_signal_connect (helper, "handle-uninstall", G_CALLBACK (handle_uninstall), NULL);
  g_signal_connect (helper, "handle-install-bundle", G_CALLBACK (handle_install_bundle), NULL);
  g_signal_connect (helper, "handle-configure-remote", G_CALLBACK (handle_configure_remote), NULL);
  g_signal_connect (helper, "handle-configure", G_CALLBACK (handle_configure), NULL);
  g_signal_connect (helper, "handle-update-remote", G_CALLBACK (handle_update_remote), NULL);
  g_signal_connect (helper, "handle-remove-local-ref", G_CALLBACK (handle_remove_local_ref), NULL);
  g_signal_connect (helper, "handle-prune-local-repo", G_CALLBACK (handle_prune_local_repo), NULL);
  g_signal_connect (helper, "handle-ensure-repo", G_CALLBACK (handle_ensure_repo), NULL);
  g_signal_connect (helper, "handle-run-triggers", G_CALLBACK (handle_run_triggers), NULL);
  g_signal_connect (helper, "handle-update-summary", G_CALLBACK (handle_update_summary), NULL);
  g_signal_connect (helper, "handle-generate-oci-summary", G_CALLBACK (handle_generate_oci_summary), NULL);
  g_signal_connect (helper, "handle-get-revokefs-fd", G_CALLBACK (handle_get_revokefs_fd), NULL);
  g_signal_connect (helper, "handle-cancel-pull", G_CALLBACK (handle_cancel_pull), NULL);

  g_signal_connect (helper, "g-authorize-method",
                    G_CALLBACK (flatpak_authorize_method_handler),
                    NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (helper),
                                         connection,
                                         "/org/freedesktop/Flatpak/SystemHelper",
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_error_free (error);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("Name acquired");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_debug ("Name lost");
  unref_skeleton_in_timeout ();
}

static void
binary_file_changed_cb (GFileMonitor     *file_monitor,
                        GFile            *file,
                        GFile            *other_file,
                        GFileMonitorEvent event_type,
                        gpointer          data)
{
  static gboolean got_it = FALSE;

  if (!got_it)
    {
      g_debug ("binary file changed");
      unref_skeleton_in_timeout ();
    }

  got_it = TRUE;
}

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("FH: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

static gboolean
opt_verbose_cb (const gchar *option_name,
                const gchar *value,
                gpointer     data,
                GError     **error)
{
  opt_verbose++;
  return TRUE;
}

int
main (int    argc,
      char **argv)
{
  gchar exe_path[PATH_MAX + 1];
  ssize_t exe_path_len;
  gboolean replace;
  gboolean show_version;
  GBusNameOwnerFlags flags;
  GOptionContext *context;
  g_autoptr(GError) error = NULL;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  "Replace old daemon.", NULL },
    { "verbose", 'v', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, &opt_verbose_cb, "Show debug information, -vv for more detail", NULL },
    { "ostree-verbose", 0, 0, G_OPTION_ARG_NONE, &opt_ostree_verbose,"Show OSTree debug information", NULL },
    { "session", 0, 0, G_OPTION_ARG_NONE, &on_session_bus,  "Run in session, not system scope (for tests).", NULL },
    { "no-idle-exit", 0, 0, G_OPTION_ARG_NONE, &no_idle_exit,  "Don't exit when idle.", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show program version.", NULL},
    { NULL }
  };

  /* The child repo shared between the client process and the
     system-helper really needs to support creating files that
     are readable by others, so override the umask to 022
     Ideally this should be set when needed, but umask is thread-unsafe
     so there is really no local way to fix this.
  */
  umask(022);

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  if (g_getenv ("FLATPAK_DISABLE_REVOKEFS"))
    disable_revokefs = TRUE;

  g_set_prgname (argv[0]);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  context = g_option_context_new ("");

  replace = FALSE;
  show_version = FALSE;

  g_option_context_set_summary (context, "Flatpak system helper");
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name (), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      g_option_context_free (context);
      return 1;
    }

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  flatpak_disable_fancy_output ();

  if (opt_verbose > 0)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);
  if (opt_verbose > 1)
    g_log_set_handler (G_LOG_DOMAIN "2", G_LOG_LEVEL_DEBUG, message_handler, NULL);

  if (opt_ostree_verbose)
    g_log_set_handler ("OSTree", G_LOG_LEVEL_DEBUG, message_handler, NULL);

  if (!on_session_bus)
    {
      authority = polkit_authority_get_sync (NULL, &error);
      if (authority == NULL)
        {
          g_printerr ("Can't get polkit authority: %s\n", error->message);
          return 1;
        }
    }

  exe_path_len = readlink ("/proc/self/exe", exe_path, sizeof (exe_path) - 1);
  if (exe_path_len > 0)
    {
      exe_path[exe_path_len] = 0;
      GFileMonitor *monitor;
      g_autoptr(GFile) exe = NULL;
      g_autoptr(GError) local_error = NULL;

      exe = g_file_new_for_path (exe_path);
      monitor =  g_file_monitor_file (exe,
                                      G_FILE_MONITOR_NONE,
                                      NULL,
                                      &local_error);
      if (monitor == NULL)
        g_warning ("Failed to set watch on %s: %s", exe_path, error->message);
      else
        g_signal_connect (monitor, "changed",
                          G_CALLBACK (binary_file_changed_cb), NULL);
    }

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  name_owner_id = g_bus_own_name (on_session_bus ? G_BUS_TYPE_SESSION  : G_BUS_TYPE_SYSTEM,
                                  "org.freedesktop.Flatpak.SystemHelper",
                                  flags,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  cache_dirs_in_use = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  G_LOCK (cache_dirs_in_use);
  g_clear_pointer (&cache_dirs_in_use, g_hash_table_destroy);
  G_UNLOCK (cache_dirs_in_use);

  return 0;
}
