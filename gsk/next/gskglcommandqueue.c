/* gskglcommandqueue.c
 *
 * Copyright 2017 Timm Bäder <mail@baedert.org>
 * Copyright 2018 Matthias Clasen <mclasen@redhat.com>
 * Copyright 2018 Alexander Larsson <alexl@redhat.com>
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

#include <gdk/gdkglcontextprivate.h>
#include <gdk/gdkmemorytextureprivate.h>
#include <gsk/gskdebugprivate.h>
#include <epoxy/gl.h>
#include <string.h>

#include "gskglattachmentstateprivate.h"
#include "gskglbufferprivate.h"
#include "gskglcommandqueueprivate.h"
#include "gskgluniformstateprivate.h"

typedef enum _GskGLCommandKind
{
  /* The batch will perform a glClear() */
  GSK_GL_COMMAND_KIND_CLEAR,

  /* THe batch represents a new debug group */
  GSK_GL_COMMAND_KIND_PUSH_DEBUG_GROUP,

  /* The batch represents the end of a debug group */
  GSK_GL_COMMAND_KIND_POP_DEBUG_GROUP,

  /* The batch will perform a glDrawArrays() */
  GSK_GL_COMMAND_KIND_DRAW,
} GskGLCommandKind;

typedef struct _GskGLCommandBind
{
  /* @texture is the value passed to glActiveTexture(), the "slot" the
   * texture will be placed into. We always use GL_TEXTURE_2D so we don't
   * waste any bits here to indicate that.
   */
  guint texture : 5;

  /* The identifier for the texture created with glGenTextures(). */
  guint id : 27;
} GskGLCommandBind;

G_STATIC_ASSERT (sizeof (GskGLCommandBind) == 4);

typedef struct _GskGLCommandBatchAny
{
  /* A GskGLCommandKind indicating what the batch will do */
  guint kind : 8;

  /* The program's identifier to use for determining if we can merge two
   * batches together into a single set of draw operations. We put this
   * here instead of the GskGLCommandDraw so that we can use the extra
   * bits here without making the structure larger.
   */
  guint program : 24;

  /* The index of the next batch following this one. This is used
   * as a sort of integer-based linked list to simplify out-of-order
   * batching without moving memory around. -1 indicates last batch.
   */
  int next_batch_index;

  /* The viewport size of the batch. We check this as we process
   * batches to determine if we need to resize the viewport.
   */
  struct {
    guint16 width;
    guint16 height;
  } viewport;
} GskGLCommandBatchAny;

G_STATIC_ASSERT (sizeof (GskGLCommandBatchAny) == 12);

typedef struct _GskGLCommandDraw
{
  GskGLCommandBatchAny head;

  /* There doesn't seem to be a limit on the framebuffer identifier that
   * can be returned, so we have to use a whole unsigned for the framebuffer
   * we are drawing to. When processing batches, we check to see if this
   * changes and adjust the render target accordingly. Some sorting is
   * performed to reduce the amount we change framebuffers.
   */
  guint framebuffer;

  /* The number of uniforms to change. This must be less than or equal to
   * GL_MAX_UNIFORM_LOCATIONS but only guaranteed up to 1024 by any OpenGL
   * implementation to be conformant.
   */
  guint uniform_count : 11;

  /* The number of textures to bind, which is only guaranteed up to 16
   * by the OpenGL specification to be conformant.
   */
  guint bind_count : 5;

  /* GL_MAX_ELEMENTS_VERTICES specifies 33000 for this which requires 16-bit
   * to address all possible counts <= GL_MAX_ELEMENTS_VERTICES.
   */
  guint vbo_count : 16;

  /* The offset within the VBO containing @vbo_count vertices to send with
   * glDrawArrays().
   */
  guint vbo_offset;

  /* The offset within the array of uniform changes to be made containing
   * @uniform_count #GskGLCommandUniform elements to apply.
   */
  guint uniform_offset;

  /* The offset within the array of bind changes to be made containing
   * @bind_count #GskGLCommandBind elements to apply.
   */
  guint bind_offset;
} GskGLCommandDraw;

