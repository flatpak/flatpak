#include "config.h"

#define FUSE_USE_VERSION 26

#include <glib-unix.h>

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <assert.h>
#include <glib/gprintf.h>
#include <gio/gio.h>
#include <pthread.h>
#include <sys/statfs.h>

#include "flatpak-portal-error.h"
#include "xdp-fuse.h"
#include "xdp-util.h"
#include "flatpak-utils.h"

#define NON_DOC_DIR_PERMS 0500
#define DOC_DIR_PERMS 0700

/* TODO: What do we put here */
#define ATTR_CACHE_TIME 60.0
#define ENTRY_CACHE_TIME 60.0

/* We pretend that the file is hardlinked. This causes most apps to do
   a truncating overwrite, which suits us better, as we do the atomic
   rename ourselves anyway. */
#define DOC_FILE_NLINK 2

typedef enum {
  XDP_INODE_ROOT,
  XDP_INODE_BY_APP,
  XDP_INODE_APP_DIR, /* app id */
  XDP_INODE_APP_DOC_DIR, /* app_id + doc id */
  XDP_INODE_DOC_DIR, /* doc id */
  XDP_INODE_DOC_FILE, /* doc id (NULL if tmp), name (== basename) */
} XdpInodeType;

typedef struct _XdpInode XdpInode;

struct _XdpInode
{
  gint ref_count; /* atomic */

  /* These are all immutable */
  fuse_ino_t   ino;
  XdpInodeType type;
  XdpInode    *parent;
  char        *app_id;
  char        *doc_id;

  /* For doc dirs */
  char *basename;
  char *dirname;
  dev_t dir_dev;
  ino_t dir_ino;

  /* mutable data */

  GList   *children; /* lazily filled, protected by inodes lock */
  char    *filename; /* variable (for non-dirs), null if deleted,
                        protected by inodes lock *and* mutex */
  gboolean is_doc; /* True if this is the document file for this dir */

  /* Used when the file is open, protected by mutex */
  GMutex   mutex; /* Always lock inodes lock (if needed) before mutex */
  GList   *open_files;
  int      dir_fd;
  int      fd; /* RW fd for tempfiles, RO fd for doc files */
  char    *backing_filename;
  char    *trunc_filename;
  int      trunc_fd;
  gboolean truncated;
};

typedef struct _XdpFile XdpFile;

struct _XdpFile
{
  XdpInode *inode;
  int       open_mode;
};

#define ROOT_INODE 1
#define BY_APP_INODE 2
#define BY_APP_NAME "by-app"

static GHashTable *dir_to_inode_nr;

static GHashTable *inodes; /* The in memory XdpInode:s, protected by inodes lock */
static XdpInode *root_inode;
static XdpInode *by_app_inode;
static fuse_ino_t next_inode_nr = 3;

G_LOCK_DEFINE (inodes);

static GThread *fuse_thread = NULL;
static struct fuse_session *session = NULL;
static struct fuse_chan *main_ch = NULL;
static char *mount_path = NULL;
static pthread_t fuse_pthread = 0;

static int
reopen_fd (int fd, int flags)
{
  g_autofree char *path = g_strdup_printf ("/proc/self/fd/%d", fd);

  return open (path, flags | O_CLOEXEC);
}

/* Call with inodes lock held */
static fuse_ino_t
allocate_inode_unlocked (void)
{
  fuse_ino_t next = next_inode_nr++;

  /* Bail out on overflow, to avoid reuse */
  if (next <= 0)
    g_assert_not_reached ();

  return next;
}

static fuse_ino_t
get_dir_inode_nr_unlocked (const char *app_id, const char *doc_id)
{
  gpointer res;
  fuse_ino_t allocated;
  g_autofree char *dir = NULL;

  if (app_id == NULL)
    {
      dir = g_strdup (doc_id);
    }
  else
    {
      if (doc_id == NULL)
        dir = g_strconcat (app_id, "/", NULL);
      else
        dir = g_build_filename (app_id, doc_id, NULL);
    }

  res = g_hash_table_lookup (dir_to_inode_nr, dir);
  if (res != NULL)
    return (fuse_ino_t) (gsize) res;

  allocated = allocate_inode_unlocked ();
  g_hash_table_insert (dir_to_inode_nr, g_strdup (dir), (gpointer) allocated);
  return allocated;
}

static fuse_ino_t
get_dir_inode_nr (const char *app_id, const char *doc_id)
{
  AUTOLOCK (inodes);
  return get_dir_inode_nr_unlocked (app_id, doc_id);
}

static void
allocate_app_dir_inode_nr (char **app_ids)
{
  int i;

  AUTOLOCK (inodes);
  for (i = 0; app_ids[i] != NULL; i++)
    get_dir_inode_nr_unlocked (app_ids[i], NULL);
}

static char **
get_allocated_app_dirs (void)
{
  GHashTableIter iter;
  gpointer key, value;
  GPtrArray *array = g_ptr_array_new ();

  AUTOLOCK (inodes);
  g_hash_table_iter_init (&iter, dir_to_inode_nr);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key;

      if (g_str_has_suffix (name, "/"))
        {
          char *app = strndup (name, strlen (name) - 1);
          g_ptr_array_add (array, app);
        }
    }
  g_ptr_array_add (array, NULL);
  return (char **) g_ptr_array_free (array, FALSE);
}

static void xdp_inode_unref_internal (XdpInode *inode,
                                      gboolean  locked);
static void xdp_inode_unref (XdpInode *inode);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpInode, xdp_inode_unref)

static void
xdp_inode_destroy (XdpInode *inode, gboolean locked)
{
  g_assert (inode->dir_fd == -1);
  g_assert (inode->fd == -1);
  g_assert (inode->trunc_fd == -1);
  g_assert (inode->trunc_filename == NULL);
  g_assert (inode->children == NULL);
  xdp_inode_unref_internal (inode->parent, locked);
  g_free (inode->backing_filename);
  g_free (inode->filename);
  g_free (inode->dirname);
  g_free (inode->app_id);
  g_free (inode->doc_id);
  g_free (inode);
}

static XdpInode *
xdp_inode_ref (XdpInode *inode)
{
  if (inode)
    g_atomic_int_inc (&inode->ref_count);
  return inode;
}

static void
xdp_inode_unref_internal (XdpInode *inode, gboolean locked)
{
  gint old_ref;

  if (inode == NULL)
    return;

  /* here we want to atomically do: if (ref_count>1) { ref_count--; return; } */
retry_atomic_decrement1:
  old_ref = g_atomic_int_get (&inode->ref_count);
  if (old_ref > 1)
    {
      if (!g_atomic_int_compare_and_exchange ((int *) &inode->ref_count, old_ref, old_ref - 1))
        goto retry_atomic_decrement1;
    }
  else
    {
      if (old_ref <= 0)
        {
          g_warning ("Can't unref dead inode");
          return;
        }
      /* Protect against revival from xdp_inode_lookup() */
      if (!locked)
        G_LOCK (inodes);
      if (!g_atomic_int_compare_and_exchange ((int *) &inode->ref_count, old_ref, old_ref - 1))
        {
          if (!locked)
            G_UNLOCK (inodes);
          goto retry_atomic_decrement1;
        }

      g_hash_table_remove (inodes, (gpointer) inode->ino);
      if (inode->parent)
        inode->parent->children = g_list_remove (inode->parent->children, inode);

      if (!locked)
        G_UNLOCK (inodes);

      xdp_inode_destroy (inode, locked);
    }
}

static void
xdp_inode_unref (XdpInode *inode)
{
  return xdp_inode_unref_internal (inode, FALSE);
}

static XdpInode *
xdp_inode_new_unlocked (fuse_ino_t   ino,
                        XdpInodeType type,
                        XdpInode    *parent,
                        const char  *filename,
                        const char  *app_id,
                        const char  *doc_id)
{
  XdpInode *inode;

  inode = g_new0 (XdpInode, 1);
  inode->ino = ino;
  inode->type = type;
  inode->parent = xdp_inode_ref (parent);
  inode->filename = g_strdup (filename);
  inode->app_id = g_strdup (app_id);
  inode->doc_id = g_strdup (doc_id);
  inode->ref_count = 1;
  inode->dir_fd = -1;
  inode->fd = -1;
  inode->trunc_fd = -1;

  if (parent)
    parent->children = g_list_prepend (parent->children, inode);
  g_hash_table_insert (inodes, (gpointer) ino, inode);

  return inode;
}

