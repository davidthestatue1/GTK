// VERTEX_SHADER:
uniform vec4 u_color;

_OUT_ vec4 final_color;

void main() {
  gl_Position = gsk_project(aPosition);

  vUv = vec2(aUv.x, aUv.y);

  final_color = gsk_premultiply(u_color) * u_alpha;
}

// FRAGMENT_SHADER:

_IN_ vec4 final_color;

void main() {
  vec4 diffuse = GskTexture(u_source, vUv);

  gskSetOutputColor(final_color * diffuse.a);
}