G_STATIC_ASSERT (sizeof (GskGLCommandDraw) == 32);

typedef struct _GskGLCommandUniform
{
  GskGLUniformInfo info;
  guint            location;
} GskGLCommandUniform;

G_STATIC_ASSERT (sizeof (GskGLCommandUniform) == 8);

typedef union _GskGLCommandBatch
{
  GskGLCommandBatchAny    any;
  GskGLCommandDraw        draw;
  struct {
    GskGLCommandBatchAny  any;
    const char           *debug_group;
  } debug_group;
  struct {
    GskGLCommandBatchAny  any;
    guint bits;
    guint framebuffer;
  } clear;
} GskGLCommandBatch;

G_STATIC_ASSERT (sizeof (GskGLCommandBatch) == 32);

G_DEFINE_TYPE (GskGLCommandQueue, gsk_gl_command_queue, G_TYPE_OBJECT)

static inline void
gsk_gl_command_queue_capture_png (GskGLCommandQueue *self,
                                  const char        *filename,
                                  guint              width,
                                  guint              height)
{
  cairo_surface_t *surface;
  gpointer data;
  guint stride;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (filename != NULL);

  stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, width);
  data = g_malloc_n (height, stride);

  glReadPixels (0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, data);
  surface = cairo_image_surface_create_for_data (data, CAIRO_FORMAT_ARGB32, width, height, stride);
  cairo_surface_write_to_png (surface, filename);

  cairo_surface_destroy (surface);
  g_free (data);
}

static void
gsk_gl_command_queue_save (GskGLCommandQueue *self)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  g_ptr_array_add (self->saved_state,
                   gsk_gl_attachment_state_save (self->attachments));
}

static void
gsk_gl_command_queue_restore (GskGLCommandQueue *self)
{
  GskGLAttachmentState *saved;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->saved_state->len > 0);

  saved = g_ptr_array_steal_index (self->saved_state,
                                   self->saved_state->len - 1);

  gsk_gl_attachment_state_restore (saved);
}

static void
gsk_gl_command_queue_dispose (GObject *object)
{
  GskGLCommandQueue *self = (GskGLCommandQueue *)object;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  g_clear_object (&self->context);
  g_clear_pointer (&self->batches, g_array_unref);
  g_clear_pointer (&self->attachments, gsk_gl_attachment_state_unref);
  g_clear_pointer (&self->uniforms, gsk_gl_uniform_state_unref);
  g_clear_pointer (&self->vertices, gsk_gl_buffer_free);
  g_clear_pointer (&self->batch_draws, g_array_unref);
  g_clear_pointer (&self->batch_binds, g_array_unref);
  g_clear_pointer (&self->batch_uniforms, g_array_unref);
  g_clear_pointer (&self->saved_state, g_ptr_array_unref);

  G_OBJECT_CLASS (gsk_gl_command_queue_parent_class)->dispose (object);
}

static void
gsk_gl_command_queue_class_init (GskGLCommandQueueClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gsk_gl_command_queue_dispose;
}

static void
gsk_gl_command_queue_init (GskGLCommandQueue *self)
{
  self->max_texture_size = -1;

  self->batches = g_array_new (FALSE, TRUE, sizeof (GskGLCommandBatch));
  self->batch_draws = g_array_new (FALSE, FALSE, sizeof (GskGLCommandDraw));
  self->batch_binds = g_array_new (FALSE, FALSE, sizeof (GskGLCommandBind));
  self->batch_uniforms = g_array_new (FALSE, FALSE, sizeof (GskGLCommandUniform));
  self->vertices = gsk_gl_buffer_new (GL_ARRAY_BUFFER, sizeof (GskGLDrawVertex));
  self->saved_state = g_ptr_array_new_with_free_func ((GDestroyNotify)gsk_gl_attachment_state_unref);
  self->debug_groups = g_string_chunk_new (4096);
}

