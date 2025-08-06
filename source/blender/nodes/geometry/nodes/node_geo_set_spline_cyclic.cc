/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_spline_cyclic_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Curve", "Geometry")
      .supported_type({GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil})
      .description("Curves to change the cyclic state of");
  b.add_output<decl::Geometry>("Curve", "Geometry").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Bool>("Cyclic").field_on_all();
}

static void set_curve_cyclic(bke::CurvesGeometry &curves,
                             const fn::FieldContext &field_context,
                             const Field<bool> &selection,
                             const Field<bool> &cyclic)
{
  bke::try_capture_field_on_geometry(curves.attributes_for_write(),
                                     field_context,
                                     "cyclic",
                                     bke::AttrDomain::Curve,
                                     selection,
                                     cyclic);
}

static void set_grease_pencil_cyclic(GreasePencil &grease_pencil,
                                     const Field<bool> &selection,
                                     const Field<bool> &cyclic)
{
  using namespace blender::bke::greasepencil;
  for (const int layer_index : grease_pencil.layers().index_range()) {
    Drawing *drawing = grease_pencil.get_eval_drawing(grease_pencil.layer(layer_index));
    if (drawing == nullptr) {
      continue;
    }
    set_curve_cyclic(
        drawing->strokes_for_write(),
        bke::GreasePencilLayerFieldContext(grease_pencil, AttrDomain::Curve, layer_index),
        selection,
        cyclic);
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const Field<bool> selection = params.extract_input<Field<bool>>("Selection");
  const Field<bool> cyclic = params.extract_input<Field<bool>>("Cyclic");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (Curves *curves_id = geometry_set.get_curves_for_write()) {
      bke::CurvesGeometry &curves = curves_id->geometry.wrap();
      const bke::CurvesFieldContext field_context{*curves_id, AttrDomain::Curve};
      set_curve_cyclic(curves, field_context, selection, cyclic);
    }
    if (GreasePencil *grease_pencil = geometry_set.get_grease_pencil_for_write()) {
      set_grease_pencil_cyclic(*grease_pencil, selection, cyclic);
    }
  });

  params.set_output("Geometry", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetSplineCyclic", GEO_NODE_SET_SPLINE_CYCLIC);
  ntype.ui_name = "Set Spline Cyclic";
  ntype.ui_description =
      "Control whether each spline loops back on itself by changing the \"cyclic\" attribute";
  ntype.enum_name_legacy = "SET_SPLINE_CYCLIC";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_spline_cyclic_cc
