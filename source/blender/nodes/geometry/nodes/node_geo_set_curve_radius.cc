/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_curve_radius_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Curve")
      .supported_type({GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil})
      .description("Curves to set the radius on");
  b.add_output<decl::Geometry>("Curve").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Float>("Radius")
      .min(0.0f)
      .default_value(0.005f)
      .subtype(PROP_DISTANCE)
      .field_on_all();
}

static void set_radius(bke::CurvesGeometry &curves,
                       const fn::FieldContext &field_context,
                       const Field<bool> &selection,
                       const Field<float> &radius)
{
  bke::try_capture_field_on_geometry(curves.attributes_for_write(),
                                     field_context,
                                     "radius",
                                     bke::AttrDomain::Point,
                                     selection,
                                     radius);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve");
  const Field<bool> selection = params.extract_input<Field<bool>>("Selection");
  const Field<float> radius = params.extract_input<Field<float>>("Radius");

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (Curves *curves_id = geometry_set.get_curves_for_write()) {
      bke::CurvesGeometry &curves = curves_id->geometry.wrap();
      const bke::CurvesFieldContext field_context(*curves_id, AttrDomain::Point);
      set_radius(curves, field_context, selection, radius);
    }
    if (GreasePencil *grease_pencil = geometry_set.get_grease_pencil_for_write()) {
      using namespace blender::bke::greasepencil;
      for (const int layer_index : grease_pencil->layers().index_range()) {
        Drawing *drawing = grease_pencil->get_eval_drawing(grease_pencil->layer(layer_index));
        if (drawing == nullptr) {
          continue;
        }
        set_radius(
            drawing->strokes_for_write(),
            bke::GreasePencilLayerFieldContext(*grease_pencil, AttrDomain::Point, layer_index),
            selection,
            radius);
      }
    }
  });

  params.set_output("Curve", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetCurveRadius", GEO_NODE_SET_CURVE_RADIUS);
  ntype.ui_name = "Set Curve Radius";
  ntype.ui_description = "Set the radius of the curve at each control point";
  ntype.enum_name_legacy = "SET_CURVE_RADIUS";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_curve_radius_cc
