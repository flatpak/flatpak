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

#include "xdg-app-error.h"
#include "xdp-fuse.h"
#include "xdp-util.h"
#include "xdg-app-utils.h"

/* Layout:

   "/ (STD_DIRS:1)
    "by-app/" (STD_DIRS:2)
      "org.gnome.gedit/" (APP_DIR:app id)
        "$id/" (APP_DOC_DIR:app_id<<32|doc_id)
          <same as DOC_DIR>
    "$id" (APP_DOC_DIR:(app_id==0)<<32|doc_idid)
      $basename (APP_DOC_FILE:app_id<<32|doc_id) (app id == 0 if not in app dir)
      $tmpfile (TMPFILE:tmp_id)
*/

#define BY_APP_INO 2

#define NON_DOC_DIR_PERMS 0500
#define DOC_DIR_PERMS 0700

/* The (fake) directories don't really change */
#define DIRS_ATTR_CACHE_TIME 60.0

/* We pretend that the file is hardlinked. This causes most apps to do
   a truncating overwrite, which suits us better, as we do the atomic
   rename ourselves anyway. This way we don't weirdly change the inode
   after the rename. */
#define DOC_FILE_NLINK 2

typedef enum {
  STD_DIRS_INO_CLASS,
  TMPFILE_INO_CLASS,
  APP_DIR_INO_CLASS,
  APP_DOC_DIR_INO_CLASS,
  APP_DOC_FILE_INO_CLASS,
} XdpInodeClass;

#define BY_APP_NAME "by-app"

static GHashTable *app_name_to_id;
static GHashTable *app_id_to_name;
static guint32 next_app_id = 1;

G_LOCK_DEFINE(app_id);

static GThread *fuse_thread = NULL;
static struct fuse_session *session = NULL;
static struct fuse_chan *main_ch = NULL;
static char *mount_path = NULL;
static pthread_t fuse_pthread = 0;

static int
steal_fd (int *fdp)
{
  int fd = *fdp;
  *fdp = -1;
  return fd;
}

static int
get_user_perms (const struct stat *stbuf)
{
  /* Strip out exec and setuid bits */
  return stbuf->st_mode & 0666;
}

static double
get_attr_cache_time (int st_mode)
{
  if (S_ISDIR (st_mode))
    return DIRS_ATTR_CACHE_TIME;
  return 0.0;
}

static double
get_entry_cache_time (fuse_ino_t inode)
{
  /* We have to disable entry caches because otherwise we have a race
     on rename. The kernel set the target inode as NOEXIST after a
     rename, which breaks in the tmp over real case due to us reusing
     the old non-temp inode. */
  return 0.0;
}

/******************************* XdpTmp *******************************
 *
 * XdpTmp is a ref-counted object representing a temporary file created
 * on the outer filesystem which is stored next to a real file in the fuse
 * filesystem. Its useful to support write-to-tmp-then-rename-over-target
 * operations.
 *
 * locking:
 *
 * The global list of outstanding Tmp are protected by the tmp_files
 * lock.  Use it when doing lookups by name or id, or when changing
 * the list (add/remove) or name of a tmpfile.
 *
 * Each instance has a mutex that locks access to the backing path,
 * as it can be removed at runtime. Use get/steal_backing_basename() to
 * safely access it.
 *
 ******************************* XdpTmp *******************************/

static volatile gint next_tmp_id = 1;

typedef struct
{
  volatile gint ref_count;

  /* These are immutable, no lock needed */
  guint64 parent_inode;
  guint32 tmp_id;
  XdgAppDbEntry *entry;

  /* Changes always done under tmp_files lock */
  char *name;

  GMutex mutex;

  /* protected by mutex */
  char *backing_basename;
} XdpTmp;

/* Owns a ref to the files */
static GList *tmp_files = NULL;
G_LOCK_DEFINE(tmp_files);

static XdpTmp *
xdp_tmp_ref (XdpTmp *tmp)
{
  g_atomic_int_inc (&tmp->ref_count);
  return tmp;
}

static void
xdp_tmp_unref (XdpTmp *tmp)
{
  if (g_atomic_int_dec_and_test (&tmp->ref_count))
    {
      xdg_app_db_entry_unref (tmp->entry);
      g_free (tmp->name);
      g_free (tmp->backing_basename);
      g_free (tmp);
    }
}

char *
xdp_tmp_get_backing_basename (XdpTmp *tmp)
{
  char *res;
  g_mutex_lock (&tmp->mutex);
  res = g_strdup (tmp->backing_basename);
  g_mutex_unlock (&tmp->mutex);

  return res;
}

char *
xdp_tmp_steal_backing_basename (XdpTmp *tmp)
{
  char *res;
  g_mutex_lock (&tmp->mutex);
  res = tmp->backing_basename;
  tmp->backing_basename = NULL;
  g_mutex_unlock (&tmp->mutex);

  return res;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdpTmp, xdp_tmp_unref)

/* Must first take tmp_files lock */
static XdpTmp *
find_tmp_by_name_nolock (guint64 parent_inode,
                         const char *name)
{
  GList *l;

  for (l = tmp_files; l != NULL; l = l->next)
    {
      XdpTmp *tmp = l->data;
      if (tmp->parent_inode == parent_inode &&
          strcmp (tmp->name, name) == 0)
        return xdp_tmp_ref (tmp);
    }

  return NULL;
}

/* Takes tmp_files lock */
static XdpTmp *
find_tmp_by_name (guint64 parent_inode,
                  const char *name)
{
  AUTOLOCK(tmp_files);
  return find_tmp_by_name_nolock (parent_inode, name);
}

/* Takes tmp_files lock */
static XdpTmp *
find_tmp_by_id (guint32 tmp_id)
{
  GList *l;

  AUTOLOCK(tmp_files);

  for (l = tmp_files; l != NULL; l = l->next)
    {
      XdpTmp *tmp = l->data;
      if (tmp->tmp_id == tmp_id)
        return xdp_tmp_ref (tmp);
    }

  return NULL;
}

/* Caller must hold tmp_files lock */
static XdpTmp *
xdp_tmp_new_nolock (fuse_ino_t parent,
                    XdgAppDbEntry *entry,
                    const char *name,
                    const char *tmp_basename)
{
  XdpTmp *tmp;
  g_autofree char *tmp_dirname = NULL;

  /* We store the pathname instead of dir_fd + basename, because
     its very easy to get a lot of tempfiles leaking and that would
     mean quite a lot of open fds */
  tmp_dirname = xdp_entry_dup_dirname (entry);

  tmp = g_new0 (XdpTmp, 1);
  tmp->ref_count = 2; /* One owned by tmp_files */
  tmp->tmp_id = g_atomic_int_add (&next_tmp_id, 1);
  tmp->parent_inode = parent;
  tmp->name = g_strdup (name);
  tmp->entry = xdg_app_db_entry_ref (entry);
  tmp->backing_basename = g_strdup (tmp_basename);

  tmp_files = g_list_prepend (tmp_files, tmp);

  return tmp;
}

/* Caller must own tmp_files lock */
static void
xdp_tmp_unlink_nolock (XdpTmp *tmp)
{

  g_autofree char *backing_basename = NULL;

  backing_basename = xdp_tmp_steal_backing_basename (tmp);
  if (backing_basename)
    {
      glnx_fd_close int dir_fd = xdp_entry_open_dir (tmp->entry);
      if (dir_fd)
        unlinkat (dir_fd, backing_basename, 0);
    }

  tmp_files = g_list_remove (tmp_files, tmp);
  xdp_tmp_unref (tmp);
}

