/* gskglrenderjob.c
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
#include <gsk/gskrendernodeprivate.h>
#include <math.h>
#include <string.h>

#include "gskglcommandqueueprivate.h"
#include "gskgldriverprivate.h"
#include "gskglprogramprivate.h"
#include "gskglrenderjobprivate.h"

#define ORTHO_NEAR_PLANE   -10000
#define ORTHO_FAR_PLANE     10000
#define MAX_GRADIENT_STOPS  6


#define rounded_rect_top_left(r)                                                        \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x,                                               \
                      r->bounds.origin.y,                                               \
                      r->corner[0].width, r->corner[0].height))
#define rounded_rect_top_right(r) \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x + r->bounds.size.width - r->corner[1].width,   \
                      r->bounds.origin.y, \
                      r->corner[1].width, r->corner[1].height))
#define rounded_rect_bottom_right(r) \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x + r->bounds.size.width - r->corner[2].width,   \
                      r->bounds.origin.y + r->bounds.size.height - r->corner[2].height, \
                      r->corner[2].width, r->corner[2].height))
#define rounded_rect_bottom_left(r)                                                     \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x,                                               \
                      r->bounds.origin.y + r->bounds.size.height - r->corner[2].height, \
                      r->corner[3].width, r->corner[3].height))
#define rounded_rect_corner0(r)   rounded_rect_top_left(r)
#define rounded_rect_corner1(r)   rounded_rect_top_right(r)
#define rounded_rect_corner2(r)   rounded_rect_bottom_right(r)
#define rounded_rect_corner3(r)   rounded_rect_bottom_left(r)
#define rounded_rect_corner(r, i) (rounded_rect_corner##i(r))

struct _GskGLRenderJob
{
  GskNextDriver     *driver;
  GskGLCommandQueue *command_queue;
  cairo_region_t    *region;
  guint              framebuffer;
  graphene_rect_t    viewport;
  graphene_matrix_t  projection;
  GArray            *modelview;
  GArray            *clip;
  float              offset_x;
  float              offset_y;
  float              scale_x;
  float              scale_y;
  guint              flip_y : 1;
};

typedef struct _GskGLRenderClip
{
  GskRoundedRect rect;
  bool           is_rectilinear;
} GskGLRenderClip;

typedef struct _GskGLRenderModelview
{
  GskTransform *transform;
  float scale_x;
  float scale_y;
  float offset_x_before;
  float offset_y_before;
  graphene_matrix_t matrix;
} GskGLRenderModelview;

static void gsk_gl_render_job_visit_node (GskGLRenderJob *job,
                                          GskRenderNode  *node);

static inline gboolean G_GNUC_PURE
node_is_invisible (const GskRenderNode *node)
{
  return node->bounds.size.width == 0.0f ||
         node->bounds.size.height == 0.0f ||
         isnan (node->bounds.size.width) ||
         isnan (node->bounds.size.height);
}

static inline gboolean G_GNUC_PURE
node_supports_transform (GskRenderNode *node)
{
  /* Some nodes can't handle non-trivial transforms without being
   * rendered to a texture (e.g. rotated clips, etc.). Some however work
   * just fine, mostly because they already draw their child to a
   * texture and just render the texture manipulated in some way, think
   * opacity or color matrix.
   */

  switch ((int)gsk_render_node_get_node_type (node))
    {
      case GSK_COLOR_NODE:
      case GSK_OPACITY_NODE:
      case GSK_COLOR_MATRIX_NODE:
      case GSK_TEXTURE_NODE:
      case GSK_CROSS_FADE_NODE:
      case GSK_LINEAR_GRADIENT_NODE:
      case GSK_DEBUG_NODE:
      case GSK_TEXT_NODE:
        return TRUE;

      case GSK_TRANSFORM_NODE:
        return node_supports_transform (gsk_transform_node_get_child (node));

      default:
        return FALSE;
    }
}

