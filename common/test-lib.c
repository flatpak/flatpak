#include "config.h"

#include "libglnx/libglnx.h"

#include <flatpak.h>
#include <gio/gunixoutputstream.h>


static void
progress_cb (const char *status,
             guint       progress,
             gboolean    estimating,
             gpointer    user_data)
{
  g_print ("status: %s, progress: %d estimating: %d, user_data: %p\n", status, progress, estimating, user_data);
}

static gboolean
monitor_callback (GFileMonitor    * monitor,
                  GFile           * child,
                  GFile           * other_file,
                  GFileMonitorEvent eflags)
{
  g_print ("Database changed\n");
  return TRUE;
}

int
main (int argc, char *argv[])
{
  FlatpakInstallation *installation;
  FlatpakInstalledRef *app1;
  FlatpakInstalledRef *app2;
  FlatpakRemoteRef *remote_ref;
  g_autoptr(GPtrArray) remotes = NULL;
  g_autoptr(GError) error = NULL;
  int i, j, k;

  installation = flatpak_installation_new_user (NULL, &error);
  if (installation == NULL)
    {
      g_print ("error: %s\n", error->message);
      return 1;
    }

  if (0)
    {
      const char *list[] = { "gnome-apps", "app/org.gnome.iagno/x86_64/stable",
                             "gnome", "runtime/org.gnome.Sdk/x86_64/3.20" };

      for (j = 0; j < G_N_ELEMENTS (list); j += 2)
        {
          g_print ("looking for related to ref: %s\n", list[j + 1]);

          for (k = 0; k < 2; k++)
            {
              g_autoptr(GError) local_error = NULL;
              g_autoptr(GPtrArray) related = NULL;


              if (k == 0)
                related = flatpak_installation_list_remote_related_refs_sync (installation,
                                                                              list[j],
                                                                              list[j + 1],
                                                                              NULL,
                                                                              &local_error);
              else
                related = flatpak_installation_list_installed_related_refs_sync (installation,
                                                                                 list[j],
                                                                                 list[j + 1],
                                                                                 NULL,
                                                                                 &local_error);

              if (related == NULL)
                {
                  g_warning ("Error: %s", local_error->message);
                  continue;
                }

              g_print ("%s related:\n", (k == 0) ? "remote" : "local");
              for (i = 0; i < related->len; i++)
                {
                  FlatpakRelatedRef *rel = g_ptr_array_index (related, i);
                  const char * const *subpaths = flatpak_related_ref_get_subpaths (rel);
                  g_autofree char *subpaths_str = NULL;

                  if (subpaths)
                    {
                      g_autofree char *subpaths_joined = g_strjoinv (",", (char **) subpaths);
                      subpaths_str = g_strdup_printf (" subpaths: %s", subpaths_joined);
                    }
                  else
                    subpaths_str = g_strdup ("");
                  g_print ("%d %s %s %s %s dl:%d del:%d%s\n",
                           flatpak_ref_get_kind (FLATPAK_REF (rel)),
                           flatpak_ref_get_name (FLATPAK_REF (rel)),
                           flatpak_ref_get_arch (FLATPAK_REF (rel)),
                           flatpak_ref_get_branch (FLATPAK_REF (rel)),
                           flatpak_ref_get_commit (FLATPAK_REF (rel)),
                           flatpak_related_ref_should_download (rel),
                           flatpak_related_ref_should_delete (rel),
                           subpaths_str);
                }
            }
        }

      return 0;
    }

  if (argc == 4)
    {
      GFileMonitor * monitor = flatpak_installation_create_monitor (installation, NULL, NULL);
      GMainLoop *main_loop;

      g_signal_connect (monitor, "changed", (GCallback) monitor_callback, NULL);
      main_loop = g_main_loop_new (NULL, FALSE);
      g_main_loop_run (main_loop);
    }

  if (argc == 3)
    {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      app1 = flatpak_installation_install (installation,
                                           argv[1],
                                           FLATPAK_REF_KIND_APP,
                                           argv[2],
                                           NULL, NULL,
                                           progress_cb, (gpointer) 0xdeadbeef,
                                           NULL, &error);
      G_GNUC_END_IGNORE_DEPRECATIONS
      if (app1 == NULL)
        g_print ("Error: %s\n", error->message);
      else
        g_print ("Installed %s: %s\n", argv[2],
                 flatpak_ref_get_commit (FLATPAK_REF (app1)));

      return 0;
    }

  if (argc == 2)
    {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      app1 = flatpak_installation_update (installation,
                                          FLATPAK_UPDATE_FLAGS_NONE,
                                          FLATPAK_REF_KIND_APP,
                                          argv[1],
                                          NULL, NULL,
                                          progress_cb, (gpointer) 0xdeadbeef,
                                          NULL, &error);
      G_GNUC_END_IGNORE_DEPRECATIONS
      if (app1 == NULL)
        g_print ("Error: %s\n", error->message);
      else
        g_print ("Updated %s: %s\n", argv[1],
                 flatpak_ref_get_commit (FLATPAK_REF (app1)));

      return 0;
    }

  g_print ("\n**** Loading bundle\n");
  {
    g_autoptr(GFile) f = g_file_new_for_commandline_arg ("tests/hello.pak");
    g_autoptr(FlatpakBundleRef) bundle = flatpak_bundle_ref_new (f, &error);
    if (bundle == NULL)
      {
        g_print ("Error loading bundle: %s\n", error->message);
        g_clear_error (&error);
      }
    else
      {
        g_autofree char *path = g_file_get_path (flatpak_bundle_ref_get_file (bundle));
        g_autoptr(GBytes) metadata = flatpak_bundle_ref_get_metadata (bundle);
        g_autoptr(GBytes) appdata = flatpak_bundle_ref_get_appstream (bundle);
        g_print ("%d %s %s %s %s %s %"G_GUINT64_FORMAT "\n%s\n",
                 flatpak_ref_get_kind (FLATPAK_REF (bundle)),
                 flatpak_ref_get_name (FLATPAK_REF (bundle)),
                 flatpak_ref_get_arch (FLATPAK_REF (bundle)),
                 flatpak_ref_get_branch (FLATPAK_REF (bundle)),
                 flatpak_ref_get_commit (FLATPAK_REF (bundle)),
                 path,
                 flatpak_bundle_ref_get_installed_size (bundle),
                 (char *) g_bytes_get_data (metadata, NULL));

        if (appdata != NULL)
          {
            g_autoptr(GZlibDecompressor) decompressor = NULL;
            g_autoptr(GOutputStream) out2 = NULL;
            g_autoptr(GOutputStream) out = NULL;

            out = g_unix_output_stream_new (1, FALSE);
            decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
            out2 = g_converter_output_stream_new (out, G_CONVERTER (decompressor));

            if (!g_output_stream_write_all (out2,
                                            g_bytes_get_data (appdata, NULL),
                                            g_bytes_get_size (appdata),
                                            NULL, NULL, &error))
              {
                g_print ("Error decompressing appdata: %s\n", error->message);
                g_clear_error (&error);
              }
          }
      }
  }

  g_print ("\n**** Checking for updates\n");
  {
    g_autoptr(GPtrArray) updates =
      flatpak_installation_list_installed_refs_for_update (installation,
                                                           NULL, &error);

    if (updates == NULL)
      {
        g_print ("check for updates error: %s\n", error->message);
        g_clear_error (&error);
      }
    else
      {
        for (i = 0; i < updates->len; i++)
          {
            FlatpakInstalledRef *ref = g_ptr_array_index (updates, i);
            g_print ("%d %s %s %s %s %s %s %s %d %"G_GUINT64_FORMAT "\n",
                     flatpak_ref_get_kind (FLATPAK_REF (ref)),
                     flatpak_ref_get_name (FLATPAK_REF (ref)),
                     flatpak_ref_get_arch (FLATPAK_REF (ref)),
                     flatpak_ref_get_branch (FLATPAK_REF (ref)),
                     flatpak_ref_get_commit (FLATPAK_REF (ref)),
                     flatpak_installed_ref_get_latest_commit (ref),
                     flatpak_installed_ref_get_origin (ref),
                     flatpak_installed_ref_get_deploy_dir (ref),
                     flatpak_installed_ref_get_is_current (ref),
                     flatpak_installed_ref_get_installed_size (ref));
          }
      }
  }

  g_print ("\n**** Listing all installed refs\n");
  {
    g_autoptr(GPtrArray) refs = NULL;

    refs = flatpak_installation_list_installed_refs (installation,
                                                     NULL, NULL);

    for (i = 0; i < refs->len; i++)
      {
        FlatpakInstalledRef *ref = g_ptr_array_index (refs, i);
        g_print ("%d %s %s %s %s %s %s %s %d %"G_GUINT64_FORMAT "\n",
                 flatpak_ref_get_kind (FLATPAK_REF (ref)),
                 flatpak_ref_get_name (FLATPAK_REF (ref)),
                 flatpak_ref_get_arch (FLATPAK_REF (ref)),
                 flatpak_ref_get_branch (FLATPAK_REF (ref)),
                 flatpak_ref_get_commit (FLATPAK_REF (ref)),
                 flatpak_installed_ref_get_latest_commit (ref),
                 flatpak_installed_ref_get_origin (ref),
                 flatpak_installed_ref_get_deploy_dir (ref),
                 flatpak_installed_ref_get_is_current (ref),
                 flatpak_installed_ref_get_installed_size (ref));
      }
  }

  g_print ("**** Listing all installed apps\n");
  {
    g_autoptr(GPtrArray) apps = NULL;

    apps = flatpak_installation_list_installed_refs_by_kind (installation,
                                                             FLATPAK_REF_KIND_APP,
                                                             NULL, NULL);

    for (i = 0; i < apps->len; i++)
      {
        FlatpakInstalledRef *app = g_ptr_array_index (apps, i);

        g_print ("%d %s %s %s %s %s %s %s %d %"G_GUINT64_FORMAT "\n",
                 flatpak_ref_get_kind (FLATPAK_REF (app)),
                 flatpak_ref_get_name (FLATPAK_REF (app)),
                 flatpak_ref_get_arch (FLATPAK_REF (app)),
                 flatpak_ref_get_branch (FLATPAK_REF (app)),
                 flatpak_ref_get_commit (FLATPAK_REF (app)),
                 flatpak_installed_ref_get_latest_commit (app),
                 flatpak_installed_ref_get_origin (app),
                 flatpak_installed_ref_get_deploy_dir (app),
                 flatpak_installed_ref_get_is_current (app),
                 flatpak_installed_ref_get_installed_size (app));
        g_print ("metadata:\n%s\n", (char *) g_bytes_get_data (flatpak_installed_ref_load_metadata (app, NULL, NULL), NULL));
      }
  }

  g_print ("\n**** Listing all installed runtimes\n");
  {
    g_autoptr(GPtrArray) runtimes = NULL;

    runtimes = flatpak_installation_list_installed_refs_by_kind (installation,
                                                                 FLATPAK_REF_KIND_RUNTIME,
                                                                 NULL, NULL);

    for (i = 0; i < runtimes->len; i++)
      {
        FlatpakInstalledRef *runtime = g_ptr_array_index (runtimes, i);
        g_print ("%d %s %s %s %s %s %s %d\n",
                 flatpak_ref_get_kind (FLATPAK_REF (runtime)),
                 flatpak_ref_get_name (FLATPAK_REF (runtime)),
                 flatpak_ref_get_arch (FLATPAK_REF (runtime)),
                 flatpak_ref_get_branch (FLATPAK_REF (runtime)),
                 flatpak_ref_get_commit (FLATPAK_REF (runtime)),
                 flatpak_installed_ref_get_origin (runtime),
                 flatpak_installed_ref_get_deploy_dir (runtime),
                 flatpak_installed_ref_get_is_current (runtime));
      }
  }

  g_print ("\n**** Getting installed gedit master\n");
  app1 = flatpak_installation_get_installed_ref (installation,
                                                 FLATPAK_REF_KIND_APP,
                                                 "org.gnome.gedit",
                                                 NULL, "master", NULL, NULL);
  if (app1)
    {
      g_print ("gedit master: %d %s %s %s %s %s %s %d\n",
               flatpak_ref_get_kind (FLATPAK_REF (app1)),
               flatpak_ref_get_name (FLATPAK_REF (app1)),
               flatpak_ref_get_arch (FLATPAK_REF (app1)),
               flatpak_ref_get_branch (FLATPAK_REF (app1)),
               flatpak_ref_get_commit (FLATPAK_REF (app1)),
               flatpak_installed_ref_get_origin (app1),
               flatpak_installed_ref_get_deploy_dir (app1),
               flatpak_installed_ref_get_is_current (app1));
    }
  if (!flatpak_installation_launch (installation,
                                    "org.gnome.gedit",
                                    NULL, NULL, NULL,
                                    NULL, &error))
    {
      g_print ("launch gedit error: %s\n", error->message);
      g_clear_error (&error);
    }

  g_print ("\n**** Getting current installed gedit\n");
  app2 = flatpak_installation_get_current_installed_app (installation,
                                                         "org.gnome.gedit",
                                                         NULL, NULL);
  if (app2)
    {
      g_print ("gedit current: %d %s %s %s %s %s %s %d\n",
               flatpak_ref_get_kind (FLATPAK_REF (app2)),
               flatpak_ref_get_name (FLATPAK_REF (app2)),
               flatpak_ref_get_arch (FLATPAK_REF (app2)),
               flatpak_ref_get_branch (FLATPAK_REF (app2)),
               flatpak_ref_get_commit (FLATPAK_REF (app2)),
               flatpak_installed_ref_get_origin (app2),
               flatpak_installed_ref_get_deploy_dir (app2),
               flatpak_installed_ref_get_is_current (app2));
    }


  g_print ("\n**** Listing remotes\n");
  remotes = flatpak_installation_list_remotes (installation,
                                               NULL, NULL);

  for (i = 0; i < remotes->len; i++)
    {
      FlatpakRemote *remote = g_ptr_array_index (remotes, i);
      g_autoptr(GPtrArray) refs = NULL;
      const char *collection_id = NULL;

      collection_id = flatpak_remote_get_collection_id (remote);

      g_print ("\nRemote: %s %u %d %s %s %s %s %d %d %s\n",
               flatpak_remote_get_name (remote),
               flatpak_remote_get_remote_type (remote),
               flatpak_remote_get_prio (remote),
               flatpak_remote_get_url (remote),
               collection_id,
               flatpak_remote_get_title (remote),
               flatpak_remote_get_default_branch (remote),
               flatpak_remote_get_gpg_verify (remote),
               flatpak_remote_get_noenumerate (remote),
               g_file_get_path (flatpak_remote_get_appstream_dir (remote, NULL)));

      g_print ("\n**** Listing remote refs on %s\n", flatpak_remote_get_name (remote));
      refs = flatpak_installation_list_remote_refs_sync (installation, flatpak_remote_get_name (remote),
                                                         NULL, NULL);
      if (refs)
        {
          for (j = 0; j < refs->len; j++)
            {
              FlatpakRemoteRef *ref = g_ptr_array_index (refs, j);
              g_print ("%d %s %s %s %s %s\n",
                       flatpak_ref_get_kind (FLATPAK_REF (ref)),
                       flatpak_ref_get_name (FLATPAK_REF (ref)),
                       flatpak_ref_get_arch (FLATPAK_REF (ref)),
                       flatpak_ref_get_branch (FLATPAK_REF (ref)),
                       flatpak_ref_get_commit (FLATPAK_REF (ref)),
                       flatpak_remote_ref_get_remote_name (ref));

              if (j == 0)
                {
                  guint64 download_size;
                  guint64 installed_size;

                  if (!flatpak_installation_fetch_remote_size_sync (installation,
                                                                    flatpak_remote_get_name (remote),
                                                                    FLATPAK_REF (ref),
                                                                    &download_size,
                                                                    &installed_size,
                                                                    NULL, &error))
                    {
                      g_print ("error fetching sizes: %s\n", error->message);
                      g_clear_error (&error);
                    }
                  else
                    {
                      g_print ("Download size: %"G_GUINT64_FORMAT " Installed size: %"G_GUINT64_FORMAT "\n",
                               download_size, installed_size);
                    }
                }
            }
        }

      g_print ("\n**** Getting remote platform 3.20 on %s\n", flatpak_remote_get_name (remote));
      error = NULL;
      remote_ref = flatpak_installation_fetch_remote_ref_sync (installation, flatpak_remote_get_name (remote),
                                                               FLATPAK_REF_KIND_RUNTIME,
                                                               "org.gnome.Platform", NULL, "3.20",
                                                               NULL, &error);
      if (remote_ref)
        {
          GBytes *metadata;

          g_print ("%d %s %s %s %s %s\n",
                   flatpak_ref_get_kind (FLATPAK_REF (remote_ref)),
                   flatpak_ref_get_name (FLATPAK_REF (remote_ref)),
                   flatpak_ref_get_arch (FLATPAK_REF (remote_ref)),
                   flatpak_ref_get_branch (FLATPAK_REF (remote_ref)),
                   flatpak_ref_get_commit (FLATPAK_REF (remote_ref)),
                   flatpak_remote_ref_get_remote_name (remote_ref));

          metadata = flatpak_installation_fetch_remote_metadata_sync (installation, flatpak_remote_get_name (remote),
                                                                      FLATPAK_REF (remote_ref), NULL, &error);
          if (metadata)
            {
              g_print ("metadata: %s\n", (char *) g_bytes_get_data (metadata, NULL));
            }
          else
            {
              g_print ("fetch error\n");
              g_print ("error: %s\n", error->message);
              g_clear_error (&error);
            }
        }
      else
        {
          g_print ("error: %s\n", error->message);
          g_clear_error (&error);
        }
    }
  return 0;
}
