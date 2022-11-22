# Copyright (C) 2016 Colin Walters <walters@verbum.org>
# Copyright (C) 2018 Alexander Larsson <alexl@redhat.com>
#
# SPDX-License-Identifier: LGPL-2.0+
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

EXTRA_DIST += revokefs/meson.build

libexec_PROGRAMS += revokefs-fuse

noinst_PROGRAMS += revokefs-demo

revokefs_fuse_SOURCES = revokefs/main.c revokefs/writer.c revokefs/writer.h

revokefs_fuse_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 $(FUSE_CFLAGS)
revokefs_fuse_LDADD = libglnx.la $(BASE_LIBS) $(FUSE_LIBS)

revokefs_demo_SOURCES = revokefs/demo.c
revokefs_demo_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS)
revokefs_demo_LDADD = $(BASE_LIBS)