static inline gboolean
rounded_inner_rect_contains_rect (const GskRoundedRect  *rounded,
                                  const graphene_rect_t *rect)
{
  const graphene_rect_t *rounded_bounds = &rounded->bounds;
  graphene_rect_t inner;
  float offset_x;
  float offset_y;

  /* TODO: This is pretty conservative and we could go further,
   *       more fine-grained checks to avoid offscreen drawing.
   */

  offset_x = MAX (rounded->corner[GSK_CORNER_TOP_LEFT].width,
                  rounded->corner[GSK_CORNER_BOTTOM_LEFT].width);
  offset_y = MAX (rounded->corner[GSK_CORNER_TOP_LEFT].height,
                  rounded->corner[GSK_CORNER_TOP_RIGHT].height);

  inner.origin.x = rounded_bounds->origin.x + offset_x;
  inner.origin.y = rounded_bounds->origin.y + offset_y;
  inner.size.width = rounded_bounds->size.width - offset_x -
                     MAX (rounded->corner[GSK_CORNER_TOP_RIGHT].width,
                          rounded->corner[GSK_CORNER_BOTTOM_RIGHT].width);
  inner.size.height = rounded_bounds->size.height - offset_y -
                      MAX (rounded->corner[GSK_CORNER_BOTTOM_LEFT].height,
                           rounded->corner[GSK_CORNER_BOTTOM_RIGHT].height);

  return graphene_rect_contains_rect (&inner, rect);
}

static inline gboolean G_GNUC_PURE
rect_intersects (const graphene_rect_t *r1,
                 const graphene_rect_t *r2)
{
  /* Assume both rects are already normalized, as they usually are */
  if (r1->origin.x > (r2->origin.x + r2->size.width) ||
      (r1->origin.x + r1->size.width) < r2->origin.x)
    return FALSE;
  else if (r1->origin.y > (r2->origin.y + r2->size.height) ||
      (r1->origin.y + r1->size.height) < r2->origin.y)
    return FALSE;
  else
    return TRUE;
}

static inline gboolean G_GNUC_PURE
rect_contains_rect (const graphene_rect_t *r1,
                    const graphene_rect_t *r2)
{
  if (r2->origin.x >= r1->origin.x &&
      (r2->origin.x + r2->size.width) <= (r1->origin.x + r1->size.width) &&
      r2->origin.y >= r1->origin.y &&
      (r2->origin.y + r2->size.height) <= (r1->origin.y + r1->size.height))
    return TRUE;
  else
    return FALSE;
}

static inline gboolean
rounded_rect_has_corner (const GskRoundedRect *r,
                         guint                 i)
{
  return r->corner[i].width > 0 && r->corner[i].height > 0;
}

/* Current clip is NOT rounded but new one is definitely! */
static inline gboolean
intersect_rounded_rectilinear (const graphene_rect_t *non_rounded,
                               const GskRoundedRect  *rounded,
                               GskRoundedRect        *result)
{
  bool corners[4];

  /* Intersects with top left corner? */
  corners[0] = rounded_rect_has_corner (rounded, 0) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 0));
  /* top right? */
  corners[1] = rounded_rect_has_corner (rounded, 1) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 1));
  /* bottom right? */
  corners[2] = rounded_rect_has_corner (rounded, 2) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 2));
  /* bottom left */
  corners[3] = rounded_rect_has_corner (rounded, 3) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 3));

  if (corners[0] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 0)))
    return FALSE;
  if (corners[1] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 1)))
    return FALSE;
  if (corners[2] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 2)))
    return FALSE;
  if (corners[3] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 3)))
    return FALSE;

  /* We do intersect with at least one of the corners, but in such a way that the
   * intersection between the two clips can still be represented by a single rounded
   * rect in a trivial way. do that. */
  graphene_rect_intersection (non_rounded, &rounded->bounds, &result->bounds);

  for (int i = 0; i < 4; i++)
    {
      if (corners[i])
        result->corner[i] = rounded->corner[i];
      else
        result->corner[i].width = result->corner[i].height = 0;
    }

  return TRUE;
}

static void
init_projection_matrix (graphene_matrix_t     *projection,
                        const graphene_rect_t *viewport,
                        gboolean               flip_y)
{
  graphene_matrix_init_ortho (projection,
                              viewport->origin.x,
                              viewport->origin.x + viewport->size.width,
                              viewport->origin.y,
                              viewport->origin.y + viewport->size.height,
                              ORTHO_NEAR_PLANE,
                              ORTHO_FAR_PLANE);

  if (!flip_y)
    graphene_matrix_scale (projection, 1, -1, 1);
}

static inline GskGLRenderModelview *
gsk_gl_render_job_get_modelview (GskGLRenderJob *job)
{
  if (job->modelview->len > 0)
    return &g_array_index (job->modelview,
                           GskGLRenderModelview,
                           job->modelview->len - 1);
  else
    return NULL;
}

