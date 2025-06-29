/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_paint.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "DEG_depsgraph.hh"

#include "DNA_brush_types.h"

#include "WM_api.hh"

#include "BLI_task.hh"

#include "curves_sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

class SmoothOperation : public CurvesSculptStrokeOperation {
 private:
  /** Only used when a 3D brush is used. */
  CurvesBrush3D brush_3d_;

  friend struct SmoothOperationExecutor;

 public:
  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override;
};

/**
 * Utility class that actually executes the update when the stroke is updated. That's useful
 * because it avoids passing a very large number of parameters between functions.
 */
struct SmoothOperationExecutor {
  SmoothOperation *self_ = nullptr;
  CurvesSculptCommonContext ctx_;

  Object *object_ = nullptr;
  Curves *curves_id_ = nullptr;
  CurvesGeometry *curves_ = nullptr;

  VArray<float> point_factors_;
  IndexMaskMemory selected_curve_memory_;
  IndexMask curve_selection_;

  CurvesSculpt *curves_sculpt_ = nullptr;
  const Brush *brush_ = nullptr;
  float brush_radius_base_re_;
  float brush_radius_factor_;
  float brush_strength_;
  float2 brush_pos_re_;

  CurvesSurfaceTransforms transforms_;

  SmoothOperationExecutor(const bContext &C) : ctx_(C) {}

  void execute(SmoothOperation &self, const bContext &C, const StrokeExtension &stroke_extension)
  {
    UNUSED_VARS(C, stroke_extension);
    self_ = &self;

    object_ = CTX_data_active_object(&C);
    curves_id_ = static_cast<Curves *>(object_->data);
    curves_ = &curves_id_->geometry.wrap();
    if (curves_->is_empty()) {
      return;
    }

    curves_sculpt_ = ctx_.scene->toolsettings->curves_sculpt;
    brush_ = BKE_paint_brush_for_read(&curves_sculpt_->paint);
    brush_radius_base_re_ = BKE_brush_size_get(&curves_sculpt_->paint, brush_);
    brush_radius_factor_ = brush_radius_factor(*brush_, stroke_extension);
    brush_strength_ = brush_strength_get(curves_sculpt_->paint, *brush_, stroke_extension);
    brush_pos_re_ = stroke_extension.mouse_position;

    point_factors_ = *curves_->attributes().lookup_or_default<float>(
        ".selection", bke::AttrDomain::Point, 1.0f);
    curve_selection_ = curves::retrieve_selected_curves(*curves_id_, selected_curve_memory_);
    transforms_ = CurvesSurfaceTransforms(*object_, curves_id_->surface);

    const eBrushFalloffShape falloff_shape = eBrushFalloffShape(brush_->falloff_shape);
    if (stroke_extension.is_first) {
      if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE || (U.uiflag & USER_ORBIT_SELECTION)) {
        self.brush_3d_ = *sample_curves_3d_brush(*ctx_.depsgraph,
                                                 *ctx_.region,
                                                 *ctx_.v3d,
                                                 *ctx_.rv3d,
                                                 *object_,
                                                 brush_pos_re_,
                                                 brush_radius_base_re_);
        remember_stroke_position(
            *curves_sculpt_,
            math::transform_point(transforms_.curves_to_world, self_->brush_3d_.position_cu));
      }
    }

    Array<float> point_smooth_factors(curves_->points_num(), 0.0f);

