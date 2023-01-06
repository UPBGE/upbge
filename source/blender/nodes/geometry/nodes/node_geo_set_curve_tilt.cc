/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_curve_tilt_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Curve")).supported_type(GEO_COMPONENT_TYPE_CURVE);
  b.add_input<decl::Bool>(N_("Selection")).default_value(true).hide_value().field_on_all();
  b.add_input<decl::Float>(N_("Tilt")).subtype(PROP_ANGLE).field_on_all();
  b.add_output<decl::Geometry>(N_("Curve")).propagate_all();
}

static void set_tilt(bke::CurvesGeometry &curves,
                     const Field<bool> &selection_field,
                     const Field<float> &tilt_field)
{
  if (curves.points_num() == 0) {
    return;
  }
  MutableAttributeAccessor attributes = curves.attributes_for_write();
  AttributeWriter<float> tilts = attributes.lookup_or_add_for_write<float>("tilt",
                                                                           ATTR_DOMAIN_POINT);

  bke::CurvesFieldContext field_context{curves, ATTR_DOMAIN_POINT};
  fn::FieldEvaluator evaluator{field_context, curves.points_num()};
  evaluator.set_selection(selection_field);
  evaluator.add_with_destination(tilt_field, tilts.varray);
  evaluator.evaluate();

  tilts.finish();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  Field<float> tilt_field = params.extract_input<Field<float>>("Tilt");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (Curves *curves_id = geometry_set.get_curves_for_write()) {
      set_tilt(bke::CurvesGeometry::wrap(curves_id->geometry), selection_field, tilt_field);
    }
  });

  params.set_output("Curve", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_set_curve_tilt_cc

void register_node_type_geo_set_curve_tilt()
{
  namespace file_ns = blender::nodes::node_geo_set_curve_tilt_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_SET_CURVE_TILT, "Set Curve Tilt", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  nodeRegisterType(&ntype);
}
