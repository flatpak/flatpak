#include "config.h"

#include "libglnx/libglnx.h"

#include <xdg-app.h>


static void
progress_cb (const char *status,
             guint progress,
             gboolean estimating,
             gpointer user_data)
{
  g_print ("status: %s, progress: %d estimating: %d, user_data: %p\n", status, progress, estimating, user_data);
}

int
main (int argc, char *argv[])
{
  XdgAppInstallation *installation;
  XdgAppInstalledRef *app1;
  XdgAppInstalledRef *app2;
  XdgAppRemoteRef *remote_ref;
  XdgAppRemote **remotes;
  GError *error = NULL;
  int i, j;

  installation = xdg_app_installation_new_user ();

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

  g_print ("\n**** Listing all installed refs\n");
  {
    g_autoptr(GPtrArray) refs = NULL;

    refs = xdg_app_installation_list_installed_refs (installation,
                                                     NULL, NULL);

    for (i = 0; i < refs->len; i++)
      {
        XdgAppInstalledRef *ref = g_ptr_array_index(refs,i);
        g_print ("%d %s %s %s %s %s %s %d\n",
                 xdg_app_ref_get_kind (XDG_APP_REF(ref)),
                 xdg_app_ref_get_name (XDG_APP_REF(ref)),
                 xdg_app_ref_get_arch (XDG_APP_REF(ref)),
                 xdg_app_ref_get_branch (XDG_APP_REF(ref)),
                 xdg_app_ref_get_commit (XDG_APP_REF(ref)),
                 xdg_app_installed_ref_get_origin (ref),
                 xdg_app_installed_ref_get_deploy_dir (ref),
                 xdg_app_installed_ref_get_is_current (ref));
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

        g_print ("%d %s %s %s %s %s %s %d\n",
                 xdg_app_ref_get_kind (XDG_APP_REF(app)),
                 xdg_app_ref_get_name (XDG_APP_REF(app)),
                 xdg_app_ref_get_arch (XDG_APP_REF(app)),
                 xdg_app_ref_get_branch (XDG_APP_REF(app)),
                 xdg_app_ref_get_commit (XDG_APP_REF(app)),
                 xdg_app_installed_ref_get_origin (app),
                 xdg_app_installed_ref_get_deploy_dir (app),
                 xdg_app_installed_ref_get_is_current (app));
        g_print ("metadata:\n%s\n", xdg_app_installed_ref_load_metadata (app, NULL, NULL));
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

  for (i = 0; remotes[i] != NULL; i++)
    {
      g_autoptr(GPtrArray) refs = NULL;
      g_print ("\nRemote: %s %s %s %d %d\n",
               xdg_app_remote_get_name (remotes[i]),
               xdg_app_remote_get_url (remotes[i]),
               xdg_app_remote_get_title (remotes[i]),
               xdg_app_remote_get_gpg_verify (remotes[i]),
               xdg_app_remote_get_noenumerate (remotes[i]));

      g_print ("\n**** Listing remote refs on %s\n", xdg_app_remote_get_name (remotes[i]));
      refs = xdg_app_remote_list_refs_sync (remotes[i], NULL, NULL);
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
            }
        }

      g_print ("\n**** Getting remote gedit master on %s\n", xdg_app_remote_get_name (remotes[i]));
      error = NULL;
      remote_ref = xdg_app_remote_fetch_ref_sync (remotes[i],
                                                  XDG_APP_REF_KIND_APP,
                                                  "org.gnome.gedit", NULL, "master",
                                                  NULL, &error);
      if (remote_ref)
        {
          GBytes *metadata;

          g_print ("%d %s %s %s %s %s\n",
                   xdg_app_ref_get_kind (XDG_APP_REF(remote_ref)),
                   xdg_app_ref_get_name (XDG_APP_REF(remote_ref)),
                   xdg_app_ref_get_arch (XDG_APP_REF(remote_ref)),
                   xdg_app_ref_get_branch (XDG_APP_REF(remote_ref)),
                   xdg_app_ref_get_commit (XDG_APP_REF(remote_ref)),
                   xdg_app_remote_ref_get_remote_name (remote_ref));

          metadata = xdg_app_remote_fetch_metadata_sync (remotes[i], xdg_app_ref_get_commit (XDG_APP_REF(remote_ref)), NULL, &error);
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
        }
      else
        {
          g_print ("error: %s\n", error->message);
          g_clear_error (&error);
        }
    }
  return 0;
}
