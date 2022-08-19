
#pragma BLENDER_REQUIRE(common_view_clipping_lib.glsl)
#pragma BLENDER_REQUIRE(common_view_lib.glsl)

void main()
{
  GPU_INTEL_VERTEX_SHADER_WORKAROUND

  /* Reuse the FREESTYLE flag to determine is GPencil. */
  bool is_gpencil = ((data & EDGE_FREESTYLE) != 0);
  if ((data & VERT_SELECTED) != 0) {
    if ((data & VERT_ACTIVE) != 0) {
      finalColor = colorEditMeshActive;
    }
    else {
      finalColor = (!is_gpencil) ? colorVertexSelect : colorGpencilVertexSelect;
    }
  }
  else {
    finalColor = (!is_gpencil) ? colorVertex : colorGpencilVertex;
  }

  vec3 world_pos = point_object_to_world(pos);
  gl_Position = point_world_to_ndc(world_pos);
  gl_PointSize = (!is_gpencil) ? sizeVertex * 2.0 : sizeVertexGpencil * 2.0;
  view_clipping_distances(world_pos);

  bool show_handle = showCurveHandles;
  if ((curveHandleDisplay == CURVE_HANDLE_SELECTED) && ((data & VERT_SELECTED_BEZT_HANDLE) == 0)) {
    show_handle = false;
  }

  if (!show_handle && ((data & BEZIER_HANDLE) != 0)) {
    /* We set the vertex at the camera origin to generate 0 fragments. */
    gl_Position = vec4(0.0, 0.0, -3e36, 0.0);
  }
}
