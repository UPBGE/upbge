/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "curves_sculpt_intern.h"
#include "paint_intern.h"

#include "BLI_math_vector.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "BKE_attribute.h"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"

#include "ED_curves_sculpt.h"

struct ARegion;
struct RegionView3D;
struct Depsgraph;
struct View3D;
struct Object;
struct Brush;
struct Scene;
struct BVHTreeFromMesh;
struct ReportList;

namespace blender::ed::sculpt_paint {

using bke::CurvesGeometry;
using bke::CurvesSurfaceTransforms;

struct StrokeExtension {
  bool is_first;
  float2 mouse_position;
  float pressure;
  ReportList *reports = nullptr;
};

float brush_radius_factor(const Brush &brush, const StrokeExtension &stroke_extension);
float brush_radius_get(const Scene &scene,
                       const Brush &brush,
                       const StrokeExtension &stroke_extension);

float brush_strength_factor(const Brush &brush, const StrokeExtension &stroke_extension);
float brush_strength_get(const Scene &scene,
                         const Brush &brush,
                         const StrokeExtension &stroke_extension);

/**
 * Base class for stroke based operations in curves sculpt mode.
 */
class CurvesSculptStrokeOperation {
 public:
  virtual ~CurvesSculptStrokeOperation() = default;
  virtual void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) = 0;
};

std::unique_ptr<CurvesSculptStrokeOperation> new_add_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_comb_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_delete_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_snake_hook_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_grow_shrink_operation(
    const BrushStrokeMode brush_mode, const bContext &C);
std::unique_ptr<CurvesSculptStrokeOperation> new_selection_paint_operation(
    const BrushStrokeMode brush_mode, const bContext &C);
std::unique_ptr<CurvesSculptStrokeOperation> new_pinch_operation(const BrushStrokeMode brush_mode,
                                                                 const bContext &C);
std::unique_ptr<CurvesSculptStrokeOperation> new_smooth_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_puff_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_density_operation(
    const BrushStrokeMode brush_mode, const bContext &C, const StrokeExtension &stroke_start);
std::unique_ptr<CurvesSculptStrokeOperation> new_slide_operation();

struct CurvesBrush3D {
  float3 position_cu;
  float radius_cu;
};

/**
 * Find 3d brush position based on cursor position for curves sculpting.
 */
std::optional<CurvesBrush3D> sample_curves_3d_brush(const Depsgraph &depsgraph,
                                                    const ARegion &region,
                                                    const View3D &v3d,
                                                    const RegionView3D &rv3d,
                                                    const Object &curves_object,
                                                    const float2 &brush_pos_re,
                                                    const float brush_radius_re);

Vector<float4x4> get_symmetry_brush_transforms(eCurvesSymmetryType symmetry);

/**
 * Get the floating point selection on the curve domain, averaged from points if necessary.
 */
VArray<float> get_curves_selection(const Curves &curves_id);

/**
 * Get the floating point selection on the curve domain, copied from curves if necessary.
 */
VArray<float> get_point_selection(const Curves &curves_id);

void move_last_point_and_resample(MutableSpan<float3> positions, const float3 &new_last_position);

class CurvesSculptCommonContext {
 public:
  const Depsgraph *depsgraph = nullptr;
  const Scene *scene = nullptr;
  ARegion *region = nullptr;
  const View3D *v3d = nullptr;
  RegionView3D *rv3d = nullptr;

  CurvesSculptCommonContext(const bContext &C);
};

std::optional<CurvesBrush3D> sample_curves_surface_3d_brush(
    const Depsgraph &depsgraph,
    const ARegion &region,
    const View3D &v3d,
    const CurvesSurfaceTransforms &transforms,
    const BVHTreeFromMesh &surface_bvh,
    const float2 &brush_pos_re,
    const float brush_radius_re);

float transform_brush_radius(const float4x4 &transform,
                             const float3 &brush_position,
                             const float old_radius);

void report_empty_original_surface(ReportList *reports);
void report_empty_evaluated_surface(ReportList *reports);
void report_missing_surface(ReportList *reports);
void report_missing_uv_map_on_original_surface(ReportList *reports);
void report_missing_uv_map_on_evaluated_surface(ReportList *reports);
void report_invalid_uv_map(ReportList *reports);

}  // namespace blender::ed::sculpt_paint
