/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>

#include "curves_sculpt_intern.hh"

#include "BLI_float4x4.hh"
#include "BLI_kdtree.h"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

#include "PIL_time.h"

#include "DEG_depsgraph.h"

#include "BKE_attribute_math.hh"
#include "BKE_brush.h"
#include "BKE_bvhutils.h"
#include "BKE_context.h"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_geometry_set.hh"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_sample.hh"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_report.h"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_screen.h"
#include "ED_view3d.h"

#include "GEO_add_curves_on_mesh.hh"

#include "WM_api.h"

#include "DEG_depsgraph_query.h"

/**
 * The code below uses a suffix naming convention to indicate the coordinate space:
 * cu: Local space of the curves object that is being edited.
 * su: Local space of the surface object.
 * wo: World space.
 * re: 2D coordinates within the region.
 */

namespace blender::ed::sculpt_paint {

using bke::CurvesGeometry;

class AddOperation : public CurvesSculptStrokeOperation {
 private:
  /** Used when some data should be interpolated from existing curves. */
  KDTree_3d *curve_roots_kdtree_ = nullptr;

  friend struct AddOperationExecutor;

 public:
  ~AddOperation() override
  {
    if (curve_roots_kdtree_ != nullptr) {
      BLI_kdtree_3d_free(curve_roots_kdtree_);
    }
  }

  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override;
};

/**
 * Utility class that actually executes the update when the stroke is updated. That's useful
 * because it avoids passing a very large number of parameters between functions.
 */
struct AddOperationExecutor {
  AddOperation *self_ = nullptr;
  CurvesSculptCommonContext ctx_;

  Object *curves_ob_orig_ = nullptr;
  Curves *curves_id_orig_ = nullptr;
  CurvesGeometry *curves_orig_ = nullptr;

  Object *surface_ob_eval_ = nullptr;
  Mesh *surface_eval_ = nullptr;
  Span<MLoopTri> surface_looptris_eval_;
  VArraySpan<float2> surface_uv_map_eval_;
  BVHTreeFromMesh surface_bvh_eval_;

  const CurvesSculpt *curves_sculpt_ = nullptr;
  const Brush *brush_ = nullptr;
  const BrushCurvesSculptSettings *brush_settings_ = nullptr;
  int add_amount_;
  bool use_front_face_;

  float brush_radius_re_;
  float2 brush_pos_re_;

  CurvesSurfaceTransforms transforms_;

  AddOperationExecutor(const bContext &C) : ctx_(C)
  {
  }

  void execute(AddOperation &self, const bContext &C, const StrokeExtension &stroke_extension)
  {
    self_ = &self;
    curves_ob_orig_ = CTX_data_active_object(&C);

    curves_id_orig_ = static_cast<Curves *>(curves_ob_orig_->data);
    curves_orig_ = &CurvesGeometry::wrap(curves_id_orig_->geometry);

    if (curves_id_orig_->surface == nullptr || curves_id_orig_->surface->type != OB_MESH) {
      report_missing_surface(stroke_extension.reports);
      return;
    }

    transforms_ = CurvesSurfaceTransforms(*curves_ob_orig_, curves_id_orig_->surface);

    Object &surface_ob_orig = *curves_id_orig_->surface;
    Mesh &surface_orig = *static_cast<Mesh *>(surface_ob_orig.data);
    if (surface_orig.totpoly == 0) {
      report_empty_original_surface(stroke_extension.reports);
      return;
    }

    surface_ob_eval_ = DEG_get_evaluated_object(ctx_.depsgraph, &surface_ob_orig);
    if (surface_ob_eval_ == nullptr) {
      return;
    }
    surface_eval_ = BKE_object_get_evaluated_mesh(surface_ob_eval_);
    if (surface_eval_->totpoly == 0) {
      report_empty_evaluated_surface(stroke_extension.reports);
      return;
    }

    curves_sculpt_ = ctx_.scene->toolsettings->curves_sculpt;
    brush_ = BKE_paint_brush_for_read(&curves_sculpt_->paint);
    brush_settings_ = brush_->curves_sculpt_settings;
    brush_radius_re_ = brush_radius_get(*ctx_.scene, *brush_, stroke_extension);
    brush_pos_re_ = stroke_extension.mouse_position;

    use_front_face_ = brush_->flag & BRUSH_FRONTFACE;
    const eBrushFalloffShape falloff_shape = static_cast<eBrushFalloffShape>(
        brush_->falloff_shape);
    add_amount_ = std::max(0, brush_settings_->add_amount);

    if (add_amount_ == 0) {
      return;
    }

    /* Find UV map. */
    VArraySpan<float2> surface_uv_map;
    if (curves_id_orig_->surface_uv_map != nullptr) {
      surface_uv_map = bke::mesh_attributes(surface_orig)
                           .lookup<float2>(curves_id_orig_->surface_uv_map, ATTR_DOMAIN_CORNER);
      surface_uv_map_eval_ = bke::mesh_attributes(*surface_eval_)
                                 .lookup<float2>(curves_id_orig_->surface_uv_map,
                                                 ATTR_DOMAIN_CORNER);
    }

    if (surface_uv_map.is_empty()) {
      report_missing_uv_map_on_original_surface(stroke_extension.reports);
      return;
    }
    if (surface_uv_map_eval_.is_empty()) {
      report_missing_uv_map_on_evaluated_surface(stroke_extension.reports);
      return;
    }

    const double time = PIL_check_seconds_timer() * 1000000.0;
    /* Use a pointer cast to avoid overflow warnings. */
    RandomNumberGenerator rng{*(uint32_t *)(&time)};

    BKE_bvhtree_from_mesh_get(&surface_bvh_eval_, surface_eval_, BVHTREE_FROM_LOOPTRI, 2);
    BLI_SCOPED_DEFER([&]() { free_bvhtree_from_mesh(&surface_bvh_eval_); });

    surface_looptris_eval_ = {BKE_mesh_runtime_looptri_ensure(surface_eval_),
                              BKE_mesh_runtime_looptri_len(surface_eval_)};

    /* Sample points on the surface using one of multiple strategies. */
    Vector<float2> sampled_uvs;
    if (add_amount_ == 1) {
      this->sample_in_center_with_symmetry(sampled_uvs);
    }
    else if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      this->sample_projected_with_symmetry(rng, sampled_uvs);
    }
    else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
      this->sample_spherical_with_symmetry(rng, sampled_uvs);
    }
    else {
      BLI_assert_unreachable();
    }

