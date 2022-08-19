/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"
#include "BKE_editmesh.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"
#include "BKE_type_conversions.hh"

#include "BLI_float3x3.hh"
#include "BLI_task.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "NOD_socket_search_link.hh"

#include "GEO_reverse_uv_sampler.hh"

#include "DEG_depsgraph_query.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_deform_curves_on_surface_cc {

using attribute_math::mix3;
using bke::CurvesGeometry;
using geometry::ReverseUVSampler;

NODE_STORAGE_FUNCS(NodeGeometryCurveTrim)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Curves")).supported_type(GEO_COMPONENT_TYPE_CURVE);
  b.add_output<decl::Geometry>(N_("Curves"));
}

static void deform_curves(const CurvesGeometry &curves,
                          const Mesh &surface_mesh_old,
                          const Mesh &surface_mesh_new,
                          const Span<float2> curve_attachment_uvs,
                          const ReverseUVSampler &reverse_uv_sampler_old,
                          const ReverseUVSampler &reverse_uv_sampler_new,
                          const Span<float3> corner_normals_old,
                          const Span<float3> corner_normals_new,
                          const Span<float3> rest_positions,
                          const float4x4 &surface_to_curves,
                          MutableSpan<float3> r_positions,
                          MutableSpan<float3x3> r_rotations,
                          std::atomic<int> &r_invalid_uv_count)
{
  /* Find attachment points on old and new mesh. */
  const int curves_num = curves.curves_num();
  Array<ReverseUVSampler::Result> surface_samples_old(curves_num);
  Array<ReverseUVSampler::Result> surface_samples_new(curves_num);
  threading::parallel_invoke(
      1024 < curves_num,
      [&]() { reverse_uv_sampler_old.sample_many(curve_attachment_uvs, surface_samples_old); },
      [&]() { reverse_uv_sampler_new.sample_many(curve_attachment_uvs, surface_samples_new); });

  const float4x4 curves_to_surface = surface_to_curves.inverted();

  threading::parallel_for(curves.curves_range(), 256, [&](const IndexRange range) {
    for (const int curve_i : range) {
      const ReverseUVSampler::Result &surface_sample_old = surface_samples_old[curve_i];
      if (surface_sample_old.type != ReverseUVSampler::ResultType::Ok) {
        r_invalid_uv_count++;
        continue;
      }
      const ReverseUVSampler::Result &surface_sample_new = surface_samples_new[curve_i];
      if (surface_sample_new.type != ReverseUVSampler::ResultType::Ok) {
        r_invalid_uv_count++;
        continue;
      }

      const MLoopTri &looptri_old = *surface_sample_old.looptri;
      const MLoopTri &looptri_new = *surface_sample_new.looptri;
      const float3 &bary_weights_old = surface_sample_old.bary_weights;
      const float3 &bary_weights_new = surface_sample_new.bary_weights;

      const int corner_0_old = looptri_old.tri[0];
      const int corner_1_old = looptri_old.tri[1];
      const int corner_2_old = looptri_old.tri[2];

      const int corner_0_new = looptri_new.tri[0];
      const int corner_1_new = looptri_new.tri[1];
      const int corner_2_new = looptri_new.tri[2];

      const int vert_0_old = surface_mesh_old.mloop[corner_0_old].v;
      const int vert_1_old = surface_mesh_old.mloop[corner_1_old].v;
      const int vert_2_old = surface_mesh_old.mloop[corner_2_old].v;

      const int vert_0_new = surface_mesh_new.mloop[corner_0_new].v;
      const int vert_1_new = surface_mesh_new.mloop[corner_1_new].v;
      const int vert_2_new = surface_mesh_new.mloop[corner_2_new].v;

      const float3 &normal_0_old = corner_normals_old[corner_0_old];
      const float3 &normal_1_old = corner_normals_old[corner_1_old];
      const float3 &normal_2_old = corner_normals_old[corner_2_old];
      const float3 normal_old = math::normalize(
          mix3(bary_weights_old, normal_0_old, normal_1_old, normal_2_old));

      const float3 &normal_0_new = corner_normals_new[corner_0_new];
      const float3 &normal_1_new = corner_normals_new[corner_1_new];
      const float3 &normal_2_new = corner_normals_new[corner_2_new];
      const float3 normal_new = math::normalize(
          mix3(bary_weights_new, normal_0_new, normal_1_new, normal_2_new));

      const float3 &pos_0_old = surface_mesh_old.mvert[vert_0_old].co;
      const float3 &pos_1_old = surface_mesh_old.mvert[vert_1_old].co;
      const float3 &pos_2_old = surface_mesh_old.mvert[vert_2_old].co;
      const float3 pos_old = mix3(bary_weights_old, pos_0_old, pos_1_old, pos_2_old);

      const float3 &pos_0_new = surface_mesh_new.mvert[vert_0_new].co;
      const float3 &pos_1_new = surface_mesh_new.mvert[vert_1_new].co;
      const float3 &pos_2_new = surface_mesh_new.mvert[vert_2_new].co;
      const float3 pos_new = mix3(bary_weights_new, pos_0_new, pos_1_new, pos_2_new);

      /* The translation is just the difference between the old and new position on the surface. */
      const float3 translation = pos_new - pos_old;

      const float3 &rest_pos_0 = rest_positions[vert_0_new];
      const float3 &rest_pos_1 = rest_positions[vert_1_new];

      /* The tangent reference direction is used to determine the rotation of the surface point
       * around its normal axis. It's important that the old and new tangent reference are computed
       * in a consistent way. If the surface has not been rotated, the old and new tangent
       * reference have to have the same direction. For that reason, the old tangent reference is
       * computed based on the rest position attribute instead of positions on the old mesh. This
       * way the old and new tangent reference use the same topology.
       *
       * TODO: Figure out if this can be smoothly interpolated across the surface as well.
       * Currently, this is a source of discontinuity in the deformation, because the vector
       * changes instantly from one triangle to the next. */
      const float3 tangent_reference_dir_old = rest_pos_1 - rest_pos_0;
      const float3 tangent_reference_dir_new = pos_1_new - pos_0_new;

      /* Compute first local tangent based on the (potentially smoothed) normal and the tangent
       * reference. */
      const float3 tangent_x_old = math::normalize(
          math::cross(normal_old, tangent_reference_dir_old));
      const float3 tangent_x_new = math::normalize(
          math::cross(normal_new, tangent_reference_dir_new));

      /* The second tangent defined by the normal and first tangent. */
      const float3 tangent_y_old = math::normalize(math::cross(normal_old, tangent_x_old));
      const float3 tangent_y_new = math::normalize(math::cross(normal_new, tangent_x_new));

      /* Construct rotation matrix that encodes the orientation of the old surface position. */
      float3x3 rotation_old;
      copy_v3_v3(rotation_old.values[0], tangent_x_old);
      copy_v3_v3(rotation_old.values[1], tangent_y_old);
      copy_v3_v3(rotation_old.values[2], normal_old);

      /* Construct rotation matrix that encodes the orientation of the new surface position. */
      float3x3 rotation_new;
      copy_v3_v3(rotation_new.values[0], tangent_x_new);
      copy_v3_v3(rotation_new.values[1], tangent_y_new);
      copy_v3_v3(rotation_new.values[2], normal_new);

      /* Can use transpose instead of inverse because the matrix is orthonormal. In the case of
       * zero-area triangles, the matrix would not be orthonormal, but in this case, none of this
       * works anyway. */
      const float3x3 rotation_old_inv = rotation_old.transposed();

      /* Compute a rotation matrix that rotates points from the old to the new surface
       * orientation. */
      const float3x3 rotation = rotation_new * rotation_old_inv;
      float4x4 rotation_4x4;
      copy_m4_m3(rotation_4x4.values, rotation.values);

      /* Construction transformation matrix for this surface position that includes rotation and
       * translation. */
      float4x4 surface_transform = float4x4::identity();
      /* Subtract and add #pos_old, so that the rotation origin is the position on the surface. */
      sub_v3_v3(surface_transform.values[3], pos_old);
      mul_m4_m4_pre(surface_transform.values, rotation_4x4.values);
      add_v3_v3(surface_transform.values[3], pos_old);
      add_v3_v3(surface_transform.values[3], translation);

      /* Change the basis of the transformation so to that it can be applied in the local space of
       * the curves. */
      const float4x4 curve_transform = surface_to_curves * surface_transform * curves_to_surface;

      /* Actually transform all points. */
      const IndexRange points = curves.points_for_curve(curve_i);
      for (const int point_i : points) {
        const float3 old_point_pos = r_positions[point_i];
        const float3 new_point_pos = curve_transform * old_point_pos;
        r_positions[point_i] = new_point_pos;
      }

      if (!r_rotations.is_empty()) {
        for (const int point_i : points) {
          r_rotations[point_i] = rotation * r_rotations[point_i];
        }
      }
    }
  });
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet curves_geometry = params.extract_input<GeometrySet>("Curves");

  Mesh *surface_mesh_orig = nullptr;
  bool free_suface_mesh_orig = false;
  BLI_SCOPED_DEFER([&]() {
    if (free_suface_mesh_orig) {
      BKE_id_free(nullptr, surface_mesh_orig);
    }
  });

  auto pass_through_input = [&]() { params.set_output("Curves", std::move(curves_geometry)); };

  const Object *self_ob_eval = params.self_object();
  if (self_ob_eval == nullptr || self_ob_eval->type != OB_CURVES) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error, TIP_("Node only works for curves objects"));
    return;
  }
  const Curves *self_curves_eval = static_cast<const Curves *>(self_ob_eval->data);
  if (self_curves_eval->surface_uv_map == nullptr || self_curves_eval->surface_uv_map[0] == '\0') {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error, TIP_("Surface UV map not defined"));
    return;
  }
  /* Take surface information from self-object. */
  Object *surface_ob_eval = self_curves_eval->surface;
  const StringRefNull uv_map_name = self_curves_eval->surface_uv_map;
  const StringRefNull rest_position_name = "rest_position";

  if (!curves_geometry.has_curves()) {
    pass_through_input();
    return;
  }
  if (surface_ob_eval == nullptr || surface_ob_eval->type != OB_MESH) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error, TIP_("Curves not attached to a surface"));
    return;
  }
  Object *surface_ob_orig = DEG_get_original_object(surface_ob_eval);
  Mesh &surface_object_data = *static_cast<Mesh *>(surface_ob_orig->data);

  if (BMEditMesh *em = surface_object_data.edit_mesh) {
    surface_mesh_orig = BKE_mesh_from_bmesh_for_eval_nomain(em->bm, NULL, &surface_object_data);
    free_suface_mesh_orig = true;
  }
  else {
    surface_mesh_orig = &surface_object_data;
  }
  Mesh *surface_mesh_eval = BKE_modifier_get_evaluated_mesh_from_evaluated_object(surface_ob_eval);
  if (surface_mesh_eval == nullptr) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error, TIP_("Surface has no mesh"));
    return;
  }

  BKE_mesh_wrapper_ensure_mdata(surface_mesh_eval);

  const AttributeAccessor mesh_attributes_eval = bke::mesh_attributes(*surface_mesh_eval);
  const AttributeAccessor mesh_attributes_orig = bke::mesh_attributes(*surface_mesh_orig);

  Curves &curves_id = *curves_geometry.get_curves_for_write();
  CurvesGeometry &curves = CurvesGeometry::wrap(curves_id.geometry);

  if (!mesh_attributes_eval.contains(uv_map_name)) {
    pass_through_input();
    char *message = BLI_sprintfN(TIP_("Evaluated surface missing UV map: \"%s\""),
                                 uv_map_name.c_str());
    params.error_message_add(NodeWarningType::Error, message);
    MEM_freeN(message);
    return;
  }
  if (!mesh_attributes_orig.contains(uv_map_name)) {
    pass_through_input();
    char *message = BLI_sprintfN(TIP_("Original surface missing UV map: \"%s\""),
                                 uv_map_name.c_str());
    params.error_message_add(NodeWarningType::Error, message);
    MEM_freeN(message);
    return;
  }
  if (!mesh_attributes_eval.contains(rest_position_name)) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Evaluated surface missing attribute: \"rest_position\""));
    return;
  }
  if (curves.surface_uv_coords().is_empty() && curves.curves_num() > 0) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Curves are not attached to any UV map"));
    return;
  }
  const VArraySpan<float2> uv_map_orig = mesh_attributes_orig.lookup<float2>(uv_map_name,
                                                                             ATTR_DOMAIN_CORNER);
  const VArraySpan<float2> uv_map_eval = mesh_attributes_eval.lookup<float2>(uv_map_name,
                                                                             ATTR_DOMAIN_CORNER);
  const VArraySpan<float3> rest_positions = mesh_attributes_eval.lookup<float3>(rest_position_name,
                                                                                ATTR_DOMAIN_POINT);
  const Span<float2> surface_uv_coords = curves.surface_uv_coords();

  const Span<MLoopTri> looptris_orig{BKE_mesh_runtime_looptri_ensure(surface_mesh_orig),
                                     BKE_mesh_runtime_looptri_len(surface_mesh_orig)};
  const Span<MLoopTri> looptris_eval{BKE_mesh_runtime_looptri_ensure(surface_mesh_eval),
                                     BKE_mesh_runtime_looptri_len(surface_mesh_eval)};
  const ReverseUVSampler reverse_uv_sampler_orig{uv_map_orig, looptris_orig};
  const ReverseUVSampler reverse_uv_sampler_eval{uv_map_eval, looptris_eval};

  /* Retrieve face corner normals from each mesh. It's necessary to use face corner normals
   * because face normals or vertex normals may lose information (custom normals, auto smooth) in
   * some cases. It isn't yet possible to retrieve lazily calculated face corner normals from a
   * const mesh, so they are calculated here every time. */
  Array<float3> corner_normals_orig(surface_mesh_orig->totloop);
  Array<float3> corner_normals_eval(surface_mesh_eval->totloop);
  BKE_mesh_calc_normals_split_ex(
      surface_mesh_orig, nullptr, reinterpret_cast<float(*)[3]>(corner_normals_orig.data()));
  BKE_mesh_calc_normals_split_ex(
      surface_mesh_eval, nullptr, reinterpret_cast<float(*)[3]>(corner_normals_eval.data()));

  std::atomic<int> invalid_uv_count = 0;

  const bke::CurvesSurfaceTransforms transforms{*self_ob_eval, surface_ob_eval};

  bke::CurvesEditHints *edit_hints = curves_geometry.get_curve_edit_hints_for_write();
  MutableSpan<float3> edit_hint_positions;
  MutableSpan<float3x3> edit_hint_rotations;
  if (edit_hints != nullptr) {
    if (edit_hints->positions.has_value()) {
      edit_hint_positions = *edit_hints->positions;
    }
    if (!edit_hints->deform_mats.has_value()) {
      edit_hints->deform_mats.emplace(edit_hints->curves_id_orig.geometry.point_num,
                                      float3x3::identity());
      edit_hints->deform_mats->fill(float3x3::identity());
    }
    edit_hint_rotations = *edit_hints->deform_mats;
  }

  if (edit_hint_positions.is_empty()) {
    deform_curves(curves,
                  *surface_mesh_orig,
                  *surface_mesh_eval,
                  surface_uv_coords,
                  reverse_uv_sampler_orig,
                  reverse_uv_sampler_eval,
                  corner_normals_orig,
                  corner_normals_eval,
                  rest_positions,
                  transforms.surface_to_curves,
                  curves.positions_for_write(),
                  edit_hint_rotations,
                  invalid_uv_count);
  }
  else {
    /* First deform the actual curves in the input geometry. */
    deform_curves(curves,
                  *surface_mesh_orig,
                  *surface_mesh_eval,
                  surface_uv_coords,
                  reverse_uv_sampler_orig,
                  reverse_uv_sampler_eval,
                  corner_normals_orig,
                  corner_normals_eval,
                  rest_positions,
                  transforms.surface_to_curves,
                  curves.positions_for_write(),
                  {},
                  invalid_uv_count);
    /* Then also deform edit curve information for use in sculpt mode. */
    const CurvesGeometry &curves_orig = CurvesGeometry::wrap(edit_hints->curves_id_orig.geometry);
    deform_curves(curves_orig,
                  *surface_mesh_orig,
                  *surface_mesh_eval,
                  surface_uv_coords,
                  reverse_uv_sampler_orig,
                  reverse_uv_sampler_eval,
                  corner_normals_orig,
                  corner_normals_eval,
                  rest_positions,
                  transforms.surface_to_curves,
                  edit_hint_positions,
                  edit_hint_rotations,
                  invalid_uv_count);
  }

  curves.tag_positions_changed();

  if (invalid_uv_count) {
    char *message = BLI_sprintfN(TIP_("Invalid surface UVs on %d curves"),
                                 invalid_uv_count.load());
    params.error_message_add(NodeWarningType::Warning, message);
    MEM_freeN(message);
  }

  params.set_output("Curves", curves_geometry);
}

}  // namespace blender::nodes::node_geo_deform_curves_on_surface_cc

void register_node_type_geo_deform_curves_on_surface()
{
  namespace file_ns = blender::nodes::node_geo_deform_curves_on_surface_cc;

  static bNodeType ntype;
  geo_node_type_base(
      &ntype, GEO_NODE_DEFORM_CURVES_ON_SURFACE, "Deform Curves on Surface", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  node_type_size(&ntype, 170, 120, 700);
  nodeRegisterType(&ntype);
}
