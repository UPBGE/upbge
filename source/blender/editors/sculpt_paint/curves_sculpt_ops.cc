/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_kdtree.h"
#include "BLI_rand.hh"
#include "BLI_utildefines.h"
#include "BLI_vector_set.hh"

#include "BKE_brush.h"
#include "BKE_bvhutils.h"
#include "BKE_context.h"
#include "BKE_curves.hh"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_paint.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_toolsystem.h"

#include "ED_curves.h"
#include "ED_curves_sculpt.h"
#include "ED_image.h"
#include "ED_object.h"
#include "ED_screen.h"
#include "ED_space_api.h"
#include "ED_view3d.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "DNA_brush_types.h"
#include "DNA_curves_types.h"
#include "DNA_screen_types.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "curves_sculpt_intern.h"
#include "curves_sculpt_intern.hh"
#include "paint_intern.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "GPU_immediate.h"
#include "GPU_immediate_util.h"
#include "GPU_matrix.h"
#include "GPU_state.h"

/* -------------------------------------------------------------------- */
/** \name Poll Functions
 * \{ */

bool CURVES_SCULPT_mode_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->mode & OB_MODE_SCULPT_CURVES;
}

bool CURVES_SCULPT_mode_poll_view3d(bContext *C)
{
  if (!CURVES_SCULPT_mode_poll(C)) {
    return false;
  }
  if (CTX_wm_region_view3d(C) == nullptr) {
    return false;
  }
  return true;
}

/** \} */

namespace blender::ed::sculpt_paint {

using blender::bke::CurvesGeometry;

/* -------------------------------------------------------------------- */
/** \name * SCULPT_CURVES_OT_brush_stroke
 * \{ */

float brush_radius_factor(const Brush &brush, const StrokeExtension &stroke_extension)
{
  if (BKE_brush_use_size_pressure(&brush)) {
    return stroke_extension.pressure;
  }
  return 1.0f;
}

float brush_radius_get(const Scene &scene,
                       const Brush &brush,
                       const StrokeExtension &stroke_extension)
{
  return BKE_brush_size_get(&scene, &brush) * brush_radius_factor(brush, stroke_extension);
}

float brush_strength_factor(const Brush &brush, const StrokeExtension &stroke_extension)
{
  if (BKE_brush_use_alpha_pressure(&brush)) {
    return stroke_extension.pressure;
  }
  return 1.0f;
}

float brush_strength_get(const Scene &scene,
                         const Brush &brush,
                         const StrokeExtension &stroke_extension)
{
  return BKE_brush_alpha_get(&scene, &brush) * brush_strength_factor(brush, stroke_extension);
}

static std::unique_ptr<CurvesSculptStrokeOperation> start_brush_operation(
    bContext &C, wmOperator &op, const StrokeExtension &stroke_start)
{
  const BrushStrokeMode mode = static_cast<BrushStrokeMode>(RNA_enum_get(op.ptr, "mode"));

  const Scene &scene = *CTX_data_scene(&C);
  const CurvesSculpt &curves_sculpt = *scene.toolsettings->curves_sculpt;
  const Brush &brush = *BKE_paint_brush_for_read(&curves_sculpt.paint);
  switch (brush.curves_sculpt_tool) {
    case CURVES_SCULPT_TOOL_COMB:
      return new_comb_operation();
    case CURVES_SCULPT_TOOL_DELETE:
      return new_delete_operation();
    case CURVES_SCULPT_TOOL_SNAKE_HOOK:
      return new_snake_hook_operation();
    case CURVES_SCULPT_TOOL_ADD:
      return new_add_operation();
    case CURVES_SCULPT_TOOL_GROW_SHRINK:
      return new_grow_shrink_operation(mode, C);
    case CURVES_SCULPT_TOOL_SELECTION_PAINT:
      return new_selection_paint_operation(mode, C);
    case CURVES_SCULPT_TOOL_PINCH:
      return new_pinch_operation(mode, C);
    case CURVES_SCULPT_TOOL_SMOOTH:
      return new_smooth_operation();
    case CURVES_SCULPT_TOOL_PUFF:
      return new_puff_operation();
    case CURVES_SCULPT_TOOL_DENSITY:
      return new_density_operation(mode, C, stroke_start);
    case CURVES_SCULPT_TOOL_SLIDE:
      return new_slide_operation();
  }
  BLI_assert_unreachable();
  return {};
}

struct SculptCurvesBrushStrokeData {
  std::unique_ptr<CurvesSculptStrokeOperation> operation;
  PaintStroke *stroke;
};

static bool stroke_get_location(bContext *C,
                                float out[3],
                                const float mouse[2],
                                bool UNUSED(force_original))
{
  out[0] = mouse[0];
  out[1] = mouse[1];
  out[2] = 0;
  UNUSED_VARS(C);
  return true;
}

static bool stroke_test_start(bContext *C, struct wmOperator *op, const float mouse[2])
{
  UNUSED_VARS(C, op, mouse);
  return true;
}

static void stroke_update_step(bContext *C,
                               wmOperator *op,
                               PaintStroke *UNUSED(stroke),
                               PointerRNA *stroke_element)
{
  SculptCurvesBrushStrokeData *op_data = static_cast<SculptCurvesBrushStrokeData *>(
      op->customdata);

  StrokeExtension stroke_extension;
  RNA_float_get_array(stroke_element, "mouse", stroke_extension.mouse_position);
  stroke_extension.pressure = RNA_float_get(stroke_element, "pressure");
  stroke_extension.reports = op->reports;

  if (!op_data->operation) {
    stroke_extension.is_first = true;
    op_data->operation = start_brush_operation(*C, *op, stroke_extension);
  }
  else {
    stroke_extension.is_first = false;
  }

  if (op_data->operation) {
    op_data->operation->on_stroke_extended(*C, stroke_extension);
  }
}

static void stroke_done(const bContext *C, PaintStroke *stroke)
{
  UNUSED_VARS(C, stroke);
}

static int sculpt_curves_stroke_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  SculptCurvesBrushStrokeData *op_data = MEM_new<SculptCurvesBrushStrokeData>(__func__);
  op_data->stroke = paint_stroke_new(C,
                                     op,
                                     stroke_get_location,
                                     stroke_test_start,
                                     stroke_update_step,
                                     nullptr,
                                     stroke_done,
                                     event->type);
  op->customdata = op_data;

