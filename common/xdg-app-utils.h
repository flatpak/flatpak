/*
 * Copyright © 2014 Red Hat, Inc
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

#ifndef __XDG_APP_UTILS_H__
#define __XDG_APP_UTILS_H__

#include <string.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"
#include <gio/gio.h>
#include <libsoup/soup.h>
#include "xdg-app-dbus.h"
#include "xdg-app-dir.h"
#include <ostree.h>

gboolean xdg_app_fail (GError **error, const char *format, ...);

gint xdg_app_strcmp0_ptr (gconstpointer  a,
                          gconstpointer  b);

const char * xdg_app_path_match_prefix (const char *pattern,
                                        const char *path);

const char * xdg_app_get_arch (void);

GBytes * xdg_app_read_stream (GInputStream *in,
                              gboolean null_terminate,
                              GError **error);

gboolean xdg_app_variant_bsearch_str (GVariant   *array,
                                      const char *str,
                                      int        *out_pos);

gboolean xdg_app_has_name_prefix (const char *string,
                                  const char *name);
gboolean xdg_app_is_valid_name (const char *string);
gboolean xdg_app_is_valid_branch (const char *string);

char **xdg_app_decompose_ref (const char *ref,
                              GError **error);

char * xdg_app_compose_ref (gboolean app,
                            const char *name,
                            const char *branch,
                            const char *arch,
                            GError **error);

char * xdg_app_build_untyped_ref (const char *runtime,
                                  const char *branch,
                                  const char *arch);
char * xdg_app_build_runtime_ref (const char *runtime,
                                  const char *branch,
                                  const char *arch);
char * xdg_app_build_app_ref (const char *app,
                              const char *branch,
                              const char *arch);
GFile * xdg_app_find_deploy_dir_for_ref (const char *ref,
                                         GCancellable *cancellable,
                                         GError **error);
XdgAppDeploy * xdg_app_find_deploy_for_ref (const char *ref,
                                            GCancellable *cancellable,
                                            GError **error);
char ** xdg_app_list_deployed_refs (const char *type,
				    const char *name_prefix,
				    const char *branch,
				    const char *arch,
				    GCancellable *cancellable,
				    GError **error);

gboolean xdg_app_overlay_symlink_tree (GFile    *source,
                                       GFile    *destination,
                                       const char *symlink_prefix,
                                       GCancellable  *cancellable,
                                       GError       **error);
gboolean xdg_app_remove_dangling_symlinks (GFile    *dir,
                                           GCancellable  *cancellable,
                                           GError       **error);

gboolean xdg_app_supports_bundles (OstreeRepo *repo);

void  xdg_app_invocation_lookup_app_id        (GDBusMethodInvocation  *invocation,
                                               GCancellable           *cancellable,
                                               GAsyncReadyCallback     callback,
                                               gpointer                user_data);

char *xdg_app_invocation_lookup_app_id_finish (GDBusMethodInvocation  *invocation,
                                               GAsyncResult           *result,
                                               GError                **error);

void  xdg_app_connection_track_name_owners    (GDBusConnection        *connection);

#if !GLIB_CHECK_VERSION(2,40,0)
static inline gboolean
g_key_file_save_to_file (GKeyFile     *key_file,
			 const gchar  *filename,
			 GError      **error)
{
  gchar *contents;
  gboolean success;
  gsize length;

  contents = g_key_file_to_data (key_file, &length, NULL);
  success = g_file_set_contents (filename, contents, length, error);
  g_free (contents);

  return success;
}
#endif

/* Returns the first string in subset that is not in strv */
static inline const gchar *
g_strv_subset (const gchar * const *strv,
               const gchar * const *subset)
{
  int i;

  for (i = 0; subset[i]; i++)
    {
      const char *key;

      key = subset[i];
      if (!g_strv_contains (strv, key))
        return key;
    }

  return NULL;
}

static inline void xdg_app_auto_unlock_helper (GMutex **mutex)
{
  if (*mutex)
    g_mutex_unlock (*mutex);
}

static inline GMutex *xdg_app_auto_lock_helper (GMutex *mutex)
{
  if (mutex)
    g_mutex_lock (mutex);
  return mutex;
}

gint xdg_app_mkstempat (int dir_fd,
                        gchar *tmpl,
                        int flags,
                        int mode);


typedef struct XdgAppTablePrinter XdgAppTablePrinter;

XdgAppTablePrinter *xdg_app_table_printer_new               (void);
void                xdg_app_table_printer_free              (XdgAppTablePrinter *printer);
void                xdg_app_table_printer_add_column        (XdgAppTablePrinter *printer,
                                                             const char         *text);
void                xdg_app_table_printer_append_with_comma (XdgAppTablePrinter *printer,
                                                             const char         *text);