/******************************* XdpFh *******************************
 *
 * XdpFh is a ref-counted object representing an open file on the
 * filesystem. Normally it has a regular fd you can do only the allowed
 * i/o on, although in the case of a direct write to a document file
 * it has two fds, one is the read-only fd to the file, and the other
 * is a read-write to a temporary file which is only used once the
 * file is truncated (and is renamed over the real file on close).
 *
 * locking:
 *
 * The global list of outstanding Fh is protected by the open_files
 * lock.  Use it when doing lookups by inode, or when changing
 * the list (add/remove), or when otherwise traversing the list.
 *
 * Each instance has a mutex that must be locked when doing some
 * kind of operation on the file handle, to serialize both lower
 * layer i/o as well as access to the members.
 *
 * To avoid deadlocks or just slow locking, never aquire the
 * open_files lock and a lock on a Fh at the same time.
 *
 ******************************* XdpFh *******************************/


typedef struct
{
  volatile gint ref_count;

  /* These are immutable, no lock needed */
  guint32 tmp_id;
  fuse_ino_t inode;
  int dir_fd;
  char *trunc_basename;
  char *real_basename;
  gboolean can_write;

  /* These need a lock whenever they are used */
  int fd;
  int trunc_fd;
  gboolean truncated;
  gboolean readonly;

  GMutex mutex;
} XdpFh;

static GList *open_files = NULL;
G_LOCK_DEFINE(open_files);

static XdpFh *
xdp_fh_ref (XdpFh *fh)
{
  g_atomic_int_inc (&fh->ref_count);
  return fh;
}

static void
xdp_fh_finalize (XdpFh *fh)
{
  if (fh->truncated)
    {
      fsync (fh->trunc_fd);
      if (renameat (fh->dir_fd, fh->trunc_basename,
                    fh->dir_fd, fh->real_basename) != 0)
        g_warning ("Unable to replace truncated document");
    }
  else if (fh->trunc_basename)
    unlinkat (fh->dir_fd, fh->trunc_basename, 0);

  if (fh->fd >= 0)
    close (fh->fd);

  if (fh->trunc_fd >= 0)
    close (fh->trunc_fd);

  if (fh->dir_fd >= 0)
    close (fh->dir_fd);

  g_clear_pointer (&fh->trunc_basename, g_free);
  g_clear_pointer (&fh->real_basename, g_free);

  g_free (fh);
}

static void
xdp_fh_unref (XdpFh *fh)
{
  if (g_atomic_int_dec_and_test (&fh->ref_count))
    {
      /* There is a tiny race here where fhs can be on the open_files list
         with refcount 0, so make sure to skip such while under the open_files
         lock */
      {
        AUTOLOCK (open_files);
        open_files = g_list_remove (open_files, fh);
      }

      xdp_fh_finalize (fh);
    }
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdpFh, xdp_fh_unref)

static void
xdp_fh_lock (XdpFh *fh)
{
  g_mutex_lock (&fh->mutex);
}

static void
xdp_fh_unlock (XdpFh *fh)
{
  g_mutex_unlock (&fh->mutex);
}

static inline void xdp_fh_auto_unlock_helper (XdpFh **fhp)
{
  if (*fhp)
    xdp_fh_unlock (*fhp);
}

static inline XdpFh *xdp_fh_auto_lock_helper (XdpFh *fh)
{
  if (fh)
    xdp_fh_lock (fh);
  return fh;
}

#define XDP_FH_AUTOLOCK(_fh) G_GNUC_UNUSED __attribute__((cleanup(xdp_fh_auto_unlock_helper))) XdpFh * G_PASTE(xdp_fh_auto_unlock, __LINE__) = xdp_fh_auto_lock_helper (fh)

static XdpFh *
xdp_fh_new (fuse_ino_t inode,
            struct fuse_file_info *fi,
            int fd,
            XdpTmp *tmp)
{
  XdpFh *fh = g_new0 (XdpFh, 1);
  fh->inode = inode;
  fh->fd = fd;
  if (tmp)
    fh->tmp_id = tmp->tmp_id;
  fh->dir_fd = -1;
  fh->trunc_fd = -1;
  fh->ref_count = 1; /* Owned by fuse_file_info fi */

  fi->fh = (gsize)fh;

  AUTOLOCK (open_files);

  open_files = g_list_prepend (open_files, fh);

  return fh;
}

static int
xdp_fh_get_fd_nolock (XdpFh *fh)
{
  if (fh->truncated)
    return fh->trunc_fd;
  else
    return fh->fd;
}

static int
xdp_fh_fstat (XdpFh *fh,
              struct stat *stbuf)
{
  struct stat tmp_stbuf;
  int fd;

  fd = xdp_fh_get_fd_nolock (fh);
  if (fd < 0)
    return -ENOSYS;

  if (fstat (fd, &tmp_stbuf) != 0)
    return -errno;

  stbuf->st_nlink = DOC_FILE_NLINK;
  stbuf->st_mode = S_IFREG | get_user_perms (&tmp_stbuf);
  if (!fh->can_write)
    stbuf->st_mode &= ~(0222);
  stbuf->st_size = tmp_stbuf.st_size;
  stbuf->st_uid = tmp_stbuf.st_uid;
  stbuf->st_gid = tmp_stbuf.st_gid;
  stbuf->st_blksize = tmp_stbuf.st_blksize;
  stbuf->st_blocks = tmp_stbuf.st_blocks;
  stbuf->st_atim = tmp_stbuf.st_atim;
  stbuf->st_mtim = tmp_stbuf.st_mtim;
  stbuf->st_ctim = tmp_stbuf.st_ctim;

  return 0;
}

static int
xdp_fh_fstat_locked (XdpFh *fh,
                     struct stat *stbuf)
{
  XDP_FH_AUTOLOCK (fh);

  return xdp_fh_fstat (fh, stbuf);
}

static int
xdp_fh_truncate_locked (XdpFh *fh, off_t size, struct stat  *newattr)
{
  int fd;

  XDP_FH_AUTOLOCK (fh);

  if (fh->trunc_fd >= 0 && !fh->truncated)
    {
      if (size != 0)
        return -EACCES;

      fh->truncated = TRUE;
      fd = fh->trunc_fd;
    }
  else
    {
      fd = xdp_fh_get_fd_nolock (fh);
      if (fd == -1)
        return -EIO;

      if (ftruncate (fd, size) != 0)
        return - errno;
    }

  if (newattr)
    {
      int res = xdp_fh_fstat (fh, newattr);
      if (res < 0)
        return res;
    }

  return 0;
}

static void
mark_open_tmp_file_readonly (guint32 tmp_id)
{
  GList *found = NULL;
  GList *l;

  {
    AUTOLOCK (open_files);

    for (l = open_files; l != NULL; l = l->next)
      {
        XdpFh *fh = l->data;
        /* See xdp_fh_unref for details of this ref_count check */
        if (g_atomic_int_get (&fh->ref_count) > 0 &&
            fh->tmp_id == tmp_id && fh->fd >= 0)
          found = g_list_prepend (found, xdp_fh_ref (fh));
      }
  }

  /* We do the actual updates outside of the open_files lock to avoid
     potentially blocking for a long time with it held */

  for (l = found; l != NULL; l = l->next)
    {
      XdpFh *fh = l->data;
      XDP_FH_AUTOLOCK (fh);
      fh->readonly = TRUE;
      xdp_fh_unref (fh);
    }
  g_list_free (found);
}


static XdpFh *
find_open_fh (fuse_ino_t ino)
{
  GList *l;

  AUTOLOCK (open_files);

  for (l = open_files; l != NULL; l = l->next)
    {
      XdpFh *fh = l->data;
      /* See xdp_fh_unref for details of this ref_count check */
      if (fh->inode == ino &&
          g_atomic_int_get (&fh->ref_count) > 0)
        return xdp_fh_ref (fh);
    }

  return NULL;
}

