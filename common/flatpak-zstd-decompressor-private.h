#ifndef __FLATPAK_ZSTD_DECOMPRESSOR_H__
#define __FLATPAK_ZSTD_DECOMPRESSOR_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define FLATPAK_TYPE_ZSTD_DECOMPRESSOR flatpak_zstd_decompressor_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakZstdDecompressor,
                      flatpak_zstd_decompressor,
                      FLATPAK, ZSTD_DECOMPRESSOR,
                      GObject)

FlatpakZstdDecompressor *flatpak_zstd_decompressor_new (void);

G_END_DECLS

#endif /* __FLATPAK_ZSTD_DECOMPRESSOR_H__ */