  int return_value = op->type->modal(C, op, event);
  if (return_value == OPERATOR_FINISHED) {
    paint_stroke_free(C, op, op_data->stroke);
    MEM_delete(op_data);
    return OPERATOR_FINISHED;
  }

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static int sculpt_curves_stroke_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  SculptCurvesBrushStrokeData *op_data = static_cast<SculptCurvesBrushStrokeData *>(
      op->customdata);
  int return_value = paint_stroke_modal(C, op, event, &op_data->stroke);
  if (ELEM(return_value, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    MEM_delete(op_data);
  }
  return return_value;
}

static void sculpt_curves_stroke_cancel(bContext *C, wmOperator *op)
{
  SculptCurvesBrushStrokeData *op_data = static_cast<SculptCurvesBrushStrokeData *>(
      op->customdata);
  paint_stroke_cancel(C, op, op_data->stroke);
  MEM_delete(op_data);
}

static void SCULPT_CURVES_OT_brush_stroke(struct wmOperatorType *ot)
{
  ot->name = "Stroke Curves Sculpt";
  ot->idname = "SCULPT_CURVES_OT_brush_stroke";
  ot->description = "Sculpt curves using a brush";

  ot->invoke = sculpt_curves_stroke_invoke;
  ot->modal = sculpt_curves_stroke_modal;
  ot->cancel = sculpt_curves_stroke_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  paint_stroke_operator_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name * CURVES_OT_sculptmode_toggle
 * \{ */

static void curves_sculptmode_enter(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);

  Object *ob = CTX_data_active_object(C);
  BKE_paint_ensure(scene->toolsettings, (Paint **)&scene->toolsettings->curves_sculpt);
  CurvesSculpt *curves_sculpt = scene->toolsettings->curves_sculpt;

  ob->mode = OB_MODE_SCULPT_CURVES;

  ED_paint_cursor_start(&curves_sculpt->paint, CURVES_SCULPT_mode_poll_view3d);

  /* Necessary to change the object mode on the evaluated object. */
  DEG_id_tag_update(&ob->id, ID_RECALC_COPY_ON_WRITE);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
}

static void curves_sculptmode_exit(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  ob->mode = OB_MODE_OBJECT;
}

static int curves_sculptmode_toggle_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);

  const bool is_mode_set = ob->mode == OB_MODE_SCULPT_CURVES;