/******************************* Main *******************************/

static XdpInodeClass
get_class (guint64 inode)
{
  return (inode >> (64-8)) & 0xff;
}

static guint64
get_class_ino (guint64 inode)
{
  return inode & ((1L << (64-8)) - 1);
}

static guint32
get_app_id_from_app_doc_ino (guint64 inode)
{
  return inode >> 32;
}

static guint32
get_doc_id_from_app_doc_ino (guint64 inode)
{
  return inode & 0xffffffff;
}

static guint64
make_inode (XdpInodeClass class, guint64 inode)
{
  return ((guint64)class) << (64-8) | (inode & 0xffffffffffffff);
}

static guint64
make_app_doc_dir_inode (guint32 app_id, guint32 doc_id)
{
  return make_inode (APP_DOC_DIR_INO_CLASS,
                     ((guint64)app_id << 32) | (guint64)doc_id);
}

static guint64
make_app_doc_file_inode (guint32 app_id, guint32 doc_id)
{
  return make_inode (APP_DOC_FILE_INO_CLASS,
                     ((guint64)app_id << 32) | (guint64)doc_id);
}

static gboolean
name_looks_like_id (const char *name)
{
  int i;

  /* No zeros in front, we need canonical form */
  if (name[0] == '0')
    return FALSE;

  for (i = 0; i < 8; i++)
    {
      char c = name[i];
      if (c == 0)
        break;

      if (!g_ascii_isdigit(c) &&
          !(c >= 'a' && c <= 'f'))
        return FALSE;
    }

  if (name[i] != 0)
    return FALSE;

  return TRUE;
}

static guint32
get_app_id_from_name (const char *name)
{
  guint32 id;
  char *myname;

  AUTOLOCK(app_id);

  id = GPOINTER_TO_UINT (g_hash_table_lookup (app_name_to_id, name));

  if (id != 0)
    return id;

  id = next_app_id++;

  /* We rely this to not overwrap into the high byte in the inode */
  g_assert (id < 0x00ffffff);

  myname = g_strdup (name);
  g_hash_table_insert (app_name_to_id, myname, GUINT_TO_POINTER (id));
  g_hash_table_insert (app_id_to_name, GUINT_TO_POINTER (id), myname);
  return id;
}

static const char *
get_app_name_from_id (guint32 id)
{
  AUTOLOCK(app_id);
  return g_hash_table_lookup (app_id_to_name, GUINT_TO_POINTER (id));
}

static void
fill_app_name_hash (void)
{
  g_auto(GStrv) keys = NULL;
  int i;

  keys = xdp_list_apps ();
  for (i = 0; keys[i] != NULL; i++)
    get_app_id_from_name (keys[i]);
}

static gboolean
app_can_see_doc (XdgAppDbEntry *entry, guint32 app_id)
{
  const char *app_name = get_app_name_from_id (app_id);

  if (app_id == 0)
    return TRUE;

  if (app_name != NULL &&
      xdp_entry_has_permissions (entry, app_name, XDP_PERMISSION_FLAGS_READ))
    return TRUE;

  return FALSE;
}

static gboolean
app_can_write_doc (XdgAppDbEntry *entry, guint32 app_id)
{
  const char *app_name = get_app_name_from_id (app_id);

  if (app_id == 0)
    return TRUE;

  if (app_name != NULL &&
      xdp_entry_has_permissions (entry, app_name, XDP_PERMISSION_FLAGS_WRITE))
    return TRUE;

  return FALSE;
}


static int
xdp_stat (fuse_ino_t ino,
          struct stat *stbuf,
          XdgAppDbEntry **entry_out)
{
  XdpInodeClass class = get_class (ino);
  guint64 class_ino = get_class_ino (ino);
  g_autoptr (XdgAppDbEntry) entry = NULL;
  struct stat tmp_stbuf;
  g_autoptr(XdpTmp) tmp = NULL;
  g_autofree char *backing_basename = NULL;

  stbuf->st_ino = ino;

  switch (class)
    {
    case STD_DIRS_INO_CLASS:

      switch (class_ino)
        {
        case FUSE_ROOT_ID:
          stbuf->st_mode = S_IFDIR | NON_DOC_DIR_PERMS;
          stbuf->st_nlink = 2;
          break;

        case BY_APP_INO:
          stbuf->st_mode = S_IFDIR | NON_DOC_DIR_PERMS;
          stbuf->st_nlink = 2;
          break;

        default:
          return ENOENT;
        }
      break;

    case APP_DIR_INO_CLASS:
      if (get_app_name_from_id (class_ino) == 0)
        return ENOENT;

      stbuf->st_mode = S_IFDIR | NON_DOC_DIR_PERMS;
      stbuf->st_nlink = 2;
      break;

    case APP_DOC_DIR_INO_CLASS:
      {
        guint32 app_id = get_app_id_from_app_doc_ino (class_ino);
        guint32 doc_id = get_doc_id_from_app_doc_ino (class_ino);

        entry = xdp_lookup_doc (doc_id);
        if (entry == NULL || !app_can_see_doc (entry, app_id))
          return ENOENT;

        stbuf->st_mode = S_IFDIR | DOC_DIR_PERMS;
        stbuf->st_nlink = 2;
        break;
      }

    case APP_DOC_FILE_INO_CLASS:
      {
        guint32 app_id = get_app_id_from_app_doc_ino (class_ino);
        guint32 doc_id = get_doc_id_from_app_doc_ino (class_ino);
        gboolean can_write;

        entry = xdp_lookup_doc (doc_id);
        if (entry == NULL)
          return ENOENT;

        can_write = app_can_write_doc (entry, app_id);

        stbuf->st_nlink = DOC_FILE_NLINK;

        if (xdp_entry_stat (entry, &tmp_stbuf, AT_SYMLINK_NOFOLLOW) != 0)
          return ENOENT;

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
        break;
      }

    case TMPFILE_INO_CLASS:
      tmp = find_tmp_by_id (class_ino);
      if (tmp == NULL)
        return ENOENT;

      stbuf->st_mode = S_IFREG;
      stbuf->st_nlink = DOC_FILE_NLINK;

      backing_basename = xdp_tmp_get_backing_basename (tmp);

      {
        glnx_fd_close int dir_fd = xdp_entry_open_dir (tmp->entry);

        if (backing_basename == NULL ||
            dir_fd == -1 ||
            fstatat (dir_fd, backing_basename, &tmp_stbuf, 0) != 0)
          return ENOENT;
      }

      stbuf->st_mode = S_IFREG | get_user_perms (&tmp_stbuf);
      stbuf->st_size = tmp_stbuf.st_size;
      stbuf->st_uid = tmp_stbuf.st_uid;
      stbuf->st_gid = tmp_stbuf.st_gid;
      stbuf->st_blksize = tmp_stbuf.st_blksize;
      stbuf->st_blocks = tmp_stbuf.st_blocks;
      stbuf->st_atim = tmp_stbuf.st_atim;
      stbuf->st_mtim = tmp_stbuf.st_mtim;
      stbuf->st_ctim = tmp_stbuf.st_ctim;
      break;

    default:
      return ENOENT;
    }

  if (entry && entry_out)
    *entry_out = g_steal_pointer (&entry);

  return 0;
}

static void
xdp_fuse_getattr (fuse_req_t req,
                  fuse_ino_t ino,
                  struct fuse_file_info *fi)
{
  struct stat stbuf = { 0 };
  g_autoptr(XdpFh) fh = NULL;
  int res;

  g_debug ("xdp_fuse_getattr %lx (fi=%p)", ino, fi);

