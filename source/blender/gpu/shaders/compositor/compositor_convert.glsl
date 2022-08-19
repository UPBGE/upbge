#pragma BLENDER_REQUIRE(gpu_shader_compositor_texture_utilities.glsl)

void main()
{
  ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
  vec4 value = texture_load(input_tx, texel);
  imageStore(output_img, texel, CONVERT_EXPRESSION(value));
}