    if (sampled_uvs.is_empty()) {
      /* No new points have been added. */
      return;
    }

    const Span<MLoopTri> surface_looptris_orig = {BKE_mesh_runtime_looptri_ensure(&surface_orig),
                                                  BKE_mesh_runtime_looptri_len(&surface_orig)};

    /* Find normals. */
    if (!CustomData_has_layer(&surface_orig.ldata, CD_NORMAL)) {
      BKE_mesh_calc_normals_split(&surface_orig);
    }
    const Span<float3> corner_normals_su = {
        reinterpret_cast<const float3 *>(CustomData_get_layer(&surface_orig.ldata, CD_NORMAL)),
        surface_orig.totloop};

    const geometry::ReverseUVSampler reverse_uv_sampler{surface_uv_map, surface_looptris_orig};

    geometry::AddCurvesOnMeshInputs add_inputs;
    add_inputs.uvs = sampled_uvs;
    add_inputs.interpolate_length = brush_settings_->flag &
                                    BRUSH_CURVES_SCULPT_FLAG_INTERPOLATE_LENGTH;
    add_inputs.interpolate_shape = brush_settings_->flag &
                                   BRUSH_CURVES_SCULPT_FLAG_INTERPOLATE_SHAPE;
    add_inputs.interpolate_point_count = brush_settings_->flag &
                                         BRUSH_CURVES_SCULPT_FLAG_INTERPOLATE_POINT_COUNT;
    add_inputs.fallback_curve_length = brush_settings_->curve_length;
    add_inputs.fallback_point_count = std::max(2, brush_settings_->points_per_curve);
    add_inputs.transforms = &transforms_;
    add_inputs.reverse_uv_sampler = &reverse_uv_sampler;
    add_inputs.surface = &surface_orig;
    add_inputs.corner_normals_su = corner_normals_su;

    if (add_inputs.interpolate_length || add_inputs.interpolate_shape ||
        add_inputs.interpolate_point_count) {
      this->ensure_curve_roots_kdtree();
      add_inputs.old_roots_kdtree = self_->curve_roots_kdtree_;
    }

    const geometry::AddCurvesOnMeshOutputs add_outputs = geometry::add_curves_on_mesh(
        *curves_orig_, add_inputs);

    if (add_outputs.uv_error) {
      report_invalid_uv_map(stroke_extension.reports);
    }

