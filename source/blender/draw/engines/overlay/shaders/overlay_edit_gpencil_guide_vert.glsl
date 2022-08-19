
#pragma BLENDER_REQUIRE(common_view_clipping_lib.glsl)
#pragma BLENDER_REQUIRE(common_view_lib.glsl)

void main()
{
  GPU_INTEL_VERTEX_SHADER_WORKAROUND

  gl_Position = point_world_to_ndc(pPosition);
  finalColor = pColor;
  gl_PointSize = pSize;

  view_clipping_distances(pPosition);
}
