
#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(common_utiltex_lib.glsl)

#pragma BLENDER_REQUIRE(closure_eval_surface_lib.glsl)

in vec2 pos;

RESOURCE_ID_VARYING

void main()
{
  GPU_INTEL_VERTEX_SHADER_WORKAROUND

  PASS_RESOURCE_ID

  gl_Position = vec4(pos, 1.0, 1.0);
  viewPosition = project_point(ProjectionMatrixInverse, vec3(pos, 0.0));
  worldPosition = project_point(ViewProjectionMatrixInverse, vec3(pos, 0.0));
  /* Not usable. */
  viewNormal = vec3(0.0);
  worldNormal = vec3(0.0);
}
