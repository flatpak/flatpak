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

#include "xdp-error.h"
#include "xdp-fuse.h"
#include "xdp-util.h"

/* Layout:

   "/ (STD_DIRS:1)
    "by-app/" (STD_DIRS:2)
      "org.gnome.gedit/" (APP_DIR:app id)
        "$id/" (APP_DOC_DIR:app_id<<32|doc_id)
          <same as DOC_DIR>
    "$id" (DOC_DIR:doc_idid)
      $basename (DOC_FILE:doc_id)
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
  DOC_DIR_INO_CLASS,
  DOC_FILE_INO_CLASS,
  TMPFILE_INO_CLASS,
  APP_DIR_INO_CLASS,
  APP_DOC_DIR_INO_CLASS,
} XdpInodeClass;

#define BY_APP_NAME "by-app"

static GHashTable *app_name_to_id;
static GHashTable *app_id_to_name;
static guint32 next_app_id;

static guint32 next_tmp_id;

typedef struct
{
  guint64 parent_inode;
  char *name;

  char *backing_path;
  guint32 tmp_id;
} XdpTmp;

typedef struct
{
  int fd;
  fuse_ino_t inode;
  int trunc_fd;
  char *trunc_path;
  char *real_path;
  gboolean truncated;
  gboolean readonly;
  guint32 tmp_id;
} XdpFh;

static GList *tmp_files = NULL;
static GList *open_files = NULL;

static XdpTmp *
find_tmp_by_name (guint64 parent_inode,
                  const char *name)
{
  GList *l;

  for (l = tmp_files; l != NULL; l = l->next)
    {
      XdpTmp *tmp = l->data;
      if (tmp->parent_inode == parent_inode &&
          strcmp (tmp->name, name) == 0)
        return tmp;
    }

  return NULL;
}

static XdpTmp *
find_tmp_by_id (guint32 tmp_id)
{
  GList *l;

  for (l = tmp_files; l != NULL; l = l->next)
    {
      XdpTmp *tmp = l->data;
      if (tmp->tmp_id == tmp_id)
        return tmp;
    }

  return NULL;
}

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
  return g_hash_table_lookup (app_id_to_name, GUINT_TO_POINTER (id));
}

static void
fill_app_name_hash (void)
{
  glnx_strfreev char **keys = NULL;
  int i;

  keys = xdp_list_apps ();
  for (i = 0; keys[i] != NULL; i++)
    get_app_id_from_name (keys[i]);
}

static XdpFh *
xdp_fh_new (fuse_ino_t inode, struct fuse_file_info *fi, int fd, XdpTmp *tmp)
{
  XdpFh *fh = g_new0 (XdpFh, 1);
  fh->inode = inode;
  fh->fd = fd;
  if (tmp)
    fh->tmp_id = tmp->tmp_id;
  fh->trunc_fd = -1;

  open_files = g_list_prepend (open_files, fh);

  fi->fh = (gsize)fh;
  return fh;
}

static void
xdp_fh_free (XdpFh *fh)
{
  open_files = g_list_remove (open_files, fh);

  if (fh->truncated)
    {
      fsync (fh->trunc_fd);
      if (rename (fh->trunc_path, fh->real_path) != 0)
        g_warning ("Unable to replace truncated document");
    }
  else if (fh->trunc_path)
    unlink (fh->trunc_path);

  if (fh->fd >= 0)
    close (fh->fd);
  if (fh->trunc_fd >= 0)
    close (fh->trunc_fd);

  g_clear_pointer (&fh->trunc_path, g_free);
  g_clear_pointer (&fh->real_path, g_free);

  g_free (fh);
}

static int
xdp_fh_get_fd (XdpFh *fh)
{
  if (fh->truncated)
    return fh->trunc_fd;
  else
    return fh->fd;
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
get_entry_cache_time (int st_mode)
{
  if (S_ISDIR (st_mode))
    return DIRS_ATTR_CACHE_TIME;
  return 1.0;
}

static gboolean
app_can_see_doc (XdgAppDbEntry *entry, guint32 app_id)
{
  const char *app_name = get_app_name_from_id (app_id);

  if (app_name != NULL &&
      xdp_has_permissions (entry, app_name, XDP_PERMISSION_FLAGS_READ))
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
  const char *path = NULL;
  struct stat tmp_stbuf;
  XdpTmp *tmp;

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

    case DOC_DIR_INO_CLASS:
      entry = xdp_lookup_doc (class_ino);
      if (entry == NULL)
        return ENOENT;

      stbuf->st_mode = S_IFDIR | DOC_DIR_PERMS;
      stbuf->st_nlink = 2;
      break;

    case DOC_FILE_INO_CLASS:
      entry = xdp_lookup_doc (class_ino);
      if (entry == NULL)
        return ENOENT;

      stbuf->st_nlink = DOC_FILE_NLINK;

      path = xdp_get_path (entry);
      if (stat (path, &tmp_stbuf) != 0)
        return ENOENT;

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

    case TMPFILE_INO_CLASS:
      tmp = find_tmp_by_id (class_ino);
      if (tmp == NULL)
        return ENOENT;

      stbuf->st_mode = S_IFREG;
      stbuf->st_nlink = DOC_FILE_NLINK;

      if (stat (tmp->backing_path, &tmp_stbuf) != 0)
        return ENOENT;

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

static int
xdp_fstat (XdpFh *fh,
           struct stat *stbuf)
{
  struct stat tmp_stbuf;
  int fd;

  fd = xdp_fh_get_fd (fh);
  if (fd < 0)
    return -ENOSYS;

  if (fstat (fd, &tmp_stbuf) != 0)
    return -errno;

  stbuf->st_nlink = DOC_FILE_NLINK;
  stbuf->st_mode = S_IFREG | get_user_perms (&tmp_stbuf);
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

static void
xdp_fuse_getattr (fuse_req_t req,
                  fuse_ino_t ino,
                  struct fuse_file_info *fi)
{
  struct stat stbuf = { 0 };
  GList *l;
  int res;

  g_debug ("xdp_fuse_getattr %lx (fi=%p)", ino, fi);

  /* Fuse passes fi in to verify EOF during read/write/seek, but not during fstat */
  if (fi != NULL)
    {
      XdpFh *fh = (gpointer)fi->fh;

      res = xdp_fstat (fh, &stbuf);
      if (res == 0)
        {
          fuse_reply_attr (req, &stbuf, get_attr_cache_time (stbuf.st_mode));
          return;
        }
    }

  for (l = open_files; l != NULL; l = l->next)
    {
      XdpFh *fh = l->data;
      if (fh->inode == ino)
        {
          res = xdp_fstat (fh, &stbuf);
          if (res == 0)
            {
              fuse_reply_attr (req, &stbuf, get_attr_cache_time (stbuf.st_mode));
              return;
            }
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
  XdpTmp *tmp;

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
              *inode = make_inode (DOC_DIR_INO_CLASS,
                                   xdp_id_from_name (name));
              if (xdp_stat (*inode, stbuf, NULL) == 0)
                return 0;
            }

          break;

        case BY_APP_INO:
          if (g_dbus_is_name (name) && !g_dbus_is_unique_name (name))
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
    case DOC_DIR_INO_CLASS:
      if (parent_class == APP_DOC_DIR_INO_CLASS)
        entry = xdp_lookup_doc (get_doc_id_from_app_doc_ino (parent_class_ino));
      else
        entry = xdp_lookup_doc (parent_class_ino);
      if (entry != NULL)
        {
          g_autofree char *basename = xdp_dup_basename (entry);
          if (strcmp (name, basename) == 0)
            {
              *inode = make_inode (DOC_FILE_INO_CLASS, parent_class_ino);
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
                *tmp_out = tmp;
              return 0;
            }

          break;
        }

      break;

    case TMPFILE_INO_CLASS:
    case DOC_FILE_INO_CLASS:
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

  g_debug ("xdp_fuse_lookup %lx/%s", parent, name);

  memset (&e, 0, sizeof(e));

  res = xdp_lookup (parent, name, &e.ino, &e.attr, NULL, NULL);

  if (res == 0)
    {
      e.attr_timeout = get_attr_cache_time (e.attr.st_mode);
      e.entry_timeout = get_entry_cache_time (e.attr.st_mode);
      fuse_reply_entry (req, &e);
    }
  else
    {
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
      if (app_id)
        inode = make_app_doc_dir_inode (app_id, docs[i]);
      else
        inode = make_inode (DOC_DIR_INO_CLASS, docs[i]);
      doc_name = xdp_name_from_id (docs[i]);
      dirbuf_add (req, b, doc_name, inode);

    }
}

