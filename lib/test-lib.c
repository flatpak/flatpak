#include <xdg-app.h>

int
main (int argc, char *argv[])
{
  XdgAppInstallation *installation;
  XdgAppInstalledRef **apps;
  XdgAppRef **refs;
  XdgAppInstalledRef *app1;
  XdgAppInstalledRef *app2;
  XdgAppInstalledRef **runtimes;
  XdgAppRemote **remotes;
  int i, j;

  installation = xdg_app_installation_new_user ();

  apps = xdg_app_installation_list_installed_refs (installation,
                                                   XDG_APP_REF_KIND_APP,
                                                   NULL, NULL);

  for (i = 0; apps[i] != NULL; i++)
    {
      g_print ("%d %s %s %s %s %s %s %d\n",
               xdg_app_ref_get_kind (XDG_APP_REF(apps[i])),
               xdg_app_ref_get_name (XDG_APP_REF(apps[i])),
               xdg_app_ref_get_arch (XDG_APP_REF(apps[i])),
               xdg_app_ref_get_version (XDG_APP_REF(apps[i])),
               xdg_app_ref_get_commit (XDG_APP_REF(apps[i])),
               xdg_app_installed_ref_get_origin (apps[i]),
               xdg_app_installed_ref_get_deploy_dir (apps[i]),
               xdg_app_installed_ref_get_current (apps[i]));
      g_print ("metadata:\n%s\n", xdg_app_installed_ref_load_metadata (apps[i], NULL, NULL));
    }

  runtimes = xdg_app_installation_list_installed_refs (installation,
                                                       XDG_APP_REF_KIND_RUNTIME,
                                                       NULL, NULL);

  for (i = 0; runtimes[i] != NULL; i++)
    {
      g_print ("%d %s %s %s %s %s %s %d\n",
               xdg_app_ref_get_kind (XDG_APP_REF(runtimes[i])),
               xdg_app_ref_get_name (XDG_APP_REF(runtimes[i])),
               xdg_app_ref_get_arch (XDG_APP_REF(runtimes[i])),
               xdg_app_ref_get_version (XDG_APP_REF(runtimes[i])),
               xdg_app_ref_get_commit (XDG_APP_REF(runtimes[i])),
               xdg_app_installed_ref_get_origin (runtimes[i]),
               xdg_app_installed_ref_get_deploy_dir (runtimes[i]),
               xdg_app_installed_ref_get_current (runtimes[i]));
    }

  app1 = xdg_app_installation_get_installed_ref (installation, 
                                                 XDG_APP_REF_KIND_APP,
                                                 "org.gnome.gedit",
                                                 NULL, "master", NULL, NULL);
  if (app1)
    g_print ("gedit master: %d %s %s %s %s %s %s %d\n",
             xdg_app_ref_get_kind (XDG_APP_REF(app1)),
             xdg_app_ref_get_name (XDG_APP_REF(app1)),
             xdg_app_ref_get_arch (XDG_APP_REF(app1)),
             xdg_app_ref_get_version (XDG_APP_REF(app1)),
             xdg_app_ref_get_commit (XDG_APP_REF(app1)),
             xdg_app_installed_ref_get_origin (app1),
             xdg_app_installed_ref_get_deploy_dir (app1),
             xdg_app_installed_ref_get_current (app1));

  app2 = xdg_app_installation_get_current_installed_app (installation, 
                                                         "org.gnome.gedit",
                                                         NULL, NULL);
  if (app2)
    g_print ("gedit current: %d %s %s %s %s %s %s %d\n",
             xdg_app_ref_get_kind (XDG_APP_REF(app2)),
             xdg_app_ref_get_name (XDG_APP_REF(app2)),
             xdg_app_ref_get_arch (XDG_APP_REF(app2)),
             xdg_app_ref_get_version (XDG_APP_REF(app2)),
             xdg_app_ref_get_commit (XDG_APP_REF(app2)),
             xdg_app_installed_ref_get_origin (app2),
             xdg_app_installed_ref_get_deploy_dir (app2),
             xdg_app_installed_ref_get_current (app2));


  remotes = xdg_app_installation_list_remotes (installation,
                                               NULL, NULL);

  for (i = 0; remotes[i] != NULL; i++)
    {
      g_print ("%s %s %s %d %d\n",
               xdg_app_remote_get_name (remotes[i]),
               xdg_app_remote_get_url (remotes[i]),
               xdg_app_remote_get_title (remotes[i]),
               xdg_app_remote_get_gpg_verify (remotes[i]),
               xdg_app_remote_get_noenumerate (remotes[i]));

      refs = xdg_app_remote_list_refs (remotes[i],
                                       NULL, NULL);
      if (refs)
        {
          for (j = 0; refs[j] != NULL; j++)
            {
              g_print ("%d %s %s %s %s\n",
                       xdg_app_ref_get_kind (refs[j]),
                       xdg_app_ref_get_name (refs[j]),
                       xdg_app_ref_get_arch (refs[j]),
                       xdg_app_ref_get_version (refs[j]),
                       xdg_app_ref_get_commit (refs[j]));
            }
        }
    }

}