static XdpInode *
xdp_inode_new (fuse_ino_t   ino,
               XdpInodeType type,
               XdpInode    *parent,
               const char  *filename,
               const char  *app_id,
               const char  *doc_id)
{
  AUTOLOCK (inodes);
  return xdp_inode_new_unlocked (ino, type, parent, filename, app_id, doc_id);
}

static XdpInode *
xdp_inode_lookup_unlocked (fuse_ino_t inode_nr)
{
  XdpInode *inode;

  inode = g_hash_table_lookup (inodes, (gpointer) inode_nr);
  if (inode != NULL)
    return xdp_inode_ref (inode);
  return NULL;
}

static GList *
xdp_inode_list_children (XdpInode *inode)
{
  GList *list = NULL, *l;

  AUTOLOCK (inodes);
  for (l = inode->children; l != NULL; l = l->next)
    {
      XdpInode *child = l->data;

      list = g_list_prepend (list, xdp_inode_ref (child));
    }

  return g_list_reverse (list);
}

static XdpInode *
xdp_inode_lookup_child_unlocked (XdpInode *inode, const char *filename)
{
  GList *l;

  for (l = inode->children; l != NULL; l = l->next)
    {
      XdpInode *child = l->data;
      if (child->filename != NULL && strcmp (child->filename, filename) == 0)
        return xdp_inode_ref (child);
    }

  return NULL;
}

static XdpInode *
xdp_inode_lookup_child (XdpInode *inode, const char *filename)
{
  AUTOLOCK (inodes);
  return xdp_inode_lookup_child_unlocked (inode, filename);
}

static int
xdp_inode_open_dir_fd (XdpInode *dir)
{
  struct stat st_buf;
  glnx_autofd int fd = -1;

  g_assert (dir->dirname != NULL);

  fd = open (dir->dirname, O_CLOEXEC | O_PATH | O_DIRECTORY);
  if (fd == -1)
    return -1;

  if (fstat (fd, &st_buf) < 0)
    {
      errno = ENOENT;
      return -1;
    }

  if (st_buf.st_ino != dir->dir_ino ||
      st_buf.st_dev != dir->dir_dev)
    {
      errno = ENOENT;
      return -1;
    }

  return glnx_steal_fd (&fd);
}

static void
xdp_inode_unlink_backing_files (XdpInode *child_inode, int dir_fd)
{
  if (dir_fd == -1)
    {
      g_debug ("Can't unlink child inode due to no dir_fd");
      return;
    }

  if (child_inode->is_doc)
    {
      g_debug ("unlinking doc file %s", child_inode->filename);
      unlinkat (dir_fd, child_inode->filename, 0);
      if (child_inode->trunc_filename != NULL)
        {
          g_debug ("unlinking doc trunc_file %s", child_inode->trunc_filename);
          unlinkat (dir_fd, child_inode->trunc_filename, 0);
        }
    }
  else
    {
      g_debug ("unlinking tmp_file %s", child_inode->backing_filename);
      unlinkat (dir_fd, child_inode->backing_filename, 0);
    }
}

static void
xdp_inode_do_unlink (XdpInode *child_inode, int dir_fd, gboolean unlink_backing)
{
  if (unlink_backing)
    xdp_inode_unlink_backing_files (child_inode, dir_fd);

  /* Zero out filename to mark it deleted */
  g_free (child_inode->filename);
  child_inode->filename = NULL;

  /* Drop keep-alive-until-unlink ref */
  if (!child_inode->is_doc)
    xdp_inode_unref (child_inode);
}

static XdpInode *
xdp_inode_unlink_child (XdpInode *dir, const char *filename)
{
  XdpInode *child_inode;
  glnx_autofd int dir_fd = -1;

  AUTOLOCK (inodes);
  child_inode = xdp_inode_lookup_child_unlocked (dir, filename);
  if (child_inode == NULL)
    return NULL;

  g_assert (child_inode->type == XDP_INODE_DOC_FILE);
  g_assert (child_inode->filename != NULL);

  /* Here we take *both* the inodes lock and the mutex.
     The inodes lock is to make this safe against concurrent lookups,
     but the mutex is to make it safe to access inode->filename inside
     a mutex-only lock */
  g_mutex_lock (&child_inode->mutex);

  dir_fd = xdp_inode_open_dir_fd (dir);

  xdp_inode_do_unlink (child_inode, dir_fd, TRUE);

  g_mutex_unlock (&child_inode->mutex);

  return child_inode;
}

/* Sets errno */
static int
xdp_inode_rename_child (XdpInode   *dir,
                        const char *src_filename,
                        const char *dst_filename)
{
  g_autoptr(XdpInode) src_inode = NULL;
  g_autoptr(XdpInode) dst_inode = NULL;
  glnx_autofd int dir_fd = -1;
  int res;

  AUTOLOCK (inodes);
  src_inode = xdp_inode_lookup_child_unlocked (dir, src_filename);
  if (src_inode == NULL)
    {
      errno = ENOENT;
      return -1;
    }

  g_assert (src_inode->type == XDP_INODE_DOC_FILE);
  g_assert (src_inode->filename != NULL);

  dst_inode = xdp_inode_lookup_child_unlocked (dir, dst_filename);
  if (dst_inode)
    {
      g_assert (dst_inode->type == XDP_INODE_DOC_FILE);
      g_assert (dst_inode->filename != NULL);
    }

  /* Here we take *both* the inodes lock and the mutex.
     The inodes lock is to make this safe against concurrent lookups,
     but the mutex is to make it safe to access inode->filename inside
     a mutex-only lock */
  g_mutex_lock (&src_inode->mutex);
  if (dst_inode)
    g_mutex_lock (&dst_inode->mutex);

  dir_fd = xdp_inode_open_dir_fd (dir);
  res = 0;

  if (src_inode->is_doc)
    {
      /* doc -> tmp */

      /* We don't want to allow renaming an exiting doc file, because
         doing so would make a tmpfile of the real doc-file which some
         host-side app may have open. You have to make a copy and
         remove instead. */
      errno = EACCES;
      res = -1;
    }
  else if (strcmp (dst_filename, dir->basename) != 0)
    {
      /* tmp -> tmp */

      if (dst_inode)
        xdp_inode_do_unlink (dst_inode, dir_fd, TRUE);

      g_free (src_inode->filename);
      src_inode->filename = g_strdup (dst_filename);
    }
  else
    {
      /* tmp -> doc */

      g_debug ("atomic renaming %s to %s", src_inode->backing_filename, dst_filename);
      res = renameat (dir_fd, src_inode->backing_filename,
                      dir_fd, dst_filename);
      if (res == 0)
        {
          if (dst_inode != NULL)
            {
              /* Unlink, but don't remove backing files, which are now the new one */
              xdp_inode_do_unlink (dst_inode, dir_fd, FALSE);

              /* However, unlink trunc_file if its there */
              if (dst_inode->trunc_filename)
                unlinkat (dir_fd, dst_inode->trunc_filename, 0);
            }

          src_inode->is_doc = TRUE;
          g_free (src_inode->filename);
          src_inode->filename = g_strdup (dst_filename);
          g_free (src_inode->backing_filename);
          src_inode->backing_filename = g_strdup (dst_filename);

          /* Convert ->fd to read-only */
          if (src_inode->fd != -1)
            {
              int new_fd = reopen_fd (src_inode->fd, O_RDONLY);
              close (src_inode->fd);
              src_inode->fd = new_fd;
            }

          /* This neuters any outstanding write files, since we have no trunc_fd at this point.
             However, that is not really a problem, we would not support them well anyway as
             a newly opened trunc file would have to have a truncate operation initially for
             it to work anyway */
        }
    }

  g_mutex_unlock (&src_inode->mutex);
  if (dst_inode)
    g_mutex_unlock (&dst_inode->mutex);

  return res;
}