void                xdg_app_table_printer_finish_row        (XdgAppTablePrinter *printer);
void                xdg_app_table_printer_print             (XdgAppTablePrinter *printer);

gboolean xdg_app_repo_set_title (OstreeRepo    *repo,
                                 const char    *title,
                                 GError       **error);
gboolean xdg_app_repo_update    (OstreeRepo    *repo,
                                 const char   **gpg_key_ids,
                                 const char    *gpg_homedir,
                                 GCancellable  *cancellable,
                                 GError       **error);

GVariant * xdg_app_bundle_load (GFile *file,
                                char **commit,
                                char **ref,
                                char **origin,
                                guint64 *installed_size,
                                GBytes **gpg_keys,
                                GError **error);


typedef struct {
  char *id;
  char *installed_id;
  char *ref;
  char *directory;
} XdgAppExtension;

void xdg_app_extension_free (XdgAppExtension *extension);

GList *xdg_app_list_extensions (GKeyFile *metakey,
                                const char *arch,
                                const char *branch);

gboolean            xdg_app_spawn (GFile        *dir,
                                   char        **output,
                                   GError      **error,
                                   const gchar  *argv0,
                                   va_list       args);

typedef enum {
  XDG_APP_CP_FLAGS_NONE = 0,
  XDG_APP_CP_FLAGS_MERGE = 1<<0,
  XDG_APP_CP_FLAGS_NO_CHOWN = 1<<1,
  XDG_APP_CP_FLAGS_MOVE = 1<<2,
} XdgAppCpFlags;

gboolean   xdg_app_cp_a (GFile         *src,
                         GFile         *dest,
                         XdgAppCpFlags  flags,
                         GCancellable  *cancellable,
                         GError       **error);


#define xdg_app_autorm_rf _GLIB_CLEANUP(g_autoptr_cleanup_generic_gfree)

static inline void
xdg_app_temp_dir_destroy (void *p)
{
  GFile *dir = p;

  if (dir)
    {
      gs_shutil_rm_rf (dir, NULL, NULL);
      g_object_unref (dir);
    }
}

typedef GFile XdgAppTempDir;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppTempDir, xdg_app_temp_dir_destroy)

#define AUTOLOCK(name) G_GNUC_UNUSED __attribute__((cleanup(xdg_app_auto_unlock_helper))) GMutex * G_PASTE(auto_unlock, __LINE__) = xdg_app_auto_lock_helper (&G_LOCK_NAME (name))

G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeRepo, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeMutableTree, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeAsyncProgress, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeGpgVerifyResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeRepoCommitModifier, ostree_repo_commit_modifier_unref)

#ifndef SOUP_AUTOCLEANUPS_H
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SoupSession, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SoupMessage, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SoupRequest, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SoupURI, soup_uri_free)
#endif

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppSessionHelper, g_object_unref)

typedef struct XdgAppXml XdgAppXml;

struct XdgAppXml {
  gchar *element_name; /* NULL == text */
  char **attribute_names;
  char **attribute_values;
  char *text;
  XdgAppXml *parent;
  XdgAppXml *first_child;
  XdgAppXml *last_child;
  XdgAppXml *next_sibling;
};

XdgAppXml *xdg_app_xml_new       (const gchar   *element_name);
XdgAppXml *xdg_app_xml_new_text  (const gchar   *text);
void       xdg_app_xml_add       (XdgAppXml     *parent,
                                  XdgAppXml     *node);
void       xdg_app_xml_free      (XdgAppXml     *node);
XdgAppXml *xdg_app_xml_parse     (GInputStream  *in,
                                  gboolean       compressed,
                                  GCancellable  *cancellable,
                                  GError       **error);
void       xdg_app_xml_to_string (XdgAppXml     *node,
                                  GString       *res);
XdgAppXml *xdg_app_xml_unlink    (XdgAppXml     *node,
                                  XdgAppXml     *prev_sibling);
XdgAppXml *xdg_app_xml_find      (XdgAppXml     *node,
                                  const char    *type,
                                  XdgAppXml    **prev_child_out);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppXml, xdg_app_xml_free);


XdgAppXml *xdg_app_appstream_xml_new       (void);
gboolean   xdg_app_appstream_xml_migrate   (XdgAppXml     *source,
                                            XdgAppXml     *dest,
                                            const char    *ref,
                                            const char    *id,
                                            GKeyFile      *metadata);
GBytes *xdg_app_appstream_xml_root_to_data (XdgAppXml     *appstream_root,
                                            GError       **error);
gboolean   xdg_app_repo_generate_appstream (OstreeRepo    *repo,
                                            const char   **gpg_key_ids,
                                            const char    *gpg_homedir,
                                            GCancellable  *cancellable,
                                            GError       **error);

#endif /* __XDG_APP_UTILS_H__ */
