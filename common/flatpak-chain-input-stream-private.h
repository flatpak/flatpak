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
 *
 */

#pragma once

#ifndef __GI_SCANNER__

#include <gio/gio.h>

G_BEGIN_DECLS

#define FLATPAK_TYPE_CHAIN_INPUT_STREAM (flatpak_chain_input_stream_get_type ())
#define FLATPAK_CHAIN_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), FLATPAK_TYPE_CHAIN_INPUT_STREAM, FlatpakChainInputStream))
#define FLATPAK_CHAIN_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k), FLATPAK_TYPE_CHAIN_INPUT_STREAM, FlatpakChainInputStreamClass))
#define FLATPAK_IS_CHAIN_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), FLATPAK_TYPE_CHAIN_INPUT_STREAM))
#define FLATPAK_IS_CHAIN_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), FLATPAK_TYPE_CHAIN_INPUT_STREAM))
#define FLATPAK_CHAIN_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), FLATPAK_TYPE_CHAIN_INPUT_STREAM, FlatpakChainInputStreamClass))

typedef struct _FlatpakChainInputStream        FlatpakChainInputStream;
typedef struct _FlatpakChainInputStreamClass   FlatpakChainInputStreamClass;
typedef struct _FlatpakChainInputStreamPrivate FlatpakChainInputStreamPrivate;

struct _FlatpakChainInputStream
{
  GInputStream parent_instance;
};

struct _FlatpakChainInputStreamClass
{
  GInputStreamClass parent_class;

  /*< private >*/
  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType          flatpak_chain_input_stream_get_type (void) G_GNUC_CONST;

FlatpakChainInputStream * flatpak_chain_input_stream_new (GPtrArray *streams);

G_END_DECLS

#endif
