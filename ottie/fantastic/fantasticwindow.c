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

#include "config.h"

#include "fantasticwindow.h"

#include "fantasticobserver.h"

#include "ottie/ottiecreationprivate.h"
#include "ottie/ottiecompositionlayerprivate.h"
#include "ottie/ottiegroupshapeprivate.h"
#include "ottie/ottiepaintableprivate.h"
#include "ottie/ottieshapelayerprivate.h"

struct _FantasticWindow
{
  GtkApplicationWindow parent;

  GFileMonitor *file_monitor;

  OttieCreation *creation;
  OttiePaintable *paintable;
  FantasticObserver *observer;

  GtkWidget *picture;
  GtkWidget *listview;
  GtkSingleSelection *selection;
};

struct _FantasticWindowClass
{
  GtkApplicationWindowClass parent_class;
};

G_DEFINE_TYPE(FantasticWindow, fantastic_window, GTK_TYPE_APPLICATION_WINDOW);

static gboolean
load_file_contents (FantasticWindow *self,
                    GFile             *file)
{
  GBytes *bytes;

  bytes = g_file_load_bytes (file, NULL, NULL, NULL);
  if (bytes == NULL)
    return FALSE;

  if (!g_utf8_validate (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), NULL))
    {
      g_bytes_unref (bytes);
      return FALSE;
    }

  ottie_creation_load_bytes (self->creation, bytes);
#if 0
  gtk_text_buffer_set_text (self->text_buffer,
                            g_bytes_get_data (bytes, NULL),
                            g_bytes_get_size (bytes));
#endif

  g_bytes_unref (bytes);

  return TRUE;
}

static void
file_changed_cb (GFileMonitor      *monitor,
                 GFile             *file,
                 GFile             *other_file,
                 GFileMonitorEvent  event_type,
                 gpointer           user_data)
{
  FantasticWindow *self = user_data;

  if (event_type == G_FILE_MONITOR_EVENT_CHANGED)
    load_file_contents (self, file);
}

void
fantastic_window_load (FantasticWindow *self,
                          GFile            *file)
{
  GError *error = NULL;

  if (!load_file_contents (self, file))
    return;

  g_clear_object (&self->file_monitor);
  self->file_monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &error);

  if (error)
    {
      g_warning ("couldn't monitor file: %s", error->message);
      g_error_free (error);
    }
  else
    {
      g_signal_connect (self->file_monitor, "changed", G_CALLBACK (file_changed_cb), self);
    }
}

static void
open_response_cb (GtkWidget        *dialog,
                  int               response,
                  FantasticWindow *self)
{
  gtk_widget_hide (dialog);

  if (response == GTK_RESPONSE_ACCEPT)
    {
      GFile *file;

      file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));
      fantastic_window_load (self, file);
      g_object_unref (file);
    }

  gtk_window_destroy (GTK_WINDOW (dialog));
}

static void
show_open_filechooser (FantasticWindow *self)
{
  GtkWidget *dialog;

  dialog = gtk_file_chooser_dialog_new ("Open lottie file",
                                        GTK_WINDOW (self),
                                        GTK_FILE_CHOOSER_ACTION_OPEN,
                                        "_Cancel", GTK_RESPONSE_CANCEL,
                                        "_Load", GTK_RESPONSE_ACCEPT,
                                        NULL);

  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

  GFile *cwd = g_file_new_for_path (".");
  gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), cwd, NULL);
  g_object_unref (cwd);

  g_signal_connect (dialog, "response", G_CALLBACK (open_response_cb), self);
  gtk_widget_show (dialog);
}

static void
open_cb (GtkWidget        *button,
         FantasticWindow *self)
{
  show_open_filechooser (self);
}

static void
save_response_cb (GtkWidget        *dialog,
                  int               response,
                  FantasticWindow *self)
{
  gtk_widget_hide (dialog);

  if (response == GTK_RESPONSE_ACCEPT)
    {
#if 0
      GFile *file;
      char *text;
#endif
      GError *error = NULL;

#if 0
      text = get_current_text (self->text_buffer);

      file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));
      g_file_replace_contents (file, text, strlen (text),
                               NULL, FALSE,
                               G_FILE_CREATE_NONE,
                               NULL,
                               NULL,
                               &error);