static void
extract_matrix_metadata (GskGLRenderModelview *modelview)
{
  float dummy;
  graphene_matrix_t m;

  gsk_transform_to_matrix (modelview->transform, &modelview->matrix);

  switch (gsk_transform_get_category (modelview->transform))
    {
    case GSK_TRANSFORM_CATEGORY_IDENTITY:
    case GSK_TRANSFORM_CATEGORY_2D_TRANSLATE:
      modelview->scale_x = 1;
      modelview->scale_y = 1;
      break;

    case GSK_TRANSFORM_CATEGORY_2D_AFFINE:
      gsk_transform_to_affine (modelview->transform,
                               &modelview->scale_x, &modelview->scale_y,
                               &dummy, &dummy);
      break;

    case GSK_TRANSFORM_CATEGORY_UNKNOWN:
    case GSK_TRANSFORM_CATEGORY_ANY:
    case GSK_TRANSFORM_CATEGORY_3D:
    case GSK_TRANSFORM_CATEGORY_2D:
      {
        graphene_vec3_t col1;
        graphene_vec3_t col2;

        /* TODO: 90% sure this is incorrect. But we should never hit this code
         * path anyway. */
        graphene_vec3_init (&col1,
                            graphene_matrix_get_value (&m, 0, 0),
                            graphene_matrix_get_value (&m, 1, 0),
                            graphene_matrix_get_value (&m, 2, 0));

        graphene_vec3_init (&col2,
                            graphene_matrix_get_value (&m, 0, 1),
                            graphene_matrix_get_value (&m, 1, 1),
                            graphene_matrix_get_value (&m, 2, 1));

        modelview->scale_x = graphene_vec3_length (&col1);
        modelview->scale_y = graphene_vec3_length (&col2);
      }
      break;

    default:
      break;
    }
}

static void
gsk_gl_render_job_set_modelview (GskGLRenderJob *job,
                                 GskTransform   *transform)
{
  GskGLRenderModelview *modelview;

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);

  g_array_set_size (job->modelview, job->modelview->len + 1);

  modelview = &g_array_index (job->modelview,
                              GskGLRenderModelview,
                              job->modelview->len - 1);

  modelview->transform = transform;

  modelview->offset_x_before = job->offset_x;
  modelview->offset_y_before = job->offset_y;

  extract_matrix_metadata (modelview);

  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = modelview->scale_x;
  job->scale_y = modelview->scale_y;
}

static void
gsk_gl_render_job_push_modelview (GskGLRenderJob *job,
                                  GskTransform   *transform)
{
  GskGLRenderModelview *modelview;

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);
  g_assert (transform != NULL);

  g_array_set_size (job->modelview, job->modelview->len + 1);

  modelview = &g_array_index (job->modelview,
                              GskGLRenderModelview,
                              job->modelview->len - 1);

  if G_LIKELY (job->modelview->len > 1)
    {
      GskGLRenderModelview *last;
      GskTransform *t = NULL;

      last = &g_array_index (job->modelview,
                             GskGLRenderModelview,
                             job->modelview->len - 2);

      /* Multiply given matrix with our previews modelview */
      t = gsk_transform_translate (gsk_transform_ref (last->transform),
                                   &(graphene_point_t) {
                                     job->offset_x,
                                     job->offset_y
                                   });
      t = gsk_transform_transform (t, transform);
      modelview->transform = t;
    }
  else
    {
      modelview->transform = gsk_transform_ref (transform);
    }

  modelview->offset_x_before = job->offset_x;
  modelview->offset_y_before = job->offset_y;

  extract_matrix_metadata (modelview);

  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = job->scale_x;
  job->scale_y = job->scale_y;
}

static void
gsk_gl_render_job_pop_modelview (GskGLRenderJob *job)
{
  const GskGLRenderModelview *head;

  g_assert (job != NULL);
  g_assert (job->modelview);
  g_assert (job->modelview->len > 0);

  head = gsk_gl_render_job_get_modelview (job);

  job->offset_x = head->offset_x_before;
  job->offset_y = head->offset_y_before;

  gsk_transform_unref (head->transform);

  job->modelview->len--;

  if (job->modelview->len >= 1)
    {
      head = &g_array_index (job->modelview, GskGLRenderModelview, job->modelview->len - 1);

      job->scale_x = head->scale_x;
      job->scale_y = head->scale_y;
    }
}

