#pragma BLENDER_REQUIRE(gpu_shader_common_color_utils.glsl)

void combine_hsv(float h, float s, float v, out vec4 col)
{
  hsv_to_rgb(vec4(h, s, v, 1.0), col);
}
