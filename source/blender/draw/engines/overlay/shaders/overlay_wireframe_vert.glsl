#pragma BLENDER_REQUIRE(common_view_clipping_lib.glsl)
#pragma BLENDER_REQUIRE(common_view_lib.glsl)

float get_edge_sharpness(float wd)
{
  return ((wd == 0.0) ? -1.5 : wd) + wireStepParam;
}

void wire_color_get(out vec3 rim_col, out vec3 wire_col)
{
  int flag = int(abs(ObjectInfo.w));
  bool is_selected = (flag & DRW_BASE_SELECTED) != 0;
  bool is_from_set = (flag & DRW_BASE_FROM_SET) != 0;
  bool is_active = (flag & DRW_BASE_ACTIVE) != 0;

  if (is_from_set) {
    rim_col = colorWire.rgb;
    wire_col = colorWire.rgb;
  }
  else if (is_selected && useColoring) {
    if (isTransform) {
      rim_col = colorTransform.rgb;
    }
    else if (is_active) {
      rim_col = colorActive.rgb;
    }
    else {
      rim_col = colorSelect.rgb;
    }
    wire_col = colorWire.rgb;
  }
  else {
    rim_col = colorWire.rgb;
    wire_col = colorBackground.rgb;
  }
}

vec3 hsv_to_rgb(vec3 hsv)
{
  vec3 nrgb = abs(hsv.x * 6.0 - vec3(3.0, 2.0, 4.0)) * vec3(1, -1, -1) + vec3(-1, 2, 2);
  nrgb = clamp(nrgb, 0.0, 1.0);
  return ((nrgb - 1.0) * hsv.y + 1.0) * hsv.z;
}

void wire_object_color_get(out vec3 rim_col, out vec3 wire_col)
{
  int flag = int(abs(ObjectInfo.w));
  bool is_selected = (flag & DRW_BASE_SELECTED) != 0;

  if (isObjectColor) {
    rim_col = wire_col = ObjectColor.rgb * 0.5;
  }
  else {
    float hue = ObjectInfo.z;
    vec3 hsv = vec3(hue, 0.75, 0.8);
    rim_col = wire_col = hsv_to_rgb(hsv);
  }

  if (is_selected && useColoring) {
    /* "Normalize" color. */
    wire_col += 1e-4; /* Avoid division by 0. */
    float brightness = max(wire_col.x, max(wire_col.y, wire_col.z));
    wire_col *= 0.5 / brightness;
    rim_col += 0.75;
  }
  else {
    rim_col *= 0.5;
    wire_col += 0.5;
  }
}

void main()
{
  bool no_attr = all(equal(nor, vec3(0)));
  vec3 wnor = no_attr ? drw_view.viewinv[2].xyz : normalize(normal_object_to_world(nor));
  vec3 wpos = point_object_to_world(pos);

  if (isHair) {
    mat4 obmat = hairDupliMatrix;
    wpos = (obmat * vec4(pos, 1.0)).xyz;
    wnor = -normalize(mat3(obmat) * nor);
  }

  bool is_persp = (drw_view.winmat[3][3] == 0.0);
  vec3 V = (is_persp) ? normalize(drw_view.viewinv[3].xyz - wpos) : drw_view.viewinv[2].xyz;

  float facing = dot(wnor, V);

  gl_Position = point_world_to_ndc(wpos);

#ifndef CUSTOM_DEPTH_BIAS
  float facing_ratio = clamp(1.0 - facing * facing, 0.0, 1.0);
  float flip = sign(facing);           /* Flip when not facing the normal (i.e.: backfacing). */
  float curvature = (1.0 - wd * 0.75); /* Avoid making things worse for curvy areas. */
  vec3 wofs = wnor * (facing_ratio * curvature * flip);
  wofs = normal_world_to_view(wofs);

  /* Push vertex half a pixel (maximum) in normal direction. */
  gl_Position.xy += wofs.xy * drw_view.viewport_size_inverse * gl_Position.w;

  /* Push the vertex towards the camera. Helps a bit. */
  gl_Position.z -= facing_ratio * curvature * 1.0e-6 * gl_Position.w;
#endif

  /* Convert to screen position [0..sizeVp]. */
  edgeStart = ((gl_Position.xy / gl_Position.w) * 0.5 + 0.5) * sizeViewport.xy;

#ifndef SELECT_EDGES
  edgePos = edgeStart;

  vec3 rim_col, wire_col;
  if (isObjectColor || isRandomColor) {
    wire_object_color_get(rim_col, wire_col);
  }
  else {
    wire_color_get(rim_col, wire_col);
  }

  facing = clamp(abs(facing), 0.0, 1.0);

  /* Do interpolation in a non-linear space to have a better visual result. */
  rim_col = pow(rim_col, vec3(1.0 / 2.2));
  wire_col = pow(wire_col, vec3(1.0 / 2.2));
  vec3 final_front_col = mix(rim_col, wire_col, 0.35);
  finalColor.rgb = mix(rim_col, final_front_col, facing);
  finalColor.rgb = pow(finalColor.rgb, vec3(2.2));
  finalColor.a = wireOpacity;
  finalColor.rgb *= wireOpacity;
#endif

  /* Cull flat edges below threshold. */
  if (!no_attr && (get_edge_sharpness(wd) < 0.0)) {
    edgeStart = vec2(-1.0);
  }

#ifdef SELECT_EDGES
  /* HACK: to avoid losing sub-pixel object in selections, we add a bit of randomness to the
   * wire to at least create one fragment that will pass the occlusion query. */
  gl_Position.xy += drw_view.viewport_size_inverse * gl_Position.w *
                    ((gl_VertexID % 2 == 0) ? -1.0 : 1.0);
#endif

  view_clipping_distances(wpos);
}