static inline gboolean
gsk_gl_render_job_clip_is_rectilinear (GskGLRenderJob *job)
{
  if (job->clip->len == 0)
    return TRUE;
  else
    return g_array_index (job->clip, GskGLRenderClip, job->clip->len - 1).is_rectilinear;
}

static inline const GskRoundedRect *
gsk_gl_render_job_get_clip (GskGLRenderJob *job)
{
  if (job->clip->len == 0)
    return NULL;
  else
    return &g_array_index (job->clip, GskGLRenderClip, job->clip->len - 1).rect;
}

static void
gsk_gl_render_job_push_clip (GskGLRenderJob       *job,
                             const GskRoundedRect *rect)
{
  GskGLRenderClip clip;

  g_assert (job != NULL);
  g_assert (job->clip != NULL);
  g_assert (rect != NULL);

  clip.rect = *rect;
  clip.is_rectilinear = gsk_rounded_rect_is_rectilinear (rect);

  g_array_append_val (job->clip, clip);
}

static void
gsk_gl_render_job_pop_clip (GskGLRenderJob *job)
{
  g_assert (job != NULL);
  g_assert (job->clip != NULL);
  g_assert (job->clip->len > 0);

  job->clip->len--;
}

static void
gsk_gl_render_job_offset (GskGLRenderJob *job,
                          float           offset_x,
                          float           offset_y)
{
  g_assert (job != NULL);

  job->offset_x += offset_x;
  job->offset_y += offset_y;
}

static void
gsk_gl_render_job_transform_bounds (GskGLRenderJob        *job,
                                    const graphene_rect_t *rect,
                                    graphene_rect_t       *out_rect)
{
  GskGLRenderModelview *modelview;
  graphene_rect_t r;

  g_assert (job != NULL);
  g_assert (rect != NULL);
  g_assert (out_rect != NULL);

  r.origin.x = rect->origin.x + job->offset_x;
  r.origin.y = rect->origin.y + job->offset_y;
  r.size.width = rect->size.width;
  r.size.height = rect->size.width;

  if ((modelview = gsk_gl_render_job_get_modelview (job)))
    gsk_transform_transform_bounds (modelview->transform, &r, out_rect);
  else
    g_critical ("Attempt to transform with NULL modelview");
}

GskGLRenderJob *
gsk_gl_render_job_new (GskNextDriver         *driver,
                       const graphene_rect_t *viewport,
                       float                  scale_factor,
                       const cairo_region_t  *region,
                       guint                  framebuffer,
                       gboolean               flip_y)
{
  const graphene_rect_t *clip_rect = viewport;
  graphene_rect_t transformed_extents;
  GskGLRenderJob *job;

  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (driver), NULL);
  g_return_val_if_fail (viewport != NULL, NULL);
  g_return_val_if_fail (scale_factor > 0, NULL);

  job = g_slice_new0 (GskGLRenderJob);
  job->driver = g_object_ref (driver);
  job->command_queue = driver->command_queue;
  job->clip = g_array_new (FALSE, FALSE, sizeof (GskGLRenderClip));
  job->modelview = g_array_new (FALSE, FALSE, sizeof (GskGLRenderModelview));
  job->framebuffer = framebuffer;
  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = scale_factor;
  job->scale_y = scale_factor;
  job->viewport = *viewport;
  job->region = region ? cairo_region_copy (region) : NULL;
  job->flip_y = !!flip_y;

  init_projection_matrix (&job->projection, viewport, flip_y);
  gsk_gl_render_job_set_modelview (job, gsk_transform_scale (NULL, scale_factor, scale_factor));

  /* Setup our initial clip. If region is NULL then we are drawing the
   * whole viewport. Otherwise, we need to convert the region to a
   * bounding box and clip based on that.
   */

  if (region != NULL)
    {
      cairo_rectangle_int_t extents;

      cairo_region_get_extents (region, &extents);
      gsk_gl_render_job_transform_bounds (job,
                                          &GRAPHENE_RECT_INIT (extents.x,
                                                               extents.y,
                                                               extents.width,
                                                               extents.height),
                                          &transformed_extents);
      clip_rect = &transformed_extents;
    }

  gsk_gl_render_job_push_clip (job,
                               &GSK_ROUNDED_RECT_INIT (clip_rect->origin.x,
                                                       clip_rect->origin.y,
                                                       clip_rect->size.width,
                                                       clip_rect->size.height));

  return job;
}

