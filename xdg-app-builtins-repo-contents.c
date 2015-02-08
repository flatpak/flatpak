#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"
#include <libsoup/soup.h>

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static gboolean opt_show_details;
static gboolean opt_only_runtimes;
static gboolean opt_only_apps;
static gboolean opt_only_updates;

static GOptionEntry options[] = {
  { "show-details", 0, 0, G_OPTION_ARG_NONE, &opt_show_details, "Show arches and branches", NULL },
  { "runtimes", 0, 0, G_OPTION_ARG_NONE, &opt_only_runtimes, "Show only runtimes", NULL },
  { "apps", 0, 0, G_OPTION_ARG_NONE, &opt_only_apps, "Show only apps", NULL },
  { "updates", 0, 0, G_OPTION_ARG_NONE, &opt_only_updates, "Show only those where updates are available", NULL },
  { NULL }
};

static gboolean
load_contents (const char *uri, GBytes **contents, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gs_free char *scheme = NULL;

  scheme = g_uri_parse_scheme (uri);
  if (strcmp (scheme, "file") == 0)
    {
      char *buffer;
      gsize length;
      gs_unref_object GFile *file = NULL;

      g_debug ("Loading summary %s using GIO", uri);
      file = g_file_new_for_uri (uri);
      if (!g_file_load_contents (file, cancellable, &buffer, &length, NULL, NULL))
        goto out;

      *contents = g_bytes_new_take (buffer, length);
    }
  else
    {
      gs_unref_object SoupSession *session = NULL;
      gs_unref_object SoupMessage *msg = NULL;

      g_debug ("Loading summary %s using libsoup", uri);
      session = soup_session_new ();
      msg = soup_message_new ("GET", uri);
      soup_session_send_message (session, msg);

      if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
        goto out;

      *contents = g_bytes_new (msg->response_body->data, msg->response_body->length);
    }

  ret = TRUE;

  g_debug ("Received %ld bytes", g_bytes_get_size (*contents));

out:
  return ret;
}

gboolean
xdg_app_builtin_repo_contents (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_hashtable GHashTable *refs = NULL;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  gs_unref_ptrarray GPtrArray *names = NULL;
  int i;
  const char *repository;
  gs_free char *url = NULL;
  gs_free char *summary_url = NULL;
  gs_unref_bytes GBytes *bytes = NULL;

  context = g_option_context_new (" REPOSITORY - Show available runtimes and applications");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "REPOSITORY must be specified", error);
      goto out;
    }

  repository = argv[1];

  repo = xdg_app_dir_get_repo (dir);
  if (!ostree_repo_remote_get_url (repo, repository, &url, error))
    goto out;

  summary_url = g_build_filename (url, "summary", NULL);
  if (load_contents (summary_url, &bytes, cancellable, NULL))
    {
      gs_unref_variant GVariant *summary;
      gs_unref_variant GVariant *ref_list;
      int n;

      refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

      summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes, FALSE);
      ref_list = g_variant_get_child_value (summary, 0);
      n = g_variant_n_children (ref_list);
      g_debug ("Summary contains %d refs", n);
      for (i = 0; i < n; i++)
        {
          gs_unref_variant GVariant *ref = NULL;
          gs_unref_variant GVariant *csum_v = NULL;
          char *refname;
          char *checksum;

          ref = g_variant_get_child_value (ref_list, i);
          g_variant_get (ref, "(&s(t@aya{sv}))", &refname, NULL, &csum_v, NULL);

          if (!ostree_validate_rev (refname, error))
            goto out;

          checksum = ostree_checksum_from_bytes_v (csum_v);
          g_debug ("%s summary: %s -> %s\n", repository, refname, checksum);
          g_hash_table_insert (refs, g_strdup (refname), checksum);
        }
    }
  else
    {
      g_printerr ("Failed to load summary file for remote %s, listing local refs\n", repository);
      if (!ostree_repo_list_refs (repo, NULL, &refs, cancellable, error))
        goto out;
    }

  names = g_ptr_array_new_with_free_func (g_free);

  g_hash_table_iter_init (&iter, refs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *refspec = key;
      const char *checksum = value;
      gs_free char *remote = NULL;
      gs_free char *ref = NULL;
      char *name = NULL;
      char *p;

      if (!ostree_parse_refspec (refspec, &remote, &ref, error))
        goto out;

      if (remote != NULL && !g_str_equal (remote, repository))
        continue;

      if (opt_only_updates)
        {
          gs_free char *deployed = NULL;

          deployed = xdg_app_dir_read_active (dir, ref, cancellable);
          if (deployed == NULL)
            continue;

          if (g_strcmp0 (deployed, checksum) == 0)
            continue;
        }

      if (g_str_has_prefix (ref, "runtime/") && !opt_only_apps)
        {
          if (!opt_show_details)
            {
              name = g_strdup (ref + strlen ("runtime/"));
              p = strchr (name, '/');
              if (p)
                *p = 0;
            }
          else
            {
              name = g_strdup (ref);
            }
        }

      if (g_str_has_prefix (ref, "app/") && !opt_only_runtimes)
        {
          if (!opt_show_details)
            {
              name = g_strdup (ref + strlen ("app/"));
              p = strchr (name, '/');
              if (p)
                *p = 0;
            }
          else
            {
              name = g_strdup (ref);
            }
        }

      if (name)
        {
	  gboolean found = FALSE;

          for (i = 0; i < names->len; i++)
            {
              if (strcmp (name, g_ptr_array_index (names, i)) == 0)
		found = TRUE;
              break;
            }

	  if (found)
            g_ptr_array_add (names, name);
	  else
	    g_free (name);
        }
    }

  g_ptr_array_sort (names, (GCompareFunc)strcmp);

  for (i = 0; i < names->len; i++)
    g_print ("%s\n", (char *)g_ptr_array_index (names, i));

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);

  return ret;
}
