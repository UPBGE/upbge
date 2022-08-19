#pragma BLENDER_REQUIRE(gpu_shader_common_color_utils.glsl)

void node_composite_hue_saturation_value(
    vec4 color, float hue, float saturation, float value, float factor, out vec4 result)
{
  vec4 hsv;
  rgb_to_hsv(color, hsv);

  hsv.x = fract(hsv.x + hue + 0.5);
  hsv.y = clamp(hsv.y * saturation, 0.0, 1.0);
  hsv.z = hsv.z * value;

  hsv_to_rgb(hsv, result);

  result = mix(color, result, factor);
}
