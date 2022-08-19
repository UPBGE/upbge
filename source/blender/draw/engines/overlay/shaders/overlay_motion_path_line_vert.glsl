
#pragma BLENDER_REQUIRE(common_view_clipping_lib.glsl)
#pragma BLENDER_REQUIRE(common_view_lib.glsl)

#define frameCurrent mpathLineSettings.x
#define frameStart mpathLineSettings.y
#define frameEnd mpathLineSettings.z
#define cacheStart mpathLineSettings.w

/* project to screen space */
vec2 proj(vec4 pos)
{
  return (0.5 * (pos.xy / pos.w) + 0.5) * sizeViewport.xy;
}

#define SET_INTENSITY(A, B, C, min, max) \
  (((1.0 - (float(C - B) / float(C - A))) * (max - min)) + min)

void main()
{
  gl_Position = drw_view.persmat * vec4(pos, 1.0);

  interp.ss_pos = proj(gl_Position);

  int frame = gl_VertexID + cacheStart;

  float intensity; /* how faint */

  vec3 blend_base = (abs(frame - frameCurrent) == 0) ?
                        colorCurrentFrame.rgb :
                        colorBackground.rgb; /* "bleed" cframe color to ease color blending */
  bool use_custom_color = customColor.x >= 0.0;
  /* TODO: We might want something more consistent with custom color and standard colors. */
  if (frame < frameCurrent) {
    if (use_custom_color) {
      /* Custom color: previous frames color is darker than current frame */
      interp.color.rgb = customColor * 0.25;
    }
    else {
      /* black - before frameCurrent */
      if (selected) {
        intensity = SET_INTENSITY(frameStart, frame, frameCurrent, 0.25, 0.75);
      }
      else {
        intensity = SET_INTENSITY(frameStart, frame, frameCurrent, 0.68, 0.92);
      }
      interp.color.rgb = mix(colorWire.rgb, blend_base, intensity);
    }
  }
  else if (frame > frameCurrent) {
    if (use_custom_color) {
      /* Custom color: next frames color is equal to user selected color */
      interp.color.rgb = customColor;
    }
    else {
      /* blue - after frameCurrent */
      if (selected) {
        intensity = SET_INTENSITY(frameCurrent, frame, frameEnd, 0.25, 0.75);
      }
      else {
        intensity = SET_INTENSITY(frameCurrent, frame, frameEnd, 0.68, 0.92);
      }

      interp.color.rgb = mix(colorBonePose.rgb, blend_base, intensity);
    }
  }
  else {
    if (use_custom_color) {
      /* Custom color: current frame color is slightly darker than user selected color */
      interp.color.rgb = customColor * 0.5;
    }
    else {
      /* green - on frameCurrent */
      if (selected) {
        intensity = 0.92f;
      }
      else {
        intensity = 0.75f;
      }
      interp.color.rgb = mix(colorBackground.rgb, blend_base, intensity);
    }
  }

  interp.color.a = 1.0;

  view_clipping_distances(pos);
}