  /* Fuse passes fi in to verify EOF during read/write/seek, but not during fstat */
  if (fi != NULL)
    {
      XdpFh *fh = (gpointer)fi->fh;

      res = xdp_fh_fstat_locked (fh, &stbuf);
      if (res == 0)
        {
          fuse_reply_attr (req, &stbuf, get_attr_cache_time (stbuf.st_mode));
          return;
        }
    }


  fh = find_open_fh (ino);
  if (fh)
    {
      res = xdp_fh_fstat_locked (fh, &stbuf);
      if (res == 0)
        {
          fuse_reply_attr (req, &stbuf, get_attr_cache_time (stbuf.st_mode));
          return;
        }
    }

  if ((res = xdp_stat (ino, &stbuf, NULL)) != 0)
    fuse_reply_err (req, res);
  else
    fuse_reply_attr (req, &stbuf, get_attr_cache_time (stbuf.st_mode));
}

static int
xdp_lookup (fuse_ino_t parent,
            const char *name,
            fuse_ino_t *inode,
            struct stat *stbuf,
            XdgAppDbEntry **entry_out,
            XdpTmp **tmp_out)
{
  XdpInodeClass parent_class = get_class (parent);
  guint64 parent_class_ino = get_class_ino (parent);
  g_autoptr (XdgAppDbEntry) entry = NULL;
  g_autoptr (XdpTmp) tmp = NULL;

  if (entry_out)
    *entry_out = NULL;
  if (tmp_out)
    *tmp_out = NULL;

  switch (parent_class)
    {
    case STD_DIRS_INO_CLASS:

      switch (parent_class_ino)
        {
        case FUSE_ROOT_ID:
          if (strcmp (name, BY_APP_NAME) == 0)
            {
              *inode = make_inode (STD_DIRS_INO_CLASS, BY_APP_INO);
              if (xdp_stat (*inode, stbuf, NULL) == 0)
                return 0;
            }
          else if (name_looks_like_id (name))
            {
              *inode = make_app_doc_dir_inode (0, xdp_id_from_name (name));
              if (xdp_stat (*inode, stbuf, NULL) == 0)
                return 0;
            }

          break;

        case BY_APP_INO:
          if (xdg_app_is_valid_name (name))
            {
              guint32 app_id = get_app_id_from_name (name);
              *inode = make_inode (APP_DIR_INO_CLASS, app_id);
              if (xdp_stat (*inode, stbuf, NULL) == 0)
                return 0;
            }

          break;

        default:
          break;
        }
      break;

    case APP_DIR_INO_CLASS:
      {
        if (name_looks_like_id (name))
          {
            *inode = make_app_doc_dir_inode (parent_class_ino,
                                             xdp_id_from_name (name));
            if (xdp_stat (*inode, stbuf, NULL) == 0)
              return 0;
          }
      }

      break;

    case APP_DOC_DIR_INO_CLASS:
      {
        guint32 app_id = get_app_id_from_app_doc_ino (parent_class_ino);
        guint32 doc_id = get_doc_id_from_app_doc_ino (parent_class_ino);

        entry = xdp_lookup_doc (doc_id);
        if (entry != NULL)
          {
            g_autofree char *basename = xdp_entry_dup_basename (entry);
            if (strcmp (name, basename) == 0)
              {
                *inode = make_app_doc_file_inode (app_id, doc_id);
                if (xdp_stat (*inode, stbuf, NULL) == 0)
                  {
                    if (entry_out)
                      *entry_out = g_steal_pointer (&entry);
                    return 0;
                  }

                break;
              }
          }

        tmp = find_tmp_by_name (parent, name);
        if (tmp != NULL)
          {
            *inode = make_inode (TMPFILE_INO_CLASS, tmp->tmp_id);
            if (xdp_stat (*inode, stbuf, NULL) == 0)
              {
                if (entry_out)
                  *entry_out = g_steal_pointer (&entry);
                if (tmp_out)
                  *tmp_out = g_steal_pointer (&tmp);
                return 0;
              }

            break;
          }

        break;
      }

    case TMPFILE_INO_CLASS:
    case APP_DOC_FILE_INO_CLASS:
      return ENOTDIR;

    default:
      break;
    }

  return ENOENT;
}

static void
xdp_fuse_lookup (fuse_req_t req,
                 fuse_ino_t parent,
                 const char *name)
{
  struct fuse_entry_param e = {0};
  int res;

  g_debug ("xdp_fuse_lookup %lx/%s -> ", parent, name);

  memset (&e, 0, sizeof(e));

  res = xdp_lookup (parent, name, &e.ino, &e.attr, NULL, NULL);

  if (res == 0)
    {
      g_debug ("xdp_fuse_lookup <- inode %lx", (long)e.ino);
      e.attr_timeout = get_attr_cache_time (e.attr.st_mode);
      e.entry_timeout = get_entry_cache_time (e.ino);
      fuse_reply_entry (req, &e);
    }
  else
    {
      g_debug ("xdp_fuse_lookup <- error %s", strerror (res));
      fuse_reply_err (req, res);
    }
}

struct dirbuf {
  char *p;
  size_t size;
};

static void
dirbuf_add (fuse_req_t req,
            struct dirbuf *b,
            const char *name,
            fuse_ino_t ino)
{
  struct stat stbuf;

  size_t oldsize = b->size;
  b->size += fuse_add_direntry (req, NULL, 0, name, NULL, 0);
  b->p = (char *) g_realloc (b->p, b->size);
  memset (&stbuf, 0, sizeof (stbuf));
  stbuf.st_ino = ino;
  fuse_add_direntry (req, b->p + oldsize,
                     b->size - oldsize,
                     name, &stbuf,
                     b->size);
}

static void
dirbuf_add_docs (fuse_req_t req,
                 struct dirbuf *b,
                 guint32 app_id)
{
  g_autofree guint32 *docs = NULL;
  guint64 inode;
  int i;
  g_autofree char *doc_name = NULL;

  docs = xdp_list_docs ();
  for (i = 0; docs[i] != 0; i++)
    {
      if (app_id)
        {
          g_autoptr(XdgAppDbEntry) entry = xdp_lookup_doc (docs[i]);
          if (entry == NULL ||
              !app_can_see_doc (entry, app_id))
            continue;
        }
      inode = make_app_doc_dir_inode (app_id, docs[i]);
      doc_name = xdp_name_from_id (docs[i]);
      dirbuf_add (req, b, doc_name, inode);
    }
}