void
gsk_gl_render_job_free (GskGLRenderJob *job)
{
  while (job->modelview->len > 0)
    {
      GskGLRenderModelview *modelview = gsk_gl_render_job_get_modelview (job);
      g_clear_pointer (&modelview->transform, gsk_transform_unref);
      job->modelview->len--;
    }

  g_clear_object (&job->driver);
  g_clear_pointer (&job->region, cairo_region_destroy);
  g_clear_pointer (&job->modelview, g_array_unref);
  g_clear_pointer (&job->clip, g_array_unref);
  g_slice_free (GskGLRenderJob, job);
}

static inline gboolean
gsk_gl_render_job_node_overlaps_clip (GskGLRenderJob *job,
                                      GskRenderNode  *node)
{
  const GskRoundedRect *clip = gsk_gl_render_job_get_clip (job);

  if (clip != NULL)
    {
      graphene_rect_t transformed_bounds;
      gsk_gl_render_job_transform_bounds (job, &node->bounds, &transformed_bounds);
      return rect_intersects (&clip->bounds, &transformed_bounds);
    }

  return TRUE;
}

static void
gsk_gl_render_job_draw_rect (GskGLRenderJob  *job,
                             graphene_rect_t *rect)
{
  GskGLDrawVertex *vertices;
  const float min_x = job->offset_x + rect->origin.x;
  const float min_y = job->offset_y + rect->origin.y;
  const float max_x = min_x + rect->size.width;
  const float max_y = min_y + rect->size.height;

  vertices = gsk_gl_command_queue_add_vertices (job->command_queue, NULL);

  vertices[0].position[0] = min_x;
  vertices[0].position[1] = min_y;
  vertices[0].uv[0] = 0;
  vertices[0].uv[1] = 0;

  vertices[1].position[0] = min_x;
  vertices[1].position[1] = max_y;
  vertices[1].uv[0] = 0;
  vertices[1].uv[1] = 1;

  vertices[2].position[0] = max_x;
  vertices[2].position[1] = min_y;
  vertices[2].uv[0] = 1;
  vertices[2].uv[1] = 0;

  vertices[3].position[0] = max_x;
  vertices[3].position[1] = max_y;
  vertices[3].uv[0] = 1;
  vertices[3].uv[1] = 1;

  vertices[4].position[0] = min_x;
  vertices[4].position[1] = max_y;
  vertices[4].uv[0] = 0;
  vertices[4].uv[1] = 1;

  vertices[5].position[0] = max_x;
  vertices[5].position[1] = min_y;
  vertices[5].uv[0] = 1;
  vertices[5].uv[1] = 0;
}

static void
gsk_gl_render_job_visit_as_fallback (GskGLRenderJob *job,
                                     GskRenderNode  *node)
{
}

static void
gsk_gl_render_job_visit_color_node (GskGLRenderJob *job,
                                    GskRenderNode  *node)
{
  GskGLRenderModelview *modelview = gsk_gl_render_job_get_modelview (job);

  gsk_gl_program_begin_draw (job->driver->color,
                             &job->viewport,
                             &job->projection,
                             &modelview->matrix,
                             gsk_gl_render_job_get_clip (job));
  gsk_gl_program_set_uniform_color (job->driver->color,
                                    UNIFORM_COLOR_COLOR,
                                    gsk_color_node_get_color (node));
  gsk_gl_render_job_draw_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->color);
}