  if (is_mode_set) {
    if (!ED_object_mode_compat_set(C, ob, OB_MODE_SCULPT_CURVES, op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }

  if (is_mode_set) {
    curves_sculptmode_exit(C);
  }
  else {
    curves_sculptmode_enter(C);
  }

  WM_toolsystem_update_from_context_view3d(C);

  /* Necessary to change the object mode on the evaluated object. */
  DEG_id_tag_update(&ob->id, ID_RECALC_COPY_ON_WRITE);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
  return OPERATOR_FINISHED;
}

static void CURVES_OT_sculptmode_toggle(wmOperatorType *ot)
{
  ot->name = "Curve Sculpt Mode Toggle";
  ot->idname = "CURVES_OT_sculptmode_toggle";
  ot->description = "Enter/Exit sculpt mode for curves";

  ot->exec = curves_sculptmode_toggle_exec;
  ot->poll = curves::curves_poll;

  ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;
}

/** \} */

namespace select_random {

static int select_random_exec(bContext *C, wmOperator *op)
{
  VectorSet<Curves *> unique_curves = curves::get_unique_editable_curves(*C);

  const int seed = RNA_int_get(op->ptr, "seed");
  RandomNumberGenerator rng{static_cast<uint32_t>(seed)};

  const bool partial = RNA_boolean_get(op->ptr, "partial");
  const bool constant_per_curve = RNA_boolean_get(op->ptr, "constant_per_curve");
  const float probability = RNA_float_get(op->ptr, "probability");
  const float min_value = RNA_float_get(op->ptr, "min");
  const auto next_partial_random_value = [&]() {
    return rng.get_float() * (1.0f - min_value) + min_value;
  };
  const auto next_bool_random_value = [&]() { return rng.get_float() <= probability; };

  for (Curves *curves_id : unique_curves) {
    CurvesGeometry &curves = CurvesGeometry::wrap(curves_id->geometry);
    const bool was_anything_selected = curves::has_anything_selected(*curves_id);
    switch (curves_id->selection_domain) {
      case ATTR_DOMAIN_POINT: {
        MutableSpan<float> selection = curves.selection_point_float_for_write();
        if (!was_anything_selected) {
          selection.fill(1.0f);
        }
        if (partial) {
          if (constant_per_curve) {
            for (const int curve_i : curves.curves_range()) {
              const float random_value = next_partial_random_value();
              const IndexRange points = curves.points_for_curve(curve_i);
              for (const int point_i : points) {
                selection[point_i] *= random_value;
              }
            }
          }
          else {
            for (const int point_i : selection.index_range()) {
              const float random_value = next_partial_random_value();
              selection[point_i] *= random_value;
            }
          }
        }
        else {
          if (constant_per_curve) {
            for (const int curve_i : curves.curves_range()) {
              const bool random_value = next_bool_random_value();
              const IndexRange points = curves.points_for_curve(curve_i);
              if (!random_value) {
                selection.slice(points).fill(0.0f);
              }
            }
          }
          else {
            for (const int point_i : selection.index_range()) {
              const bool random_value = next_bool_random_value();
              if (!random_value) {
                selection[point_i] = 0.0f;
              }
            }
          }
        }
        break;
      }
      case ATTR_DOMAIN_CURVE: {
        MutableSpan<float> selection = curves.selection_curve_float_for_write();
        if (!was_anything_selected) {
          selection.fill(1.0f);
        }
        if (partial) {
          for (const int curve_i : curves.curves_range()) {
            const float random_value = next_partial_random_value();
            selection[curve_i] *= random_value;
          }
        }
        else {
          for (const int curve_i : curves.curves_range()) {
            const bool random_value = next_bool_random_value();
            if (!random_value) {
              selection[curve_i] = 0.0f;
            }
          }
        }
        break;
      }
    }
    MutableSpan<float> selection = curves_id->selection_domain == ATTR_DOMAIN_POINT ?
                                       curves.selection_point_float_for_write() :
                                       curves.selection_curve_float_for_write();
    const bool was_any_selected = std::any_of(
        selection.begin(), selection.end(), [](const float v) { return v > 0.0f; });
    if (was_any_selected) {
      for (float &v : selection) {
        v *= rng.get_float();
      }
    }
    else {
      for (float &v : selection) {
        v = rng.get_float();
      }
    }

    /* Use #ID_RECALC_GEOMETRY instead of #ID_RECALC_SELECT because it is handled as a generic
     * attribute for now. */
    DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves_id);
  }
  return OPERATOR_FINISHED;
}

static void select_random_ui(bContext *UNUSED(C), wmOperator *op)
{
  uiLayout *layout = op->layout;

  uiItemR(layout, op->ptr, "seed", 0, nullptr, ICON_NONE);
  uiItemR(layout, op->ptr, "constant_per_curve", 0, nullptr, ICON_NONE);
  uiItemR(layout, op->ptr, "partial", 0, nullptr, ICON_NONE);

  if (RNA_boolean_get(op->ptr, "partial")) {
    uiItemR(layout, op->ptr, "min", UI_ITEM_R_SLIDER, "Min", ICON_NONE);
  }
  else {
    uiItemR(layout, op->ptr, "probability", UI_ITEM_R_SLIDER, "Probability", ICON_NONE);
  }
}

}  // namespace select_random

static void SCULPT_CURVES_OT_select_random(wmOperatorType *ot)
{
  ot->name = "Select Random";
  ot->idname = __func__;
  ot->description = "Randomizes existing selection or create new random selection";

  ot->exec = select_random::select_random_exec;
  ot->poll = curves::editable_curves_poll;
  ot->ui = select_random::select_random_ui;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "seed",
              0,
              INT32_MIN,
              INT32_MAX,
              "Seed",
              "Source of randomness",
              INT32_MIN,
              INT32_MAX);
  RNA_def_boolean(
      ot->srna, "partial", false, "Partial", "Allow points or curves to be selected partially");
  RNA_def_float(ot->srna,
                "probability",
                0.5f,
                0.0f,
                1.0f,
                "Probability",
                "Chance of every point or curve being included in the selection",
                0.0f,
                1.0f);
  RNA_def_float(ot->srna,
                "min",
                0.0f,
                0.0f,
                1.0f,
                "Min",
                "Minimum value for the random selection",
                0.0f,
                1.0f);
  RNA_def_boolean(ot->srna,
                  "constant_per_curve",
                  true,
                  "Constant per Curve",
                  "The generated random number is the same for every control point of a curve");
}

namespace select_end {
static bool select_end_poll(bContext *C)
{
  if (!curves::editable_curves_poll(C)) {
    return false;
  }
  const Curves *curves_id = static_cast<const Curves *>(CTX_data_active_object(C)->data);
  if (curves_id->selection_domain != ATTR_DOMAIN_POINT) {
    CTX_wm_operator_poll_msg_set(C, "Only available in point selection mode");
    return false;
  }
  return true;
}

static int select_end_exec(bContext *C, wmOperator *op)
{
  VectorSet<Curves *> unique_curves = curves::get_unique_editable_curves(*C);
  const bool end_points = RNA_boolean_get(op->ptr, "end_points");
  const int amount = RNA_int_get(op->ptr, "amount");

  for (Curves *curves_id : unique_curves) {
    CurvesGeometry &curves = CurvesGeometry::wrap(curves_id->geometry);
    const bool was_anything_selected = curves::has_anything_selected(*curves_id);
    MutableSpan<float> selection = curves.selection_point_float_for_write();
    if (!was_anything_selected) {
      selection.fill(1.0f);
    }
    threading::parallel_for(curves.curves_range(), 256, [&](const IndexRange range) {
      for (const int curve_i : range) {
        const IndexRange points = curves.points_for_curve(curve_i);
        if (end_points) {
          selection.slice(points.drop_back(amount)).fill(0.0f);
        }
        else {
          selection.slice(points.drop_front(amount)).fill(0.0f);
        }
      }
    });

    /* Use #ID_RECALC_GEOMETRY instead of #ID_RECALC_SELECT because it is handled as a generic
     * attribute for now. */
    DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves_id);
  }