/* NULL if removed */
static char *
xdp_inode_get_filename (XdpInode *inode)
{
  AUTOLOCK (inodes);
  return g_strdup (inode->filename);
}

static XdpInode *
xdp_inode_ensure_document_file (XdpInode *dir)
{
  XdpInode *inode;

  g_assert (dir->type == XDP_INODE_APP_DOC_DIR || dir->type == XDP_INODE_DOC_DIR);

  AUTOLOCK (inodes);

  inode = xdp_inode_lookup_child_unlocked (dir, dir->basename);
  if (inode == NULL)
    {
      inode = xdp_inode_new_unlocked (allocate_inode_unlocked (),
                                      XDP_INODE_DOC_FILE,
                                      dir,
                                      dir->basename,
                                      dir->app_id,
                                      dir->doc_id);
      inode->backing_filename = g_strdup (dir->basename);
      inode->is_doc = TRUE;
    }

  return inode;
}

static char *
create_tmp_for_doc (XdpInode *dir, int dir_fd, int flags, mode_t mode, int *fd_out)
{
  g_autofree char *template = g_strconcat (".xdp_", dir->basename, ".XXXXXX", NULL);
  int fd;

  fd = flatpak_mkstempat (dir_fd, template, flags | O_CLOEXEC, mode);
  if (fd == -1)
    return NULL;

  g_debug ("Created temp file %s", template);
  *fd_out = fd;
  return g_steal_pointer (&template);
}

/* sets errno */
static XdpInode *
xdp_inode_create_file (XdpInode   *dir,
                       const char *filename,
                       mode_t      mode,
                       gboolean    truncate,
                       gboolean    exclusive)
{
  XdpInode *inode;
  g_autofree char *backing_filename = NULL;
  g_autofree char *trunc_filename = NULL;
  gboolean is_doc;
  glnx_autofd int dir_fd = -1;
  glnx_autofd int fd = -1;
  glnx_autofd int trunc_fd = -1;

  g_assert (dir->type == XDP_INODE_APP_DOC_DIR || dir->type == XDP_INODE_DOC_DIR);

  AUTOLOCK (inodes);

  inode = xdp_inode_lookup_child_unlocked (dir, filename);
  if (inode != NULL)
    {
      if (exclusive)
        {
          xdp_inode_unref (inode);
          errno = EEXIST;
          return NULL;
        }

      if (truncate)
        {
          /* TODO: Handle extra truncate for existing file */
          errno = ENOSYS;
          return NULL;
        }

      return inode;
    }

  dir_fd = xdp_inode_open_dir_fd (dir);
  if (dir_fd == -1)
    return NULL;

  is_doc = strcmp (dir->basename, filename) == 0;

  if (is_doc)
    {
      backing_filename = g_strdup (filename);
      int flags = O_CREAT | O_RDONLY | O_NOFOLLOW | O_CLOEXEC;

      if (exclusive)
        flags |= O_EXCL;

      g_debug ("Creating doc file %s", dir->basename);
      fd = openat (dir_fd, dir->basename, flags, mode & 0777);
      if (fd < 0)
        return NULL;

      trunc_filename = create_tmp_for_doc (dir, dir_fd, O_RDWR, mode & 0777, &trunc_fd);
      if (trunc_filename == NULL)
        return NULL;
    }
  else
    {
      backing_filename = create_tmp_for_doc (dir, dir_fd, O_RDWR, mode & 0777, &fd);
      if (backing_filename == NULL)
        return NULL;
    }

  inode = xdp_inode_new_unlocked (allocate_inode_unlocked (),
                                  XDP_INODE_DOC_FILE,
                                  dir,
                                  filename,
                                  dir->app_id,
                                  dir->doc_id);
  inode->backing_filename = g_steal_pointer (&backing_filename);
  inode->trunc_filename = g_steal_pointer (&trunc_filename);
  inode->is_doc = is_doc;
  inode->dir_fd = glnx_steal_fd (&dir_fd);
  inode->fd = glnx_steal_fd (&fd);
  inode->trunc_fd = glnx_steal_fd (&trunc_fd);
  if (inode->trunc_fd != -1 && (truncate || exclusive))
    {
      inode->truncated = TRUE;
      g_free (inode->backing_filename);
      inode->backing_filename = g_strdup (inode->trunc_filename);
    }

  /* We add an extra ref for tmp files to keep them alive until unlink */
  if (!is_doc)
    xdp_inode_ref (inode);

  return inode;
}

static XdpInode *
xdp_inode_lookup (fuse_ino_t inode_nr)
{
  AUTOLOCK (inodes);
  return xdp_inode_lookup_unlocked (inode_nr);
}

static XdpInode *
xdp_inode_get_dir_unlocked (const char *app_id, const char *doc_id, FlatpakDbEntry *entry)
{
  fuse_ino_t ino;
  XdpInode *inode;
  XdpInode *parent = NULL;
  XdpInodeType type;
  const char *filename;

  ino = get_dir_inode_nr_unlocked (app_id, doc_id);

  inode = xdp_inode_lookup_unlocked (ino);
  if (inode)
    return inode;

  if (app_id == NULL)
    {
      g_assert (doc_id != NULL);
      parent = xdp_inode_ref (root_inode);
      type = XDP_INODE_DOC_DIR;
      filename = doc_id;
    }
  else
    {
      if  (doc_id == NULL)
        {
          parent = xdp_inode_ref (by_app_inode);
          filename = app_id;
          type = XDP_INODE_APP_DIR;
        }
      else
        {
          parent = xdp_inode_get_dir_unlocked (app_id, NULL, NULL);
          filename = doc_id;
          type = XDP_INODE_APP_DOC_DIR;
        }
    }

  inode = xdp_inode_new_unlocked (ino, type, parent, filename, app_id, doc_id);
  xdp_inode_unref_internal (parent, TRUE);

  if (entry)
    {
      inode->basename = xdp_entry_dup_basename (entry);
      inode->dirname = xdp_entry_dup_dirname (entry);
      inode->dir_ino = xdp_entry_get_inode (entry);
      inode->dir_dev = xdp_entry_get_device (entry);
    }

  return inode;
}

static XdpInode *
xdp_inode_get_dir (const char *app_id, const char *doc_id, FlatpakDbEntry *entry)
{
  AUTOLOCK (inodes);
  return xdp_inode_get_dir_unlocked (app_id, doc_id, entry);
}

/********************************************************************** \
* FUSE Implementation
\***********************************************************************/

static int
get_user_perms (const struct stat *stbuf)
{
  /* Strip out exec and setuid bits */
  return stbuf->st_mode & 0666;
}

static gboolean
app_can_write_doc (FlatpakDbEntry *entry, const char *app_id)
{
  if (app_id == NULL)
    return TRUE;

  if (xdp_entry_has_permissions (entry, app_id, XDP_PERMISSION_FLAGS_WRITE))
    return TRUE;

  return FALSE;
}

static gboolean
app_can_see_doc (FlatpakDbEntry *entry, const char *app_id)
{
  if (app_id == NULL)
    return TRUE;

  if (xdp_entry_has_permissions (entry, app_id, XDP_PERMISSION_FLAGS_READ))
    return TRUE;

  return FALSE;
}

/* Call with mutex held! */
static int
xdp_inode_locked_get_fd (XdpInode *inode)
{
  if (inode->truncated)
    return inode->trunc_fd;

  return inode->fd;
}

/* Call with mutex held! */
static int
xdp_inode_locked_get_write_fd (XdpInode *inode)
{
  if (inode->is_doc)
    {
      if (!inode->truncated)
        {
          errno = ENOSYS;
          return -1;
        }
      return inode->trunc_fd;
    }

  return inode->fd;
}

static int
xdp_inode_stat (XdpInode    *inode,
                struct stat *stbuf)
{
  stbuf->st_ino = inode->ino;
  stbuf->st_uid = getuid ();
  stbuf->st_gid = getgid ();

