#ifndef XDP_ENUMS_H
#define XDP_ENUMS_H

G_BEGIN_DECLS

typedef enum {
  XDP_PERMISSION_FLAGS_READ               = (1 << 0),
  XDP_PERMISSION_FLAGS_WRITE              = (1 << 1),
  XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS  = (1 << 2),
  XDP_PERMISSION_FLAGS_DELETE             = (1 << 3),

  XDP_PERMISSION_FLAGS_ALL               = ((1 << 4) - 1)
} XdpPermissionFlags;

typedef enum {
  XDP_ADD_FLAGS_REUSE_EXISTING             = (1 << 0),
  XDP_ADD_FLAGS_PERSISTENT                 = (1 << 1),
  XDP_ADD_FLAGS_AS_NEEDED_BY_APP           = (1 << 2),

  XDP_ADD_FLAGS_FLAGS_ALL                  = ((1 << 3) - 1)
} XdpAddFullFlags;

G_END_DECLS

#endif /* XDP_ENUMS_H */