GskGLCommandQueue *
gsk_gl_command_queue_new (GdkGLContext      *context,
                          GskGLUniformState *uniforms)
{
  GskGLCommandQueue *self;

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), NULL);

  self = g_object_new (GSK_TYPE_GL_COMMAND_QUEUE, NULL);
  self->context = g_object_ref (context);
  self->attachments = gsk_gl_attachment_state_new ();

  /* Use shared uniform state if we're provided one */
  if (uniforms != NULL)
    self->uniforms = gsk_gl_uniform_state_ref (uniforms);
  else
    self->uniforms = gsk_gl_uniform_state_new ();

  /* Determine max texture size immediately and restore context */
  gdk_gl_context_make_current (context);
  glGetIntegerv (GL_MAX_TEXTURE_SIZE, &self->max_texture_size);

  return g_steal_pointer (&self);
}

static GskGLCommandBatch *
begin_next_batch (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch;
  guint index;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  index = self->batches->len;
  g_array_set_size (self->batches, index + 1);

  batch = &g_array_index (self->batches, GskGLCommandBatch, index);
  batch->any.next_batch_index = -1;

  return batch;
}

static void
enqueue_batch (GskGLCommandQueue *self)
{
  GskGLCommandBatch *prev;
  guint index;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->batches->len > 0);

  index = self->batches->len - 1;

  if (self->tail_batch_index != -1)
    {
      prev = &g_array_index (self->batches, GskGLCommandBatch, self->tail_batch_index);
      prev->any.next_batch_index = index;
    }

  self->tail_batch_index = index;
}

static void
discard_batch (GskGLCommandQueue *self)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->batches->len > 0);

  self->batches->len--;
}

void
gsk_gl_command_queue_begin_draw (GskGLCommandQueue     *self,
                                 guint                  program,
                                 const graphene_rect_t *viewport)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);
  g_return_if_fail (viewport != NULL);

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_DRAW;
  batch->any.program = program;
  batch->any.next_batch_index = -1;
  batch->any.viewport.width = viewport->size.width;
  batch->any.viewport.height = viewport->size.height;
  batch->draw.framebuffer = 0;
  batch->draw.uniform_count = 0;
  batch->draw.uniform_offset = self->batch_uniforms->len;
  batch->draw.bind_count = 0;
  batch->draw.bind_offset = self->batch_binds->len;
  batch->draw.vbo_count = 0;
  batch->draw.vbo_offset = gsk_gl_buffer_get_offset (self->vertices);

  self->in_draw = TRUE;
}

static void
gsk_gl_command_queue_uniform_snapshot_cb (const GskGLUniformInfo *info,
                                          guint                   location,
                                          gpointer                user_data)
{
  GskGLCommandQueue *self = user_data;
  GskGLCommandUniform uniform;

  g_assert (info != NULL);
  g_assert (info->initial == FALSE);
  g_assert (info->changed == TRUE);
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  uniform.location = location;
  uniform.info = *info;

  g_array_append_val (self->batch_uniforms, uniform);
}

