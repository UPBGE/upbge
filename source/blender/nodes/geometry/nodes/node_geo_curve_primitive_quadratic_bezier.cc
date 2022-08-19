/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_primitive_quadratic_bezier_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>(N_("Resolution"))
      .default_value(16)
      .min(3)
      .max(256)
      .subtype(PROP_UNSIGNED)
      .description(N_("The number of edges on the curve"));
  b.add_input<decl::Vector>(N_("Start"))
      .default_value({-1.0f, 0.0f, 0.0f})
      .subtype(PROP_TRANSLATION)
      .description(N_("Position of the first control point"));
  b.add_input<decl::Vector>(N_("Middle"))
      .default_value({0.0f, 2.0f, 0.0f})
      .subtype(PROP_TRANSLATION)
      .description(N_("Position of the middle control point"));
  b.add_input<decl::Vector>(N_("End"))
      .default_value({1.0f, 0.0f, 0.0f})
      .subtype(PROP_TRANSLATION)
      .description(N_("Position of the last control point"));
  b.add_output<decl::Geometry>(N_("Curve"));
}

static Curves *create_quadratic_bezier_curve(const float3 p1,
                                             const float3 p2,
                                             const float3 p3,
                                             const int resolution)
{
  Curves *curves_id = bke::curves_new_nomain_single(resolution + 1, CURVE_TYPE_POLY);
  bke::CurvesGeometry &curves = bke::CurvesGeometry::wrap(curves_id->geometry);

  MutableSpan<float3> positions = curves.positions_for_write();

  const float step = 1.0f / resolution;
  for (const int i : IndexRange(resolution + 1)) {
    const float factor = step * i;
    const float3 q1 = math::interpolate(p1, p2, factor);
    const float3 q2 = math::interpolate(p2, p3, factor);
    positions[i] = math::interpolate(q1, q2, factor);
  }

  return curves_id;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Curves *curves = create_quadratic_bezier_curve(
      params.extract_input<float3>("Start"),
      params.extract_input<float3>("Middle"),
      params.extract_input<float3>("End"),
      std::max(params.extract_input<int>("Resolution"), 3));
  params.set_output("Curve", GeometrySet::create_with_curves(curves));
}

}  // namespace blender::nodes::node_geo_curve_primitive_quadratic_bezier_cc

void register_node_type_geo_curve_primitive_quadratic_bezier()
{
  namespace file_ns = blender::nodes::node_geo_curve_primitive_quadratic_bezier_cc;

  static bNodeType ntype;
  geo_node_type_base(
      &ntype, GEO_NODE_CURVE_PRIMITIVE_QUADRATIC_BEZIER, "Quadratic Bezier", NODE_CLASS_GEOMETRY);
  ntype.declare = file_ns::node_declare;
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  nodeRegisterType(&ntype);
}
