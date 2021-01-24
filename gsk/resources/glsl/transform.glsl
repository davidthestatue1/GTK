// VERTEX_SHADER:
uniform mat4 u_transform;

void main() {
  // We are not using gsk_project() here of course. That's the entire
  // point of this shader.
  mat4 scale_matrix = mat4(u_scale.x, 0.0,       0.0, 0.0,
                           0.0,       u_scale.y, 0.0, 0.0,
                           0.0,       0.0,       1.0, 0.0,
                           0.0,       0.0,       0.0, 1.0);
  mat4 transform = scale_matrix * u_transform;
  gl_Position = u_projection * transform * vec4(aPosition, 0.0, 1.0)  ;

  vUv = vec2(aUv.x, aUv.y);
}

// FRAGMENT_SHADER:
void main() {
  vec4 diffuse = GskTexture(u_source, vUv);

  gskSetOutputColor(diffuse * u_alpha);
}
