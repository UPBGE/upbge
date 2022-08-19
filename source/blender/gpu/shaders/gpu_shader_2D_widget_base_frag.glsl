#pragma BLENDER_REQUIRE(gpu_shader_colorspace_lib.glsl)

vec3 compute_masks(vec2 uv)
{
  bool upper_half = uv.y > outRectSize.y * 0.5;
  bool right_half = uv.x > outRectSize.x * 0.5;
  float corner_rad;

  /* Correct aspect ratio for 2D views not using uniform scalling.
   * uv is already in pixel space so a uniform scale should give us a ratio of 1. */
  float ratio = (butCo != -2.0) ? (dFdy(uv.y) / dFdx(uv.x)) : 1.0;
  vec2 uv_sdf = uv;
  uv_sdf.x *= ratio;

  if (right_half) {
    uv_sdf.x = outRectSize.x * ratio - uv_sdf.x;
  }
  if (upper_half) {
    uv_sdf.y = outRectSize.y - uv_sdf.y;
    corner_rad = right_half ? outRoundCorners.z : outRoundCorners.w;
  }
  else {
    corner_rad = right_half ? outRoundCorners.y : outRoundCorners.x;
  }

  /* Fade emboss at the border. */
  float emboss_size = upper_half ? 0.0 : min(1.0, uv_sdf.x / (corner_rad * ratio));

  /* Signed distance field from the corner (in pixel).
   * inner_sdf is sharp and outer_sdf is rounded. */
  uv_sdf -= corner_rad;
  float inner_sdf = max(0.0, min(uv_sdf.x, uv_sdf.y));
  float outer_sdf = -length(min(uv_sdf, 0.0));
  float sdf = inner_sdf + outer_sdf + corner_rad;

  /* Clamp line width to be at least 1px wide. This can happen if the projection matrix
   * has been scaled (i.e: Node editor)... */
  float line_width = (lineWidth > 0.0) ? max(fwidth(uv.y), lineWidth) : 0.0;

  const float aa_radius = 0.5;
  vec3 masks;
  masks.x = smoothstep(-aa_radius, aa_radius, sdf);
  masks.y = smoothstep(-aa_radius, aa_radius, sdf - line_width);
  masks.z = smoothstep(-aa_radius, aa_radius, sdf + line_width * emboss_size);

  /* Compose masks together to avoid having too much alpha. */
  masks.zx = max(vec2(0.0), masks.zx - masks.xy);

  return masks;
}

vec4 do_checkerboard()
{
  float size = checkerColorAndSize.z;
  vec2 phase = mod(gl_FragCoord.xy, size * 2.0);

  if ((phase.x > size && phase.y < size) || (phase.x < size && phase.y > size)) {
    return vec4(checkerColorAndSize.xxx, 1.0);
  }
  else {
    return vec4(checkerColorAndSize.yyy, 1.0);
  }
}

void main()
{
  if (min(1.0, -butCo) > discardFac) {
    discard;
  }

  vec3 masks = compute_masks(uvInterp);

  if (butCo > 0.0) {
    /* Alpha checker widget. */
    if (butCo > 0.5) {
      vec4 checker = do_checkerboard();
      fragColor = mix(checker, innerColor, innerColor.a);
    }
    else {
      /* Set alpha to 1.0. */
      fragColor = innerColor;
    }
    fragColor.a = 1.0;
  }
  else {
    /* Premultiply here. */
    fragColor = innerColor * vec4(innerColor.aaa, 1.0);
  }
  fragColor *= masks.y;
  fragColor += masks.x * borderColor;
  fragColor += masks.z * embossColor;

  /* Un-premult because the blend equation is already doing the mult. */
  if (fragColor.a > 0.0) {
    fragColor.rgb /= fragColor.a;
  }

  fragColor = blender_srgb_to_framebuffer_space(fragColor);
}
