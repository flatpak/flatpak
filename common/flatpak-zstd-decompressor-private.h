#ifndef __FLATPAK_ZSTD_DECOMPRESSOR_H__
#define __FLATPAK_ZSTD_DECOMPRESSOR_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define FLATPAK_TYPE_ZSTD_DECOMPRESSOR         (flatpak_zstd_decompressor_get_type ())
#define FLATPAK_ZSTD_DECOMPRESSOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), FLATPAK_TYPE_ZSTD_DECOMPRESSOR, FlatpakZstdDecompressor))
#define FLATPAK_ZSTD_DECOMPRESSOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), FLATPAK_TYPE_ZSTD_DECOMPRESSOR, FlatpakZstdDecompressorClass))
#define G_IS_ZSTD_DECOMPRESSOR(o)              (G_TYPE_CHECK_INSTANCE_TYPE ((o), FLATPAK_TYPE_ZSTD_DECOMPRESSOR))
#define G_IS_ZSTD_DECOMPRESSOR_CLASS(k)        (G_TYPE_CHECK_CLASS_TYPE ((k), FLATPAK_TYPE_ZSTD_DECOMPRESSOR))
#define FLATPAK_ZSTD_DECOMPRESSOR_GET_CLASS(o) (FLATPAK_TYPE_INSTANCE_GET_CLASS ((o), FLATPAK_TYPE_ZSTD_DECOMPRESSOR, FlatpakZstdDecompressorClass))

typedef struct _FlatpakZstdDecompressor   FlatpakZstdDecompressor;
typedef struct _FlatpakZstdDecompressorClass   FlatpakZstdDecompressorClass;

struct _FlatpakZstdDecompressorClass
{
  GObjectClass parent_class;
};

GLIB_AVAILABLE_IN_ALL
GType              flatpak_zstd_decompressor_get_type (void) G_GNUC_CONST;

GLIB_AVAILABLE_IN_ALL
FlatpakZstdDecompressor *flatpak_zstd_decompressor_new (void);

G_END_DECLS

#endif /* __FLATPAK_ZSTD_DECOMPRESSOR_H__ */
