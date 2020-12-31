/*
 * Copyright © 2020 Benjamin Otte
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
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Benjamin Otte <otte@gnome.org>
 */

#ifndef __OTTIE_PATH_VALUE_PRIVATE_H__
#define __OTTIE_PATH_VALUE_PRIVATE_H__

#include <json-glib/json-glib.h>

#include <gsk/gsk.h>

G_BEGIN_DECLS

typedef struct _OttiePathValue OttiePathValue;

struct _OttiePathValue
{
  gboolean is_static;
  union {
    gpointer static_value;
    gpointer keyframes;
  };
};

void                      ottie_path_value_init                 (OttiePathValue         *self);
void                      ottie_path_value_clear                (OttiePathValue         *self);

GskPath *                 ottie_path_value_get                  (OttiePathValue         *self,
                                                                 double                  timestamp,
                                                                 gboolean                reverse);

gboolean                  ottie_path_value_parse                (JsonReader             *reader,
                                                                 gsize                   offset,
                                                                 gpointer                data);

G_END_DECLS

#endif /* __OTTIE_PATH_VALUE_PRIVATE_H__ */