#endif

      if (error != NULL)
        {
          GtkWidget *message_dialog;

          message_dialog = gtk_message_dialog_new (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))),
                                                   GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_INFO,
                                                   GTK_BUTTONS_OK,
                                                   "Saving failed");
          gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (message_dialog),
                                                    "%s", error->message);
          g_signal_connect (message_dialog, "response", G_CALLBACK (gtk_window_destroy), NULL);
          gtk_widget_show (message_dialog);
          g_error_free (error);
        }

#if 0
      g_free (text);
      g_object_unref (file);
#endif
    }

  gtk_window_destroy (GTK_WINDOW (dialog));
}

static void
save_cb (GtkWidget        *button,
         FantasticWindow *self)
{
  GtkWidget *dialog;

  dialog = gtk_file_chooser_dialog_new ("Save file",
                                        GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (button))),
                                        GTK_FILE_CHOOSER_ACTION_SAVE,
                                        "_Cancel", GTK_RESPONSE_CANCEL,
                                        "_Save", GTK_RESPONSE_ACCEPT,
                                        NULL);

  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

  GFile *cwd = g_file_new_for_path (".");
  gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), cwd, NULL);
  g_object_unref (cwd);

  g_signal_connect (dialog, "response", G_CALLBACK (save_response_cb), self);
  gtk_widget_show (dialog);
}

static GdkTexture *
create_texture (FantasticWindow *self)
{
  GtkSnapshot *snapshot;
  GskRenderer *renderer;
  GskRenderNode *node;
  GdkTexture *texture;
  int width, height;

  width = gdk_paintable_get_intrinsic_width (GDK_PAINTABLE (self->paintable));
  height = gdk_paintable_get_intrinsic_height (GDK_PAINTABLE (self->paintable));

  if (width <= 0 || height <= 0)
    return NULL;
  snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (GDK_PAINTABLE (self->paintable), snapshot, width, height);
  node = gtk_snapshot_free_to_node (snapshot);
  if (node == NULL)
    return NULL;

  renderer = gtk_native_get_renderer (gtk_widget_get_native (GTK_WIDGET (self)));
  texture = gsk_renderer_render_texture (renderer, node, NULL);
  gsk_render_node_unref (node);

  return texture;
}

static void
export_image_response_cb (GtkWidget  *dialog,
                          int         response,
                          GdkTexture *texture)
{
  gtk_widget_hide (dialog);

  if (response == GTK_RESPONSE_ACCEPT)
    {
      GFile *file;

      file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));
      if (!gdk_texture_save_to_png (texture, g_file_peek_path (file)))
        {
          GtkWidget *message_dialog;

          message_dialog = gtk_message_dialog_new (GTK_WINDOW (gtk_window_get_transient_for (GTK_WINDOW (dialog))),
                                                   GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_INFO,
                                                   GTK_BUTTONS_OK,
                                                   "Exporting to image failed");
          g_signal_connect (message_dialog, "response", G_CALLBACK (gtk_window_destroy), NULL);
          gtk_widget_show (message_dialog);
        }

      g_object_unref (file);
    }

  gtk_window_destroy (GTK_WINDOW (dialog));
  g_object_unref (texture);
}

static void
export_image_cb (GtkWidget        *button,
                 FantasticWindow *self)
{
  GdkTexture *texture;
  GtkWidget *dialog;

  texture = create_texture (self);
  if (texture == NULL)
    return;

  dialog = gtk_file_chooser_dialog_new ("",
                                        GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (button))),
                                        GTK_FILE_CHOOSER_ACTION_SAVE,
                                        "_Cancel", GTK_RESPONSE_CANCEL,
                                        "_Save", GTK_RESPONSE_ACCEPT,
                                        NULL);

  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
  g_signal_connect (dialog, "response", G_CALLBACK (export_image_response_cb), texture);
  gtk_widget_show (dialog);
}

static GListModel *
create_object_children (gpointer item,
                        gpointer user_data)
{
  if (OTTIE_IS_COMPOSITION_LAYER (item))
    {
      return G_LIST_MODEL (g_object_ref (ottie_composition_layer_get_composition (item)));
    }
  else if (OTTIE_IS_SHAPE_LAYER (item))
    {
      return G_LIST_MODEL (g_object_ref (ottie_shape_layer_get_shape (item)));
    }
  else if (OTTIE_IS_GROUP_SHAPE (item))
    {
      return g_object_ref (item);
    }
  else
    {
      return NULL;
    }
}

