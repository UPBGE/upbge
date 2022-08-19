/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "UI_interface.h"
#include "UI_resources.h"

#include "NOD_socket_search_link.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_primitive_quadrilateral_cc {

NODE_STORAGE_FUNCS(NodeGeometryCurvePrimitiveQuad)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>(N_("Width"))
      .default_value(2.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description(N_("The X axis size of the shape"));
  b.add_input<decl::Float>(N_("Height"))
      .default_value(2.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description(N_("The Y axis size of the shape"));
  b.add_input<decl::Float>(N_("Bottom Width"))
      .default_value(4.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description(N_("The X axis size of the shape"));
  b.add_input<decl::Float>(N_("Top Width"))
      .default_value(2.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description(N_("The X axis size of the shape"));
  b.add_input<decl::Float>(N_("Offset"))
      .default_value(1.0f)
      .subtype(PROP_DISTANCE)
      .description(
          N_("For Parallelogram, the relative X difference between the top and bottom edges. For "
             "Trapezoid, the amount to move the top edge in the positive X axis"));
  b.add_input<decl::Float>(N_("Bottom Height"))
      .default_value(3.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description(N_("The distance between the bottom point and the X axis"));
  b.add_input<decl::Float>(N_("Top Height"))
      .default_value(1.0f)
      .subtype(PROP_DISTANCE)
      .description(N_("The distance between the top point and the X axis"));
  b.add_input<decl::Vector>(N_("Point 1"))
      .default_value({-1.0f, -1.0f, 0.0f})
      .subtype(PROP_DISTANCE)
      .description(N_("The exact location of the point to use"));
  b.add_input<decl::Vector>(N_("Point 2"))
      .default_value({1.0f, -1.0f, 0.0f})
      .subtype(PROP_DISTANCE)
      .description(N_("The exact location of the point to use"));
  b.add_input<decl::Vector>(N_("Point 3"))
      .default_value({1.0f, 1.0f, 0.0f})
      .subtype(PROP_DISTANCE)
      .description(N_("The exact location of the point to use"));
  b.add_input<decl::Vector>(N_("Point 4"))
      .default_value({-1.0f, 1.0f, 0.0f})
      .subtype(PROP_DISTANCE)
      .description(N_("The exact location of the point to use"));
  b.add_output<decl::Geometry>(N_("Curve"));
}

static void node_layout(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiItemR(layout, ptr, "mode", 0, "", ICON_NONE);
}

static void node_init(bNodeTree *UNUSED(tree), bNode *node)
{
  NodeGeometryCurvePrimitiveQuad *data = MEM_cnew<NodeGeometryCurvePrimitiveQuad>(__func__);
  data->mode = GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_RECTANGLE;
  node->storage = data;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const NodeGeometryCurvePrimitiveQuad &storage = node_storage(*node);
  GeometryNodeCurvePrimitiveQuadMode mode = static_cast<GeometryNodeCurvePrimitiveQuadMode>(
      storage.mode);

  bNodeSocket *width = ((bNodeSocket *)node->inputs.first);
  bNodeSocket *height = width->next;
  bNodeSocket *bottom = height->next;
  bNodeSocket *top = bottom->next;
  bNodeSocket *offset = top->next;
  bNodeSocket *bottom_height = offset->next;
  bNodeSocket *top_height = bottom_height->next;
  bNodeSocket *p1 = top_height->next;
  bNodeSocket *p2 = p1->next;
  bNodeSocket *p3 = p2->next;
  bNodeSocket *p4 = p3->next;

  Vector<bNodeSocket *> available_sockets;

  if (mode == GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_RECTANGLE) {
    available_sockets.extend({width, height});
  }
  else if (mode == GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_PARALLELOGRAM) {
    available_sockets.extend({width, height, offset});
  }
  else if (mode == GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_TRAPEZOID) {
    available_sockets.extend({bottom, top, offset, height});
  }
  else if (mode == GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_KITE) {
    available_sockets.extend({width, bottom_height, top_height});
  }
  else if (mode == GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_POINTS) {
    available_sockets.extend({p1, p2, p3, p4});
  }

  LISTBASE_FOREACH (bNodeSocket *, sock, &node->inputs) {
    nodeSetSocketAvailability(ntree, sock, available_sockets.contains(sock));
  }
}

class SocketSearchOp {
 public:
  std::string socket_name;
  GeometryNodeCurvePrimitiveQuadMode quad_mode;
  void operator()(LinkSearchOpParams &params)
  {
    bNode &node = params.add_node("GeometryNodeCurvePrimitiveQuadrilateral");
    node_storage(node).mode = quad_mode;
    params.update_and_connect_available_socket(node, socket_name);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().fixed_declaration;
  if (params.in_out() == SOCK_OUT) {
    search_link_ops_for_declarations(params, declaration.outputs());
  }
  else if (params.node_tree().typeinfo->validate_link(
               static_cast<eNodeSocketDatatype>(params.other_socket().type), SOCK_FLOAT)) {
    params.add_item(IFACE_("Width"),
                    SocketSearchOp{"Width", GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_RECTANGLE});
    params.add_item(IFACE_("Height"),
                    SocketSearchOp{"Height", GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_RECTANGLE});
    params.add_item(IFACE_("Bottom Width"),
                    SocketSearchOp{"Bottom Width", GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_TRAPEZOID});
    params.add_item(IFACE_("Top Width"),
                    SocketSearchOp{"Top Width", GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_TRAPEZOID});
    params.add_item(IFACE_("Offset"),
                    SocketSearchOp{"Offset", GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_PARALLELOGRAM});
    params.add_item(IFACE_("Point 1"),
                    SocketSearchOp{"Point 1", GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_POINTS});
  }
}

static void create_rectangle_curve(MutableSpan<float3> positions,
                                   const float height,
                                   const float width)
{
  positions[0] = float3(width / 2.0f, height / 2.0f, 0.0f);
  positions[1] = float3(-width / 2.0f, height / 2.0f, 0.0f);
  positions[2] = float3(-width / 2.0f, -height / 2.0f, 0.0f);
  positions[3] = float3(width / 2.0f, -height / 2.0f, 0.0f);
}

static void create_points_curve(MutableSpan<float3> positions,
                                const float3 &p1,
                                const float3 &p2,
                                const float3 &p3,
                                const float3 &p4)
{
  positions[0] = p1;
  positions[1] = p2;
  positions[2] = p3;
  positions[3] = p4;
}

static void create_parallelogram_curve(MutableSpan<float3> positions,
                                       const float height,
                                       const float width,
                                       const float offset)
{
  positions[0] = float3(width / 2.0f + offset / 2.0f, height / 2.0f, 0.0f);
  positions[1] = float3(-width / 2.0f + offset / 2.0f, height / 2.0f, 0.0f);
  positions[2] = float3(-width / 2.0f - offset / 2.0f, -height / 2.0f, 0.0f);
  positions[3] = float3(width / 2.0f - offset / 2.0f, -height / 2.0f, 0.0f);
}
static void create_trapezoid_curve(MutableSpan<float3> positions,
                                   const float bottom,
                                   const float top,
                                   const float offset,
                                   const float height)
{
  positions[0] = float3(top / 2.0f + offset, height / 2.0f, 0.0f);
  positions[1] = float3(-top / 2.0f + offset, height / 2.0f, 0.0f);
  positions[2] = float3(-bottom / 2.0f, -height / 2.0f, 0.0f);
  positions[3] = float3(bottom / 2.0f, -height / 2.0f, 0.0f);
}

static void create_kite_curve(MutableSpan<float3> positions,
                              const float width,
                              const float bottom_height,
                              const float top_height)
{
  positions[0] = float3(0, -bottom_height, 0);
  positions[1] = float3(width / 2, 0, 0);
  positions[2] = float3(0, top_height, 0);
  positions[3] = float3(-width / 2.0f, 0, 0);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometryCurvePrimitiveQuad &storage = node_storage(params.node());
  const GeometryNodeCurvePrimitiveQuadMode mode = (GeometryNodeCurvePrimitiveQuadMode)storage.mode;

  Curves *curves_id = bke::curves_new_nomain_single(4, CURVE_TYPE_POLY);
  bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id->geometry);
  curves.cyclic_for_write().first() = true;

  MutableSpan<float3> positions = curves.positions_for_write();

  switch (mode) {
    case GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_RECTANGLE:
      create_rectangle_curve(positions,
                             std::max(params.extract_input<float>("Height"), 0.0f),
                             std::max(params.extract_input<float>("Width"), 0.0f));
      break;

    case GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_PARALLELOGRAM:
      create_parallelogram_curve(positions,
                                 std::max(params.extract_input<float>("Height"), 0.0f),
                                 std::max(params.extract_input<float>("Width"), 0.0f),
                                 params.extract_input<float>("Offset"));
      break;
    case GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_TRAPEZOID:
      create_trapezoid_curve(positions,
                             std::max(params.extract_input<float>("Bottom Width"), 0.0f),
                             std::max(params.extract_input<float>("Top Width"), 0.0f),
                             params.extract_input<float>("Offset"),
                             std::max(params.extract_input<float>("Height"), 0.0f));
      break;
    case GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_KITE:
      create_kite_curve(positions,
                        std::max(params.extract_input<float>("Width"), 0.0f),
                        std::max(params.extract_input<float>("Bottom Height"), 0.0f),
                        params.extract_input<float>("Top Height"));
      break;
    case GEO_NODE_CURVE_PRIMITIVE_QUAD_MODE_POINTS:
      create_points_curve(positions,
                          params.extract_input<float3>("Point 1"),
                          params.extract_input<float3>("Point 2"),
                          params.extract_input<float3>("Point 3"),
                          params.extract_input<float3>("Point 4"));
      break;
    default:
      params.set_default_remaining_outputs();
      return;
  }

  params.set_output("Curve", GeometrySet::create_with_curves(curves_id));
}

}  // namespace blender::nodes::node_geo_curve_primitive_quadrilateral_cc

void register_node_type_geo_curve_primitive_quadrilateral()
{
  namespace file_ns = blender::nodes::node_geo_curve_primitive_quadrilateral_cc;

  static bNodeType ntype;
  geo_node_type_base(
      &ntype, GEO_NODE_CURVE_PRIMITIVE_QUADRILATERAL, "Quadrilateral", NODE_CLASS_GEOMETRY);
  ntype.declare = file_ns::node_declare;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.draw_buttons = file_ns::node_layout;
  node_type_update(&ntype, file_ns::node_update);
  node_type_init(&ntype, file_ns::node_init);
  node_type_storage(&ntype,
                    "NodeGeometryCurvePrimitiveQuad",
                    node_free_standard_storage,
                    node_copy_standard_storage);
  ntype.gather_link_search_ops = file_ns::node_gather_link_searches;
  nodeRegisterType(&ntype);
}
