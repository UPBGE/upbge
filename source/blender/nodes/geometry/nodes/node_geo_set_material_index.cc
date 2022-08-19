/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_material_index_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Geometry")).supported_type(GEO_COMPONENT_TYPE_MESH);
  b.add_input<decl::Bool>(N_("Selection")).default_value(true).hide_value().supports_field();
  b.add_input<decl::Int>(N_("Material Index")).supports_field().min(0);
  b.add_output<decl::Geometry>(N_("Geometry"));
}

static void set_material_index_in_component(GeometryComponent &component,
                                            const Field<bool> &selection_field,
                                            const Field<int> &index_field)
{
  const int domain_size = component.attribute_domain_size(ATTR_DOMAIN_FACE);
  if (domain_size == 0) {
    return;
  }
  MutableAttributeAccessor attributes = *component.attributes_for_write();
  GeometryComponentFieldContext field_context{component, ATTR_DOMAIN_FACE};

  AttributeWriter<int> indices = attributes.lookup_or_add_for_write<int>("material_index",
                                                                         ATTR_DOMAIN_FACE);

  fn::FieldEvaluator evaluator{field_context, domain_size};
  evaluator.set_selection(selection_field);
  evaluator.add_with_destination(index_field, indices.varray);
  evaluator.evaluate();
  indices.finish();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  Field<int> index_field = params.extract_input<Field<int>>("Material Index");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (geometry_set.has_mesh()) {
      set_material_index_in_component(
          geometry_set.get_component_for_write<MeshComponent>(), selection_field, index_field);
    }
  });
  params.set_output("Geometry", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_set_material_index_cc

void register_node_type_geo_set_material_index()
{
  namespace file_ns = blender::nodes::node_geo_set_material_index_cc;

  static bNodeType ntype;

  geo_node_type_base(
      &ntype, GEO_NODE_SET_MATERIAL_INDEX, "Set Material Index", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  nodeRegisterType(&ntype);
}
