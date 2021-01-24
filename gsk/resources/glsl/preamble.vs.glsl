uniform mat4 u_projection;
uniform float u_alpha;
uniform vec2 u_scale;

#if defined(GSK_GLES) || defined(GSK_LEGACY)
attribute vec2 aPosition;
attribute vec2 aUv;
_OUT_ vec2 vUv;
#else
_IN_ vec2 aPosition;
_IN_ vec2 aUv;
_OUT_ vec2 vUv;
#endif

vec4
gsk_project(vec2 pos)
{
  return u_projection * vec4(u_scale * pos, 0.0, 1.0);
}

// amount is: top, right, bottom, left
GskRoundedRect
gsk_rounded_rect_shrink (GskRoundedRect r, vec4 amount)
{
  vec4 new_bounds = r.bounds + vec4(1.0,1.0,-1.0,-1.0) * amount.wxyz;
  vec4 new_corner_points1 = r.corner_points1;
  vec4 new_corner_points2 = r.corner_points2;

  if (r.corner_points1.xy == r.bounds.xy) new_corner_points1.xy = new_bounds.xy;
  if (r.corner_points1.zw == r.bounds.zy) new_corner_points1.zw = new_bounds.zy;
  if (r.corner_points2.xy == r.bounds.zw) new_corner_points2.xy = new_bounds.zw;
  if (r.corner_points2.zw == r.bounds.xw) new_corner_points2.zw = new_bounds.xw;

  return GskRoundedRect (new_bounds, new_corner_points1, new_corner_points2);
}

void
gsk_rounded_rect_offset(inout GskRoundedRect r, vec2 offset)
{
  r.bounds.xy += offset;
  r.bounds.zw += offset;
  r.corner_points1.xy += offset;
  r.corner_points1.zw += offset;
  r.corner_points2.xy += offset;
  r.corner_points2.zw += offset;
}

void
gsk_rounded_rect_transform(inout GskRoundedRect r)
{
  r.bounds.xy = u_scale * r.bounds.xy;
  r.bounds.zw = u_scale * r.bounds.zw;

  r.corner_points1.xy = (u_scale * r.corner_points1.xy);
  r.corner_points1.zw = (u_scale * r.corner_points1.zw);

  r.corner_points2.xy = (u_scale * r.corner_points2.xy);
  r.corner_points2.zw = (u_scale * r.corner_points2.zw);
}

#if defined(GSK_LEGACY)
// Can't have out or inout array parameters...
#define gsk_rounded_rect_encode(r, uni) uni[0] = r.bounds; uni[1] = r.corner_points1; uni[2] = r.corner_points2;
#else
void gsk_rounded_rect_encode(GskRoundedRect r, out _GSK_ROUNDED_RECT_UNIFORM_ out_r)
{
#if defined(GSK_GLES)
  out_r[0] = r.bounds;
  out_r[1] = r.corner_points1;
  out_r[2] = r.corner_points2;
#else
  out_r = r;
#endif
}

#endif