  return OPERATOR_FINISHED;
}
}  // namespace select_end

static void SCULPT_CURVES_OT_select_end(wmOperatorType *ot)
{
  ot->name = "Select End";
  ot->idname = __func__;
  ot->description = "Select end points of curves";

  ot->exec = select_end::select_end_exec;
  ot->poll = select_end::select_end_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "end_points",
                  true,
                  "End Points",
                  "Select points at the end of the curve as opposed to the beginning");
  RNA_def_int(
      ot->srna, "amount", 1, 0, INT32_MAX, "Amount", "Number of points to select", 0, INT32_MAX);
}

namespace select_grow {

struct GrowOperatorDataPerCurve : NonCopyable, NonMovable {
  Curves *curves_id;
  Vector<int> selected_point_indices;
  Vector<int> unselected_point_indices;
  Array<float> distances_to_selected;
  Array<float> distances_to_unselected;

  Array<float> original_selection;
  float pixel_to_distance_factor;
};

struct GrowOperatorData {
  int initial_mouse_x;
  Vector<std::unique_ptr<GrowOperatorDataPerCurve>> per_curve;
};

static void update_points_selection(const GrowOperatorDataPerCurve &data,
                                    const float distance,
                                    MutableSpan<float> points_selection)
{
  if (distance > 0.0f) {
    threading::parallel_for(
        data.unselected_point_indices.index_range(), 256, [&](const IndexRange range) {
          for (const int i : range) {
            const int point_i = data.unselected_point_indices[i];
            const float distance_to_selected = data.distances_to_selected[i];
            const float selection = distance_to_selected <= distance ? 1.0f : 0.0f;
            points_selection[point_i] = selection;
          }
        });
    threading::parallel_for(
        data.selected_point_indices.index_range(), 512, [&](const IndexRange range) {
          for (const int point_i : data.selected_point_indices.as_span().slice(range)) {
            points_selection[point_i] = 1.0f;
          }
        });
  }
  else {
    threading::parallel_for(
        data.selected_point_indices.index_range(), 256, [&](const IndexRange range) {
          for (const int i : range) {
            const int point_i = data.selected_point_indices[i];
            const float distance_to_unselected = data.distances_to_unselected[i];
            const float selection = distance_to_unselected <= -distance ? 0.0f : 1.0f;
            points_selection[point_i] = selection;
          }
        });
    threading::parallel_for(
        data.unselected_point_indices.index_range(), 512, [&](const IndexRange range) {
          for (const int point_i : data.unselected_point_indices.as_span().slice(range)) {
            points_selection[point_i] = 0.0f;
          }
        });
  }
}

static int select_grow_update(bContext *C, wmOperator *op, const float mouse_diff_x)
{
  GrowOperatorData &op_data = *static_cast<GrowOperatorData *>(op->customdata);

  for (std::unique_ptr<GrowOperatorDataPerCurve> &curve_op_data : op_data.per_curve) {
    Curves &curves_id = *curve_op_data->curves_id;
    CurvesGeometry &curves = CurvesGeometry::wrap(curves_id.geometry);
    const float distance = curve_op_data->pixel_to_distance_factor * mouse_diff_x;

    /* Grow or shrink selection based on precomputed distances. */
    switch (curves_id.selection_domain) {
      case ATTR_DOMAIN_POINT: {
        MutableSpan<float> points_selection = curves.selection_point_float_for_write();
        update_points_selection(*curve_op_data, distance, points_selection);
        break;
      }
      case ATTR_DOMAIN_CURVE: {
        Array<float> new_points_selection(curves.points_num());
        update_points_selection(*curve_op_data, distance, new_points_selection);
        /* Propagate grown point selection to the curve selection. */
        MutableSpan<float> curves_selection = curves.selection_curve_float_for_write();
        for (const int curve_i : curves.curves_range()) {
          const IndexRange points = curves.points_for_curve(curve_i);
          const Span<float> points_selection = new_points_selection.as_span().slice(points);
          const float max_selection = *std::max_element(points_selection.begin(),
                                                        points_selection.end());
          curves_selection[curve_i] = max_selection;
        }
        break;
      }
    }

    /* Use #ID_RECALC_GEOMETRY instead of #ID_RECALC_SELECT because it is handled as a generic
     * attribute for now. */
    DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &curves_id);
  }

  return OPERATOR_FINISHED;
}

