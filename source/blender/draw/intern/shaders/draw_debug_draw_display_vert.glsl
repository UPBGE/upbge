
/**
 * Display debug edge list.
 **/

void main()
{
  /* Skip the first vertex containing header data. */
  DRWDebugVert vert = drw_debug_verts_buf[gl_VertexID + 1];
  vec3 pos = uintBitsToFloat(uvec3(vert.pos0, vert.pos1, vert.pos2));
  vec4 col = vec4((uvec4(vert.color) >> uvec4(0, 8, 16, 24)) & 0xFFu) / 255.0;

  interp.color = col;
  gl_Position = persmat * vec4(pos, 1.0);
}
