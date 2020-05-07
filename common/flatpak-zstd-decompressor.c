#include "config.h"

#include "flatpak-zstd-decompressor-private.h"

#include <errno.h>
#include <zstd.h>
#include <string.h>

static void flatpak_zstd_decompressor_iface_init  (GConverterIface *iface);

struct _FlatpakZstdDecompressor
{
  GObject parent_instance;

  ZSTD_DStream *dstream;
};

G_DEFINE_TYPE_WITH_CODE (FlatpakZstdDecompressor, flatpak_zstd_decompressor, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_CONVERTER,
                                                flatpak_zstd_decompressor_iface_init))

static void
flatpak_zstd_decompressor_finalize (GObject *object)
{
  FlatpakZstdDecompressor *decompressor;

  decompressor = FLATPAK_ZSTD_DECOMPRESSOR (object);

  ZSTD_freeDStream (decompressor->dstream);

  G_OBJECT_CLASS (flatpak_zstd_decompressor_parent_class)->finalize (object);
}

static void
flatpak_zstd_decompressor_init (FlatpakZstdDecompressor *decompressor)
{
  decompressor->dstream = ZSTD_createDStream ();
}

static void
flatpak_zstd_decompressor_class_init (FlatpakZstdDecompressorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = flatpak_zstd_decompressor_finalize;
}

FlatpakZstdDecompressor *
flatpak_zstd_decompressor_new (void)
{
  FlatpakZstdDecompressor *decompressor;

  decompressor = g_object_new (FLATPAK_TYPE_ZSTD_DECOMPRESSOR, NULL);

  return decompressor;
}

static void
flatpak_zstd_decompressor_reset (GConverter *converter)
{
  FlatpakZstdDecompressor *decompressor = FLATPAK_ZSTD_DECOMPRESSOR (converter);

  ZSTD_initDStream (decompressor->dstream);
}

static GConverterResult
flatpak_zstd_decompressor_convert (GConverter *converter,
                                   const void *inbuf,
                                   gsize       inbuf_size,
                                   void       *outbuf,
                                   gsize       outbuf_size,
                                   GConverterFlags flags,
                                   gsize      *bytes_read,
                                   gsize      *bytes_written,
                                   GError    **error)
{
  FlatpakZstdDecompressor *decompressor;
  ZSTD_inBuffer input = { inbuf, inbuf_size, 0 };
  ZSTD_outBuffer output = {outbuf, outbuf_size, 0 };
  size_t res;

  decompressor = FLATPAK_ZSTD_DECOMPRESSOR (converter);

  if (decompressor->dstream == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Failed to initialize libzst");
      return G_CONVERTER_ERROR;
    }


  res = ZSTD_decompressStream(decompressor->dstream, &output , &input);
  if (ZSTD_isError (res))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Zstd decompression error: %s", ZSTD_getErrorName (res));
      return G_CONVERTER_ERROR;
    }

  *bytes_read = input.pos;
  *bytes_written = output.pos;

  if (res == 0)
    return G_CONVERTER_FINISHED;

  if (input.pos == 0 && output.pos == 0)
    {
      /* Did nothing, need more input? */
      if (flags & G_CONVERTER_INPUT_AT_END)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Zstd failed");
      else
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT, "Need more zstd data");

      return G_CONVERTER_ERROR;
    }

  return G_CONVERTER_CONVERTED;
}

static void
flatpak_zstd_decompressor_iface_init (GConverterIface *iface)
{
  iface->convert = flatpak_zstd_decompressor_convert;
  iface->reset = flatpak_zstd_decompressor_reset;
}