static void
gsk_gl_render_job_visit_linear_gradient_node (GskGLRenderJob *job,
                                              GskRenderNode  *node)
{
  int n_color_stops = gsk_linear_gradient_node_get_n_color_stops (node);

  if (n_color_stops < MAX_GRADIENT_STOPS)
    {
      GskGLRenderModelview *modelview = gsk_gl_render_job_get_modelview (job);
      const GskColorStop *stops = gsk_linear_gradient_node_get_color_stops (node, NULL);
      const graphene_point_t *start = gsk_linear_gradient_node_get_start (node);
      const graphene_point_t *end = gsk_linear_gradient_node_get_end (node);

      gsk_gl_program_begin_draw (job->driver->linear_gradient,
                                 &job->viewport,
                                 &job->projection,
                                 &modelview->matrix,
                                 gsk_gl_render_job_get_clip (job));
      gsk_gl_program_set_uniform1i (job->driver->linear_gradient,
                                    UNIFORM_LINEAR_GRADIENT_NUM_COLOR_STOPS,
                                    n_color_stops);
      gsk_gl_program_set_uniform1fv (job->driver->linear_gradient,
                                     UNIFORM_LINEAR_GRADIENT_COLOR_STOPS,
                                     n_color_stops * 5,
                                     (const float *)stops);
      gsk_gl_program_set_uniform2f (job->driver->linear_gradient,
                                    UNIFORM_LINEAR_GRADIENT_START_POINT,
                                    start->x, start->y);
      gsk_gl_program_set_uniform2f (job->driver->linear_gradient,
                                    UNIFORM_LINEAR_GRADIENT_END_POINT,
                                    end->x, end->y);
      gsk_gl_render_job_draw_rect (job, &node->bounds);
      gsk_gl_program_end_draw (job->driver->linear_gradient);
    }
  else
    {
      gsk_gl_render_job_visit_as_fallback (job, node);
    }
}

static void
gsk_gl_render_job_visit_clipped_child (GskGLRenderJob       *job,
                                       GskRenderNode        *child,
                                       const GskRoundedRect *clip)
{
  graphene_rect_t transformed_clip;
  GskRoundedRect intersection;

  gsk_gl_render_job_transform_bounds (job, &clip->bounds, &transformed_clip);

  if (gsk_gl_render_job_clip_is_rectilinear (job))
    {
      const GskRoundedRect *current_clip = gsk_gl_render_job_get_clip (job);

      memset (&intersection, 0, sizeof (GskRoundedRect));
      graphene_rect_intersection (&transformed_clip,
                                  &current_clip->bounds,
                                  &intersection.bounds);

      gsk_gl_render_job_push_clip (job, &intersection);
      gsk_gl_render_job_visit_node (job, child);
      gsk_gl_render_job_pop_clip (job);
    }
  else if (intersect_rounded_rectilinear (&transformed_clip, clip, &intersection))
    {
      gsk_gl_render_job_push_clip (job, &intersection);
      gsk_gl_render_job_visit_node (job, child);
      gsk_gl_render_job_pop_clip (job);
    }
  else
    {
#if 0
      GskRoundedRect scaled_clip;
      TextureRegion region;
      gboolean is_offscreen;

      scaled_clip = GSK_ROUNDED_RECT_INIT (clip->origin.x * job->scale_x,
                                           clip->origin.y * scale_y,
                                           clip->size.width * job->scale_x,
                                           clip->size.height * scale_y);

      gsk_gl_render_job_push_clip (job, &scaled_clip);
      if (!add_offscreen_ops (self, builder, &child->bounds,
                              child,
                              &region, &is_offscreen,
                              FORCE_OFFSCREEN))
        g_assert_not_reached ();
      gsk_gl_render_job_pop_clip (job);

      /* TODO: offscreen stuff will tweak these a bit */

      gsk_gl_program_begin_draw (job->driver->blit,
                                 &job->viewport,
                                 &job->projection,
                                 &modelview->matrix,
                                 &clip->bounds);
      gsk_gl_program_set_uniform_texture (job->driver->blit,
                                          UNIFORM_SHARED_SOURCE,
                                          GL_TEXTURE_2D,
                                          GL_TEXTURE0,
                                          region.texture_id);
      gsk_gl_program_end_draw (job->driver->blit);
#endif
    }
}

static void
gsk_gl_render_job_visit_clip_node (GskGLRenderJob *job,
                                   GskRenderNode  *node)
{
  const graphene_rect_t *clip = gsk_clip_node_get_clip (node);
  GskRenderNode *child = gsk_clip_node_get_child (node);
  GskRoundedRect rounded_clip = { .bounds = *clip };

  gsk_gl_render_job_visit_clipped_child (job, child, &rounded_clip);
}

