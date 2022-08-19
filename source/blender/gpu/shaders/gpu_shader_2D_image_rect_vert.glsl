/**
 * Simple shader that just draw one icon at the specified location
 * does not need any vertex input (producing less call to immBegin/End)
 */

void main()
{
  vec2 uv;
  vec2 co;

  if (gl_VertexID == 0) {
    co = rect_geom.xw;
    uv = rect_icon.xw;
  }
  else if (gl_VertexID == 1) {
    co = rect_geom.xy;
    uv = rect_icon.xy;
  }
  else if (gl_VertexID == 2) {
    co = rect_geom.zw;
    uv = rect_icon.zw;
  }
  else {
    co = rect_geom.zy;
    uv = rect_icon.zy;
  }

  gl_Position = ModelViewProjectionMatrix * vec4(co, 0.0f, 1.0f);
  texCoord_interp = uv;
}
