#include "config.h"

#include "libglnx/libglnx.h"

#include <xdg-app.h>
#include <gio/gunixoutputstream.h>


static void
progress_cb (const char *status,
             guint progress,
             gboolean estimating,
             gpointer user_data)
{
  g_print ("status: %s, progress: %d estimating: %d, user_data: %p\n", status, progress, estimating, user_data);
}

static gboolean
monitor_callback (GFileMonitor* monitor,
                  GFile* child,
                  GFile* other_file,
                  GFileMonitorEvent eflags)
{
  g_print ("Database changed\n");
  return TRUE;
}

int
main (int argc, char *argv[])
{
  XdgAppInstallation *installation;
  XdgAppInstalledRef *app1;
  XdgAppInstalledRef *app2;
  XdgAppRemoteRef *remote_ref;
  g_autoptr(GPtrArray) remotes = NULL;
  GError *error = NULL;
  int i, j;

  installation = xdg_app_installation_new_user (NULL, &error);
  if (installation == NULL)
    {
      g_print ("error: %s\n", error->message);
      return 1;
    }

  if (argc == 4)
    {
      GFileMonitor * monitor = xdg_app_installation_create_monitor (installation, NULL, NULL);
      GMainLoop *main_loop;

      g_signal_connect (monitor, "changed", (GCallback)monitor_callback, NULL);
      main_loop = g_main_loop_new (NULL, FALSE);
      g_main_loop_run (main_loop);
    }

  if (argc == 3)
    {
      app1 = xdg_app_installation_install (installation,
                                           argv[1],
                                           XDG_APP_REF_KIND_APP,
                                           argv[2],
                                           NULL, NULL,
                                           progress_cb, (gpointer)0xdeadbeef,
                                           NULL, &error);
      if (app1 == NULL)
        g_print ("Error: %s\n", error->message);
      else
        g_print ("Installed %s: %s\n", argv[2],
                 xdg_app_ref_get_commit (XDG_APP_REF (app1)));

      return 0;
    }

  if (argc == 2)
    {
      app1 = xdg_app_installation_update (installation,
                                          XDG_APP_UPDATE_FLAGS_NONE,
                                          XDG_APP_REF_KIND_APP,
                                          argv[1],
                                          NULL, NULL,
                                          progress_cb, (gpointer)0xdeadbeef,
                                          NULL, &error);
      if (app1 == NULL)
        g_print ("Error: %s\n", error->message);
      else
        g_print ("Updated %s: %s\n", argv[1],
                 xdg_app_ref_get_commit (XDG_APP_REF (app1)));

      return 0;
    }

  g_print ("\n**** Loading bundle\n");
  {
    g_autoptr(GFile) f = g_file_new_for_commandline_arg ("tests/hello.xdgapp");
    g_autoptr(XdgAppBundleRef) bundle = xdg_app_bundle_ref_new (f, &error);
    if (bundle == NULL)
      {
        g_print ("Error loading bundle: %s\n", error->message);
        g_clear_error (&error);
      }
    else
      {
        g_autofree char *path = g_file_get_path (xdg_app_bundle_ref_get_file (bundle));
        g_autoptr(GBytes) metadata = xdg_app_bundle_ref_get_metadata (bundle);
        g_autoptr(GBytes) appdata = xdg_app_bundle_ref_get_appstream (bundle);
        g_print ("%d %s %s %s %s %s %"G_GUINT64_FORMAT"\n%s\n",
                 xdg_app_ref_get_kind (XDG_APP_REF(bundle)),
                 xdg_app_ref_get_name (XDG_APP_REF(bundle)),
                 xdg_app_ref_get_arch (XDG_APP_REF(bundle)),
                 xdg_app_ref_get_branch (XDG_APP_REF(bundle)),
                 xdg_app_ref_get_commit (XDG_APP_REF(bundle)),
                 path,
                 xdg_app_bundle_ref_get_installed_size (bundle),
                 (char *)g_bytes_get_data (metadata, NULL));

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
      xdg_app_installation_list_installed_refs_for_update (installation,
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
            XdgAppInstalledRef *ref = g_ptr_array_index(updates,i);
            g_print ("%d %s %s %s %s %s %s %s %d %"G_GUINT64_FORMAT"\n",
                     xdg_app_ref_get_kind (XDG_APP_REF(ref)),
                     xdg_app_ref_get_name (XDG_APP_REF(ref)),
                     xdg_app_ref_get_arch (XDG_APP_REF(ref)),
                     xdg_app_ref_get_branch (XDG_APP_REF(ref)),
                     xdg_app_ref_get_commit (XDG_APP_REF(ref)),
                     xdg_app_installed_ref_get_latest_commit (ref),
                     xdg_app_installed_ref_get_origin (ref),
                     xdg_app_installed_ref_get_deploy_dir (ref),
                     xdg_app_installed_ref_get_is_current (ref),
                     xdg_app_installed_ref_get_installed_size (ref));
          }
      }
  }

  g_print ("\n**** Listing all installed refs\n");
  {
    g_autoptr(GPtrArray) refs = NULL;

    refs = xdg_app_installation_list_installed_refs (installation,
                                                     NULL, NULL);

    for (i = 0; i < refs->len; i++)
      {
        XdgAppInstalledRef *ref = g_ptr_array_index(refs,i);
        g_print ("%d %s %s %s %s %s %s %s %d %"G_GUINT64_FORMAT"\n",
                 xdg_app_ref_get_kind (XDG_APP_REF(ref)),
                 xdg_app_ref_get_name (XDG_APP_REF(ref)),
                 xdg_app_ref_get_arch (XDG_APP_REF(ref)),
                 xdg_app_ref_get_branch (XDG_APP_REF(ref)),
                 xdg_app_ref_get_commit (XDG_APP_REF(ref)),
                 xdg_app_installed_ref_get_latest_commit (ref),
                 xdg_app_installed_ref_get_origin (ref),
                 xdg_app_installed_ref_get_deploy_dir (ref),
                 xdg_app_installed_ref_get_is_current (ref),
                 xdg_app_installed_ref_get_installed_size (ref));
      }
  }

  g_print ("**** Listing all installed apps\n");
  {
    g_autoptr(GPtrArray) apps = NULL;

    apps = xdg_app_installation_list_installed_refs_by_kind (installation,
                                                             XDG_APP_REF_KIND_APP,
                                                             NULL, NULL);

    for (i = 0; i < apps->len; i++)
      {
        XdgAppInstalledRef *app = g_ptr_array_index(apps,i);

        g_print ("%d %s %s %s %s %s %s %s %d %"G_GUINT64_FORMAT"\n",
                 xdg_app_ref_get_kind (XDG_APP_REF(app)),
                 xdg_app_ref_get_name (XDG_APP_REF(app)),
                 xdg_app_ref_get_arch (XDG_APP_REF(app)),
                 xdg_app_ref_get_branch (XDG_APP_REF(app)),
                 xdg_app_ref_get_commit (XDG_APP_REF(app)),
                 xdg_app_installed_ref_get_latest_commit (app),
                 xdg_app_installed_ref_get_origin (app),
                 xdg_app_installed_ref_get_deploy_dir (app),
                 xdg_app_installed_ref_get_is_current (app),
                 xdg_app_installed_ref_get_installed_size (app));
        g_print ("metadata:\n%s\n", (char *)g_bytes_get_data (xdg_app_installed_ref_load_metadata (app, NULL, NULL), NULL));
      }
  }

  g_print ("\n**** Listing all installed runtimes\n");
  {
    g_autoptr(GPtrArray) runtimes = NULL;

    runtimes = xdg_app_installation_list_installed_refs_by_kind (installation,
                                                                 XDG_APP_REF_KIND_RUNTIME,
                                                                 NULL, NULL);

    for (i = 0; i < runtimes->len; i++)
      {
        XdgAppInstalledRef *runtime = g_ptr_array_index(runtimes,i);
        g_print ("%d %s %s %s %s %s %s %d\n",
                 xdg_app_ref_get_kind (XDG_APP_REF(runtime)),
                 xdg_app_ref_get_name (XDG_APP_REF(runtime)),
                 xdg_app_ref_get_arch (XDG_APP_REF(runtime)),
                 xdg_app_ref_get_branch (XDG_APP_REF(runtime)),
                 xdg_app_ref_get_commit (XDG_APP_REF(runtime)),
                 xdg_app_installed_ref_get_origin (runtime),
                 xdg_app_installed_ref_get_deploy_dir (runtime),
                 xdg_app_installed_ref_get_is_current (runtime));
      }
  }

  g_print ("\n**** Getting installed gedit master\n");
  app1 = xdg_app_installation_get_installed_ref (installation,
                                                 XDG_APP_REF_KIND_APP,
                                                 "org.gnome.gedit",
                                                 NULL, "master", NULL, NULL);
  if (app1)
    {
      g_print ("gedit master: %d %s %s %s %s %s %s %d\n",
               xdg_app_ref_get_kind (XDG_APP_REF(app1)),
               xdg_app_ref_get_name (XDG_APP_REF(app1)),
               xdg_app_ref_get_arch (XDG_APP_REF(app1)),
               xdg_app_ref_get_branch (XDG_APP_REF(app1)),
               xdg_app_ref_get_commit (XDG_APP_REF(app1)),
               xdg_app_installed_ref_get_origin (app1),
               xdg_app_installed_ref_get_deploy_dir (app1),
               xdg_app_installed_ref_get_is_current (app1));
    }
  if (!xdg_app_installation_launch (installation,
                                    "org.gnome.gedit",
                                    NULL, NULL, NULL,
                                    NULL, &error))
    {
      g_print ("launch gedit error: %s\n", error->message);
      g_clear_error (&error);
    }

  g_print ("\n**** Getting current installed gedit\n");
  app2 = xdg_app_installation_get_current_installed_app (installation,
                                                         "org.gnome.gedit",
                                                         NULL, NULL);
  if (app2)
    g_print ("gedit current: %d %s %s %s %s %s %s %d\n",
             xdg_app_ref_get_kind (XDG_APP_REF(app2)),
             xdg_app_ref_get_name (XDG_APP_REF(app2)),
             xdg_app_ref_get_arch (XDG_APP_REF(app2)),
             xdg_app_ref_get_branch (XDG_APP_REF(app2)),
             xdg_app_ref_get_commit (XDG_APP_REF(app2)),
             xdg_app_installed_ref_get_origin (app2),
             xdg_app_installed_ref_get_deploy_dir (app2),
             xdg_app_installed_ref_get_is_current (app2));


  g_print ("\n**** Listing remotes\n");
  remotes = xdg_app_installation_list_remotes (installation,
                                               NULL, NULL);

  for (i = 0; i < remotes->len; i++)
    {
      XdgAppRemote *remote = g_ptr_array_index(remotes, i);
      g_autoptr(GPtrArray) refs = NULL;
      g_print ("\nRemote: %s %d %s %s %d %d %s\n",
               xdg_app_remote_get_name (remote),
               xdg_app_remote_get_prio (remote),
               xdg_app_remote_get_url (remote),
               xdg_app_remote_get_title (remote),
               xdg_app_remote_get_gpg_verify (remote),
               xdg_app_remote_get_noenumerate (remote),
               g_file_get_path (xdg_app_remote_get_appstream_dir (remote, NULL)));

      g_print ("\n**** Listing remote refs on %s\n", xdg_app_remote_get_name (remote));
      refs = xdg_app_installation_list_remote_refs_sync (installation, xdg_app_remote_get_name (remote),
                                                         NULL, NULL);
      if (refs)
        {
          for (j = 0; j < refs->len; j++)
            {
              XdgAppRemoteRef *ref = g_ptr_array_index(refs,j);
              g_print ("%d %s %s %s %s %s\n",
                       xdg_app_ref_get_kind (XDG_APP_REF(ref)),
                       xdg_app_ref_get_name (XDG_APP_REF(ref)),
                       xdg_app_ref_get_arch (XDG_APP_REF(ref)),
                       xdg_app_ref_get_branch (XDG_APP_REF(ref)),
                       xdg_app_ref_get_commit (XDG_APP_REF(ref)),
                       xdg_app_remote_ref_get_remote_name (ref));

              if (j == 0)
                {
                  guint64 download_size;
                  guint64 installed_size;
                  if (!xdg_app_installation_fetch_remote_size_sync (installation,
                                                                    xdg_app_remote_get_name (remote),
                                                                    xdg_app_ref_get_commit (XDG_APP_REF(ref)),
                                                                    &download_size,
                                                                    &installed_size,
                                                                    NULL, &error))
                    {
                      g_print ("error fetching sizes: %s\n", error->message);
                      g_clear_error (&error);
                    }
                  else
                    g_print ("Download size: %"G_GUINT64_FORMAT" Installed size: %"G_GUINT64_FORMAT"\n",
                             download_size, installed_size);

                  if (!xdg_app_installation_fetch_remote_size_sync2 (installation,
                                                                     xdg_app_remote_get_name (remote),
                                                                     XDG_APP_REF(ref),
                                                                     &download_size,
                                                                     &installed_size,
                                                                     NULL, &error))
                    {
                      g_print ("error fetching sizes2: %s\n", error->message);
                      g_clear_error (&error);
                    }
                  else
                    g_print ("Download size2: %"G_GUINT64_FORMAT" Installed size2: %"G_GUINT64_FORMAT"\n",
                             download_size, installed_size);

                }
            }
        }

      g_print ("\n**** Getting remote platform 3.20 on %s\n", xdg_app_remote_get_name (remote));
      error = NULL;
      remote_ref = xdg_app_installation_fetch_remote_ref_sync (installation, xdg_app_remote_get_name (remote),
                                                               XDG_APP_REF_KIND_RUNTIME,
                                                               "org.gnome.Platform", NULL, "3.20",
                                                               NULL, &error);
      if (remote_ref)
        {
          GBytes *metadata;
          GBytes *metadata2;

          g_print ("%d %s %s %s %s %s\n",
                   xdg_app_ref_get_kind (XDG_APP_REF(remote_ref)),
                   xdg_app_ref_get_name (XDG_APP_REF(remote_ref)),
                   xdg_app_ref_get_arch (XDG_APP_REF(remote_ref)),
                   xdg_app_ref_get_branch (XDG_APP_REF(remote_ref)),
                   xdg_app_ref_get_commit (XDG_APP_REF(remote_ref)),
                   xdg_app_remote_ref_get_remote_name (remote_ref));

          metadata = xdg_app_installation_fetch_remote_metadata_sync (installation, xdg_app_remote_get_name (remote),
                                                                      xdg_app_ref_get_commit (XDG_APP_REF(remote_ref)), NULL, &error);
          if (metadata)
            {
              g_print ("metadata: %s\n", (char *)g_bytes_get_data (metadata, NULL));
            }
          else
            {
              g_print ("fetch error\n");
              g_print ("error: %s\n", error->message);
              g_clear_error (&error);
            }

          metadata2 = xdg_app_installation_fetch_remote_metadata_sync2 (installation, xdg_app_remote_get_name (remote),
                                                                        XDG_APP_REF(remote_ref), NULL, &error);
          if (metadata2)
            {
              g_print ("metadata2: %s\n", (char *)g_bytes_get_data (metadata2, NULL));
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