  switch (inode->type)
    {
    case XDP_INODE_ROOT:
    case XDP_INODE_BY_APP:
    case XDP_INODE_APP_DIR:
      stbuf->st_mode = S_IFDIR | NON_DOC_DIR_PERMS;
      stbuf->st_nlink = 2;
      break;

    case XDP_INODE_DOC_DIR:
    case XDP_INODE_APP_DOC_DIR:
      stbuf->st_mode = S_IFDIR | DOC_DIR_PERMS;
      stbuf->st_nlink = 2;
      break;

    case XDP_INODE_DOC_FILE:
      {
        g_autoptr(FlatpakDbEntry) entry = NULL;
        struct stat tmp_stbuf;
        gboolean can_see, can_write;
        int fd, res, errsv;

        entry = xdp_lookup_doc (inode->doc_id);
        if (entry == NULL)
          {
            errno = ENOENT;
            return -1;
          }

        can_see = app_can_see_doc (entry, inode->app_id);
        can_write = app_can_write_doc (entry, inode->app_id);

        if (!can_see)
          {
            errno = ENOENT;
            return -1;
          }

        g_mutex_lock (&inode->mutex);

        fd = xdp_inode_locked_get_fd (inode);
        if (fd != -1)
          {
            res = fstat (fd, &tmp_stbuf);
          }
        else
          {
            glnx_autofd int dir_fd = xdp_inode_open_dir_fd (inode->parent);

            if (dir_fd == -1)
              res = -1;
            else
              res = fstatat (dir_fd, inode->backing_filename,
                             &tmp_stbuf, AT_SYMLINK_NOFOLLOW);
          }
        errsv = errno;

        g_mutex_unlock (&inode->mutex);

        if (res != 0)
          {
            errno = errsv;
            return -1;
          }

        stbuf->st_mode = S_IFREG | get_user_perms (&tmp_stbuf);
        if (!can_write)
          stbuf->st_mode &= ~(0222);
        stbuf->st_size = tmp_stbuf.st_size;
        stbuf->st_uid = tmp_stbuf.st_uid;
        stbuf->st_gid = tmp_stbuf.st_gid;
        stbuf->st_blksize = tmp_stbuf.st_blksize;
        stbuf->st_blocks = tmp_stbuf.st_blocks;
        stbuf->st_atim = tmp_stbuf.st_atim;
        stbuf->st_mtim = tmp_stbuf.st_mtim;
        stbuf->st_ctim = tmp_stbuf.st_ctim;
      }
      break;

    default:
      g_assert_not_reached ();
    }

  return 0;
}

