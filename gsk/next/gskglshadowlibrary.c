/* gskglshadowlibrary.c
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "gskgldriverprivate.h"
#include "gskglshadowlibraryprivate.h"

struct _GskGLShadowLibrary
{
  GskGLTextureLibrary parent_instance;
};

G_DEFINE_TYPE (GskGLShadowLibrary, gsk_gl_shadow_library, GSK_TYPE_GL_TEXTURE_LIBRARY)

GskGLShadowLibrary *
gsk_gl_shadow_library_new (GskNextDriver *driver)
{
  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (driver), NULL);

  return g_object_new (GSK_TYPE_GL_SHADOW_LIBRARY,
                       "driver", driver,
                       NULL);
}

static void
gsk_gl_shadow_library_class_init (GskGLShadowLibraryClass *klass)
{
}

static void
gsk_gl_shadow_library_init (GskGLShadowLibrary *self)
{
  gsk_gl_texture_library_set_funcs (GSK_GL_TEXTURE_LIBRARY (self),
                                    NULL, NULL, NULL, NULL);
}