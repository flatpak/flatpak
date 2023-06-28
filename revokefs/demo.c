#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

static char *opt_fuse_options;

static GOptionEntry options[] = {
  { "fuse-options", 'o', 0, G_OPTION_ARG_STRING, &opt_fuse_options, "FUSE mount options", "opt,[opt...]" },
  { NULL }
};

/* Close the write end of the socket in the backend after forking. */
static void
backend_setup (gpointer data)
{
  int write_socket = GPOINTER_TO_INT (data);
  close (write_socket);
}

int
main (int argc, char *argv[])
{
  g_autoptr(GOptionContext) context = NULL;
  int sockets[2];
  int pipes[2];
  g_autofree char *socket_0 = NULL;
  g_autofree char *socket_1 = NULL;
  g_autofree char *exit_with_opt = NULL;
  GError *error = NULL;
  char buf[20];
  g_autoptr(GPtrArray) fuse_argv = NULL;
  GPid backend_pid, fuse_pid;
  g_autofree char *umount_stderr = NULL;
  int umount_status = 0;

  context = g_option_context_new ("basepath targetpath");
  g_option_context_set_summary (context, "Demo for revokefs-fuse");
  g_option_context_add_main_entries (context, options, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s\n", error->message);
      exit (EXIT_FAILURE);
    }

  if (argc != 3)
    {
      g_printerr ("Usage: %s basepath targetpath\n", g_get_prgname ());
      exit (EXIT_FAILURE);
    }

  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets))
    {
      perror ("Failed to create socket pair");
      exit (EXIT_FAILURE);
    }

  if (pipe (pipes) == -1)
    {
      perror ("Failed to create pipe");
      exit (EXIT_FAILURE);
    }

  socket_0 = g_strdup_printf ("--socket=%d", sockets[0]);
  socket_1 = g_strdup_printf ("--socket=%d", sockets[1]);
  exit_with_opt = g_strdup_printf ("--exit-with-fd=%d", pipes[1]);

  char *backend_argv[] =
    {
     "./revokefs-fuse",
     "--backend",
     socket_0,
     exit_with_opt,
     argv[1],
     NULL
    };

  if (!g_spawn_async (NULL,
                      backend_argv,
                      NULL,
                      G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                      backend_setup,
                      GINT_TO_POINTER (sockets[1]),
                      &backend_pid, &error))
    {
      g_printerr ("Failed to launch backend: %s\n", error->message);
      exit (EXIT_FAILURE);
    }

  /* Close backend side of the socket and pipe so they don't get into
   * the fuse child. */
  close (sockets[0]);
  close (pipes[1]);

  fuse_argv = g_ptr_array_new ();
  g_ptr_array_add (fuse_argv, "./revokefs-fuse");
  g_ptr_array_add (fuse_argv, socket_1);
  if (opt_fuse_options)
    {
      g_ptr_array_add (fuse_argv, "-o");
      g_ptr_array_add (fuse_argv, opt_fuse_options);
    }
  g_ptr_array_add (fuse_argv, argv[1]);
  g_ptr_array_add (fuse_argv, argv[2]);
  g_ptr_array_add (fuse_argv, NULL);

  if (!g_spawn_async (NULL,
                      (char **) fuse_argv->pdata,
                      NULL,
                      G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                      NULL, NULL,
                      &fuse_pid, &error))
    {
      g_printerr ("Failed to FUSE process: %s\n", error->message);
      exit (EXIT_FAILURE);
    }

  g_print ("Started revokefs, press enter to revoke");
  if (!fgets(buf, sizeof(buf), stdin))
    {
      perror ("fgets");
    }

  g_print ("Revoking write permissions\n");
  shutdown (sockets[1], SHUT_RDWR);
  close (pipes[0]);

  /* Unmount the target */
  char *umount_argv[] =
    {
     "fusermount",
     "-u",
     argv[2],
     NULL
    };

  if (!g_spawn_sync (NULL,
                     umount_argv,
                     NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                     NULL, NULL, NULL,
                     &umount_stderr, &umount_status, &error))
    {
      g_printerr ("Spawning fusermount failed: %s\n", error->message);
      exit (EXIT_FAILURE);
    }
  if (!g_spawn_check_exit_status (umount_status, &error))
    {
      g_printerr ("Failed to unmount target: %s", error->message);
      g_printerr ("%s", umount_stderr);
      exit (EXIT_FAILURE);
    }
}