static void select_grow_invoke_per_curve(Curves &curves_id,
                                         Object &curves_ob,
                                         const ARegion &region,
                                         const View3D &v3d,
                                         const RegionView3D &rv3d,
                                         GrowOperatorDataPerCurve &curve_op_data)
{
  curve_op_data.curves_id = &curves_id;
  CurvesGeometry &curves = CurvesGeometry::wrap(curves_id.geometry);
  const Span<float3> positions = curves.positions();

  /* Find indices of selected and unselected points. */
  switch (curves_id.selection_domain) {
    case ATTR_DOMAIN_POINT: {
      const VArray<float> points_selection = curves.selection_point_float();
      curve_op_data.original_selection.reinitialize(points_selection.size());
      points_selection.materialize(curve_op_data.original_selection);
      for (const int point_i : points_selection.index_range()) {
        const float point_selection = points_selection[point_i];
        if (point_selection > 0.0f) {
          curve_op_data.selected_point_indices.append(point_i);
        }
        else {
          curve_op_data.unselected_point_indices.append(point_i);
        }
      }

      break;
    }
    case ATTR_DOMAIN_CURVE: {
      const VArray<float> curves_selection = curves.selection_curve_float();
      curve_op_data.original_selection.reinitialize(curves_selection.size());
      curves_selection.materialize(curve_op_data.original_selection);
      for (const int curve_i : curves_selection.index_range()) {
        const float curve_selection = curves_selection[curve_i];
        const IndexRange points = curves.points_for_curve(curve_i);
        if (curve_selection > 0.0f) {
          for (const int point_i : points) {
            curve_op_data.selected_point_indices.append(point_i);
          }
        }
        else {
          for (const int point_i : points) {
            curve_op_data.unselected_point_indices.append(point_i);
          }
        }
      }
      break;
    }
  }

  threading::parallel_invoke(
      1024 < curve_op_data.selected_point_indices.size() +
                 curve_op_data.unselected_point_indices.size(),
      [&]() {
        /* Build KD-tree for the selected points. */
        KDTree_3d *kdtree = BLI_kdtree_3d_new(curve_op_data.selected_point_indices.size());
        BLI_SCOPED_DEFER([&]() { BLI_kdtree_3d_free(kdtree); });
        for (const int point_i : curve_op_data.selected_point_indices) {
          const float3 &position = positions[point_i];
          BLI_kdtree_3d_insert(kdtree, point_i, position);
        }
        BLI_kdtree_3d_balance(kdtree);

        /* For each unselected point, compute the distance to the closest selected point. */
        curve_op_data.distances_to_selected.reinitialize(
            curve_op_data.unselected_point_indices.size());
        threading::parallel_for(curve_op_data.unselected_point_indices.index_range(),
                                256,
                                [&](const IndexRange range) {
                                  for (const int i : range) {
                                    const int point_i = curve_op_data.unselected_point_indices[i];
                                    const float3 &position = positions[point_i];
                                    KDTreeNearest_3d nearest;
                                    BLI_kdtree_3d_find_nearest(kdtree, position, &nearest);
                                    curve_op_data.distances_to_selected[i] = nearest.dist;
                                  }
                                });
      },
      [&]() {
        /* Build KD-tree for the unselected points. */
        KDTree_3d *kdtree = BLI_kdtree_3d_new(curve_op_data.unselected_point_indices.size());
        BLI_SCOPED_DEFER([&]() { BLI_kdtree_3d_free(kdtree); });
        for (const int point_i : curve_op_data.unselected_point_indices) {
          const float3 &position = positions[point_i];
          BLI_kdtree_3d_insert(kdtree, point_i, position);
        }
        BLI_kdtree_3d_balance(kdtree);

        /* For each selected point, compute the distance to the closest unselected point. */
        curve_op_data.distances_to_unselected.reinitialize(
            curve_op_data.selected_point_indices.size());
        threading::parallel_for(
            curve_op_data.selected_point_indices.index_range(), 256, [&](const IndexRange range) {
              for (const int i : range) {
                const int point_i = curve_op_data.selected_point_indices[i];
                const float3 &position = positions[point_i];
                KDTreeNearest_3d nearest;
                BLI_kdtree_3d_find_nearest(kdtree, position, &nearest);
                curve_op_data.distances_to_unselected[i] = nearest.dist;
              }
            });
      });

  float4x4 curves_to_world_mat = curves_ob.obmat;
  float4x4 world_to_curves_mat = curves_to_world_mat.inverted();

  float4x4 projection;
  ED_view3d_ob_project_mat_get(&rv3d, &curves_ob, projection.values);

  /* Compute how mouse movements in screen space are converted into grow/shrink distances in
   * object space. */
  curve_op_data.pixel_to_distance_factor = threading::parallel_reduce(
      curve_op_data.selected_point_indices.index_range(),
      256,
      FLT_MAX,
      [&](const IndexRange range, float pixel_to_distance_factor) {
        for (const int i : range) {
          const int point_i = curve_op_data.selected_point_indices[i];
          const float3 &pos_cu = positions[point_i];

          float2 pos_re;
          ED_view3d_project_float_v2_m4(&region, pos_cu, pos_re, projection.values);
          if (pos_re.x < 0 || pos_re.y < 0 || pos_re.x > region.winx || pos_re.y > region.winy) {
            continue;
          }
          /* Compute how far this point moves in curve space when it moves one unit in screen
           * space. */
          const float2 pos_offset_re = pos_re + float2(1, 0);
          float3 pos_offset_wo;
          ED_view3d_win_to_3d(
              &v3d, &region, curves_to_world_mat * pos_cu, pos_offset_re, pos_offset_wo);
          const float3 pos_offset_cu = world_to_curves_mat * pos_offset_wo;
          const float dist_cu = math::distance(pos_cu, pos_offset_cu);
          const float dist_re = math::distance(pos_re, pos_offset_re);
          const float factor = dist_cu / dist_re;
          math::min_inplace(pixel_to_distance_factor, factor);
        }
        return pixel_to_distance_factor;
      },
      [](const float a, const float b) { return std::min(a, b); });
}