static void
gsk_gl_render_job_visit_rounded_clip_node (GskGLRenderJob *job,
                                           GskRenderNode  *node)
{
  const GskRoundedRect *current_clip;
  const GskRoundedRect *clip;
  GskRenderNode *child;
  GskRoundedRect transformed_clip;
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  gboolean need_offscreen;

  child = gsk_rounded_clip_node_get_child (node);
  if (node_is_invisible (child))
    return;

  clip = gsk_rounded_clip_node_get_clip (node);
  current_clip = gsk_gl_render_job_get_clip (job);

  gsk_gl_render_job_transform_bounds (job, &clip->bounds, &transformed_clip.bounds);

  for (guint i = 0; i < 4; i++)
    {
      transformed_clip.corner[i].width = clip->corner[i].width * scale_x;
      transformed_clip.corner[i].height = clip->corner[i].height * scale_y;
    }

  if (gsk_rounded_rect_is_rectilinear (clip))
    {
      GskRoundedRect intersected_clip;

      if (intersect_rounded_rectilinear (&current_clip->bounds,
                                         &transformed_clip,
                                         &intersected_clip))
        {
          gsk_gl_render_job_push_clip (job, &intersected_clip);
          gsk_gl_render_job_visit_node (job, child);
          gsk_gl_render_job_pop_clip (job);
          g_print ("All good\n");
          return;
        }
    }

  /* After this point we are really working with a new and a current clip
   * which both have rounded corners.
   */

  if (job->clip->len <= 1)
    need_offscreen = FALSE;
  else if (rounded_inner_rect_contains_rect (current_clip, &transformed_clip.bounds))
    need_offscreen = FALSE;
  else
    need_offscreen = TRUE;

  if (!need_offscreen)
    {
      /* If the new clip entirely contains the current clip, the intersection is simply
       * the current clip, so we can ignore the new one.
       */
      if (rounded_inner_rect_contains_rect (&transformed_clip, &current_clip->bounds))
        {
          gsk_gl_render_job_visit_node (job, child);
        }
      else
        {
          /* TODO: Intersect current and new clip */
          gsk_gl_render_job_push_clip (job, &transformed_clip);
          gsk_gl_render_job_visit_node (job, child);
          gsk_gl_render_job_pop_clip (job);
        }
    }

#if 0
  else
    {
      GskRoundedRect scaled_clip;
      gboolean is_offscreen;
      TextureRegion region;
      /* NOTE: We are *not* transforming the clip by the current modelview here.
       *       We instead draw the untransformed clip to a texture and then transform
       *       that texture.
       *
       *       We do, however, apply the scale factor to the child clip of course.
       */
      scaled_clip.bounds.origin.x = clip->bounds.origin.x * scale_x;
      scaled_clip.bounds.origin.y = clip->bounds.origin.y * scale_y;
      scaled_clip.bounds.size.width = clip->bounds.size.width * scale_x;
      scaled_clip.bounds.size.height = clip->bounds.size.height * scale_y;

      /* Increase corner radius size by scale factor */
      for (i = 0; i < 4; i ++)
        {
          scaled_clip.corner[i].width = clip->corner[i].width * scale_x;
          scaled_clip.corner[i].height = clip->corner[i].height * scale_y;
        }

      ops_push_clip (builder, &scaled_clip);
      if (!add_offscreen_ops (self, builder, &node->bounds,
                              child,
                              &region, &is_offscreen,
                              0))
        g_assert_not_reached ();

      ops_pop_clip (builder);

      ops_set_program (builder, &self->programs->blit_program);
      ops_set_texture (builder, region.texture_id);

      load_offscreen_vertex_data (ops_draw (builder, NULL), node, builder);
    }
#endif
}

static void
gsk_gl_render_job_visit_transform_node (GskGLRenderJob *job,
                                        GskRenderNode  *node)
{
  GskTransform *transform = gsk_transform_node_get_transform (node);
  const GskTransformCategory category = gsk_transform_get_category (transform);
  GskRenderNode *child = gsk_transform_node_get_child (node);

  switch (category)
    {
    case GSK_TRANSFORM_CATEGORY_IDENTITY:
      gsk_gl_render_job_visit_node (job, child);
    break;

    case GSK_TRANSFORM_CATEGORY_2D_TRANSLATE:
      {
        float dx, dy;

        gsk_transform_to_translate (transform, &dx, &dy);
        gsk_gl_render_job_offset (job, dx, dy);
        gsk_gl_render_job_visit_node (job, child);
        gsk_gl_render_job_offset (job, -dx, -dy);
      }
    break;

    case GSK_TRANSFORM_CATEGORY_2D_AFFINE:
      {
        gsk_gl_render_job_push_modelview (job, transform);
        gsk_gl_render_job_visit_node (job, child);
        gsk_gl_render_job_pop_modelview (job);
      }
    break;

    case GSK_TRANSFORM_CATEGORY_2D:
    case GSK_TRANSFORM_CATEGORY_3D:
    case GSK_TRANSFORM_CATEGORY_ANY:
    case GSK_TRANSFORM_CATEGORY_UNKNOWN:
      g_warning ("TODO: complex transform\n");
    break;

    default:
      g_assert_not_reached ();
    }
}