static void
notify_prepared_cb (OttieCreation   *creation,
                    GParamSpec      *pspec,
                    FantasticWindow *self)
{
  GtkTreeListModel *treemodel;

  if (ottie_creation_is_prepared (creation))
    {
      treemodel = gtk_tree_list_model_new (g_object_ref (G_LIST_MODEL (ottie_creation_get_composition (self->creation))),
                                           FALSE,
                                           TRUE,
                                           create_object_children,
                                           NULL,
                                           NULL);
      self->selection = gtk_single_selection_new (G_LIST_MODEL (treemodel));
      gtk_list_view_set_model (GTK_LIST_VIEW (self->listview), GTK_SELECTION_MODEL (self->selection));
    }
  else
    {
      g_clear_object (&self->selection);
      gtk_list_view_set_model (GTK_LIST_VIEW (self->listview), NULL);
    }
}

static void
fantastic_window_finalize (GObject *object)
{
  FantasticWindow *self = FANTASTIC_WINDOW (object);

  g_object_unref (self->observer);
  g_clear_object (&self->selection);

  G_OBJECT_CLASS (fantastic_window_parent_class)->finalize (object);
}

static void
fantastic_window_select_object (FantasticWindow *self,
                                OttieObject     *object)
{
  for (guint i = 0; i < g_list_model_get_n_items (G_LIST_MODEL (self->selection)); i++)
    {
      gpointer tree_item = g_list_model_get_item (G_LIST_MODEL (self->selection), i);
      gpointer item = gtk_tree_list_row_get_item (tree_item);
      gboolean match;

      match = item == object;
      g_object_unref (item);
      g_object_unref (tree_item);

      if (match)
        {
          gtk_single_selection_set_selected (self->selection, i);
          break;
        }
    }
}

static void
pressed_cb (GtkGestureClick *click,
            int              n_press,
            double           x,
            double           y,
            FantasticWindow *self)
{
  GtkPicture *picture;
  OttieObject *found;
  graphene_rect_t bounds;

  picture = GTK_PICTURE (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (click)));
  gtk_picture_get_paintable_bounds (picture, &bounds);
  x -= bounds.origin.x;
  y -= bounds.origin.y;
  x *= ottie_creation_get_width (self->creation) / bounds.size.width;
  y *= ottie_creation_get_height (self->creation) / bounds.size.height;
  found = fantastic_observer_pick (self->observer, x, y);
  if (found)
    fantastic_window_select_object (self, found);
}

static void
fantastic_window_class_init (FantasticWindowClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = fantastic_window_finalize;

  g_type_ensure (OTTIE_TYPE_CREATION);
  g_type_ensure (OTTIE_TYPE_PAINTABLE);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gtk/gtk4/fantastic/fantasticwindow.ui");

  gtk_widget_class_bind_template_child (widget_class, FantasticWindow, creation);
  gtk_widget_class_bind_template_child (widget_class, FantasticWindow, paintable);
  gtk_widget_class_bind_template_child (widget_class, FantasticWindow, picture);
  gtk_widget_class_bind_template_child (widget_class, FantasticWindow, listview);

  gtk_widget_class_bind_template_callback (widget_class, open_cb);
  gtk_widget_class_bind_template_callback (widget_class, save_cb);
  gtk_widget_class_bind_template_callback (widget_class, export_image_cb);
  gtk_widget_class_bind_template_callback (widget_class, notify_prepared_cb);
  gtk_widget_class_bind_template_callback (widget_class, pressed_cb);
}

static void
window_open (GSimpleAction *action,
             GVariant      *parameter,
             gpointer       user_data)
{
  FantasticWindow *self = user_data;

  show_open_filechooser (self);
}

static GActionEntry win_entries[] = {
  { "open", window_open, NULL, NULL, NULL },
};

static void
fantastic_window_init (FantasticWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_action_map_add_action_entries (G_ACTION_MAP (self), win_entries, G_N_ELEMENTS (win_entries), self);

  self->observer = fantastic_observer_new ();
  ottie_paintable_set_observer (self->paintable, OTTIE_RENDER_OBSERVER (self->observer));
}

FantasticWindow *
fantastic_window_new (FantasticApplication *application)
{
  return g_object_new (FANTASTIC_WINDOW_TYPE,
                       "application", application,
                       NULL);
}
