/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "flatpak-chain-input-stream-private.h"

enum {
  PROP_0,
  PROP_STREAMS
};

struct _FlatpakChainInputStreamPrivate
{
  GPtrArray *streams;
  guint      index;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakChainInputStream, flatpak_chain_input_stream, G_TYPE_INPUT_STREAM)


static void     flatpak_chain_input_stream_set_property (GObject      *object,
                                                         guint         prop_id,
                                                         const GValue *value,
                                                         GParamSpec   *pspec);
static void     flatpak_chain_input_stream_get_property (GObject    *object,
                                                         guint       prop_id,
                                                         GValue     *value,
                                                         GParamSpec *pspec);
static void     flatpak_chain_input_stream_finalize (GObject *object);
static gssize   flatpak_chain_input_stream_read (GInputStream *stream,
                                                 void         *buffer,
                                                 gsize         count,
                                                 GCancellable *cancellable,
                                                 GError      **error);
static gboolean flatpak_chain_input_stream_close (GInputStream *stream,
                                                  GCancellable *cancellable,
                                                  GError      **error);

static void
flatpak_chain_input_stream_class_init (FlatpakChainInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);

  gobject_class->get_property = flatpak_chain_input_stream_get_property;
  gobject_class->set_property = flatpak_chain_input_stream_set_property;
  gobject_class->finalize     = flatpak_chain_input_stream_finalize;

  stream_class->read_fn = flatpak_chain_input_stream_read;
  stream_class->close_fn = flatpak_chain_input_stream_close;

  /*
   * FlatpakChainInputStream:streams: (element-type GInputStream)
   *
   * Chain of input streams read in order.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_STREAMS,
                                   g_param_spec_pointer ("streams",
                                                         "", "",
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS));
}

static void
flatpak_chain_input_stream_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  FlatpakChainInputStream *self;
  FlatpakChainInputStreamPrivate *priv;

  self = FLATPAK_CHAIN_INPUT_STREAM (object);
  priv = flatpak_chain_input_stream_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_STREAMS:
      priv->streams = g_ptr_array_ref (g_value_get_pointer (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_chain_input_stream_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  FlatpakChainInputStream *self;
  FlatpakChainInputStreamPrivate *priv;

  self = FLATPAK_CHAIN_INPUT_STREAM (object);
  priv = flatpak_chain_input_stream_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_STREAMS:
      g_value_set_pointer (value, priv->streams);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
flatpak_chain_input_stream_finalize (GObject *object)
{
  FlatpakChainInputStream *stream;
  FlatpakChainInputStreamPrivate *priv;

  stream = (FlatpakChainInputStream *) (object);
  priv = flatpak_chain_input_stream_get_instance_private (stream);

  g_ptr_array_unref (priv->streams);

  G_OBJECT_CLASS (flatpak_chain_input_stream_parent_class)->finalize (object);
}

static void
flatpak_chain_input_stream_init (FlatpakChainInputStream *self)
{
}

FlatpakChainInputStream *
flatpak_chain_input_stream_new (GPtrArray *streams)
{
  FlatpakChainInputStream *stream;

  stream = g_object_new (FLATPAK_TYPE_CHAIN_INPUT_STREAM,
                         "streams", streams,
                         NULL);

  return (FlatpakChainInputStream *) (stream);
}

static gssize
flatpak_chain_input_stream_read (GInputStream *stream,
                                 void         *buffer,
                                 gsize         count,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  FlatpakChainInputStream *self = (FlatpakChainInputStream *) stream;
  FlatpakChainInputStreamPrivate *priv = flatpak_chain_input_stream_get_instance_private (self);
  GInputStream *child;
  gssize res = -1;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;

  if (priv->index >= priv->streams->len)
    return 0;

  res = 0;
  while (res == 0 && priv->index < priv->streams->len)
    {
      child = priv->streams->pdata[priv->index];
      res = g_input_stream_read (child,
                                 buffer,
                                 count,
                                 cancellable,
                                 error);
      if (res == 0)
        priv->index++;
    }

  return res;
}

static gboolean
flatpak_chain_input_stream_close (GInputStream *stream,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  gboolean ret = FALSE;
  FlatpakChainInputStream *self = (gpointer) stream;
  FlatpakChainInputStreamPrivate *priv = flatpak_chain_input_stream_get_instance_private (self);
  guint i;

  for (i = 0; i < priv->streams->len; i++)
    {
      GInputStream *child = priv->streams->pdata[i];
      if (!g_input_stream_close (child, cancellable, error))
        goto out;
    }

  ret = TRUE;
out:
  return ret;
}