static void
gsk_gl_render_job_visit_node (GskGLRenderJob *job,
                              GskRenderNode  *node)
{
  g_assert (job != NULL);
  g_assert (node != NULL);
  g_assert (GSK_IS_NEXT_DRIVER (job->driver));
  g_assert (GSK_IS_GL_COMMAND_QUEUE (job->command_queue));

  if (node_is_invisible (node) ||
      !gsk_gl_render_job_node_overlaps_clip (job, node))
    return;

  switch (gsk_render_node_get_node_type (node))
    {
    case GSK_CONTAINER_NODE:
      {
        guint i;
        guint p;

        for (i = 0, p = gsk_container_node_get_n_children (node);
             i < p;
             i ++)
          {
            GskRenderNode *child = gsk_container_node_get_child (node, i);
            gsk_gl_render_job_visit_node (job, child);
          }
      }
    break;

    case GSK_DEBUG_NODE:
      {
        const char *message = gsk_debug_node_get_message (node);

        if (message != NULL)
          gsk_gl_command_queue_push_debug_group (job->command_queue, message);

        gsk_gl_render_job_visit_node (job, gsk_debug_node_get_child (node));

        if (message != NULL)
          gsk_gl_command_queue_pop_debug_group (job->command_queue);
      }
    break;

    case GSK_COLOR_NODE:
      gsk_gl_render_job_visit_color_node (job, node);
    break;

    case GSK_LINEAR_GRADIENT_NODE:
      gsk_gl_render_job_visit_linear_gradient_node (job, node);
    break;

    case GSK_TRANSFORM_NODE:
      gsk_gl_render_job_visit_transform_node (job, node);
    break;

    case GSK_CLIP_NODE:
      gsk_gl_render_job_visit_clip_node (job, node);
    break;

    case GSK_ROUNDED_CLIP_NODE:
      gsk_gl_render_job_visit_rounded_clip_node (job, node);
    break;

    case GSK_BLEND_NODE:
    case GSK_BLUR_NODE:
    case GSK_BORDER_NODE:
    case GSK_CAIRO_NODE:
    case GSK_COLOR_MATRIX_NODE:
    case GSK_CONIC_GRADIENT_NODE:
    case GSK_CROSS_FADE_NODE:
    case GSK_GL_SHADER_NODE:
    case GSK_INSET_SHADOW_NODE:
    case GSK_OPACITY_NODE:
    case GSK_OUTSET_SHADOW_NODE:
    case GSK_RADIAL_GRADIENT_NODE:
    case GSK_REPEATING_LINEAR_GRADIENT_NODE:
    case GSK_REPEATING_RADIAL_GRADIENT_NODE:
    case GSK_REPEAT_NODE:
    case GSK_SHADOW_NODE:
    case GSK_TEXTURE_NODE:
    case GSK_TEXT_NODE:
    break;

    case GSK_NOT_A_RENDER_NODE:
    default:
      g_assert_not_reached ();
    break;
    }
}

void
gsk_gl_render_job_render (GskGLRenderJob *job,
                          GskRenderNode  *root)
{
  GdkGLContext *context;

  g_return_if_fail (job != NULL);
  g_return_if_fail (root != NULL);
  g_return_if_fail (GSK_IS_NEXT_DRIVER (job->driver));

  context = gsk_next_driver_get_context (job->driver);

  gsk_next_driver_begin_frame (job->driver);

  if (job->framebuffer != 0)
    gsk_gl_command_queue_bind_framebuffer (job->command_queue, job->framebuffer);

  gsk_gl_command_queue_clear (job->command_queue, 0, &job->viewport);

  gdk_gl_context_push_debug_group (context, "Building command queue");
  gsk_gl_render_job_visit_node (job, root);
  gdk_gl_context_pop_debug_group (context);

  gdk_gl_context_push_debug_group (context, "Executing command queue");
  gsk_gl_command_queue_execute (job->command_queue);
  gdk_gl_context_pop_debug_group (context);

  gsk_next_driver_end_frame (job->driver);
}