static void
xdp_fuse_lookup (fuse_req_t  req,
                 fuse_ino_t  parent,
                 const char *name)
{
  g_autoptr(XdpInode) parent_inode = NULL;
  struct fuse_entry_param e = {0};
  g_autoptr(XdpInode) child_inode = NULL;
  g_autoptr(FlatpakDbEntry) entry = NULL;

  g_debug ("xdp_fuse_lookup %lx/%s -> ", parent, name);

  parent_inode = xdp_inode_lookup (parent);
  if (parent_inode == NULL)
    {
      g_debug ("xdp_fuse_lookup <- error parent ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  /* Default */
  e.attr_timeout = ATTR_CACHE_TIME;
  e.entry_timeout = ENTRY_CACHE_TIME;

  switch (parent_inode->type)
    {
    case XDP_INODE_ROOT:
      if (strcmp (name, BY_APP_NAME) == 0)
        {
          child_inode = xdp_inode_ref (by_app_inode);
        }
      else
        {
          entry = xdp_lookup_doc (name);
          if (entry != NULL)
            child_inode = xdp_inode_get_dir (NULL, name, entry);
        }
      break;

    case XDP_INODE_BY_APP:
      /* This lazily creates the app dir */
      if (flatpak_is_valid_name (name, NULL))
        child_inode = xdp_inode_get_dir (name, NULL, NULL);
      break;

    case XDP_INODE_APP_DIR:
      entry = xdp_lookup_doc (name);
      if (entry != NULL &&
          app_can_see_doc (entry, parent_inode->app_id))
        child_inode = xdp_inode_get_dir (parent_inode->app_id, name, entry);
      break;

    case XDP_INODE_APP_DOC_DIR:
    case XDP_INODE_DOC_DIR:
      {
        g_autoptr(XdpInode) doc_inode = NULL;
        entry = xdp_lookup_doc (parent_inode->doc_id);
        if (entry == NULL)
          {
            g_debug ("xdp_fuse_lookup <- error no parent entry ENOENT");
            fuse_reply_err (req, ENOENT);
            return;
          }

        /* Ensure it is alive at least during lookup_child () */
        doc_inode = xdp_inode_ensure_document_file (parent_inode);

        child_inode = xdp_inode_lookup_child (parent_inode, name);

        /* We verify in the stat below if the backing file exists */

        /* Files can be changed from outside the fuse fs, so don't cache any data */
        e.attr_timeout = 0;
        e.entry_timeout = 0;
      }
      break;

    case XDP_INODE_DOC_FILE:
      fuse_reply_err (req, ENOTDIR);
      return;

    default:
      break;
    }

  if (child_inode == NULL)
    {
      g_debug ("xdp_fuse_lookup <- error child ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (xdp_inode_stat (child_inode, &e.attr) != 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  e.ino = child_inode->ino;

  g_debug ("xdp_fuse_lookup <- inode %lx", (long) e.ino);
  xdp_inode_ref (child_inode); /* Ref given to the kernel, returned in xdp_fuse_forget() */
  fuse_reply_entry (req, &e);
}

static void
xdp_fuse_forget (fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
  g_autoptr(XdpInode) inode = NULL;
  g_debug ("xdp_fuse_forget %lx %ld -> ", ino, nlookup);

  inode = xdp_inode_lookup (ino);
  if (inode == NULL)
    {
      g_warning ("xdp_fuse_forget, unknown inode");
    }
  else
    {
      while (nlookup > 0)
        {
          xdp_inode_unref (inode);
          nlookup--;
        }
    }

  fuse_reply_none (req);
}

struct dirbuf
{
  char  *p;
  size_t size;
};

static void
dirbuf_add (fuse_req_t     req,
            struct dirbuf *b,
            const char    *name,
            fuse_ino_t     ino,
            mode_t         mode)
{
  struct stat stbuf;

  size_t oldsize = b->size;

  b->size += fuse_add_direntry (req, NULL, 0, name, NULL, 0);
  b->p = (char *) g_realloc (b->p, b->size);
  memset (&stbuf, 0, sizeof (stbuf));
  stbuf.st_ino = ino;
  stbuf.st_mode = mode;
  fuse_add_direntry (req, b->p + oldsize,
                     b->size - oldsize,
                     name, &stbuf,
                     b->size);
}

static void
dirbuf_add_docs (fuse_req_t     req,
                 struct dirbuf *b,
                 const char    *app_id)
{
  g_auto(GStrv) docs = NULL;
  fuse_ino_t ino;
  int i;

  docs = xdp_list_docs ();
  for (i = 0; docs[i] != NULL; i++)
    {
      if (app_id)
        {
          g_autoptr(FlatpakDbEntry) entry = xdp_lookup_doc (docs[i]);
          if (entry == NULL ||
              !app_can_see_doc (entry, app_id))
            continue;
        }
      ino = get_dir_inode_nr (app_id, docs[i]);
      dirbuf_add (req, b, docs[i], ino, S_IFDIR);
    }
}

static int
reply_buf_limited (fuse_req_t  req,
                   const char *buf,
                   size_t      bufsize,
                   off_t       off,
                   size_t      maxsize)
{
  if (off < bufsize)
    return fuse_reply_buf (req, buf + off,
                           MIN (bufsize - off, maxsize));
  else
    return fuse_reply_buf (req, NULL, 0);
}

static void
xdp_fuse_readdir (fuse_req_t req, fuse_ino_t ino, size_t size,
                  off_t off, struct fuse_file_info *fi)
{
  struct dirbuf *b = (struct dirbuf *) (gsize) (fi->fh);

  reply_buf_limited (req, b->p, b->size, off, size);
}

static void
xdp_fuse_opendir (fuse_req_t             req,
                  fuse_ino_t             ino,
                  struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = NULL;
  struct dirbuf b = {0};

  g_debug ("xdp_fuse_opendir %lx", ino);

  inode = xdp_inode_lookup (ino);
  if (inode == NULL)
    {
      g_debug ("xdp_fuse_opendir <- error ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  switch (inode->type)
    {
    case XDP_INODE_ROOT:
      dirbuf_add (req, &b, ".", ROOT_INODE, S_IFDIR);
      dirbuf_add (req, &b, "..", ROOT_INODE, S_IFDIR);
      dirbuf_add (req, &b, BY_APP_NAME, BY_APP_INODE, S_IFDIR);
      dirbuf_add_docs (req, &b, NULL);
      break;

    case XDP_INODE_BY_APP:
      {
        g_auto(GStrv) db_app_ids = NULL;
        g_auto(GStrv) app_ids = NULL;
        int i;

        dirbuf_add (req, &b, ".", BY_APP_INODE, S_IFDIR);
        dirbuf_add (req, &b, "..", ROOT_INODE, S_IFDIR);

        /* Ensure that all apps from db are allocated */
        db_app_ids = xdp_list_apps ();
        allocate_app_dir_inode_nr (db_app_ids);

        /* But return all allocated dirs. We might have app dirs
           that have no permissions, and are thus not in the db */
        app_ids = get_allocated_app_dirs ();
        for (i = 0; app_ids[i] != NULL; i++)
          dirbuf_add (req, &b, app_ids[i],
                      get_dir_inode_nr (app_ids[i], NULL), S_IFDIR);
      }
      break;

    case XDP_INODE_APP_DIR:
      dirbuf_add (req, &b, ".", inode->ino, S_IFDIR);
      dirbuf_add (req, &b, "..", BY_APP_INODE, S_IFDIR);
      dirbuf_add_docs (req, &b, inode->app_id);
      break;

    case XDP_INODE_DOC_FILE:
      fuse_reply_err (req, ENOTDIR);
      break;

    case XDP_INODE_APP_DOC_DIR:
    case XDP_INODE_DOC_DIR:
      {
        GList *children, *l;
        g_autoptr(XdpInode) doc_inode = NULL;
        g_autoptr(FlatpakDbEntry) entry = NULL;

        entry = xdp_lookup_doc (inode->doc_id);
        if (entry == NULL)
          {
            fuse_reply_err (req, ENOENT);
            break;
          }

        dirbuf_add (req, &b, ".", inode->ino, S_IFDIR);
        dirbuf_add (req, &b, "..", inode->parent->ino, S_IFDIR);

        /* Ensure it is alive at least during list_children () */
        doc_inode = xdp_inode_ensure_document_file (inode);

        children = xdp_inode_list_children (inode);

        for (l = children; l != NULL; l = l->next)
          {
            struct stat stbuf;
            XdpInode *child = l->data;
            g_autofree char *filename = xdp_inode_get_filename (child);
            if (filename != NULL && xdp_inode_stat (child, &stbuf) == 0)
              dirbuf_add (req, &b, filename, child->ino, stbuf.st_mode);
            xdp_inode_unref (child);
          }
        g_list_free (children);
      }
      break;

    default:
      g_assert_not_reached ();
    }

  if (b.p != NULL)
    {
      fi->fh = (gsize) g_memdup (&b, sizeof (b));
      if (fuse_reply_open (req, fi) == -ENOENT)
        {
          g_free (b.p);
          g_free ((gpointer) (gsize) (fi->fh));
        }
    }
}

static void
xdp_fuse_releasedir (fuse_req_t             req,
                     fuse_ino_t             ino,
                     struct fuse_file_info *fi)
{
  struct dirbuf *b = (struct dirbuf *) (gsize) (fi->fh);

  g_free (b->p);
  g_free (b);
  fuse_reply_err (req, 0);
}



static void
xdp_fuse_getattr (fuse_req_t             req,
                  fuse_ino_t             ino,
                  struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = NULL;
  struct stat stbuf = { 0 };

  g_debug ("xdp_fuse_getattr %lx (fi=%p)", ino, fi);

  inode = xdp_inode_lookup (ino);
  if (inode == NULL)
    {
      g_debug ("xdp_fuse_getattr <- error ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (xdp_inode_stat (inode,  &stbuf) != 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  fuse_reply_attr (req, &stbuf, ATTR_CACHE_TIME);
}

static void
xdp_fuse_fsyncdir (fuse_req_t             req,
                   fuse_ino_t             ino,
                   int                    datasync,
                   struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = NULL;

  g_debug ("xdp_fuse_fsyncdir %lx %p", ino, fi);

  inode = xdp_inode_lookup (ino);
  if (inode == NULL)
    {
      g_debug ("xdp_fuse_fsyncdir <- error ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (inode->type == XDP_INODE_APP_DOC_DIR ||
      inode->type == XDP_INODE_DOC_DIR)
    {
      g_autoptr(FlatpakDbEntry) entry =  xdp_lookup_doc (inode->doc_id);
      if (entry != NULL)
        {
          g_autofree char *dirname = xdp_entry_dup_dirname (entry);
          int fd = open (dirname, O_DIRECTORY | O_RDONLY);
          if (fd >= 0)
            {
              if (datasync)
                fdatasync (fd);
              else
                fsync (fd);
              close (fd);
            }
        }
    }

  fuse_reply_err (req, 0);
}

static XdpFile *
xdp_file_new (XdpInode *inode,
              int       open_mode)
{
  XdpFile *file = g_new (XdpFile, 1);

  file->inode = xdp_inode_ref (inode);
  file->open_mode = open_mode;

  return file;
}

/* Call with mutex held */
static void
xdp_inode_locked_close_unneeded_fds (XdpInode *inode)
{
  gboolean has_open_for_write = FALSE;
  GList *l;

  for (l = inode->open_files; l != NULL; l = l->next)
    {
      XdpFile *file = l->data;

      if (file->open_mode != O_RDONLY)
        {
          has_open_for_write = TRUE;
          break;
        }
    }

  if (!has_open_for_write)
    {
      if (inode->truncated)
        {
          if (inode->open_files != NULL && inode->fd != -1)
            {
              /* We're not going to close the ->fd, so we repoint it to the trunc_fd, but reopened O_RDONLY */
              close (inode->fd);
              inode->fd = reopen_fd (inode->trunc_fd, O_RDONLY);
            }

          if (inode->filename != NULL)
            {
              /* not removed, replace original */
              fsync (inode->trunc_fd);
              g_free (inode->backing_filename);
              inode->backing_filename = g_strdup (inode->filename);
              g_debug ("moving %s to %s", inode->trunc_filename, inode->backing_filename);
              if (renameat (inode->dir_fd, inode->trunc_filename,
                            inode->dir_fd, inode->backing_filename) != 0)
                g_warning ("Unable to replace truncated document: %s", g_strerror (errno));
            }

          inode->truncated = FALSE;
        }
      else if (inode->trunc_filename != NULL)
        {
          unlinkat (inode->dir_fd, inode->trunc_filename, 0);
          g_debug ("unlinked truc_filename %s", inode->trunc_filename);
        }

      if (inode->trunc_fd != -1)
        {
          close (inode->trunc_fd);
          inode->trunc_fd = -1;
          g_free (inode->trunc_filename);
          inode->trunc_filename = NULL;
        }
    }

  if (inode->open_files == NULL)
    {
      if (inode->fd != -1)
        {
          close (inode->fd);
          inode->fd = -1;
        }

      if (inode->dir_fd != -1)
        {
          close (inode->dir_fd);
          inode->dir_fd = -1;
        }
    }
}

static void
xdp_file_free (XdpFile *file)
{
  XdpInode *inode = file->inode;

  g_mutex_lock (&inode->mutex);
  inode->open_files = g_list_remove (inode->open_files, file);

  xdp_inode_locked_close_unneeded_fds (inode);

  g_mutex_unlock (&inode->mutex);
  xdp_inode_unref (inode);
  g_free (file);
}

/* sets errno */
static int
xdp_inode_locked_ensure_fd_open (XdpInode       *inode,
                                 FlatpakDbEntry *entry,
                                 gboolean        for_write)
{
  /* Ensure all fds are open */
  if (inode->dir_fd == -1)
    {
      inode->dir_fd = xdp_inode_open_dir_fd (inode->parent);
      if (inode->dir_fd == -1)
        return -1;
    }

  if (for_write)
    {
      if (faccessat (inode->dir_fd, inode->backing_filename, W_OK, 0) != 0)
        return -1;
    }

  if (inode->fd == -1)
    {
      int mode = O_NOFOLLOW | O_CLOEXEC;

      if (inode->is_doc)
        mode |= O_RDONLY;
      else
        mode |= O_RDWR;

      inode->fd = openat (inode->dir_fd, inode->backing_filename, mode);
      if (inode->fd < 0)
        return -1;
    }

  if (inode->is_doc && for_write && inode->trunc_fd == -1)
    {
      struct stat st_buf;
      mode_t mode = 0600;

      if (fstat (inode->fd, &st_buf) == 0)
        mode = get_user_perms (&st_buf);

      g_assert (inode->trunc_filename == NULL);
      inode->trunc_filename = create_tmp_for_doc (inode->parent, inode->dir_fd, O_RDWR, mode,
                                                  &inode->trunc_fd);
      if (inode->trunc_filename == NULL)
        return -1;
    }

  return 0;
}

static void
xdp_fuse_open (fuse_req_t             req,
               fuse_ino_t             ino,
               struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = NULL;
  g_autoptr(FlatpakDbEntry) entry = NULL;
  gboolean can_write;
  int open_mode;
  XdpFile *file = NULL;
  int errsv;

  g_debug ("xdp_fuse_open %lx flags %o", ino, fi->flags);

  inode = xdp_inode_lookup (ino);
  if (inode == NULL)
    {
      g_debug ("xdp_fuse_open <- no inode error ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (inode->type != XDP_INODE_DOC_FILE)
    {
      g_debug ("xdp_fuse_open <- error EISDIR");
      fuse_reply_err (req, EISDIR);
      return;
    }

  entry = xdp_lookup_doc (inode->doc_id);
  if (entry == NULL ||
      !app_can_see_doc (entry, inode->app_id))
    {
      g_debug ("xdp_fuse_open <- no entry error ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  can_write = app_can_write_doc (entry, inode->app_id);

  open_mode = fi->flags & 3;

  if (open_mode != O_RDONLY && !can_write)
    {
      g_debug ("xdp_fuse_open <- no write EACCES");
      fuse_reply_err (req, EACCES);
      return;
    }

  g_mutex_lock (&inode->mutex);

  if (xdp_inode_locked_ensure_fd_open (inode, entry,
                                       open_mode != O_RDONLY) == 0)
    {
      file = xdp_file_new (inode, open_mode);
      inode->open_files = g_list_prepend (inode->open_files, file);
      errsv = 0;
    }
  else
    {
      errsv = errno;
      xdp_inode_locked_close_unneeded_fds (inode);
    }

  g_mutex_unlock (&inode->mutex);

  if (file != NULL)
    {
      fi->fh = (gsize) file;
      if (fuse_reply_open (req, fi))
        xdp_file_free (file);
    }
  else
    {
      fuse_reply_err (req, errsv);
    }
}


static void
xdp_fuse_create (fuse_req_t             req,
                 fuse_ino_t             parent,
                 const char            *filename,
                 mode_t                 mode,
                 struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) parent_inode = NULL;
  g_autoptr(FlatpakDbEntry) entry = NULL;
  struct fuse_entry_param e = {0};
  gboolean can_see, can_write;
  int open_mode;
  XdpFile *file = NULL;
  XdpInode *inode;
  int errsv;

  g_debug ("xdp_fuse_create %lx/%s, flags %o", parent, filename, fi->flags);

  parent_inode = xdp_inode_lookup (parent);
  if (parent_inode == NULL)
    {
      g_debug ("xdp_fuse_create <- error parent ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (parent_inode->type == XDP_INODE_DOC_FILE)
    {
      g_debug ("xdp_fuse_create <- error parent ENOTDIR");
      fuse_reply_err (req, ENOTDIR);
      return;
    }

  if (parent_inode->type != XDP_INODE_APP_DOC_DIR &&
      parent_inode->type != XDP_INODE_DOC_DIR)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  entry = xdp_lookup_doc (parent_inode->doc_id);
  if (entry == NULL)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  can_see = app_can_see_doc (entry, parent_inode->app_id);
  if (!can_see)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  can_write = app_can_write_doc (entry, parent_inode->app_id);
  if (!can_write)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  inode = xdp_inode_create_file (parent_inode, filename,
                                 mode,
                                 (fi->flags & O_TRUNC) != 0,
                                 (fi->flags & O_EXCL) != 0);
  if (inode == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  g_mutex_lock (&inode->mutex);

  open_mode = fi->flags & 3;

  if (xdp_inode_locked_ensure_fd_open (inode, entry,
                                       open_mode != O_RDONLY) == 0)
    {
      file = xdp_file_new (inode, open_mode);
      inode->open_files = g_list_prepend (inode->open_files, file);
      errsv = 0;
    }
  else
    {
      errsv = errno;
      xdp_inode_locked_close_unneeded_fds (inode);
    }

  g_mutex_unlock (&inode->mutex);

  if (file != NULL)
    {
      if (xdp_inode_stat (inode, &e.attr) != 0)
        {
          xdp_file_free (file);
          fuse_reply_err (req, errno);
          return;
        }

      e.ino = inode->ino;
      if (inode->is_doc)
        {
          e.attr_timeout = 0;
          e.entry_timeout = 0;
        }
      else
        {
          e.attr_timeout = ATTR_CACHE_TIME;
          e.entry_timeout = ENTRY_CACHE_TIME;
        }

      xdp_inode_ref (inode); /* Ref given to the kernel, returned in xdp_fuse_forget() */

      fi->fh = (gsize) file;
      if (fuse_reply_create (req, &e, fi))
        {
          xdp_file_free (file);
          xdp_inode_unref (inode);
        }
    }
  else
    {
      fuse_reply_err (req, errsv);
    }
}

static void
xdp_fuse_read (fuse_req_t             req,
               fuse_ino_t             ino,
               size_t                 size,
               off_t                  off,
               struct fuse_file_info *fi)
{
  XdpFile *file = (gpointer) (gsize) fi->fh;
  XdpInode *inode = file->inode;
  struct fuse_bufvec bufv = FUSE_BUFVEC_INIT (size);
  int fd;

  g_debug ("xdp_fuse_real %lx %ld %ld", ino, (long) size, (long) off);

  g_mutex_lock (&inode->mutex);

  fd = xdp_inode_locked_get_fd (inode);
  if (fd == -1)
    {
      static char c = 'x';
      bufv.buf[0].flags = 0;
      bufv.buf[0].mem = &c;
      bufv.buf[0].size = 0;

      fuse_reply_data (req, &bufv, FUSE_BUF_NO_SPLICE);
    }
  else
    {
      bufv.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
      bufv.buf[0].fd = fd;
      bufv.buf[0].pos = off;

      fuse_reply_data (req, &bufv, FUSE_BUF_SPLICE_MOVE);
    }

  g_mutex_unlock (&inode->mutex);
}

static void
xdp_fuse_release (fuse_req_t             req,
                  fuse_ino_t             ino,
                  struct fuse_file_info *fi)
{
  XdpFile *file = (gpointer) (gsize) fi->fh;

  g_debug ("xdp_fuse_release %lx (fi=%p)", ino, fi);

  xdp_file_free (file);
  fuse_reply_err (req, 0);
}

static int
truncateat (int dir_fd, const char *filename, int size)
{
  int fd;
  int errsv, res;

  fd = openat (dir_fd, filename, O_RDWR);
  if (fd == -1)
    return -1;

  res = ftruncate (fd, size);
  errsv = errno;

  close (fd);

  errno = errsv;
  return res;
}

static void
xdp_fuse_setattr (fuse_req_t             req,
                  fuse_ino_t             ino,
                  struct stat           *attr,
                  int                    to_set,
                  struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = NULL;
  g_autoptr(FlatpakDbEntry) entry = NULL;
  double attr_cache_time = ATTR_CACHE_TIME;
  struct stat newattr = {0};
  gboolean can_write;
  int res = 0;

  g_debug ("xdp_fuse_setattr %lx %x %p", ino, to_set, fi);

  inode = xdp_inode_lookup (ino);
  if (inode == NULL)
    {
      g_debug ("xdp_fuse_setattr <- error ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (inode->type != XDP_INODE_DOC_FILE)
    {
      g_debug ("xdp_fuse_setattr <- not file ENOSYS");
      fuse_reply_err (req, ENOSYS);
      return;
    }

  entry = xdp_lookup_doc (inode->doc_id);
  if (entry == NULL ||
      !app_can_see_doc (entry, inode->app_id))
    {
      g_debug ("xdp_fuse_setattr <- no entry error ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  can_write = app_can_write_doc (entry, inode->app_id);

  if (to_set == FUSE_SET_ATTR_SIZE)
    {
      g_mutex_lock (&inode->mutex);

      if (!can_write)
        {
          res = EACCES;
        }
      else if (inode->is_doc)
        {
          /* Only allow ftruncate with the file open for write. We could
           * allow a truncate, but it would have to be implemented as
           * an atomic-replace-with-empty-file to not affect other apps
           * having the file open.
           * Also, only support truncate-to-zero on first truncation, to
           * avoid having to copy lots of data from the old file to the
           * trunc_fd.
           */
          if (inode->trunc_fd == -1)
            {
              res = EACCES;
            }
          else if (!inode->truncated && attr->st_size != 0)
            {
              res = ENOSYS;
            }
          else
            {
              if (ftruncate (inode->trunc_fd, attr->st_size) != 0)
                {
                  res = errno;
                }
              else if (!inode->truncated)
                {
                  inode->truncated = TRUE;
                  g_free (inode->backing_filename);
                  inode->backing_filename = g_strdup (inode->trunc_filename);
                }
            }
        }
      else
        {
          if (inode->fd)
            {
              if (ftruncate (inode->fd, attr->st_size) != 0)
                res = errno;
            }
          else
            {
              glnx_autofd int dir_fd = xdp_inode_open_dir_fd (inode->parent);
              if (dir_fd == -1 ||
                  truncateat (dir_fd, inode->backing_filename, attr->st_size) != 0)
                res = errno;
            }
        }
      g_mutex_unlock (&inode->mutex);
    }
  else if (to_set == FUSE_SET_ATTR_MODE)
    {
      if (!can_write)
        {
          res = EACCES;
        }
      else
        {
          int fd = xdp_inode_locked_get_write_fd (inode);
          if (fd == -1 ||
              fchmod (fd, get_user_perms (attr)) != 0)
            res = errno;
        }
    }
  else
    {
      res = ENOSYS;
    }

  if (res != 0)
    {
      fuse_reply_err (req, res);
    }
  else
    {
      if (xdp_inode_stat (inode, &newattr) != 0)
        fuse_reply_err (req, errno);
      else
        fuse_reply_attr (req, &newattr, attr_cache_time);
    }
}

static void
xdp_fuse_write (fuse_req_t             req,
                fuse_ino_t             ino,
                const char            *buf,
                size_t                 size,
                off_t                  off,
                struct fuse_file_info *fi)
{
  XdpFile *file = (gpointer) (gsize) fi->fh;
  XdpInode *inode = file->inode;
  int fd;
  int res;

  g_debug ("xdp_fuse_write %lx %ld %ld", ino, (long) size, (long) off);

  g_mutex_lock (&inode->mutex);

  fd = xdp_inode_locked_get_write_fd (inode);
  if (fd < 0)
    {
      fuse_reply_err (req, errno);
    }
  else
    {
      res = pwrite (fd, buf, size, off);
      if (res < 0)
        fuse_reply_err (req, errno);
      else
        fuse_reply_write (req, res);
    }

  g_mutex_unlock (&inode->mutex);
}

static void
xdp_fuse_write_buf (fuse_req_t             req,
                    fuse_ino_t             ino,
                    struct fuse_bufvec    *bufv,
                    off_t                  off,
                    struct fuse_file_info *fi)
{
  XdpFile *file = (gpointer) (gsize) fi->fh;
  struct fuse_bufvec dst = FUSE_BUFVEC_INIT (fuse_buf_size (bufv));
  XdpInode *inode = file->inode;
  int fd;
  int res;

  g_debug ("xdp_fuse_write_buf %lx %ld", ino, (long) off);

  g_mutex_lock (&inode->mutex);

  fd = xdp_inode_locked_get_write_fd (inode);
  if (fd == -1)
    {
      g_debug ("xdp_fuse_write_buf <- error %s", strerror (errno));
      fuse_reply_err (req, errno);
    }
  else
    {
      dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
      dst.buf[0].fd = fd;
      dst.buf[0].pos = off;

      res = fuse_buf_copy (&dst, bufv, FUSE_BUF_SPLICE_NONBLOCK);
      if (res < 0)
        fuse_reply_err (req, -res);
      else
        fuse_reply_write (req, res);
    }

  g_mutex_unlock (&inode->mutex);
}

static void
xdp_fuse_fsync (fuse_req_t             req,
                fuse_ino_t             ino,
                int                    datasync,
                struct fuse_file_info *fi)
{
  g_autoptr(XdpInode) inode = NULL;
  int fd;
  int res = 0;

  g_debug ("xdp_fuse_fsync %lx", ino);

  inode = xdp_inode_lookup (ino);
  if (inode == NULL)
    {
      g_debug ("xdp_fuse_setattr <- error ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (inode->type == XDP_INODE_DOC_FILE)
    {
      g_mutex_lock (&inode->mutex);

      fd = xdp_inode_locked_get_write_fd (inode);
      if (fd != -1 && fsync (fd) != 0)
        res = errno;

      g_mutex_unlock (&inode->mutex);
    }

  fuse_reply_err (req, res);
}

static void
xdp_fuse_unlink (fuse_req_t  req,
                 fuse_ino_t  parent,
                 const char *filename)
{
  g_autoptr(XdpInode) parent_inode = NULL;
  g_autoptr(XdpInode) child_inode = NULL;

  g_debug ("xdp_fuse_unlink %lx/%s", parent, filename);

  parent_inode = xdp_inode_lookup (parent);
  if (parent_inode == NULL)
    {
      g_debug ("xdp_fuse_lookup <- error parent ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (parent_inode->type == XDP_INODE_DOC_FILE)
    {
      fuse_reply_err (req, ENOTDIR);
      return;
    }

  if (parent_inode->type != XDP_INODE_APP_DOC_DIR &&
      parent_inode->type != XDP_INODE_DOC_DIR)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  child_inode = xdp_inode_unlink_child (parent_inode, filename);
  if (child_inode == NULL)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  fuse_reply_err (req, 0);
}

static void
xdp_fuse_rename (fuse_req_t  req,
                 fuse_ino_t  parent,
                 const char *name,
                 fuse_ino_t  newparent,
                 const char *newname)
{
  g_autoptr(XdpInode) parent_inode = NULL;
  g_autoptr(FlatpakDbEntry) entry = NULL;
  gboolean can_see, can_write;

  g_debug ("xdp_fuse_rename %lx/%s -> %lx/%s", parent, name, newparent, newname);

  parent_inode = xdp_inode_lookup (parent);
  if (parent_inode == NULL)
    {
      g_debug ("xdp_fuse_rename <- error parent ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (parent_inode->type == XDP_INODE_DOC_FILE)
    {
      fuse_reply_err (req, ENOTDIR);
      return;
    }

  if (parent_inode->type != XDP_INODE_APP_DOC_DIR &&
      parent_inode->type != XDP_INODE_DOC_DIR)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  if (newparent != parent)
    {
      g_debug ("xdp_fuse_rename <- error different parents EACCES");
      fuse_reply_err (req, EACCES);
      return;
    }

  if (strcmp (name, newname) == 0)
    {
      fuse_reply_err (req, 0);
      return;
    }

  entry = xdp_lookup_doc (parent_inode->doc_id);
  if (entry == NULL)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  can_see = app_can_see_doc (entry, parent_inode->app_id);
  can_write = app_can_write_doc (entry, parent_inode->app_id);

  if (!can_see)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (!can_write)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  if (xdp_inode_rename_child (parent_inode, name, newname) != 0)
    fuse_reply_err (req, errno);
  else
    fuse_reply_err (req, 0);
}

static void
xdp_fuse_access (fuse_req_t req, fuse_ino_t ino, int mask)
{
  g_autoptr(XdpInode) inode = NULL;
  g_autoptr(FlatpakDbEntry) entry = NULL;

  g_debug ("xdp_fuse_access %lx %d", ino, mask);

  if (mask != F_OK && (mask & ~(R_OK|W_OK|X_OK)) != 0)
    {
      g_debug ("xdp_fuse_access <- error EINVAL");
      fuse_reply_err (req, EINVAL);
      return;
    }

  inode = xdp_inode_lookup (ino);
  if (inode == NULL)
    {
      g_debug ("xdp_fuse_access <- error ENOENT");
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (inode->type != XDP_INODE_DOC_FILE)
    {
      int dir_mask = 0;

      switch (inode->type)
        {
        case XDP_INODE_ROOT:
        case XDP_INODE_BY_APP:
        case XDP_INODE_APP_DIR:
          dir_mask = R_OK | X_OK;
          break;
        case XDP_INODE_APP_DOC_DIR:
        case XDP_INODE_DOC_DIR:
          dir_mask = R_OK | X_OK | W_OK;
          break;

        default:
          g_assert_not_reached ();
        }

      if (mask != F_OK && ((mask & dir_mask) != mask))
        {
          fuse_reply_err (req, EACCES);
          return;
        }
    }
  else /* A file */
    {
      entry = xdp_lookup_doc (inode->doc_id);
      if (entry == NULL ||
          !app_can_see_doc (entry, inode->app_id))
        {
          g_debug ("xdp_fuse_access <- no entry error ENOENT");
          fuse_reply_err (req, ENOENT);
          return;
        }

      if (mask == F_OK)
        {
          if (!app_can_see_doc (entry, inode->app_id))
            {
              fuse_reply_err (req, EACCES);
              return;
            }
        }
      else
        {
          if (((mask & R_OK) && !app_can_see_doc (entry, inode->app_id)) ||
              ((mask & W_OK) && !app_can_write_doc (entry, inode->app_id)) ||
              (mask & X_OK))
            {
              fuse_reply_err (req, EACCES);
              return;
            }
        }
    }

  fuse_reply_err (req, 0);
}

static struct fuse_lowlevel_ops xdp_fuse_oper = {
  .lookup       = xdp_fuse_lookup,
  .forget       = xdp_fuse_forget,
  .getattr      = xdp_fuse_getattr,
  .opendir      = xdp_fuse_opendir,
  .readdir      = xdp_fuse_readdir,
  .releasedir   = xdp_fuse_releasedir,
  .fsyncdir     = xdp_fuse_fsyncdir,
  .open         = xdp_fuse_open,
  .read         = xdp_fuse_read,
  .release      = xdp_fuse_release,
  .setattr      = xdp_fuse_setattr,
  .write        = xdp_fuse_write,
  .write_buf    = xdp_fuse_write_buf,
  .fsync        = xdp_fuse_fsync,
  .create       = xdp_fuse_create,
  .unlink       = xdp_fuse_unlink,
  .rename       = xdp_fuse_rename,
  .access       = xdp_fuse_access,
};

/* Called when a apps permissions to see a document is changed,
   and with null opt_app_id when the doc is created/removed */
void
xdp_fuse_invalidate_doc_app (const char *doc_id,
                             const char *opt_app_id)
{
  g_autoptr(XdpInode) inode = NULL;
  fuse_ino_t ino;
  GList *l;

  /* This can happen if fuse is not initialized yet for the very
     first dbus message that activated the service */
  if (main_ch == NULL)
    return;

  g_debug ("invalidate %s/%s", doc_id, opt_app_id ? opt_app_id : "*");

  AUTOLOCK (inodes);
  ino = get_dir_inode_nr_unlocked (opt_app_id, doc_id);
  inode = xdp_inode_lookup_unlocked (ino);
  if (inode != NULL)
    {
      fuse_lowlevel_notify_inval_inode (main_ch, inode->ino, 0, 0);
      fuse_lowlevel_notify_inval_entry (main_ch, inode->parent->ino,
                                        inode->filename, strlen (inode->filename));

      for (l = inode->children; l != NULL; l = l->next)
        {
          XdpInode *child = l->data;

          fuse_lowlevel_notify_inval_inode (main_ch, child->ino, 0, 0);
          if (child->filename != NULL)
            fuse_lowlevel_notify_inval_entry (main_ch, inode->ino,
                                              child->filename, strlen (child->filename));
        }
    }
}

char *
xdp_fuse_lookup_id_for_inode (ino_t ino)
{
  g_autoptr(XdpInode) inode = NULL;

  inode = xdp_inode_lookup (ino);
  if (inode == NULL)
    return NULL;

  if (inode->type != XDP_INODE_DOC_FILE ||
      !inode->is_doc)
    return NULL;

  return g_strdup (inode->doc_id);
}

const char *
xdp_fuse_get_mountpoint (void)
{
  if (mount_path == NULL)
    mount_path = g_build_filename (g_get_user_runtime_dir (), "doc", NULL);
  return mount_path;
}

void
xdp_fuse_exit (void)
{
  if (session)
    fuse_session_exit (session);

  if (fuse_pthread)
    pthread_kill (fuse_pthread, SIGHUP);

  if (fuse_thread)
    g_thread_join (fuse_thread);
}

static gpointer
xdp_fuse_mainloop (gpointer data)
{
  fuse_pthread = pthread_self ();

  fuse_session_loop_mt (session);

  fuse_session_remove_chan (main_ch);
  fuse_session_destroy (session);
  fuse_unmount (mount_path, main_ch);
  return NULL;
}

gboolean
xdp_fuse_init (GError **error)
{
  char *argv[] = { "xdp-fuse", "-osplice_write,splice_move" };
  struct fuse_args args = FUSE_ARGS_INIT (G_N_ELEMENTS (argv), argv);
  struct stat st;
  struct statfs stfs;
  const char *path;
  int statfs_res;

  inodes =
    g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
  root_inode = xdp_inode_new (ROOT_INODE, XDP_INODE_ROOT, NULL, "/", NULL, NULL);
  by_app_inode = xdp_inode_new (BY_APP_INODE, XDP_INODE_BY_APP, root_inode, BY_APP_NAME, NULL, NULL);
  dir_to_inode_nr =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  path = xdp_fuse_get_mountpoint ();
  if ((stat (path, &st) == -1 && errno == ENOTCONN) ||
      (((statfs_res = statfs (path, &stfs)) == -1 && errno == ENOTCONN) ||
       (statfs_res == 0 && stfs.f_type == 0x65735546 /* fuse */)))
    {
      int count;
      char *umount_argv[] = { "fusermount", "-u", "-z", (char *) path, NULL };

      g_spawn_sync (NULL, umount_argv, NULL, G_SPAWN_SEARCH_PATH,
                    NULL, NULL, NULL, NULL, NULL, NULL);

      g_usleep (10000); /* 10ms */
      count = 0;
      while (stat (path, &st) == -1 && count < 10)
        g_usleep (10000); /* 10ms */
    }

  if (g_mkdir_with_parents (path, 0700))
    {
      g_set_error (error, FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_FAILED,
                   "Unable to create dir %s", path);
      return FALSE;
    }

  main_ch = fuse_mount (path, &args);
  if (main_ch == NULL)
    {
      g_set_error (error, FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_FAILED, "Can't mount fuse fs");
      return FALSE;
    }

  session = fuse_lowlevel_new (&args, &xdp_fuse_oper,
                               sizeof (xdp_fuse_oper), NULL);
  if (session == NULL)
    {
      g_set_error (error, FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_FAILED,
                   "Can't create fuse session");
      return FALSE;
    }
  fuse_session_add_chan (session, main_ch);

  fuse_thread = g_thread_new ("fuse mainloop", xdp_fuse_mainloop, session);

  return TRUE;
}