void
gsk_gl_command_queue_end_draw (GskGLCommandQueue *self)
{
  GskGLCommandBatch *last_batch;
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->batches->len > 0);
  g_return_if_fail (self->in_draw == TRUE);

  if (self->batches->len > 1)
    last_batch = &g_array_index (self->batches, GskGLCommandBatch, self->batches->len - 2);
  else
    last_batch = NULL;

  batch = &g_array_index (self->batches, GskGLCommandBatch, self->batches->len - 1);

  g_assert (batch->any.kind == GSK_GL_COMMAND_KIND_DRAW);

  if G_UNLIKELY (batch->draw.vbo_count == 0)
    {
      discard_batch (self);
      self->in_draw = FALSE;
      return;
    }

  /* Track the destination framebuffer in case it changed */
  batch->draw.framebuffer = self->attachments->fbo.id;
  self->attachments->fbo.changed = FALSE;

  /* Track the list of uniforms that changed */
  batch->draw.uniform_offset = self->batch_uniforms->len;
  gsk_gl_uniform_state_snapshot (self->uniforms,
                                 batch->any.program,
                                 gsk_gl_command_queue_uniform_snapshot_cb,
                                 self);
  batch->draw.uniform_count = self->batch_uniforms->len - batch->draw.uniform_offset;

  /* Track the bind attachments that changed */
  batch->draw.bind_offset = self->batch_binds->len;
  batch->draw.bind_count = 0;
  for (guint i = 0; i < G_N_ELEMENTS (self->attachments->textures); i++)
    {
      GskGLBindTexture *texture = &self->attachments->textures[i];

      if (texture->changed && texture->id > 0)
        {
          GskGLCommandBind bind;

          texture->changed = FALSE;

          bind.texture = texture->texture;
          bind.id = texture->id;

          g_array_append_val (self->batch_binds, bind);

          batch->draw.bind_count++;
        }
    }

  /* Do simple chaining of draw to last batch. */
  /* TODO: Use merging capabilities for out-or-order batching */
  if (last_batch != NULL &&
      last_batch->any.kind == GSK_GL_COMMAND_KIND_DRAW &&
      last_batch->any.program == batch->any.program &&
      last_batch->any.viewport.width == batch->any.viewport.width &&
      last_batch->any.viewport.height == batch->any.viewport.height &&
      last_batch->draw.framebuffer == batch->draw.framebuffer &&
      batch->draw.uniform_count == 0 &&
      batch->draw.bind_count == 0 &&
      last_batch->draw.vbo_offset + last_batch->draw.vbo_count == batch->draw.vbo_offset)
    {
      last_batch->draw.vbo_count += batch->draw.vbo_count;
      discard_batch (self);
    }
  else
    {
      enqueue_batch (self);
    }

  self->in_draw = FALSE;
}

GskGLDrawVertex *
gsk_gl_command_queue_add_vertices (GskGLCommandQueue     *self,
                                   const GskGLDrawVertex  vertices[GSK_GL_N_VERTICES])
{
  GskGLCommandBatch *batch;
  GskGLDrawVertex *dest;
  guint offset;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), NULL);
  g_return_val_if_fail (self->in_draw == TRUE, NULL);

  batch = &g_array_index (self->batches, GskGLCommandBatch, self->batches->len - 1);
  batch->draw.vbo_count += GSK_GL_N_VERTICES;

  dest = gsk_gl_buffer_advance (self->vertices, GSK_GL_N_VERTICES, &offset);

  if (vertices != NULL)
    {
      memcpy (dest, vertices, sizeof (GskGLDrawVertex) * GSK_GL_N_VERTICES);
      return NULL;
    }

  return dest;
}

void
gsk_gl_command_queue_clear (GskGLCommandQueue     *self,
                            guint                  clear_bits,
                            const graphene_rect_t *viewport)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);
  g_return_if_fail (self->batches->len < G_MAXINT);

  if (clear_bits == 0)
    clear_bits = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_CLEAR;
  batch->any.viewport.width = viewport->size.width;
  batch->any.viewport.height = viewport->size.height;
  batch->clear.bits = clear_bits;
  batch->clear.framebuffer = self->attachments->fbo.id;
  batch->any.next_batch_index = -1;
  batch->any.program = 0;

  enqueue_batch (self);

  self->attachments->fbo.changed = FALSE;
}

void
gsk_gl_command_queue_push_debug_group (GskGLCommandQueue *self,
                                       const char        *debug_group)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);
  g_return_if_fail (self->batches->len < G_MAXINT);

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_PUSH_DEBUG_GROUP;
  batch->debug_group.debug_group = g_string_chunk_insert (self->debug_groups, debug_group);
  batch->any.next_batch_index = -1;
  batch->any.program = 0;

  enqueue_batch (self);
}

