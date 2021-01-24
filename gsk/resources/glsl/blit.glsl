// VERTEX_SHADER:
void main() {
  gl_Position = gsk_project(aPosition);

  vUv = vec2(aUv.x, aUv.y);
}

// FRAGMENT_SHADER:
void main() {
  vec4 diffuse = GskTexture(u_source, vUv);

  gskSetOutputColor(diffuse * u_alpha);
}