static void
dirbuf_add_doc_file (fuse_req_t req,
                     struct dirbuf *b,
                     XdgAppDbEntry *entry,
                     guint32 doc_id,
                     guint32 app_id)
{
  struct stat tmp_stbuf;
  guint64 inode;
  g_autofree char *basename = xdp_entry_dup_basename (entry);

  inode = make_app_doc_file_inode (app_id, doc_id);

  if (xdp_entry_stat (entry, &tmp_stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    dirbuf_add (req, b, basename, inode);
}

static void
dirbuf_add_tmp_files (fuse_req_t req,
                      struct dirbuf *b,
                      guint64 dir_inode)
{
  GList *l;

  AUTOLOCK(tmp_files);

  for (l = tmp_files; l != NULL; l = l->next)
    {
      XdpTmp *tmp = l->data;
      if (tmp->parent_inode == dir_inode)
        dirbuf_add (req, b, tmp->name,
                    make_inode (TMPFILE_INO_CLASS, tmp->tmp_id));
    }
}

static int
reply_buf_limited (fuse_req_t req,
                   const char *buf,
                   size_t bufsize,
                   off_t off,
                   size_t maxsize)
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
  struct dirbuf *b = (struct dirbuf *)(fi->fh);

  reply_buf_limited (req, b->p, b->size, off, size);
}

static void
xdp_fuse_opendir (fuse_req_t req,
                  fuse_ino_t ino,
                  struct fuse_file_info *fi)
{
  struct stat stbuf = {0};
  struct dirbuf b = {0};
  XdpInodeClass class;
  guint64 class_ino;
  g_autoptr (XdgAppDbEntry) entry = NULL;
  int res;

  g_debug ("xdp_fuse_opendir %lx", ino);

  if ((res = xdp_stat (ino, &stbuf, &entry)) != 0)
    {
      fuse_reply_err (req, res);
      return;
    }

  if ((stbuf.st_mode & S_IFMT) != S_IFDIR)
    {
      fuse_reply_err (req, ENOTDIR);
      return;
    }

  class = get_class (ino);
  class_ino = get_class_ino (ino);

  switch (class)
    {
    case STD_DIRS_INO_CLASS:
      switch (class_ino)
        {
        case FUSE_ROOT_ID:
          dirbuf_add (req, &b, ".", FUSE_ROOT_ID);
          dirbuf_add (req, &b, "..", FUSE_ROOT_ID);
          dirbuf_add (req, &b, BY_APP_NAME,
                      make_inode (STD_DIRS_INO_CLASS, BY_APP_INO));
          dirbuf_add_docs (req, &b, 0);
          break;

        case BY_APP_INO:
          dirbuf_add (req, &b, ".", ino);
          dirbuf_add (req, &b, "..", FUSE_ROOT_ID);

          /* Update for any possible new app */
          fill_app_name_hash ();

          {
            GHashTableIter iter;
            gpointer key, value;

            AUTOLOCK(app_id);

            g_hash_table_iter_init (&iter, app_name_to_id);
            while (g_hash_table_iter_next (&iter, &key, &value))
              {
                const char *name = key;
                guint32 id = GPOINTER_TO_UINT(value);

                if (strlen (name) > 0)
                  dirbuf_add (req, &b, name,
                              make_inode (APP_DIR_INO_CLASS, id));
              }
          }
          break;

        default:
          break;
        }
      break;

    case APP_DIR_INO_CLASS:
      {
        dirbuf_add (req, &b, ".", ino);
        dirbuf_add (req, &b, "..", make_inode (STD_DIRS_INO_CLASS, BY_APP_INO));
        dirbuf_add_docs (req, &b, class_ino);
        break;
      }

      break;

    case APP_DOC_DIR_INO_CLASS:
      dirbuf_add (req, &b, ".", ino);
      if (get_app_id_from_app_doc_ino (class_ino) == 0)
        dirbuf_add (req, &b, "..", FUSE_ROOT_ID);
      else
        dirbuf_add (req, &b, "..", make_inode (APP_DIR_INO_CLASS,
                                               get_app_id_from_app_doc_ino (class_ino)));
      dirbuf_add_doc_file (req, &b, entry,
                           get_doc_id_from_app_doc_ino (class_ino),
                           get_app_id_from_app_doc_ino (class_ino));
      dirbuf_add_tmp_files (req, &b, ino);
      break;

    case APP_DOC_FILE_INO_CLASS:
    case TMPFILE_INO_CLASS:
      /* These should have returned ENOTDIR above */
    default:
      break;
    }

  if (b.p == NULL)
    fuse_reply_err (req, EIO);
  else
    {
      fi->fh = (gsize)g_memdup (&b, sizeof (b));
      if (fuse_reply_open (req, fi) == -ENOENT)
        {
          g_free (b.p);
          g_free ((gpointer)(fi->fh));
        }
    }
}

static void
xdp_fuse_releasedir (fuse_req_t req,
                     fuse_ino_t ino,
                     struct fuse_file_info *fi)
{
  struct dirbuf *b = (struct dirbuf *)(fi->fh);
  g_free (b->p);
  g_free (b);
  fuse_reply_err (req, 0);
}

static int
get_open_flags (struct fuse_file_info *fi)
{
  /* TODO: Maybe limit the flags set more */
  return fi->flags & ~(O_EXCL|O_CREAT);
}

static char *
create_tmp_for_doc (XdgAppDbEntry *entry, int dir_fd, int flags, int *fd_out)
{
  g_autofree char *basename = xdp_entry_dup_basename (entry);
  g_autofree char *template = g_strconcat (".xdp_", basename, ".XXXXXX", NULL);
  int fd;

  fd = xdg_app_mkstempat (dir_fd, template, flags|O_CLOEXEC, 0600);
  if (fd == -1)
    return NULL;

  *fd_out = fd;
  return g_steal_pointer (&template);
}

static void
xdp_fuse_open (fuse_req_t req,
               fuse_ino_t ino,
               struct fuse_file_info *fi)
{
  XdpInodeClass class = get_class (ino);
  guint64 class_ino = get_class_ino (ino);
  struct stat stbuf = {0};
  g_autoptr (XdgAppDbEntry) entry = NULL;
  g_autoptr(XdpTmp) tmp = NULL;
  glnx_fd_close int fd = -1;
  int res;
  XdpFh *fh = NULL;

  g_debug ("xdp_fuse_open %lx", ino);

  if ((res = xdp_stat (ino, &stbuf, &entry)) != 0)
    {
      fuse_reply_err (req, res);
      return;
    }

  if ((stbuf.st_mode & S_IFMT) != S_IFREG)
    {
      fuse_reply_err (req, EISDIR);
      return;
    }

  if (entry && class == APP_DOC_FILE_INO_CLASS)
    {
      g_autofree char *tmp_basename = NULL;
      glnx_fd_close int write_fd = -1;
      glnx_fd_close int dir_fd = -1;
      g_autofree char *basename = xdp_entry_dup_basename (entry);
      guint32 app_id = get_app_id_from_app_doc_ino (class_ino);
      gboolean can_write;

      dir_fd = xdp_entry_open_dir (entry);
      if (dir_fd == -1)
        {
          fuse_reply_err (req, errno);
          return;
        }

      can_write = app_can_write_doc (entry, app_id);

      if ((fi->flags & 3) != O_RDONLY)
        {
          if (!can_write)
            {
              fuse_reply_err (req, EACCES);
              return;
            }

          if (faccessat (dir_fd, basename, W_OK, 0) != 0)
            {
              fuse_reply_err (req, errno);
              return;
            }

          tmp_basename = create_tmp_for_doc (entry, dir_fd, O_RDWR, &write_fd);
          if (tmp_basename == NULL)
            {
              fuse_reply_err (req, errno);
              return;
            }
        }

      fd = openat (dir_fd, basename, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
      if (fd < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
      fh = xdp_fh_new (ino, fi, steal_fd(&fd), NULL);
      fh->can_write = can_write;
      fh->dir_fd = steal_fd (&dir_fd);
      fh->trunc_fd = steal_fd (&write_fd);
      fh->trunc_basename = g_steal_pointer (&tmp_basename);
      fh->real_basename = g_strdup (basename);
      if (fuse_reply_open (req, fi))
        xdp_fh_unref (fh);
    }
  else if (class == TMPFILE_INO_CLASS &&
           (tmp = find_tmp_by_id (class_ino)))
    {
      glnx_fd_close int dir_fd = xdp_entry_open_dir (tmp->entry);
      g_autofree char *backing_basename = xdp_tmp_get_backing_basename (tmp);
      if (dir_fd == -1 || backing_basename == NULL)
        {
          fuse_reply_err (req, ENOENT);
          return;
        }

      fd = openat (dir_fd, backing_basename, get_open_flags (fi)|O_NOFOLLOW|O_CLOEXEC);
      if (fd < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
      fh = xdp_fh_new (ino, fi, steal_fd (&fd), tmp);
      fh->can_write = TRUE;
      if (fuse_reply_open (req, fi))
        xdp_fh_unref (fh);
    }
  else
    fuse_reply_err (req, EIO);
}

static void
xdp_fuse_create (fuse_req_t req,
                 fuse_ino_t parent,
                 const char *name,
                 mode_t mode,
                 struct fuse_file_info *fi)
{
  struct fuse_entry_param e = {0};
  XdpInodeClass parent_class = get_class (parent);
  guint64 parent_class_ino = get_class_ino (parent);
  struct stat stbuf;
  XdpFh *fh;
  g_autoptr(XdgAppDbEntry) entry = NULL;
  g_autofree char *basename = NULL;
  glnx_fd_close int fd = -1;
  gboolean can_write;
  int res;
  guint32 app_id = 0;
  guint32 doc_id;

  g_debug ("xdp_fuse_create %lx/%s, flags %o", parent, name, fi->flags);

  if ((res = xdp_stat (parent, &stbuf, &entry)) != 0)
    {
      fuse_reply_err (req, res);
      return;
    }

  if ((stbuf.st_mode & S_IFMT) != S_IFDIR)
    {
      fuse_reply_err (req, ENOTDIR);
      return;
    }

  if (parent_class != APP_DOC_DIR_INO_CLASS)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  app_id = get_app_id_from_app_doc_ino (parent_class_ino);
  doc_id = get_doc_id_from_app_doc_ino (parent_class_ino);

  can_write = app_can_write_doc (entry, app_id);

  basename = xdp_entry_dup_basename (entry);
  if (strcmp (name, basename) == 0)
    {
      g_autofree char *tmp_basename = NULL;
      glnx_fd_close int write_fd = -1;
      glnx_fd_close int dir_fd = -1;

      dir_fd = xdp_entry_open_dir (entry);
      if (dir_fd == -1)
        {
          fuse_reply_err (req, errno);
          return;
        }

      if (!can_write)
        {
          fuse_reply_err (req, EACCES);
          return;
        }

      tmp_basename = create_tmp_for_doc (entry, dir_fd, O_RDWR, &write_fd);
      if (tmp_basename == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }

      fd = openat (dir_fd, basename, O_CREAT|O_EXCL|O_RDONLY|O_NOFOLLOW|O_CLOEXEC, mode & 0777);
      if (fd < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }

      e.ino = make_app_doc_file_inode (app_id, doc_id);

      fh = xdp_fh_new (e.ino, fi, steal_fd (&fd), NULL);
      fh->can_write = TRUE;
      fh->dir_fd = steal_fd (&dir_fd);
      fh->truncated = TRUE;
      fh->trunc_fd = steal_fd (&write_fd);
      fh->trunc_basename = g_steal_pointer (&tmp_basename);
      fh->real_basename = g_strdup (basename);

      if (xdp_fh_fstat_locked (fh, &e.attr) != 0)
        {
          xdp_fh_unref (fh);
          fuse_reply_err (req, EIO);
          return;
        }

      e.attr_timeout = get_attr_cache_time (e.attr.st_mode);
      e.entry_timeout = get_entry_cache_time (e.ino);

      if (fuse_reply_create (req, &e, fi))
        xdp_fh_unref (fh);
    }
  else
    {
      g_autoptr(XdpTmp) tmpfile = NULL;

      G_LOCK(tmp_files);
      tmpfile = find_tmp_by_name_nolock (parent, name);
      if (tmpfile != NULL && fi->flags & O_EXCL)
        {
          G_UNLOCK(tmp_files);
          fuse_reply_err (req, EEXIST);
          return;
        }

      if (!can_write)
        {
          fuse_reply_err (req, EACCES);
          return;
        }

      if (tmpfile)
        {
          glnx_fd_close int dir_fd = xdp_entry_open_dir (tmpfile->entry);
          g_autofree char *backing_basename = NULL;

          G_UNLOCK(tmp_files);

          backing_basename = xdp_tmp_get_backing_basename (tmpfile);
          if (dir_fd == -1 || backing_basename == NULL)
            {
              fuse_reply_err (req, EINVAL);
              return;
            }

          fd = openat (dir_fd, backing_basename, get_open_flags (fi)|O_NOFOLLOW|O_CLOEXEC);
          if (fd == -1)
            {
              fuse_reply_err (req, errno);
              return;
            }
        }
      else
        {
          int errsv;
          g_autofree char *tmp_basename = NULL;
          glnx_fd_close int dir_fd = -1;

          dir_fd = xdp_entry_open_dir (entry);
          if (dir_fd == -1)
            {
              fuse_reply_err (req, errno);
              return;
            }

          tmp_basename = create_tmp_for_doc (entry, dir_fd, get_open_flags (fi), &fd);
          if (tmp_basename == NULL)
            return;

          tmpfile = xdp_tmp_new_nolock (parent, entry, name, tmp_basename);
          errsv = errno;
          G_UNLOCK(tmp_files);

          if (tmpfile == NULL)
            {
              fuse_reply_err (req, errsv);
              return;
            }
        }

      e.ino = make_inode (TMPFILE_INO_CLASS, tmpfile->tmp_id);
      if (xdp_stat (e.ino, &e.attr, NULL) != 0)
        {
          fuse_reply_err (req, EIO);
          return;
        }
      e.attr_timeout = get_attr_cache_time (e.attr.st_mode);
      e.entry_timeout = get_entry_cache_time (e.ino);

      fh = xdp_fh_new (e.ino, fi, steal_fd (&fd), tmpfile);
      fh->can_write = TRUE;
      if (fuse_reply_create (req, &e, fi))
        xdp_fh_unref (fh);
    }
}

static void
xdp_fuse_read (fuse_req_t req,
               fuse_ino_t ino,
               size_t size,
               off_t off,
               struct fuse_file_info *fi)
{
  XdpFh *fh = (gpointer)fi->fh;
  struct fuse_bufvec bufv = FUSE_BUFVEC_INIT (size);
  static char c = 'x';
  int fd;

  XDP_FH_AUTOLOCK (fh);

  fd = xdp_fh_get_fd_nolock (fh);
  if (fd == -1)
    {
      bufv.buf[0].flags = 0;
      bufv.buf[0].mem = &c;
      bufv.buf[0].size = 0;

      fuse_reply_data (req, &bufv, FUSE_BUF_NO_SPLICE);
      return;
    }

  bufv.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
  bufv.buf[0].fd = fd;
  bufv.buf[0].pos = off;

  fuse_reply_data (req, &bufv, FUSE_BUF_SPLICE_MOVE);
}

static  void
xdp_fuse_write (fuse_req_t req,
                fuse_ino_t ino,
                const char *buf,
                size_t size,
                off_t off,
                struct fuse_file_info *fi)
{
  XdpFh *fh = (gpointer)fi->fh;
  gssize res;
  int fd;

  XDP_FH_AUTOLOCK (fh);

  if (fh->readonly)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  fd = xdp_fh_get_fd_nolock (fh);
  if (fd == -1)
    {
      fuse_reply_err (req, EIO);
      return;
    }

  res = pwrite (fd, buf, size, off);
  if (res < 0)
    fuse_reply_err (req, errno);
  else
    fuse_reply_write (req, res);
}

static void
xdp_fuse_write_buf (fuse_req_t req,
                    fuse_ino_t ino,
                    struct fuse_bufvec *bufv,
                    off_t off,
                    struct fuse_file_info *fi)
{
  XdpFh *fh = (gpointer)fi->fh;
  struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(bufv));
  gssize res;
  int fd;

  XDP_FH_AUTOLOCK (fh);

  if (fh->readonly)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  fd = xdp_fh_get_fd_nolock (fh);
  if (fd == -1)
    {
      fuse_reply_err (req, EIO);
      return;
    }

  dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
  dst.buf[0].fd = fd;
  dst.buf[0].pos = off;

  res = fuse_buf_copy (&dst, bufv, FUSE_BUF_SPLICE_NONBLOCK);
  if (res < 0)
    fuse_reply_err (req, -res);
  else
    fuse_reply_write (req, res);
}

static  void
xdp_fuse_release (fuse_req_t req,
                  fuse_ino_t ino,
                  struct fuse_file_info *fi)
{
  XdpFh *fh = (gpointer)fi->fh;

  g_debug ("xdp_fuse_release %lx (fi=%p, refcount: %d)", ino, fi, fh->ref_count);

  xdp_fh_unref (fh);
  fuse_reply_err (req, 0);
}

static void
xdp_fuse_rename (fuse_req_t req,
                 fuse_ino_t parent,
                 const char *name,
                 fuse_ino_t newparent,
                 const char *newname)
{
  XdpInodeClass parent_class = get_class (parent);
  guint64 parent_class_ino = get_class_ino (parent);
  g_autoptr (XdgAppDbEntry) entry = NULL;
  int res;
  fuse_ino_t inode;
  struct stat stbuf = {0};
  g_autofree char *basename = NULL;
  g_autoptr(XdpTmp) tmp = NULL;
  g_autoptr(XdpTmp) other_tmp = NULL;
  guint32 app_id = 0;
  guint32 doc_id = 0;
  gboolean can_write;

  g_debug ("xdp_fuse_rename %lx/%s -> %lx/%s", parent, name, newparent, newname);

  res = xdp_lookup (parent, name,  &inode, &stbuf, &entry, &tmp);
  if (res != 0)
    {
      fuse_reply_err (req, res);
      return;
    }

  if (parent_class != APP_DOC_DIR_INO_CLASS)
    {
      /* Only allow renames in (app) doc dirs */
      fuse_reply_err (req, EACCES);
      return;
    }

  app_id = get_app_id_from_app_doc_ino (parent_class_ino);
  doc_id = get_doc_id_from_app_doc_ino (parent_class_ino);

  can_write = app_can_write_doc (entry, app_id);

  /* Only allow renames inside the same dir */
  if (!can_write ||
      parent != newparent ||
      entry == NULL ||
      /* Also, don't allow renaming non-tmpfiles */
      tmp == NULL)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  basename = xdp_entry_dup_basename (entry);

  if (strcmp (newname, basename) == 0)
    {
      glnx_fd_close int dir_fd = -1;
      g_autofree char *backing_basename = NULL;
      /* Rename tmpfile to regular file */

      dir_fd = xdp_entry_open_dir (entry);
      if (dir_fd == -1)
        {
          fuse_reply_err (req, errno);
          return;
        }

      /* Steal backing path so we don't delete it when unlinking tmp */
      backing_basename = xdp_tmp_steal_backing_basename (tmp);
      if (backing_basename == NULL)
        {
          fuse_reply_err (req, EINVAL);
          return;
        }

      /* Stop writes to all outstanding fds to the temp file */
      mark_open_tmp_file_readonly (tmp->tmp_id);

      if (renameat (dir_fd, backing_basename,
                    dir_fd, basename) != 0)
        {
          fuse_reply_err (req, errno);
          return;
        }

      AUTOLOCK(tmp_files);

      xdp_tmp_unlink_nolock (tmp);

      fuse_reply_err (req, 0);

      /* We actually turn the old inode to a different one after the rename, so
         we need to invalidate the target entry */

      fuse_lowlevel_notify_inval_entry (main_ch, make_app_doc_dir_inode (app_id, doc_id),
                                        basename, strlen (basename));
    }
  else
    {
      /* Rename tmpfile to other tmpfile name */

      AUTOLOCK(tmp_files);

      other_tmp = find_tmp_by_name_nolock (newparent, newname);
      if (other_tmp)
        xdp_tmp_unlink_nolock (other_tmp);

      g_free (tmp->name);
      tmp->name = g_strdup (newname);
      fuse_reply_err (req, 0);
   }
}

static void
xdp_fuse_setattr (fuse_req_t req,
                  fuse_ino_t ino,
                  struct stat *attr,
                  int to_set,
                  struct fuse_file_info *fi)
{
  g_debug ("xdp_fuse_setattr %lx %x %p", ino, to_set, fi);

  if (to_set == FUSE_SET_ATTR_SIZE && fi != NULL)
    {
      XdpFh *fh = (gpointer)fi->fh;
      int res;
      struct stat newattr = {0};

      /* ftruncate */

      if (!fh->can_write)
        {
          fuse_reply_err (req, EACCES);
          return;
        }

      res = xdp_fh_truncate_locked (fh, attr->st_size, &newattr);
      if (res < 0)
        {
          fuse_reply_err (req, res);
          return;
        }

      fuse_reply_attr (req, &newattr, get_attr_cache_time (newattr.st_mode));
    }
  else if (to_set == FUSE_SET_ATTR_SIZE && fi == NULL)
    {
      int res = 0;
      struct stat newattr = {0};
      struct stat *newattrp = &newattr;
      g_autoptr(XdpFh) fh = NULL;

      /* truncate, truncate any open files (but EACCES if not open) */

      fh = find_open_fh (ino);
      if (fh)
        {
          if (!fh->can_write)
            {
              fuse_reply_err (req, EACCES);
              return;
            }
          res = xdp_fh_truncate_locked (fh, attr->st_size, newattrp);
          newattrp = NULL;
        }
      else
        {
          fuse_reply_err (req, EACCES);
          return;
        }

      if (res < 0)
        {
          fuse_reply_err (req, -res);
          return;
        }

      fuse_reply_attr (req, &newattr, get_attr_cache_time (newattr.st_mode));
    }
  else if (to_set == FUSE_SET_ATTR_MODE)
    {
      gboolean found = FALSE;
      int res, err = -1;
      struct stat newattr = {0};
      XdpFh *fh;

      fh = find_open_fh (ino);
      if (fh)
        {
          int fd, errsv;

          if (!fh->can_write)
            {
              fuse_reply_err (req, EACCES);
              return;
            }

          XDP_FH_AUTOLOCK (fh);

          fd = xdp_fh_get_fd_nolock (fh);
          if (fd != -1)
            {
              res = fchmod (fd, get_user_perms (attr));
              errsv = errno;

              found = TRUE;

              if (res != 0)
                err = -errsv;
              else
                err = xdp_fh_fstat (fh, &newattr);
            }
        }

      if (!found)
        {
          fuse_reply_err (req, EACCES);
          return;
        }

      if (err < 0)
        {
          fuse_reply_err (req, -err);
          return;
        }

      fuse_reply_attr (req, &newattr, get_attr_cache_time (newattr.st_mode));
    }
  else
    fuse_reply_err (req, ENOSYS);
}

static void
xdp_fuse_fsyncdir (fuse_req_t req,
                   fuse_ino_t ino,
                   int datasync,
                   struct fuse_file_info *fi)
{
  XdpInodeClass class = get_class (ino);
  guint64 class_ino = get_class_ino (ino);
  guint32 doc_id;

  if (class == APP_DOC_DIR_INO_CLASS)
    {
      g_autoptr (XdgAppDbEntry) entry = NULL;
      doc_id = get_doc_id_from_app_doc_ino (class_ino);

      entry = xdp_lookup_doc (doc_id);
      if (entry != NULL)
        {
          g_autofree char *dirname = xdp_entry_dup_dirname (entry);
          int fd = open (dirname, O_DIRECTORY|O_RDONLY);
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

static void
xdp_fuse_fsync (fuse_req_t req,
                fuse_ino_t ino,
                int datasync,
                struct fuse_file_info *fi)
{
  XdpInodeClass class = get_class (ino);

  if (class == APP_DOC_FILE_INO_CLASS ||
      class == TMPFILE_INO_CLASS)
    {
      XdpFh *fh = (gpointer)fi->fh;

      XDP_FH_AUTOLOCK (fh);

      if (fh->fd >= 0)
        fsync (fh->fd);
      if (fh->truncated && fh->trunc_fd >= 0)
        fsync (fh->trunc_fd);
    }

  fuse_reply_err (req, 0);
}

static void
xdp_fuse_unlink (fuse_req_t req,
                 fuse_ino_t parent,
                 const char *name)
{
  XdpInodeClass parent_class = get_class (parent);
  guint64 parent_class_ino = get_class_ino (parent);
  g_autoptr (XdgAppDbEntry) entry = NULL;
  int res;
  fuse_ino_t inode;
  struct stat stbuf = {0};
  g_autofree char *basename = NULL;
  g_autoptr (XdpTmp) tmp = NULL;
  guint32 app_id = 0;
  gboolean can_write;

  g_debug ("xdp_fuse_unlink %lx/%s", parent, name);

  res = xdp_lookup (parent, name,  &inode, &stbuf, &entry, &tmp);
  if (res != 0)
    {
      fuse_reply_err (req, res);
      return;
    }

  if (entry == NULL)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  if (parent_class != APP_DOC_DIR_INO_CLASS)
    {
      /* Only allow unlink in (app) doc dirs */
      fuse_reply_err (req, EACCES);
      return;
    }

  app_id = get_app_id_from_app_doc_ino (parent_class_ino);

  can_write = app_can_write_doc (entry, app_id);
  if (!can_write)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  basename = xdp_entry_dup_basename (entry);
  if (strcmp (name, basename) == 0)
    {
      glnx_fd_close int dir_fd = -1;

      dir_fd = xdp_entry_open_dir (entry);
      if (dir_fd == -1)
        {
          fuse_reply_err (req, errno);
          return;
        }

      if (unlinkat (dir_fd, basename, 0) != 0)
        {
          fuse_reply_err (req, errno);
          return;
        }

      fuse_reply_err (req, 0);
    }
  else
    {
      AUTOLOCK(tmp_files);
      xdp_tmp_unlink_nolock (tmp);

      fuse_reply_err (req, 0);
   }
}

static struct fuse_lowlevel_ops xdp_fuse_oper = {
  .lookup       = xdp_fuse_lookup,
  .getattr      = xdp_fuse_getattr,
  .opendir      = xdp_fuse_opendir,
  .readdir      = xdp_fuse_readdir,
  .releasedir   = xdp_fuse_releasedir,
  .fsyncdir     = xdp_fuse_fsyncdir,
  .open         = xdp_fuse_open,
  .create       = xdp_fuse_create,
  .read         = xdp_fuse_read,
  .write        = xdp_fuse_write,
  .write_buf    = xdp_fuse_write_buf,
  .release      = xdp_fuse_release,
  .rename       = xdp_fuse_rename,
  .setattr      = xdp_fuse_setattr,
  .fsync        = xdp_fuse_fsync,
  .unlink       = xdp_fuse_unlink,
};

/* Called when a apps permissions to see a document is changed */
void
xdp_fuse_invalidate_doc_app (const char  *doc_id_s,
                             const char  *app_id_s,
                             XdgAppDbEntry *entry)
{
  guint32 app_id = get_app_id_from_name (app_id_s);
  guint32 doc_id = xdp_id_from_name (doc_id_s);
  g_autofree char *basename = xdp_entry_dup_basename (entry);

  g_debug ("invalidate %s/%s\n", doc_id_s, app_id_s);

  /* This can happen if fuse is not initialized yet for the very
     first dbus message that activated the service */
  if (main_ch == NULL)
    return;

  fuse_lowlevel_notify_inval_inode (main_ch, make_app_doc_file_inode (app_id, doc_id), 0, 0);
  fuse_lowlevel_notify_inval_entry (main_ch, make_app_doc_dir_inode (app_id, doc_id),
                                    basename, strlen (basename));
  fuse_lowlevel_notify_inval_inode (main_ch, make_app_doc_dir_inode (app_id, doc_id), 0, 0);
  fuse_lowlevel_notify_inval_entry (main_ch, make_inode (APP_DIR_INO_CLASS, app_id),
                                    doc_id_s, strlen (doc_id_s));
}

/* Called when a document id is created/removed */
void
xdp_fuse_invalidate_doc (const char  *doc_id_s,
                         XdgAppDbEntry *entry)
{
  guint32 doc_id = xdp_id_from_name (doc_id_s);
  g_autofree char *basename = xdp_entry_dup_basename (entry);

  g_debug ("invalidate %s\n", doc_id_s);

  /* This can happen if fuse is not initialized yet for the very
     first dbus message that activated the service */
  if (main_ch == NULL)
    return;

  fuse_lowlevel_notify_inval_inode (main_ch, make_app_doc_file_inode (0, doc_id), 0, 0);
  fuse_lowlevel_notify_inval_entry (main_ch, make_app_doc_dir_inode (0, doc_id),
                                    basename, strlen (basename));
  fuse_lowlevel_notify_inval_inode (main_ch, make_app_doc_dir_inode (0, doc_id), 0, 0);
  fuse_lowlevel_notify_inval_entry (main_ch, FUSE_ROOT_ID, doc_id_s, strlen (doc_id_s));
}

guint32
xdp_fuse_lookup_id_for_inode (ino_t inode)
{
  XdpInodeClass class = get_class (inode);
  guint64 class_ino = get_class_ino (inode);

  if (class != APP_DOC_FILE_INO_CLASS)
    return 0;

  return get_doc_id_from_app_doc_ino (class_ino);
}

const char *
xdp_fuse_get_mountpoint (void)
{
  if (mount_path == NULL)
    mount_path = g_build_filename (g_get_user_runtime_dir(), "doc", NULL);
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

  fuse_session_remove_chan(main_ch);
  fuse_session_destroy (session);
  fuse_unmount (mount_path, main_ch);
  return NULL;
}

gboolean
xdp_fuse_init (GError **error)
{
  char *argv[] = { "xdp-fuse", "-osplice_write,splice_move,splice_read" };
  struct fuse_args args = FUSE_ARGS_INIT(G_N_ELEMENTS(argv), argv);
  struct stat st;
  const char *mount_path;

  app_name_to_id =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  app_id_to_name =
    g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);

  mount_path = xdp_fuse_get_mountpoint ();

  if (stat (mount_path, &st) == -1 && errno == ENOTCONN)
    {
      char *argv[] = { "fusermount", "-u", (char *)mount_path, NULL };

      g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                    NULL, NULL, NULL, NULL, NULL, NULL);
    }

  if (g_mkdir_with_parents  (mount_path, 0700))
    {
      g_set_error (error, XDG_APP_ERROR, XDG_APP_ERROR_FAILED,
                   "Unable to create dir %s\n", mount_path);
      return FALSE;
    }

  main_ch = fuse_mount (mount_path, &args);
  if (main_ch == NULL)
    {
      g_set_error (error, XDG_APP_ERROR, XDG_APP_ERROR_FAILED, "Can't mount fuse fs");
      return FALSE;
    }

  session = fuse_lowlevel_new (&args, &xdp_fuse_oper,
                               sizeof (xdp_fuse_oper), NULL);
  if (session == NULL)
    {
      g_set_error (error, XDG_APP_ERROR, XDG_APP_ERROR_FAILED,
                   "Can't create fuse session");
      return FALSE;
    }
  fuse_session_add_chan (session, main_ch);

  fuse_thread = g_thread_new ("fuse mainloop", xdp_fuse_mainloop, session);

  return TRUE;
}
