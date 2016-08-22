/* builder-context.c
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
#include <unistd.h>

#include "flatpak-utils.h"
#include "builder-context.h"

struct BuilderContext
{
  GObject         parent;

  GFile          *app_dir;
  GFile          *base_dir;
  SoupSession    *soup_session;
  char           *arch;
  char           *stop_at;

  GFile          *download_dir;
  GFile          *state_dir;
  GFile          *build_dir;
  GFile          *cache_dir;
  GFile          *ccache_dir;

  BuilderOptions *options;
  gboolean        keep_build_dirs;
  char          **cleanup;
  char          **cleanup_platform;
  gboolean        use_ccache;
  gboolean        build_runtime;
  gboolean        separate_locales;
  gboolean        sandboxed;
};

typedef struct
{
  GObjectClass parent_class;
} BuilderContextClass;

G_DEFINE_TYPE (BuilderContext, builder_context, G_TYPE_OBJECT);

enum {
  PROP_0,
  PROP_APP_DIR,
  PROP_BASE_DIR,
  LAST_PROP
};


static void
builder_context_finalize (GObject *object)
{
  BuilderContext *self = (BuilderContext *) object;

  g_clear_object (&self->state_dir);
  g_clear_object (&self->download_dir);
  g_clear_object (&self->build_dir);
  g_clear_object (&self->cache_dir);
  g_clear_object (&self->ccache_dir);
  g_clear_object (&self->app_dir);
  g_clear_object (&self->base_dir);
  g_clear_object (&self->soup_session);
  g_clear_object (&self->options);
  g_free (self->arch);
  g_free (self->stop_at);
  g_strfreev (self->cleanup);
  g_strfreev (self->cleanup_platform);

  G_OBJECT_CLASS (builder_context_parent_class)->finalize (object);
}

static void
builder_context_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  BuilderContext *self = BUILDER_CONTEXT (object);

  switch (prop_id)
    {
    case PROP_BASE_DIR:
      g_value_set_object (value, self->base_dir);
      break;

    case PROP_APP_DIR:
      g_value_set_object (value, self->app_dir);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_context_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  BuilderContext *self = BUILDER_CONTEXT (object);

  switch (prop_id)
    {
    case PROP_BASE_DIR:
      g_set_object (&self->base_dir, g_value_get_object (value));
      break;

    case PROP_APP_DIR:
      g_set_object (&self->app_dir, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_context_constructed (GObject *object)
{
  BuilderContext *self = BUILDER_CONTEXT (object);

  self->state_dir = g_file_get_child (self->base_dir, ".flatpak-builder");
  self->download_dir = g_file_get_child (self->state_dir, "downloads");
  self->build_dir = g_file_get_child (self->state_dir, "build");
  self->cache_dir = g_file_get_child (self->state_dir, "cache");
  self->ccache_dir = g_file_get_child (self->state_dir, "ccache");
}

static void
builder_context_class_init (BuilderContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = builder_context_constructed;
  object_class->finalize = builder_context_finalize;
  object_class->get_property = builder_context_get_property;
  object_class->set_property = builder_context_set_property;

  g_object_class_install_property (object_class,
                                   PROP_APP_DIR,
                                   g_param_spec_object ("app-dir",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  g_object_class_install_property (object_class,
                                   PROP_BASE_DIR,
                                   g_param_spec_object ("base-dir",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
}

static void
builder_context_init (BuilderContext *self)
{
}

GFile *
builder_context_get_base_dir (BuilderContext *self)
{
  return self->base_dir;
}

GFile *
builder_context_get_state_dir (BuilderContext *self)
{
  return self->state_dir;
}

GFile *
builder_context_get_app_dir (BuilderContext *self)
{
  return self->app_dir;
}

GFile *
builder_context_get_download_dir (BuilderContext *self)
{
  return self->download_dir;
}

GFile *
builder_context_get_cache_dir (BuilderContext *self)
{
  return self->cache_dir;
}

GFile *
builder_context_get_build_dir (BuilderContext *self)
{
  return self->build_dir;
}

GFile *
builder_context_get_ccache_dir (BuilderContext *self)
{
  return self->ccache_dir;
}

SoupSession *
builder_context_get_soup_session (BuilderContext *self)
{
  if (self->soup_session == NULL)
    {
      const char *http_proxy;

      self->soup_session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT, "flatpak-builder ",
                                                          SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                          SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                          SOUP_SESSION_TIMEOUT, 60,
                                                          SOUP_SESSION_IDLE_TIMEOUT, 60,
                                                          NULL);
      http_proxy = g_getenv ("http_proxy");
      if (http_proxy)
        {
          g_autoptr(SoupURI) proxy_uri = soup_uri_new (http_proxy);
          if (!proxy_uri)
            g_warning ("Invalid proxy URI '%s'", http_proxy);
          else
            g_object_set (self->soup_session, SOUP_SESSION_PROXY_URI, proxy_uri, NULL);
        }
    }

  return self->soup_session;
}

const char *
builder_context_get_arch (BuilderContext *self)
{
  if (self->arch == NULL)
    self->arch = g_strdup (flatpak_get_arch ());

  return (const char *) self->arch;
}

void
builder_context_set_arch (BuilderContext *self,
                          const char     *arch)
{
  g_free (self->arch);
  self->arch = g_strdup (arch);
}

const char *
builder_context_get_stop_at (BuilderContext *self)
{
  return self->stop_at;
}

void
builder_context_set_stop_at (BuilderContext *self,
                             const char     *module)
{
  g_free (self->stop_at);
  self->stop_at = g_strdup (module);
}

BuilderOptions *
builder_context_get_options (BuilderContext *self)
{
  return self->options;
}

void
builder_context_set_options (BuilderContext *self,
                             BuilderOptions *option)
{
  g_set_object (&self->options, option);
}

int
builder_context_get_n_cpu (BuilderContext *self)
{
  return (int) sysconf (_SC_NPROCESSORS_ONLN);
}

void
builder_context_set_keep_build_dirs (BuilderContext *self,
                                     gboolean        keep_build_dirs)
{
  self->keep_build_dirs = keep_build_dirs;
}

void
builder_context_set_global_cleanup (BuilderContext *self,
                                    const char    **cleanup)
{
  g_strfreev (self->cleanup);
  self->cleanup = g_strdupv ((char **) cleanup);
}

const char **
builder_context_get_global_cleanup (BuilderContext *self)
{
  return (const char **) self->cleanup;
}

void
builder_context_set_global_cleanup_platform (BuilderContext *self,
                                             const char    **cleanup)
{
  g_strfreev (self->cleanup_platform);
  self->cleanup_platform = g_strdupv ((char **) cleanup);
}

const char **
builder_context_get_global_cleanup_platform (BuilderContext *self)
{
  return (const char **) self->cleanup_platform;
}

gboolean
builder_context_get_keep_build_dirs (BuilderContext *self)
{
  return self->keep_build_dirs;
}

void
builder_context_set_sandboxed (BuilderContext *self,
                               gboolean        sandboxed)
{
  self->sandboxed = sandboxed;
}

gboolean
builder_context_get_sandboxed (BuilderContext *self)
{
  return self->sandboxed;
}

gboolean
builder_context_get_build_runtime (BuilderContext *self)
{
  return self->build_runtime;
}

void
builder_context_set_build_runtime (BuilderContext *self,
                                   gboolean        build_runtime)
{
  self->build_runtime = !!build_runtime;
}

gboolean
builder_context_get_separate_locales (BuilderContext *self)
{
  return self->separate_locales;
}

void
builder_context_set_separate_locales (BuilderContext *self,
                                      gboolean        separate_locales)
{
  self->separate_locales = !!separate_locales;
}

gboolean
builder_context_enable_ccache (BuilderContext *self,
                               GError        **error)
{
  g_autofree char *ccache_path = g_file_get_path (self->ccache_dir);
  g_autofree char *ccache_bin_path = g_build_filename (ccache_path, "bin", NULL);
  int i;
  static const char *compilers[] = {
    "cc",
    "c++",
    "gcc",
    "g++"
  };

  if (g_mkdir_with_parents (ccache_bin_path, 0755) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  for (i = 0; i < G_N_ELEMENTS (compilers); i++)
    {
      const char *symlink_path = g_build_filename (ccache_bin_path, compilers[i], NULL);
      if (symlink ("/usr/bin/ccache", symlink_path) && errno != EEXIST)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }

  self->use_ccache = TRUE;

  return TRUE;
}

char **
builder_context_extend_env (BuilderContext *self,
                            char          **envp)
{
  if (self->use_ccache)
    {
      const char *old_path = g_environ_getenv (envp, "PATH");
      g_autofree char *new_path = NULL;
      if (old_path == NULL)
        old_path = "/app/bin:/usr/bin"; /* This is the flatpak default PATH */

      new_path = g_strdup_printf ("/run/ccache/bin:%s", old_path);
      envp = g_environ_setenv (envp, "PATH", new_path, TRUE);
      envp = g_environ_setenv (envp, "CCACHE_DIR", "/run/ccache", TRUE);
    }

  return envp;
}

BuilderContext *
builder_context_new (GFile *base_dir,
                     GFile *app_dir)
{
  return g_object_new (BUILDER_TYPE_CONTEXT,
                       "base-dir", base_dir,
                       "app-dir", app_dir,
                       NULL);
}
