/* gskgliconlibrary.c
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

#include "gskgliconlibraryprivate.h"

struct _GskGLIconLibrary
{
  GskGLTextureLibrary parent_instance;
};

G_DEFINE_TYPE (GskGLIconLibrary, gsk_gl_icon_library, GSK_TYPE_GL_TEXTURE_LIBRARY)

GskGLIconLibrary *
gsk_gl_icon_library_new (GdkGLContext *context)
{
  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), NULL);

  return g_object_new (GSK_TYPE_GL_ICON_LIBRARY,
                       "context", context,
                       NULL);
}

static void
gsk_gl_icon_library_class_init (GskGLIconLibraryClass *klass)
{
}

static void
gsk_gl_icon_library_init (GskGLIconLibrary *self)
{
}