    if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      this->find_projected_smooth_factors_with_symmetry(point_smooth_factors);
    }
    else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
      this->find_spherical_smooth_factors_with_symmetry(point_smooth_factors);
    }
    else {
      BLI_assert_unreachable();
    }

    this->smooth(point_smooth_factors);
    curves_->tag_positions_changed();
    DEG_id_tag_update(&curves_id_->id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &curves_id_->id);
    ED_region_tag_redraw(ctx_.region);
  }

  void find_projected_smooth_factors_with_symmetry(MutableSpan<float> r_point_smooth_factors)
  {
    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->find_projected_smooth_factors(brush_transform, r_point_smooth_factors);
    }
  }

  void find_projected_smooth_factors(const float4x4 &brush_transform,
                                     MutableSpan<float> r_point_smooth_factors)
  {
    const float4x4 brush_transform_inv = math::invert(brush_transform);

    const float brush_radius_re = brush_radius_base_re_ * brush_radius_factor_;
    const float brush_radius_sq_re = pow2f(brush_radius_re);

    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx_.rv3d, object_);

    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      for (const int point_i : points) {
        const float3 &pos_cu = math::transform_point(brush_transform_inv,
                                                     deformation.positions[point_i]);
        const float2 pos_re = ED_view3d_project_float_v2_m4(ctx_.region, pos_cu, projection);
        const float dist_to_brush_sq_re = math::distance_squared(pos_re, brush_pos_re_);
        if (dist_to_brush_sq_re > brush_radius_sq_re) {
          continue;
        }

        const float dist_to_brush_re = std::sqrt(dist_to_brush_sq_re);
        const float radius_falloff = BKE_brush_curve_strength(
            brush_, dist_to_brush_re, brush_radius_re);
        /* Used to make the brush easier to use. Otherwise a strength of 1 would be way too
         * large. */
        const float weight_factor = 0.1f;
        const float weight = weight_factor * brush_strength_ * radius_falloff *
                             point_factors_[point_i];
        math::max_inplace(r_point_smooth_factors[point_i], weight);
      }
    });
  }

  void find_spherical_smooth_factors_with_symmetry(MutableSpan<float> r_point_smooth_factors)
  {
    float3 brush_pos_wo;
    ED_view3d_win_to_3d(
        ctx_.v3d,
        ctx_.region,
        math::transform_point(transforms_.curves_to_world, self_->brush_3d_.position_cu),
        brush_pos_re_,
        brush_pos_wo);
    const float3 brush_pos_cu = math::transform_point(transforms_.world_to_curves, brush_pos_wo);
    const float brush_radius_cu = self_->brush_3d_.radius_cu * brush_radius_factor_;

    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->find_spherical_smooth_factors(math::transform_point(brush_transform, brush_pos_cu),
                                          brush_radius_cu,
                                          r_point_smooth_factors);
    }
  }

  void find_spherical_smooth_factors(const float3 &brush_pos_cu,
                                     const float brush_radius_cu,
                                     MutableSpan<float> r_point_smooth_factors)
  {
    const float brush_radius_sq_cu = pow2f(brush_radius_cu);
    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      for (const int point_i : points) {
        const float3 &pos_cu = deformation.positions[point_i];
        const float dist_to_brush_sq_cu = math::distance_squared(pos_cu, brush_pos_cu);
        if (dist_to_brush_sq_cu > brush_radius_sq_cu) {
          continue;
        }

        const float dist_to_brush_cu = std::sqrt(dist_to_brush_sq_cu);
        const float radius_falloff = BKE_brush_curve_strength(
            brush_, dist_to_brush_cu, brush_radius_cu);
        /* Used to make the brush easier to use. Otherwise a strength of 1 would be way too
         * large. */
        const float weight_factor = 0.1f;
        const float weight = weight_factor * brush_strength_ * radius_falloff *
                             point_factors_[point_i];
        math::max_inplace(r_point_smooth_factors[point_i], weight);
      }
    });
  }

  void smooth(const Span<float> point_smooth_factors)
  {
    const OffsetIndices points_by_curve = curves_->points_by_curve();
    MutableSpan<float3> positions = curves_->positions_for_write();

    curve_selection_.foreach_segment(GrainSize(256), [&](const IndexMaskSegment segment) {
      Vector<float3> old_positions;
      for (const int curve_i : segment) {
        const IndexRange points = points_by_curve[curve_i];
        old_positions.clear();
        old_positions.extend(positions.slice(points));
        for (const int i : IndexRange(points.size()).drop_front(1).drop_back(1)) {
          const int point_i = points[i];
          const float smooth_factor = point_smooth_factors[point_i];
          if (smooth_factor == 0.0f) {
            continue;
          }
          /* Move towards the middle of the neighboring points. */
          const float3 old_pos = old_positions[i];
          const float3 &prev_pos = old_positions[i - 1];
          const float3 &next_pos = old_positions[i + 1];
          const float3 goal_pos = math::midpoint(prev_pos, next_pos);
          const float3 new_pos = math::interpolate(old_pos, goal_pos, smooth_factor);
          positions[point_i] = new_pos;
        }
      }
    });
  }
};

void SmoothOperation::on_stroke_extended(const bContext &C,
                                         const StrokeExtension &stroke_extension)
{
  SmoothOperationExecutor executor{C};
  executor.execute(*this, C, stroke_extension);
}

std::unique_ptr<CurvesSculptStrokeOperation> new_smooth_operation()
{
  return std::make_unique<SmoothOperation>();
}

}  // namespace blender::ed::sculpt_paint
