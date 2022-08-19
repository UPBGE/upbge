
#pragma BLENDER_REQUIRE(gpencil_common_lib.glsl)

void main()
{
  vec4 color;

  /* Remember, this is associated alpha (aka. premult). */
  color.rgb = textureLod(colorBuf, uvcoordsvar.xy, 0).rgb;
  /* Stroke only render mono-chromatic revealage. We convert to alpha. */
  color.a = 1.0 - textureLod(revealBuf, uvcoordsvar.xy, 0).r;

  float mask = textureLod(maskBuf, uvcoordsvar.xy, 0).r;
  mask *= blendOpacity;

  fragColor = vec4(1.0, 0.0, 1.0, 1.0);
  fragRevealage = vec4(1.0, 0.0, 1.0, 1.0);

  blend_mode_output(blendMode, color, mask, fragColor, fragRevealage);
}