void
gsk_gl_command_queue_pop_debug_group (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);
  g_return_if_fail (self->batches->len < G_MAXINT);

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_POP_DEBUG_GROUP;
  batch->debug_group.debug_group = NULL;
  batch->any.next_batch_index = -1;
  batch->any.program = 0;

  enqueue_batch (self);
}

GdkGLContext *
gsk_gl_command_queue_get_context (GskGLCommandQueue *self)
{
  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), NULL);

  return self->context;
}

void
gsk_gl_command_queue_make_current (GskGLCommandQueue *self)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (GDK_IS_GL_CONTEXT (self->context));

  gdk_gl_context_make_current (self->context);
}

void
gsk_gl_command_queue_delete_program (GskGLCommandQueue *self,
                                     guint              program)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_command_queue_make_current (self);
  glDeleteProgram (program);
  gsk_gl_uniform_state_clear_program (self->uniforms, program);
}

static void
apply_uniform (GskGLUniformState      *state,
               const GskGLUniformInfo *info,
               guint                   location)
{
  const union {
    graphene_matrix_t matrix[0];
    GskRoundedRect rounded_rect[0];
    float fval[0];
    int ival[0];
  } *data;

  data = gsk_gl_uniform_state_get_uniform_data (state, info->offset);

  switch (info->format)
    {
    case GSK_GL_UNIFORM_FORMAT_1F:
      glUniform1f (location, data->fval[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_2F:
      glUniform2f (location, data->fval[0], data->fval[1]);
      break;

    case GSK_GL_UNIFORM_FORMAT_3F:
      glUniform3f (location, data->fval[0], data->fval[1], data->fval[2]);
      break;

    case GSK_GL_UNIFORM_FORMAT_4F:
      glUniform4f (location, data->fval[0], data->fval[1], data->fval[2], data->fval[3]);
      break;

    case GSK_GL_UNIFORM_FORMAT_1FV:
      glUniform1fv (location, info->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_2FV:
      glUniform2fv (location, info->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_3FV:
      glUniform3fv (location, info->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_4FV:
      glUniform4fv (location, info->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_1I:
    case GSK_GL_UNIFORM_FORMAT_TEXTURE:
      glUniform1i (location, data->ival[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_2I:
      glUniform2i (location, data->ival[0], data->ival[1]);
      break;

    case GSK_GL_UNIFORM_FORMAT_3I:
      glUniform3i (location, data->ival[0], data->ival[1], data->ival[2]);
      break;

    case GSK_GL_UNIFORM_FORMAT_4I:
      glUniform4i (location, data->ival[0], data->ival[1], data->ival[2], data->ival[3]);
      break;

    case GSK_GL_UNIFORM_FORMAT_MATRIX: {
      float mat[16];
      graphene_matrix_to_float (&data->matrix[0], mat);
      glUniformMatrix4fv (location, 1, GL_FALSE, mat);
      break;
    }

    case GSK_GL_UNIFORM_FORMAT_COLOR:
      glUniform4fv (location, 1, &data->fval[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT:
      if (info->send_corners)
        glUniform4fv (location, 3, (const float *)&data->rounded_rect[0]);
      else
        glUniform4fv (location, 1, (const float *)&data->rounded_rect[0]);
      break;

    default:
      g_assert_not_reached ();
    }
}

static inline void
apply_viewport (guint16 *current_width,
                guint16 *current_height,
                guint16 width,
                guint16 height)
{
  if (*current_width != width || *current_height != height)
    {
      *current_width = width;
      *current_height = height;
      glViewport (0, 0, width, height);
    }
}

static inline void
apply_scissor (guint                        framebuffer,
               guint                        surface_height,
               guint                        scale_factor,
               const cairo_rectangle_int_t *scissor,
               gboolean                     has_scissor)
{
  if (framebuffer != 0 || !has_scissor)
    {
      glDisable (GL_SCISSOR_TEST);
      return;
    }

  glEnable (GL_SCISSOR_TEST);
  glScissor (scissor->x * scale_factor,
             surface_height - (scissor->height * scale_factor) - (scissor->y * scale_factor),
             scissor->width * scale_factor,
             scissor->height * scale_factor);

}

/**
 * gsk_gl_command_queue_execute:
 * @self: a #GskGLCommandQueue
 * @surface_height: the height of the backing surface
 * @scale_factor: the scale factor of the backing surface
 * #scissor: (nullable): the scissor clip if any
 *
 * Executes all of the batches in the command queue.
 */
void
gsk_gl_command_queue_execute (GskGLCommandQueue    *self,
                              guint                 surface_height,
                              guint                 scale_factor,
                              const cairo_region_t *scissor)
{
  cairo_rectangle_int_t scissor_rect;
  GLuint framebuffer = 0;
  GLuint vao_id;
  int next_batch_index;
  guint program = 0;
  guint16 width = 0;
  guint16 height = 0;
  guint count = 0;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);

  if (self->batches->len == 0)
    return;

  glEnable (GL_DEPTH_TEST);
  glDepthFunc (GL_LEQUAL);

  /* Pre-multiplied alpha */
  glEnable (GL_BLEND);
  glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation (GL_FUNC_ADD);

  glGenVertexArrays (1, &vao_id);
  glBindVertexArray (vao_id);

  gsk_gl_buffer_submit (self->vertices);

  /* 0 = position location */
  glEnableVertexAttribArray (0);
  glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE,
                         sizeof (GskGLDrawVertex),
                         (void *) G_STRUCT_OFFSET (GskGLDrawVertex, position));

  /* 1 = texture coord location */
  glEnableVertexAttribArray (1);
  glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE,
                         sizeof (GskGLDrawVertex),
                         (void *) G_STRUCT_OFFSET (GskGLDrawVertex, uv));

  /* Setup initial scissor clip */
  if (scissor != NULL)
    {
      g_assert (cairo_region_num_rectangles (scissor) == 1);
      cairo_region_get_rectangle (scissor, 0, &scissor_rect);
    }

  apply_scissor (framebuffer,
                 surface_height,
                 scale_factor,
                 &scissor_rect,
                 scissor != NULL);

  next_batch_index = 0;

  while (next_batch_index >= 0)
    {
      const GskGLCommandBatch *batch = &g_array_index (self->batches, GskGLCommandBatch, next_batch_index);

      g_assert (batch->any.next_batch_index != next_batch_index);

      count++;

      switch (batch->any.kind)
        {
        case GSK_GL_COMMAND_KIND_CLEAR:
          if G_UNLIKELY (framebuffer != batch->clear.framebuffer)
            {
              framebuffer = batch->clear.framebuffer;
              glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
              apply_scissor (framebuffer,
                             surface_height,
                             scale_factor,
                             &scissor_rect,
                             scissor != NULL);
            }

          apply_viewport (&width,
                          &height,
                          batch->any.viewport.width,
                          batch->any.viewport.height);

          glClear (batch->clear.bits);
        break;

        case GSK_GL_COMMAND_KIND_PUSH_DEBUG_GROUP:
          gdk_gl_context_push_debug_group (self->context, batch->debug_group.debug_group);
        break;

        case GSK_GL_COMMAND_KIND_POP_DEBUG_GROUP:
          gdk_gl_context_pop_debug_group (self->context);
        break;

        case GSK_GL_COMMAND_KIND_DRAW:
          if (batch->any.program != program)
            {
              program = batch->any.program;
              glUseProgram (program);
            }

          if G_UNLIKELY (batch->draw.framebuffer != framebuffer)
            {
              framebuffer = batch->draw.framebuffer;
              glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
              apply_scissor (framebuffer,
                             surface_height,
                             scale_factor,
                             &scissor_rect,
                             scissor != NULL);
            }

          apply_viewport (&width,
                          &height,
                          batch->any.viewport.width,
                          batch->any.viewport.height);

          if G_UNLIKELY (batch->draw.bind_count > 0)
            {
              for (guint i = 0; i < batch->draw.bind_count; i++)
                {
                  guint index = batch->draw.bind_offset + i;
                  GskGLCommandBind *bind = &g_array_index (self->batch_binds, GskGLCommandBind, index);

                  glActiveTexture (GL_TEXTURE0 + bind->texture);
                  glBindTexture (GL_TEXTURE_2D, bind->id);
                }
            }

          if (batch->draw.uniform_count > 0)
            {
              for (guint i = 0; i < batch->draw.uniform_count; i++)
                {
                  guint index = batch->draw.uniform_offset + i;
                  GskGLCommandUniform *u = &g_array_index (self->batch_uniforms, GskGLCommandUniform, index);

                  g_assert (index < self->batch_uniforms->len);

                  apply_uniform (self->uniforms, &u->info, u->location);
                }
            }

          glDrawArrays (GL_TRIANGLES, batch->draw.vbo_offset, batch->draw.vbo_count);

        break;

        default:
          g_assert_not_reached ();
        }

#if 0
      if (batch->any.kind == GSK_GL_COMMAND_KIND_DRAW ||
          batch->any.kind == GSK_GL_COMMAND_KIND_CLEAR)
        {
          char filename[32];
          g_snprintf (filename, sizeof filename,
                      "capture%u_batch%d_kind%u.png",
                      count++, next_batch_index, batch->any.kind);
          gsk_gl_command_queue_capture_png (self, filename, width, height);
        }
#endif

      next_batch_index = batch->any.next_batch_index;
    }

  glDeleteVertexArrays (1, &vao_id);
}

void
gsk_gl_command_queue_begin_frame (GskGLCommandQueue *self)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->batches->len == 0);

  self->tail_batch_index = -1;
}

/**
 * gsk_gl_command_queue_end_frame:
 * @self: a #GskGLCommandQueue
 *
 * This function performs cleanup steps that need to be done after
 * a frame has finished. This is not performed as part of the command
 * queue execution to allow for the frame to be submitted as soon
 * as possible.
 *
 * However, it should be executed after the draw contexts end_frame
 * has been called to swap the OpenGL framebuffers.
 */
void
gsk_gl_command_queue_end_frame (GskGLCommandQueue *self)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->saved_state->len == 0);

  gsk_gl_uniform_state_end_frame (self->uniforms);

  g_string_chunk_clear (self->debug_groups);

  self->batches->len = 0;
  self->batch_draws->len = 0;
  self->batch_uniforms->len = 0;
  self->batch_binds->len = 0;
  self->tail_batch_index = -1;
}

gboolean
gsk_gl_command_queue_create_render_target (GskGLCommandQueue *self,
                                           int                width,
                                           int                height,
                                           int                min_filter,
                                           int                mag_filter,
                                           guint             *out_fbo_id,
                                           guint             *out_texture_id)
{
  GLuint fbo_id = 0;
  GLint texture_id;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), FALSE);
  g_return_val_if_fail (width > 0, FALSE);
  g_return_val_if_fail (height > 0, FALSE);
  g_return_val_if_fail (out_fbo_id != NULL, FALSE);
  g_return_val_if_fail (out_texture_id != NULL, FALSE);

  gsk_gl_command_queue_save (self);

  texture_id = gsk_gl_command_queue_create_texture (self,
                                                    width, height,
                                                    min_filter, mag_filter);

  if (texture_id == -1)
    {
      *out_fbo_id = 0;
      *out_texture_id = 0;
      gsk_gl_command_queue_restore (self);
      return FALSE;
    }

  fbo_id = gsk_gl_command_queue_create_framebuffer (self);

  glBindFramebuffer (GL_FRAMEBUFFER, fbo_id);
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0);
  g_assert_cmphex (glCheckFramebufferStatus (GL_FRAMEBUFFER), ==, GL_FRAMEBUFFER_COMPLETE);

  gsk_gl_command_queue_restore (self);

  *out_fbo_id = fbo_id;
  *out_texture_id = texture_id;

  return TRUE;
}

