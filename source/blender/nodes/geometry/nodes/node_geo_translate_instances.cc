/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_task.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_translate_instances_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Instances")).only_instances();
  b.add_input<decl::Bool>(N_("Selection")).default_value(true).hide_value().supports_field();
  b.add_input<decl::Vector>(N_("Translation")).subtype(PROP_TRANSLATION).supports_field();
  b.add_input<decl::Bool>(N_("Local Space")).default_value(true).supports_field();
  b.add_output<decl::Geometry>(N_("Instances"));
}

static void translate_instances(GeoNodeExecParams &params, InstancesComponent &instances_component)
{
  GeometryComponentFieldContext field_context{instances_component, ATTR_DOMAIN_INSTANCE};

  fn::FieldEvaluator evaluator{field_context, instances_component.instances_num()};
  evaluator.set_selection(params.extract_input<Field<bool>>("Selection"));
  evaluator.add(params.extract_input<Field<float3>>("Translation"));
  evaluator.add(params.extract_input<Field<bool>>("Local Space"));
  evaluator.evaluate();

  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  const VArray<float3> translations = evaluator.get_evaluated<float3>(0);
  const VArray<bool> local_spaces = evaluator.get_evaluated<bool>(1);

  MutableSpan<float4x4> instance_transforms = instances_component.instance_transforms();

  threading::parallel_for(selection.index_range(), 1024, [&](IndexRange range) {
    for (const int i_selection : range) {
      const int i = selection[i_selection];
      if (local_spaces[i]) {
        instance_transforms[i] *= float4x4::from_location(translations[i]);
      }
      else {
        add_v3_v3(instance_transforms[i].values[3], translations[i]);
      }
    }
  });
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Instances");
  if (geometry_set.has_instances()) {
    InstancesComponent &instances = geometry_set.get_component_for_write<InstancesComponent>();
    translate_instances(params, instances);
  }
  params.set_output("Instances", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_translate_instances_cc

void register_node_type_geo_translate_instances()
{
  namespace file_ns = blender::nodes::node_geo_translate_instances_cc;

  static bNodeType ntype;

  geo_node_type_base(
      &ntype, GEO_NODE_TRANSLATE_INSTANCES, "Translate Instances", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  nodeRegisterType(&ntype);
}