static void
dirbuf_add_doc_file (fuse_req_t req,
                     struct dirbuf *b,
                     XdgAppDbEntry *entry,
                     guint32 doc_id)
{
  struct stat tmp_stbuf;
  const char *path = xdp_get_path (entry);
  g_autofree char *basename = xdp_dup_basename (entry);
  if (stat (path, &tmp_stbuf) == 0)
    dirbuf_add (req, b, basename,
                make_inode (DOC_FILE_INO_CLASS, doc_id));
}

static void
dirbuf_add_tmp_files (fuse_req_t req,
                      struct dirbuf *b,
                      guint64 dir_inode)
{
  GList *l;

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
  g_autofree char *basename = NULL;
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

    case DOC_DIR_INO_CLASS:
      dirbuf_add (req, &b, ".", ino);
      dirbuf_add (req, &b, "..", FUSE_ROOT_ID);
      dirbuf_add_doc_file (req, &b, entry, class_ino);
      dirbuf_add_tmp_files (req, &b, ino);
      break;

    case APP_DOC_DIR_INO_CLASS:
      dirbuf_add (req, &b, ".", ino);
      dirbuf_add (req, &b, "..", make_inode (APP_DIR_INO_CLASS,
                                             get_app_id_from_app_doc_ino (class_ino)));
      dirbuf_add_doc_file (req, &b, entry,
                           get_doc_id_from_app_doc_ino (class_ino));
      dirbuf_add_tmp_files (req, &b, ino);
      break;

    case DOC_FILE_INO_CLASS:
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
create_tmp_for_doc (XdgAppDbEntry *entry, int flags, int *fd_out)
{
  g_autofree char *dirname = xdp_dup_dirname (entry);
  g_autofree char *basename = xdp_dup_basename (entry);
  g_autofree char *template = g_strconcat (dirname, "/.", basename, ".XXXXXX", NULL);
  int fd;

  fd = g_mkstemp_full (template, flags, 0600);
  if (fd == -1)
    return NULL;

  *fd_out = fd;
  return g_steal_pointer (&template);
}


static XdpTmp *
tmpfile_new (fuse_ino_t parent,
             const char *name,
             XdgAppDbEntry *entry,
             int flags,
             int *fd_out)
{
  XdpTmp *tmp;
  g_autofree char *path = NULL;
  int fd;

  path = create_tmp_for_doc (entry, flags, &fd);
  if (path == NULL)
    return NULL;

  tmp = g_new0 (XdpTmp, 1);
  tmp->parent_inode = parent;
  tmp->name = g_strdup (name);
  tmp->backing_path = g_steal_pointer (&path);
  tmp->tmp_id = next_tmp_id++;

  if (fd_out)
    *fd_out = fd;
  else
    close (fd);

  tmp_files = g_list_prepend (tmp_files, tmp);

  return tmp;
}

static void
tmpfile_free (XdpTmp *tmp)
{
  GList *l;

  tmp_files = g_list_remove (tmp_files, tmp);

  for (l = open_files; l != NULL; l = l->next)
    {
      XdpFh *fh = l->data;
      if (fh->tmp_id == tmp->tmp_id)
        fh->tmp_id = 0;
    }

  if (tmp->backing_path)
    unlink (tmp->backing_path);

  g_free (tmp->name);
  g_free (tmp->backing_path);
  g_free (tmp);
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
  const char *path = NULL;
  XdpTmp *tmp;
  int fd, res;
  XdpFh *fh;

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

  if (entry && class == DOC_FILE_INO_CLASS)
    {
      g_autofree char *write_path = NULL;
      int write_fd = -1;

      path = xdp_get_path (entry);

      if ((fi->flags & 3) != O_RDONLY)
        {
          if (access (path, W_OK) != 0)
            {
              fuse_reply_err (req, errno);
              return;
            }
          write_path = create_tmp_for_doc (entry, O_RDWR, &write_fd);
          if (write_path == NULL)
            {
              fuse_reply_err (req, errno);
              return;
            }
        }

      fd = open (path, O_RDONLY);
      if (fd < 0)
        {
          int errsv = errno;
          if (write_fd >= 0)
            close (write_fd);
          fuse_reply_err (req, errsv);
          return;
        }
      fh = xdp_fh_new (ino, fi, fd, NULL);
      fh->trunc_fd = write_fd;
      fh->trunc_path = g_steal_pointer (&write_path);
      fh->real_path = g_strdup (path);
      if (fuse_reply_open (req, fi))
        xdp_fh_free (fh);
    }
  else if (class == TMPFILE_INO_CLASS &&
           (tmp = find_tmp_by_id (class_ino)))
    {
      fd = open (tmp->backing_path, get_open_flags (fi));
      if (fd < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
      fh = xdp_fh_new (ino, fi, fd, tmp);
      if (fuse_reply_open (req, fi))
        xdp_fh_free (fh);
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
  struct stat stbuf;
  XdpFh *fh;
  g_autoptr(XdgAppDbEntry) entry = NULL;
  g_autofree char *basename = NULL;
  const char *path = NULL;
  XdpTmp *tmpfile;
  int fd, res;

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

  if (parent_class != APP_DOC_DIR_INO_CLASS &&
      parent_class != DOC_DIR_INO_CLASS)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  basename = xdp_dup_basename (entry);
  if (strcmp (name, basename) == 0)
    {
      g_autofree char *write_path = NULL;
      int write_fd = -1;
      guint32 doc_id = xdp_id_from_name (name);

      write_path = create_tmp_for_doc (entry, O_RDWR, &write_fd);
      if (write_path == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }

      path = xdp_get_path (entry);

      fd = open (path, O_CREAT|O_EXCL|O_RDONLY);
      if (fd < 0)
        {
          int errsv = errno;
          if (write_fd >= 0)
            close (write_fd);
          fuse_reply_err (req, errsv);
          return;
        }

      e.ino = make_inode (DOC_FILE_INO_CLASS, doc_id);

      fh = xdp_fh_new (e.ino, fi, fd, NULL);
      fh->truncated = TRUE;
      fh->trunc_fd = write_fd;
      fh->trunc_path = g_steal_pointer (&write_path);
      fh->real_path = g_strdup (path);

      if (xdp_fstat (fh, &e.attr) != 0)
        {
          xdp_fh_free (fh);
          fuse_reply_err (req, EIO);
          return;
        }

      e.attr_timeout = get_attr_cache_time (e.attr.st_mode);
      e.entry_timeout = get_entry_cache_time (e.attr.st_mode);

      if (fuse_reply_create (req, &e, fi))
        xdp_fh_free (fh);
    }
  else
    {
      tmpfile = find_tmp_by_name (parent, name);
      if (tmpfile != NULL && fi->flags & O_EXCL)
        {
          fuse_reply_err (req, EEXIST);
          return;
        }

      if (tmpfile)
        {
          fd = open (tmpfile->backing_path, get_open_flags (fi));
          if (fd == -1)
            {
              fuse_reply_err (req, errno);
              return;
            }
        }
      else
        {
          tmpfile = tmpfile_new (parent, name, entry, get_open_flags (fi), &fd);
          if (tmpfile == NULL)
            {
              fuse_reply_err (req, errno);
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
      e.entry_timeout = get_entry_cache_time (e.attr.st_mode);

      fh = xdp_fh_new (e.ino, fi, fd, tmpfile);
      if (fuse_reply_create (req, &e, fi))
        xdp_fh_free (fh);
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

  fd = xdp_fh_get_fd (fh);
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

  if (fh->readonly)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  fd = xdp_fh_get_fd (fh);
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

  if (fh->readonly)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  fd = xdp_fh_get_fd (fh);
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
  xdp_fh_free (fh);
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
  g_autoptr (XdgAppDbEntry) entry = NULL;
  int res;
  fuse_ino_t inode;
  struct stat stbuf = {0};
  g_autofree char *basename = NULL;
  XdpTmp *other_tmp, *tmp;
  GList *l;

  g_debug ("xdp_fuse_rename %lx/%s -> %lx/%s", parent, name, newparent, newname);

  res = xdp_lookup (parent, name,  &inode, &stbuf, &entry, &tmp);
  if (res != 0)
    {
      fuse_reply_err (req, res);
      return;
    }

  /* Only allow renames in (app) doc dirs, and only inside the same dir */
  if ((parent_class != DOC_DIR_INO_CLASS &&
       parent_class != APP_DOC_DIR_INO_CLASS) ||
      parent != newparent ||
      entry == NULL ||
      /* Also, don't allow renaming non-tmpfiles */
      tmp == NULL)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  basename = xdp_dup_basename (entry);

  if (strcmp (newname, basename) == 0)
    {
      const char *real_path = xdp_get_path (entry);
      /* Rename tmpfile to regular file */

      /* Stop writes to all outstanding fds to the temp file */
      for (l = open_files; l != NULL; l = l->next)
        {
          XdpFh *fh = l->data;
          if (fh->tmp_id == tmp->tmp_id && fh->fd >= 0)
            fh->readonly = TRUE;
        }

      if (rename (tmp->backing_path, real_path) != 0)
        {
          fuse_reply_err (req, errno);
          return;
        }

      /* Clear backing path so we don't unlink it when freeing tmp */
      g_clear_pointer (&tmp->backing_path, g_free);
      tmpfile_free (tmp);

      fuse_reply_err (req, 0);
    }
  else
    {
      /* Rename tmpfile to other tmpfile name */

      other_tmp = find_tmp_by_name (newparent, newname);
      if (other_tmp)
        tmpfile_free (other_tmp);

      g_free (tmp->name);
      tmp->name = g_strdup (newname);
      fuse_reply_err (req, 0);
   }
}

static int
fh_truncate (XdpFh *fh, off_t size, struct stat  *newattr)
{
  int fd;

  if (fh->trunc_fd >= 0 && !fh->truncated)
    {
      if (size != 0)
        return -EACCES;

      fh->truncated = TRUE;
      fd = fh->trunc_fd;
    }
  else
    {
      fd = xdp_fh_get_fd (fh);
      if (fd == -1)
        return -EIO;

      if (ftruncate (fd, size) != 0)
        return - errno;
    }

  if (newattr)
    {
      int res = xdp_fstat (fh, newattr);
      if (res < 0)
        return res;
    }

  return 0;
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

      res = fh_truncate (fh, attr->st_size, &newattr);
      if (res < 0)
        {
          fuse_reply_err (req, res);
          return;
        }

      fuse_reply_attr (req, &newattr, get_attr_cache_time (newattr.st_mode));
    }
  else if (to_set == FUSE_SET_ATTR_SIZE && fi == NULL)
    {
      gboolean found = FALSE;
      int res = 0;
      GList *l;
      struct stat newattr = {0};
      struct stat *newattrp = &newattr;

      /* truncate, truncate any open files (but EACCES if not open) */

      for (l = open_files; l != NULL; l = l->next)
        {
          XdpFh *fh = l->data;
          if (fh->inode == ino)
            {
              found = TRUE;
              res = fh_truncate (fh, attr->st_size, newattrp);
              newattrp = NULL;
            }
        }

      if (!found)
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
      GList *l;
      struct stat newattr = {0};

      for (l = open_files; l != NULL; l = l->next)
        {
          XdpFh *fh = l->data;

          if (fh->inode == ino)
            {
              int fd = xdp_fh_get_fd (fh);
              if (fd != -1)
                {
                  res = fchmod (fd, get_user_perms (attr));
                  if (!found)
                    {
                      if (res != 0)
                        err = -errno;
                      else
                        err = xdp_fstat (fh, &newattr);
                      found = TRUE;
                    }
                }
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

  if (class == DOC_DIR_INO_CLASS ||
      class == APP_DOC_DIR_INO_CLASS)
    {
      g_autoptr (XdgAppDbEntry) entry = NULL;
      if (class == APP_DOC_DIR_INO_CLASS)
        doc_id = get_doc_id_from_app_doc_ino (class_ino);
      else
        doc_id = class_ino;

      entry = xdp_lookup_doc (doc_id);
      if (entry != NULL)
        {
          g_autofree char *dirname = xdp_dup_dirname (entry);
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

  if (class == DOC_FILE_INO_CLASS ||
      class == TMPFILE_INO_CLASS)
    {
      XdpFh *fh = (gpointer)fi->fh;
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
  g_autoptr (XdgAppDbEntry) entry = NULL;
  int res;
  fuse_ino_t inode;
  struct stat stbuf = {0};
  g_autofree char *basename = NULL;
  XdpTmp *tmp;

  g_debug ("xdp_fuse_unlink %lx/%s", parent, name);

  res = xdp_lookup (parent, name,  &inode, &stbuf, &entry, &tmp);
  if (res != 0)
    {
      fuse_reply_err (req, res);
      return;
    }

  /* Only allow unlink in (app) doc dirs */
  if ((parent_class != DOC_DIR_INO_CLASS &&
       parent_class != APP_DOC_DIR_INO_CLASS) ||
      entry == NULL)
    {
      fuse_reply_err (req, EACCES);
      return;
    }

  basename = xdp_dup_basename (entry);
  if (strcmp (name, basename) == 0)
    {
      const char *real_path = xdp_get_path (entry);

      if (unlink (real_path) != 0)
        {
          fuse_reply_err (req, errno);
          return;
        }

      fuse_reply_err (req, 0);
    }
  else
    {
      tmpfile_free (tmp);

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

typedef struct
{
  GSource     source;

  struct fuse_chan *ch;
  gpointer    fd_tag;
} FuseSource;

static gboolean
fuse_source_dispatch (GSource     *source,
                      GSourceFunc  func,
                      gpointer     user_data)
{
  FuseSource *fs = (FuseSource *)source;
  struct fuse_chan *ch = fs->ch;
  struct fuse_session *se = fuse_chan_session (ch);
  gsize bufsize = fuse_chan_bufsize (ch);

  if (g_source_query_unix_fd (source, fs->fd_tag) != 0)
    {
      int res = 0;
      char *buf = (char *) g_malloc (bufsize);

      while (TRUE)
        {
          struct fuse_chan *tmpch = ch;
          struct fuse_buf fbuf = {
            .mem = buf,
            .size = bufsize,
          };
          res = fuse_session_receive_buf (se, &fbuf, &tmpch);
          if (res == -EINTR)
            continue;
          if (res <= 0)
            break;

          fuse_session_process_buf (se, &fbuf, tmpch);
        }
      g_free (buf);
    }

  return TRUE;
}

static GSource *
fuse_source_new (struct fuse_chan *ch)
{
  static GSourceFuncs source_funcs = {
    NULL, NULL,
    fuse_source_dispatch
    /* should have a finalize, but it will never happen */
  };
  FuseSource *fs;
  GSource *source;
  GError *error = NULL;
  int fd;

  source = g_source_new (&source_funcs, sizeof (FuseSource));
  fs = (FuseSource *) source;
  fs->ch = ch;

  g_source_set_name (source, "fuse source");

  fd = fuse_chan_fd(ch);
  g_unix_set_fd_nonblocking (fd, TRUE, &error);
  g_assert_no_error (error);

  fs->fd_tag = g_source_add_unix_fd (source, fd, G_IO_IN);

  return source;
}

static struct fuse_session *session = NULL;
static struct fuse_chan *main_ch = NULL;
static char *mount_path = NULL;

const char *
xdp_fuse_get_mountpoint (void)
{
  return mount_path;
}

void
xdp_fuse_exit (void)
{
  if (session)
    fuse_session_reset (session);
  if (main_ch)
    fuse_session_remove_chan (main_ch);
  if (session)
    fuse_session_destroy (session);
  if (main_ch)
    fuse_unmount (mount_path, main_ch);
}

gboolean
xdp_fuse_init (GError **error)
{
  char *argv[] = { "xdp-fuse", "-osplice_write,splice_move,splice_read" };
  struct fuse_args args = FUSE_ARGS_INIT(G_N_ELEMENTS(argv), argv);
  struct fuse_chan *ch;
  GSource *source;

  app_name_to_id =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  app_id_to_name =
    g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
  next_app_id = 1;
  next_tmp_id = 1;

  mount_path = g_build_filename (g_get_user_runtime_dir(), "doc", NULL);
  if (g_mkdir_with_parents  (mount_path, 0700))
    {
      g_set_error (error, XDP_ERROR, XDP_ERROR_FAILED,
                   "Unable to create dir %s\n", mount_path);
      return FALSE;
    }

  main_ch = ch = fuse_mount (mount_path, &args);
  if (ch == NULL)
    {
      g_set_error (error, XDP_ERROR, XDP_ERROR_FAILED, "Can't mount fuse fs");
      return FALSE;
    }

  session = fuse_lowlevel_new (&args, &xdp_fuse_oper,
                               sizeof (xdp_fuse_oper), NULL);
  if (session == NULL)
    {
      g_set_error (error, XDP_ERROR, XDP_ERROR_FAILED,
                   "Can't create fuse session");
      return FALSE;
    }

  fuse_session_add_chan (session, ch);

  source = fuse_source_new (ch);
  g_source_attach (source, NULL);

  return TRUE;
}