int
gsk_gl_command_queue_create_texture (GskGLCommandQueue *self,
                                     int                width,
                                     int                height,
                                     int                min_filter,
                                     int                mag_filter)
{
  GLuint texture_id = 0;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), -1);

  if G_UNLIKELY (self->max_texture_size == -1)
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &self->max_texture_size);

  if (width > self->max_texture_size || height > self->max_texture_size)
    return -1;

  gsk_gl_command_queue_save (self);
  gsk_gl_command_queue_make_current (self);

  glGenTextures (1, &texture_id);

  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, texture_id);

  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (gdk_gl_context_get_use_es (self->context))
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  else
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

  gsk_gl_command_queue_restore (self);

  return (int)texture_id;
}

guint
gsk_gl_command_queue_create_framebuffer (GskGLCommandQueue *self)
{
  GLuint fbo_id;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), -1);

  gsk_gl_command_queue_make_current (self);

  glGenFramebuffers (1, &fbo_id);

  return fbo_id;
}

void
gsk_gl_command_queue_bind_framebuffer (GskGLCommandQueue *self,
                                       guint              framebuffer)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_attachment_state_bind_framebuffer (self->attachments, framebuffer);
}

int
gsk_gl_command_queue_upload_texture (GskGLCommandQueue *self,
                                     GdkTexture        *texture,
                                     guint              x_offset,
                                     guint              y_offset,
                                     guint              width,
                                     guint              height,
                                     int                min_filter,
                                     int                mag_filter)
{
  cairo_surface_t *surface = NULL;
  GdkMemoryFormat data_format;
  const guchar *data;
  gsize data_stride;
  gsize bpp;
  int texture_id;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), 0);
  g_return_val_if_fail (!GDK_IS_GL_TEXTURE (texture), 0);
  g_return_val_if_fail (x_offset + width <= gdk_texture_get_width (texture), 0);
  g_return_val_if_fail (y_offset + height <= gdk_texture_get_height (texture), 0);
  g_return_val_if_fail (min_filter == GL_LINEAR || min_filter == GL_NEAREST, 0);
  g_return_val_if_fail (mag_filter == GL_LINEAR || min_filter == GL_NEAREST, 0);

  if (width > self->max_texture_size || height > self->max_texture_size)
    {
      g_warning ("Attempt to create texture of size %ux%u but max size is %d. "
                 "Clipping will occur.",
                 width, height, self->max_texture_size);
      width = MAX (width, self->max_texture_size);
      height = MAX (height, self->max_texture_size);
    }

  texture_id = gsk_gl_command_queue_create_texture (self, width, height, min_filter, mag_filter);
  if (texture_id == -1)
    return texture_id;

  if (GDK_IS_MEMORY_TEXTURE (texture))
    {
      GdkMemoryTexture *memory_texture = GDK_MEMORY_TEXTURE (texture);
      data = gdk_memory_texture_get_data (memory_texture);
      data_format = gdk_memory_texture_get_format (memory_texture);
      data_stride = gdk_memory_texture_get_stride (memory_texture);
    }
  else
    {
      /* Fall back to downloading to a surface */
      surface = gdk_texture_download_surface (texture);
      cairo_surface_flush (surface);
      data = cairo_image_surface_get_data (surface);
      data_format = GDK_MEMORY_DEFAULT;
      data_stride = cairo_image_surface_get_stride (surface);
    }

  bpp = gdk_memory_format_bytes_per_pixel (data_format);

  /* Swtich to texture0 as 2D. We'll restore it later. */
  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, texture_id);

  gdk_gl_context_upload_texture (gdk_gl_context_get_current (),
                                 data + x_offset * bpp + y_offset * data_stride,
                                 width, height, data_stride,
                                 data_format, GL_TEXTURE_2D);

  /* Restore previous texture state if any */
  if (self->attachments->textures[0].id > 0)
    glBindTexture (self->attachments->textures[0].target,
                   self->attachments->textures[0].id);

  g_clear_pointer (&surface, cairo_surface_destroy);

  return texture_id;
}