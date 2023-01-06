/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <atomic>

#include "BKE_curves.hh"

#include "UI_interface.h"
#include "UI_resources.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_curve_handles_cc {

NODE_STORAGE_FUNCS(NodeGeometrySetCurveHandlePositions)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Curve")).supported_type(GEO_COMPONENT_TYPE_CURVE);
  b.add_input<decl::Bool>(N_("Selection")).default_value(true).hide_value().field_on_all();
  b.add_input<decl::Vector>(N_("Position"))
      .implicit_field_on_all([](const bNode &node, void *r_value) {
        const StringRef side = node_storage(node).mode == GEO_NODE_CURVE_HANDLE_LEFT ?
                                   "handle_left" :
                                   "handle_right";
        new (r_value) ValueOrField<float3>(bke::AttributeFieldInput::Create<float3>(side));
      });
  b.add_input<decl::Vector>(N_("Offset")).default_value(float3(0.0f, 0.0f, 0.0f)).field_on_all();
  b.add_output<decl::Geometry>(N_("Curve")).propagate_all();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "mode", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometrySetCurveHandlePositions *data = MEM_cnew<NodeGeometrySetCurveHandlePositions>(
      __func__);

  data->mode = GEO_NODE_CURVE_HANDLE_LEFT;
  node->storage = data;
}

static void update_handle_types_for_movement(int8_t &type, int8_t &other)
{
  switch (type) {
    case BEZIER_HANDLE_FREE:
      break;
    case BEZIER_HANDLE_AUTO:
      /* Converting auto handles to aligned handled instead of free handles is
       * arbitrary, but expected and "standard" based on behavior in edit mode. */
      if (other == BEZIER_HANDLE_AUTO) {
        /* Convert pairs of auto handles to aligned handles when moving one side. */
        type = BEZIER_HANDLE_ALIGN;
        other = BEZIER_HANDLE_ALIGN;
      }
      else {
        /* If the other handle isn't automatic, just make the handle free. */
        type = BEZIER_HANDLE_FREE;
      }
      break;
    case BEZIER_HANDLE_VECTOR:
      type = BEZIER_HANDLE_FREE;
      break;
    case BEZIER_HANDLE_ALIGN:
      /* The handle can stay aligned if the other handle is also aligned (in which case the other
       * handle should be updated to be consistent). But otherwise the handle must be made free to
       * avoid conflicting with its "aligned" type. */
      if (other != BEZIER_HANDLE_ALIGN) {
        type = BEZIER_HANDLE_FREE;
      }
      break;
  }
}

static void set_position_in_component(bke::CurvesGeometry &curves,
                                      const GeometryNodeCurveHandleMode mode,
                                      const Field<bool> &selection_field,
                                      const Field<float3> &position_field,
                                      const Field<float3> &offset_field)
{
  if (curves.points_num() == 0) {
    return;
  }

  bke::CurvesFieldContext field_context{curves, ATTR_DOMAIN_POINT};
  fn::FieldEvaluator evaluator{field_context, curves.points_num()};
  evaluator.set_selection(selection_field);
  evaluator.add(position_field);
  evaluator.add(offset_field);
  evaluator.evaluate();
  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  const VArray<float3> new_positions = evaluator.get_evaluated<float3>(0);
  const VArray<float3> new_offsets = evaluator.get_evaluated<float3>(1);

  Span<float3> positions = curves.positions();

  const bool use_left = mode == GEO_NODE_CURVE_HANDLE_LEFT;
  MutableSpan<int8_t> handle_types = use_left ? curves.handle_types_left_for_write() :
                                                curves.handle_types_right_for_write();
  MutableSpan<int8_t> handle_types_other = use_left ? curves.handle_types_right_for_write() :
                                                      curves.handle_types_left_for_write();
  MutableSpan<float3> handle_positions = use_left ? curves.handle_positions_left_for_write() :
                                                    curves.handle_positions_right_for_write();
  MutableSpan<float3> handle_positions_other = use_left ?
                                                   curves.handle_positions_right_for_write() :
                                                   curves.handle_positions_left_for_write();

  threading::parallel_for(selection.index_range(), 2048, [&](IndexRange range) {
    for (const int i : selection.slice(range)) {
      update_handle_types_for_movement(handle_types[i], handle_types_other[i]);
    }
  });

  threading::parallel_for(selection.index_range(), 2048, [&](IndexRange range) {
    for (const int i : selection.slice(range)) {
      bke::curves::bezier::set_handle_position(positions[i],
                                               HandleType(handle_types[i]),
                                               HandleType(handle_types_other[i]),
                                               new_positions[i] + new_offsets[i],
                                               handle_positions[i],
                                               handle_positions_other[i]);
    }
  });

  curves.calculate_bezier_auto_handles();

  curves.tag_positions_changed();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometrySetCurveHandlePositions &storage = node_storage(params.node());
  const GeometryNodeCurveHandleMode mode = (GeometryNodeCurveHandleMode)storage.mode;

  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  Field<float3> offset_field = params.extract_input<Field<float3>>("Offset");

  std::atomic<bool> has_curves = false;
  std::atomic<bool> has_bezier = false;

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (Curves *curves_id = geometry_set.get_curves_for_write()) {
      bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id->geometry);
      has_curves = true;
      const AttributeAccessor attributes = curves.attributes();
      if (!attributes.contains("handle_left") || !attributes.contains("handle_right")) {
        return;
      }
      has_bezier = true;

      set_position_in_component(curves, mode, selection_field, position_field, offset_field);
    }
  });

  if (has_curves && !has_bezier) {
    params.error_message_add(NodeWarningType::Info, TIP_("Input curves do not have Bezier type"));
  }

  params.set_output("Curve", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_set_curve_handles_cc

void register_node_type_geo_set_curve_handles()
{
  namespace file_ns = blender::nodes::node_geo_set_curve_handles_cc;

  static bNodeType ntype;

  geo_node_type_base(
      &ntype, GEO_NODE_SET_CURVE_HANDLES, "Set Handle Positions", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  ntype.minwidth = 100.0f;
  ntype.initfunc = file_ns::node_init;
  node_type_storage(&ntype,
                    "NodeGeometrySetCurveHandlePositions",
                    node_free_standard_storage,
                    node_copy_standard_storage);
  ntype.draw_buttons = file_ns::node_layout;
  nodeRegisterType(&ntype);
}
