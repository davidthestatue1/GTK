// VERTEX_SHADER:
uniform vec4 u_color;

_OUT_ vec4 final_color;

void main() {
  gl_Position = gsk_project(aPosition);

  final_color = gsk_premultiply(u_color) * u_alpha;
}

// FRAGMENT_SHADER:
_IN_ vec4 final_color;

void main() {
  gskSetOutputColor(final_color);
}