    DEG_id_tag_update(&curves_id_orig_->id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &curves_id_orig_->id);
    ED_region_tag_redraw(ctx_.region);
  }

  /**
   * Sample a single point exactly at the mouse position.
   */
  void sample_in_center_with_symmetry(Vector<float2> &r_sampled_uvs)
  {
    float3 ray_start_wo, ray_end_wo;
    ED_view3d_win_to_segment_clipped(
        ctx_.depsgraph, ctx_.region, ctx_.v3d, brush_pos_re_, ray_start_wo, ray_end_wo, true);
    const float3 ray_start_cu = transforms_.world_to_curves * ray_start_wo;
    const float3 ray_end_cu = transforms_.world_to_curves * ray_end_wo;

    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_orig_->symmetry));

    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      const float4x4 transform = transforms_.curves_to_surface * brush_transform;
      this->sample_in_center(r_sampled_uvs, transform * ray_start_cu, transform * ray_end_cu);
    }
  }

  void sample_in_center(Vector<float2> &r_sampled_uvs,
                        const float3 &ray_start_su,
                        const float3 &ray_end_su)
  {
    const float3 ray_direction_su = math::normalize(ray_end_su - ray_start_su);

    BVHTreeRayHit ray_hit;
    ray_hit.dist = FLT_MAX;
    ray_hit.index = -1;
    BLI_bvhtree_ray_cast(surface_bvh_eval_.tree,
                         ray_start_su,
                         ray_direction_su,
                         0.0f,
                         &ray_hit,
                         surface_bvh_eval_.raycast_callback,
                         &surface_bvh_eval_);

    if (ray_hit.index == -1) {
      return;
    }

    const int looptri_index = ray_hit.index;
    const MLoopTri &looptri = surface_looptris_eval_[looptri_index];
    const float3 brush_pos_su = ray_hit.co;
    const float3 bary_coords = bke::mesh_surface_sample::compute_bary_coord_in_triangle(
        *surface_eval_, looptri, brush_pos_su);

    const float2 uv = bke::mesh_surface_sample::sample_corner_attrribute_with_bary_coords(
        bary_coords, looptri, surface_uv_map_eval_);
    r_sampled_uvs.append(uv);
  }

  /**
   * Sample points by shooting rays within the brush radius in the 3D view.
   */
  void sample_projected_with_symmetry(RandomNumberGenerator &rng, Vector<float2> &r_sampled_uvs)
  {
    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_orig_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->sample_projected(rng, r_sampled_uvs, brush_transform);
    }
  }

  void sample_projected(RandomNumberGenerator &rng,
                        Vector<float2> &r_sampled_uvs,
                        const float4x4 &brush_transform)
  {
    const int old_amount = r_sampled_uvs.size();
    const int max_iterations = 100;
    int current_iteration = 0;
    while (r_sampled_uvs.size() < old_amount + add_amount_) {
      if (current_iteration++ >= max_iterations) {
        break;
      }
      Vector<float3> bary_coords;
      Vector<int> looptri_indices;
      Vector<float3> positions_su;

      const int missing_amount = add_amount_ + old_amount - r_sampled_uvs.size();
      const int new_points = bke::mesh_surface_sample::sample_surface_points_projected(
          rng,
          *surface_eval_,
          surface_bvh_eval_,
          brush_pos_re_,
          brush_radius_re_,
          [&](const float2 &pos_re, float3 &r_start_su, float3 &r_end_su) {
            float3 start_wo, end_wo;
            ED_view3d_win_to_segment_clipped(
                ctx_.depsgraph, ctx_.region, ctx_.v3d, pos_re, start_wo, end_wo, true);
            const float3 start_cu = brush_transform * (transforms_.world_to_curves * start_wo);
            const float3 end_cu = brush_transform * (transforms_.world_to_curves * end_wo);
            r_start_su = transforms_.curves_to_surface * start_cu;
            r_end_su = transforms_.curves_to_surface * end_cu;
          },
          use_front_face_,
          add_amount_,
          missing_amount,
          bary_coords,
          looptri_indices,
          positions_su);

      for (const int i : IndexRange(new_points)) {
        const float2 uv = bke::mesh_surface_sample::sample_corner_attrribute_with_bary_coords(
            bary_coords[i], surface_looptris_eval_[looptri_indices[i]], surface_uv_map_eval_);
        r_sampled_uvs.append(uv);
      }
    }
  }

  /**
   * Sample points in a 3D sphere around the surface position that the mouse hovers over.
   */
  void sample_spherical_with_symmetry(RandomNumberGenerator &rng, Vector<float2> &r_sampled_uvs)
  {
    const std::optional<CurvesBrush3D> brush_3d = sample_curves_surface_3d_brush(*ctx_.depsgraph,
                                                                                 *ctx_.region,
                                                                                 *ctx_.v3d,
                                                                                 transforms_,
                                                                                 surface_bvh_eval_,
                                                                                 brush_pos_re_,
                                                                                 brush_radius_re_);
    if (!brush_3d.has_value()) {
      return;
    }

    float3 view_ray_start_wo, view_ray_end_wo;
    ED_view3d_win_to_segment_clipped(ctx_.depsgraph,
                                     ctx_.region,
                                     ctx_.v3d,
                                     brush_pos_re_,
                                     view_ray_start_wo,
                                     view_ray_end_wo,
                                     true);

    const float3 view_ray_start_cu = transforms_.world_to_curves * view_ray_start_wo;
    const float3 view_ray_end_cu = transforms_.world_to_curves * view_ray_end_wo;

    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_orig_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      const float4x4 transform = transforms_.curves_to_surface * brush_transform;

      const float3 brush_pos_su = transform * brush_3d->position_cu;
      const float3 view_direction_su = math::normalize(transform * view_ray_end_cu -
                                                       transform * view_ray_start_cu);
      const float brush_radius_su = transform_brush_radius(
          transform, brush_3d->position_cu, brush_3d->radius_cu);

      this->sample_spherical(rng, r_sampled_uvs, brush_pos_su, brush_radius_su, view_direction_su);
    }
  }

  void sample_spherical(RandomNumberGenerator &rng,
                        Vector<float2> &r_sampled_uvs,
                        const float3 &brush_pos_su,
                        const float brush_radius_su,
                        const float3 &view_direction_su)
  {
    const float brush_radius_sq_su = pow2f(brush_radius_su);

    /* Find surface triangles within brush radius. */
    Vector<int> selected_looptri_indices;
    if (use_front_face_) {
      BLI_bvhtree_range_query_cpp(
          *surface_bvh_eval_.tree,
          brush_pos_su,
          brush_radius_su,
          [&](const int index, const float3 &UNUSED(co), const float UNUSED(dist_sq)) {
            const MLoopTri &looptri = surface_looptris_eval_[index];
            const float3 v0_su = surface_eval_->mvert[surface_eval_->mloop[looptri.tri[0]].v].co;
            const float3 v1_su = surface_eval_->mvert[surface_eval_->mloop[looptri.tri[1]].v].co;
            const float3 v2_su = surface_eval_->mvert[surface_eval_->mloop[looptri.tri[2]].v].co;
            float3 normal_su;
            normal_tri_v3(normal_su, v0_su, v1_su, v2_su);
            if (math::dot(normal_su, view_direction_su) >= 0.0f) {
              return;
            }
            selected_looptri_indices.append(index);
          });
    }
    else {
      BLI_bvhtree_range_query_cpp(
          *surface_bvh_eval_.tree,
          brush_pos_su,
          brush_radius_su,
          [&](const int index, const float3 &UNUSED(co), const float UNUSED(dist_sq)) {
            selected_looptri_indices.append(index);
          });
    }

    /* Density used for sampling points. This does not have to be exact, because the loop below
     * automatically runs until enough samples have been found. If too many samples are found, some
     * will be discarded afterwards. */
    const float brush_plane_area_su = M_PI * brush_radius_sq_su;
    const float approximate_density_su = add_amount_ / brush_plane_area_su;

    /* Usually one or two iterations should be enough. */
    const int max_iterations = 5;
    int current_iteration = 0;

    const int old_amount = r_sampled_uvs.size();
    while (r_sampled_uvs.size() < old_amount + add_amount_) {
      if (current_iteration++ >= max_iterations) {
        break;
      }
      Vector<float3> bary_coords;
      Vector<int> looptri_indices;
      Vector<float3> positions_su;
      const int new_points = bke::mesh_surface_sample::sample_surface_points_spherical(
          rng,
          *surface_eval_,
          selected_looptri_indices,
          brush_pos_su,
          brush_radius_su,
          approximate_density_su,
          bary_coords,
          looptri_indices,
          positions_su);
      for (const int i : IndexRange(new_points)) {
        const float2 uv = bke::mesh_surface_sample::sample_corner_attrribute_with_bary_coords(
            bary_coords[i], surface_looptris_eval_[looptri_indices[i]], surface_uv_map_eval_);
        r_sampled_uvs.append(uv);
      }
    }

    /* Remove samples when there are too many. */
    while (r_sampled_uvs.size() > old_amount + add_amount_) {
      const int index_to_remove = rng.get_int32(add_amount_) + old_amount;
      r_sampled_uvs.remove_and_reorder(index_to_remove);
    }
  }

  void ensure_curve_roots_kdtree()
  {
    if (self_->curve_roots_kdtree_ == nullptr) {
      self_->curve_roots_kdtree_ = BLI_kdtree_3d_new(curves_orig_->curves_num());
      for (const int curve_i : curves_orig_->curves_range()) {
        const int root_point_i = curves_orig_->offsets()[curve_i];
        const float3 &root_pos_cu = curves_orig_->positions()[root_point_i];
        BLI_kdtree_3d_insert(self_->curve_roots_kdtree_, curve_i, root_pos_cu);
      }
      BLI_kdtree_3d_balance(self_->curve_roots_kdtree_);
    }
  }
};

void AddOperation::on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension)
{
  AddOperationExecutor executor{C};
  executor.execute(*this, C, stroke_extension);
}

std::unique_ptr<CurvesSculptStrokeOperation> new_add_operation()
{
  return std::make_unique<AddOperation>();
}

}  // namespace blender::ed::sculpt_paint
