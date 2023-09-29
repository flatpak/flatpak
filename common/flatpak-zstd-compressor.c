/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2023 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Based on gzlibcompressor.h:
 *     Author: Alexander Larsson <alexl@redhat.com>
 * Author: Owen Taylor <otaylor@redhat.com>
 */

#include "config.h"

#include "flatpak-zstd-compressor-private.h"

#include <errno.h>
#include <string.h>
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

struct _FlatpakZstdCompressor
{
  GObject parent_instance;

  int level;
#ifdef HAVE_ZSTD
  ZSTD_CStream *cstream;
#endif
};

static void flatpak_zstd_compressor_iface_init (GConverterIface *iface);

G_DEFINE_TYPE_WITH_CODE (FlatpakZstdCompressor,
                         flatpak_zstd_compressor,
                         G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_CONVERTER,
						flatpak_zstd_compressor_iface_init))

static GConverterResult
flatpak_zstd_compressor_convert (GConverter *converter,
                                 const void *inbuf,
                                 gsize       inbuf_size,
                                 void       *outbuf,
                                 gsize       outbuf_size,
                                 GConverterFlags flags,
                                 gsize      *bytes_read,
                                 gsize      *bytes_written,
                                 GError    **error)
{
#ifdef HAVE_ZSTD
  FlatpakZstdCompressor *compressor = FLATPAK_ZSTD_COMPRESSOR (converter);
  ZSTD_inBuffer input = { inbuf, inbuf_size, 0 };
  ZSTD_outBuffer output = {outbuf, outbuf_size, 0 };
  ZSTD_EndDirective end_op;
  size_t res;

  end_op = ZSTD_e_continue;
  if (flags & G_CONVERTER_INPUT_AT_END)
    end_op = ZSTD_e_end;
  else if (flags & G_CONVERTER_FLUSH)
    end_op = ZSTD_e_flush;

  res = ZSTD_compressStream2 (compressor->cstream, &output, &input, end_op);
  if (ZSTD_isError (res))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Zstd compression error: %s", ZSTD_getErrorName (res));
      return G_CONVERTER_ERROR;
    }

  *bytes_read = input.pos;
  *bytes_written = output.pos;

  if (flags & G_CONVERTER_INPUT_AT_END && res == 0)
    return G_CONVERTER_FINISHED;

  /* We should make some progress */
  g_assert (input.pos != 0 || output.pos != 0);
  return G_CONVERTER_CONVERTED;

#else
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "libzstd not available");
  return G_CONVERTER_ERROR;
#endif
}

static void
flatpak_zstd_compressor_reset (GConverter *converter)
{
#ifdef HAVE_ZSTD
  FlatpakZstdCompressor *compressor = FLATPAK_ZSTD_COMPRESSOR (converter);

  ZSTD_initCStream(compressor->cstream, compressor->level);
#endif
}

static void
flatpak_zstd_compressor_iface_init (GConverterIface *iface)
{
  iface->convert = flatpak_zstd_compressor_convert;
  iface->reset = flatpak_zstd_compressor_reset;
}

static void
flatpak_zstd_compressor_finalize (GObject *object)
{
#ifdef HAVE_ZSTD
  FlatpakZstdCompressor *compressor = FLATPAK_ZSTD_COMPRESSOR (object);

  ZSTD_freeCStream (compressor->cstream);
#endif

  G_OBJECT_CLASS (flatpak_zstd_compressor_parent_class)->finalize (object);
}

static void
flatpak_zstd_compressor_class_init (FlatpakZstdCompressorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = flatpak_zstd_compressor_finalize;
}

static void
flatpak_zstd_compressor_init (FlatpakZstdCompressor *compressor)
{
}

FlatpakZstdCompressor *
flatpak_zstd_compressor_new (int level)
{
  FlatpakZstdCompressor *compressor;

  compressor = g_object_new (FLATPAK_TYPE_ZSTD_COMPRESSOR, NULL);

#ifdef HAVE_ZSTD
  compressor->level = level < 0 ? ZSTD_CLEVEL_DEFAULT : level;

  compressor->cstream = ZSTD_createCStream ();
  if (!compressor->cstream)
    g_error ("FlatpakZstdCompressor: Not enough memory for zstd use");

  flatpak_zstd_compressor_reset (G_CONVERTER (compressor));
#endif
  return compressor;
}