static int select_grow_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *active_ob = CTX_data_active_object(C);
  ARegion *region = CTX_wm_region(C);
  View3D *v3d = CTX_wm_view3d(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);

  GrowOperatorData *op_data = MEM_new<GrowOperatorData>(__func__);
  op->customdata = op_data;

  op_data->initial_mouse_x = event->xy[0];

  Curves &curves_id = *static_cast<Curves *>(active_ob->data);
  auto curve_op_data = std::make_unique<GrowOperatorDataPerCurve>();
  select_grow_invoke_per_curve(curves_id, *active_ob, *region, *v3d, *rv3d, *curve_op_data);
  op_data->per_curve.append(std::move(curve_op_data));

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static int select_grow_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  GrowOperatorData &op_data = *static_cast<GrowOperatorData *>(op->customdata);
  const int mouse_x = event->xy[0];
  const int mouse_diff_x = mouse_x - op_data.initial_mouse_x;
  switch (event->type) {
    case MOUSEMOVE: {
      select_grow_update(C, op, mouse_diff_x);
      break;
    }
    case LEFTMOUSE: {
      MEM_delete(&op_data);
      return OPERATOR_FINISHED;
    }
    case EVT_ESCKEY:
    case RIGHTMOUSE: {
      /* Undo operator by resetting the selection to the original value. */
      for (std::unique_ptr<GrowOperatorDataPerCurve> &curve_op_data : op_data.per_curve) {
        Curves &curves_id = *curve_op_data->curves_id;
        CurvesGeometry &curves = CurvesGeometry::wrap(curves_id.geometry);
        switch (curves_id.selection_domain) {
          case ATTR_DOMAIN_POINT: {
            MutableSpan<float> points_selection = curves.selection_point_float_for_write();
            points_selection.copy_from(curve_op_data->original_selection);
            break;
          }
          case ATTR_DOMAIN_CURVE: {
            MutableSpan<float> curves_seletion = curves.selection_curve_float_for_write();
            curves_seletion.copy_from(curve_op_data->original_selection);
            break;
          }
        }

        /* Use #ID_RECALC_GEOMETRY instead of #ID_RECALC_SELECT because it is handled as a generic
         * attribute for now. */
        DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
        WM_event_add_notifier(C, NC_GEOM | ND_DATA, &curves_id);
      }
      MEM_delete(&op_data);
      return OPERATOR_CANCELLED;
    }
  }
  return OPERATOR_RUNNING_MODAL;
}

}  // namespace select_grow

static void SCULPT_CURVES_OT_select_grow(wmOperatorType *ot)
{
  ot->name = "Select Grow";
  ot->idname = __func__;
  ot->description = "Select curves which are close to curves that are selected already";

  ot->invoke = select_grow::select_grow_invoke;
  ot->modal = select_grow::select_grow_modal;
  ot->poll = curves::editable_curves_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;
  prop = RNA_def_float(ot->srna,
                       "distance",
                       0.1f,
                       -FLT_MAX,
                       FLT_MAX,
                       "Distance",
                       "By how much to grow the selection",
                       -10.0f,
                       10.f);
  RNA_def_property_subtype(prop, PROP_DISTANCE);
}

namespace min_distance_edit {

static bool min_distance_edit_poll(bContext *C)
{
  if (!curves::curves_with_surface_poll(C)) {
    return false;
  }
  Scene *scene = CTX_data_scene(C);
  const Brush *brush = BKE_paint_brush_for_read(&scene->toolsettings->curves_sculpt->paint);
  if (brush == nullptr) {
    return false;
  }
  if (brush->curves_sculpt_tool != CURVES_SCULPT_TOOL_DENSITY) {
    return false;
  }
  return true;
}

struct MinDistanceEditData {
  /** Brush whose minimum distance is modified. */
  Brush *brush;
  float4x4 curves_to_world_mat;

  /** Where the preview is drawn. */
  float3 pos_cu;
  float3 normal_cu;

  int2 initial_mouse;
  float initial_minimum_distance;

  /** The operator uses a new cursor, but the existing cursors should be restored afterwards. */
  ListBase orig_paintcursors;
  void *cursor;

  /** Store the viewport region in case the operator was called from the header. */
  ARegion *region;
  RegionView3D *rv3d;
};

static int calculate_points_per_side(bContext *C, MinDistanceEditData &op_data)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = op_data.region;

  const float min_distance = op_data.brush->curves_sculpt_settings->minimum_distance;
  const float brush_radius = BKE_brush_size_get(scene, op_data.brush);

  float3 tangent_x_cu = math::cross(op_data.normal_cu, float3{0, 0, 1});
  if (math::is_zero(tangent_x_cu)) {
    tangent_x_cu = math::cross(op_data.normal_cu, float3{0, 1, 0});
  }
  tangent_x_cu = math::normalize(tangent_x_cu);
  const float3 tangent_y_cu = math::normalize(math::cross(op_data.normal_cu, tangent_x_cu));

  /* Sample a few points to get a good estimate of how large the grid has to be. */
  Vector<float3> points_wo;
  points_wo.append(op_data.pos_cu + min_distance * tangent_x_cu);
  points_wo.append(op_data.pos_cu + min_distance * tangent_y_cu);
  points_wo.append(op_data.pos_cu - min_distance * tangent_x_cu);
  points_wo.append(op_data.pos_cu - min_distance * tangent_y_cu);

  Vector<float2> points_re;
  for (const float3 &pos_wo : points_wo) {
    float2 pos_re;
    ED_view3d_project_v2(region, pos_wo, pos_re);
    points_re.append(pos_re);
  }

  float2 origin_re;
  ED_view3d_project_v2(region, op_data.pos_cu, origin_re);

  int needed_points = 0;
  for (const float2 &pos_re : points_re) {
    const float distance = math::length(pos_re - origin_re);
    const int needed_points_iter = (brush_radius * 2.0f) / distance;

    if (needed_points_iter > needed_points) {
      needed_points = needed_points_iter;
    }
  }

  /* Limit to a hard-coded number since it only adds noise at some point. */
  return std::min(300, needed_points);
}

static void min_distance_edit_draw(bContext *C, int UNUSED(x), int UNUSED(y), void *customdata)
{
  Scene *scene = CTX_data_scene(C);
  MinDistanceEditData &op_data = *static_cast<MinDistanceEditData *>(customdata);

  const float min_distance = op_data.brush->curves_sculpt_settings->minimum_distance;

  float3 tangent_x_cu = math::cross(op_data.normal_cu, float3{0, 0, 1});
  if (math::is_zero(tangent_x_cu)) {
    tangent_x_cu = math::cross(op_data.normal_cu, float3{0, 1, 0});
  }
  tangent_x_cu = math::normalize(tangent_x_cu);
  const float3 tangent_y_cu = math::normalize(math::cross(op_data.normal_cu, tangent_x_cu));

  const int points_per_side = calculate_points_per_side(C, op_data);
  const int points_per_axis_num = 2 * points_per_side + 1;

  Vector<float3> points_wo;
  for (const int x_i : IndexRange(points_per_axis_num)) {
    for (const int y_i : IndexRange(points_per_axis_num)) {
      const float x_iter = min_distance * (x_i - (points_per_axis_num - 1) / 2.0f);
      const float y_iter = min_distance * (y_i - (points_per_axis_num - 1) / 2.0f);

      const float3 point_pos_cu = op_data.pos_cu + op_data.normal_cu * 0.0001f +
                                  x_iter * tangent_x_cu + y_iter * tangent_y_cu;
      const float3 point_pos_wo = op_data.curves_to_world_mat * point_pos_cu;
      points_wo.append(point_pos_wo);
    }
  }

  float4 circle_col = float4(op_data.brush->add_col);
  float circle_alpha = op_data.brush->cursor_overlay_alpha;
  float brush_radius_re = BKE_brush_size_get(scene, op_data.brush);

  /* Draw the grid. */
  GPU_matrix_push();
  GPU_matrix_push_projection();
  GPU_blend(GPU_BLEND_ALPHA);

  ARegion *region = op_data.region;
  RegionView3D *rv3d = op_data.rv3d;
  wmWindow *win = CTX_wm_window(C);

  /* It does the same as: `view3d_operator_needs_opengl(C);`. */
  wmViewport(&region->winrct);
  GPU_matrix_projection_set(rv3d->winmat);
  GPU_matrix_set(rv3d->viewmat);

  GPUVertFormat *format3d = immVertexFormat();

  const uint pos3d = GPU_vertformat_attr_add(format3d, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  const uint col3d = GPU_vertformat_attr_add(format3d, "color", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);

  immBindBuiltinProgram(GPU_SHADER_3D_POINT_FIXED_SIZE_VARYING_COLOR);

  GPU_point_size(3.0f);
  immBegin(GPU_PRIM_POINTS, points_wo.size());

  float3 brush_origin_wo = op_data.curves_to_world_mat * op_data.pos_cu;
  float2 brush_origin_re;
  ED_view3d_project_v2(region, brush_origin_wo, brush_origin_re);

  /* Smooth alpha transition until the brush edge. */
  const int alpha_border_re = 20;
  const float dist_to_inner_border_re = brush_radius_re - alpha_border_re;

  for (const float3 &pos_wo : points_wo) {
    float2 pos_re;
    ED_view3d_project_v2(region, pos_wo, pos_re);

    const float dist_to_point_re = math::distance(pos_re, brush_origin_re);
    const float alpha = 1.0f - ((dist_to_point_re - dist_to_inner_border_re) / alpha_border_re);

    immAttr4f(col3d, 0.9f, 0.9f, 0.9f, alpha);
    immVertex3fv(pos3d, pos_wo);
  }
  immEnd();
  immUnbindProgram();

  /* Reset the drawing settings. */
  GPU_point_size(1.0f);
  GPU_matrix_pop_projection();
  GPU_matrix_pop();

  int4 scissor;
  GPU_scissor_get(scissor);
  wmWindowViewport(win);
  GPU_scissor(scissor[0], scissor[1], scissor[2], scissor[3]);

  /* Draw the brush circle. */
  GPU_matrix_translate_2f((float)op_data.initial_mouse.x, (float)op_data.initial_mouse.y);

  GPUVertFormat *format = immVertexFormat();
  uint pos2d = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

  immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);

  immUniformColor3fvAlpha(circle_col, circle_alpha);
  imm_draw_circle_wire_2d(pos2d, 0.0f, 0.0f, brush_radius_re, 80);

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

static int min_distance_edit_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ARegion *region = CTX_wm_region(C);
  View3D *v3d = CTX_wm_view3d(C);
  Scene *scene = CTX_data_scene(C);

  Object &curves_ob_orig = *CTX_data_active_object(C);
  Curves &curves_id_orig = *static_cast<Curves *>(curves_ob_orig.data);
  Object &surface_ob_orig = *curves_id_orig.surface;
  Object *surface_ob_eval = DEG_get_evaluated_object(depsgraph, &surface_ob_orig);
  if (surface_ob_eval == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Mesh *surface_me_eval = BKE_object_get_evaluated_mesh(surface_ob_eval);
  if (surface_me_eval == nullptr) {
    return OPERATOR_CANCELLED;
  }

  BVHTreeFromMesh surface_bvh_eval;
  BKE_bvhtree_from_mesh_get(&surface_bvh_eval, surface_me_eval, BVHTREE_FROM_LOOPTRI, 2);
  BLI_SCOPED_DEFER([&]() { free_bvhtree_from_mesh(&surface_bvh_eval); });

  const int2 mouse_pos_int_re{event->mval};
  const float2 mouse_pos_re{mouse_pos_int_re};

  float3 ray_start_wo, ray_end_wo;
  ED_view3d_win_to_segment_clipped(
      depsgraph, region, v3d, mouse_pos_re, ray_start_wo, ray_end_wo, true);

  const CurvesSurfaceTransforms transforms{curves_ob_orig, &surface_ob_orig};

  const float3 ray_start_su = transforms.world_to_surface * ray_start_wo;
  const float3 ray_end_su = transforms.world_to_surface * ray_end_wo;
  const float3 ray_direction_su = math::normalize(ray_end_su - ray_start_su);

  BVHTreeRayHit ray_hit;
  ray_hit.dist = FLT_MAX;
  ray_hit.index = -1;
  BLI_bvhtree_ray_cast(surface_bvh_eval.tree,
                       ray_start_su,
                       ray_direction_su,
                       0.0f,
                       &ray_hit,
                       surface_bvh_eval.raycast_callback,
                       &surface_bvh_eval);
  if (ray_hit.index == -1) {
    WM_report(RPT_ERROR, "Cursor must be over the surface mesh");
    return OPERATOR_CANCELLED;
  }

  const float3 hit_pos_su = ray_hit.co;
  const float3 hit_normal_su = ray_hit.no;

  const float3 hit_pos_cu = transforms.surface_to_curves * hit_pos_su;
  const float3 hit_normal_cu = math::normalize(transforms.surface_to_curves_normal *
                                               hit_normal_su);

  MinDistanceEditData *op_data = MEM_new<MinDistanceEditData>(__func__);
  op_data->curves_to_world_mat = transforms.curves_to_world;
  op_data->normal_cu = hit_normal_cu;
  op_data->pos_cu = hit_pos_cu;
  op_data->initial_mouse = event->xy;
  op_data->brush = BKE_paint_brush(&scene->toolsettings->curves_sculpt->paint);
  op_data->initial_minimum_distance = op_data->brush->curves_sculpt_settings->minimum_distance;

  if (op_data->initial_minimum_distance <= 0.0f) {
    op_data->initial_minimum_distance = 0.01f;
  }

  op->customdata = op_data;

  /* Temporarily disable other paint cursors. */
  wmWindowManager *wm = CTX_wm_manager(C);
  op_data->orig_paintcursors = wm->paintcursors;
  BLI_listbase_clear(&wm->paintcursors);

  /* Add minimum distance paint cursor. */
  op_data->cursor = WM_paint_cursor_activate(
      SPACE_TYPE_ANY, RGN_TYPE_ANY, op->type->poll, min_distance_edit_draw, op_data);

  op_data->region = CTX_wm_region(C);
  op_data->rv3d = CTX_wm_region_view3d(C);

  WM_event_add_modal_handler(C, op);
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

static int min_distance_edit_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  MinDistanceEditData &op_data = *static_cast<MinDistanceEditData *>(op->customdata);

  auto finish = [&]() {
    wmWindowManager *wm = CTX_wm_manager(C);

    /* Remove own cursor. */
    WM_paint_cursor_end(static_cast<wmPaintCursor *>(op_data.cursor));
    /* Restore original paint cursors. */
    wm->paintcursors = op_data.orig_paintcursors;

    ED_region_tag_redraw(region);
    MEM_freeN(&op_data);
  };

  switch (event->type) {
    case MOUSEMOVE: {
      const int2 mouse_pos_int_re{event->xy};
      const float2 mouse_pos_re{mouse_pos_int_re};

      const float mouse_diff_x = mouse_pos_int_re.x - op_data.initial_mouse.x;
      const float factor = powf(2, mouse_diff_x / UI_UNIT_X / 10.0f);
      op_data.brush->curves_sculpt_settings->minimum_distance = op_data.initial_minimum_distance *
                                                                factor;

      ED_region_tag_redraw(region);
      WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, nullptr);
      break;
    }
    case LEFTMOUSE: {
      if (event->val == KM_PRESS) {
        finish();
        return OPERATOR_FINISHED;
      }
      break;
    }
    case RIGHTMOUSE:
    case EVT_ESCKEY: {
      op_data.brush->curves_sculpt_settings->minimum_distance = op_data.initial_minimum_distance;
      finish();
      WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, nullptr);
      return OPERATOR_CANCELLED;
    }
  }

  return OPERATOR_RUNNING_MODAL;
}

}  // namespace min_distance_edit

static void SCULPT_CURVES_OT_min_distance_edit(wmOperatorType *ot)
{
  ot->name = "Edit Minimum Distance";
  ot->idname = __func__;
  ot->description = "Change the minimum distance used by the density brush";

  ot->poll = min_distance_edit::min_distance_edit_poll;
  ot->invoke = min_distance_edit::min_distance_edit_invoke;
  ot->modal = min_distance_edit::min_distance_edit_modal;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;
}

}  // namespace blender::ed::sculpt_paint

/* -------------------------------------------------------------------- */
/** \name * Registration
 * \{ */

void ED_operatortypes_sculpt_curves()
{
  using namespace blender::ed::sculpt_paint;
  WM_operatortype_append(SCULPT_CURVES_OT_brush_stroke);
  WM_operatortype_append(CURVES_OT_sculptmode_toggle);
  WM_operatortype_append(SCULPT_CURVES_OT_select_random);
  WM_operatortype_append(SCULPT_CURVES_OT_select_end);
  WM_operatortype_append(SCULPT_CURVES_OT_select_grow);
  WM_operatortype_append(SCULPT_CURVES_OT_min_distance_edit);
}

/** \} */
