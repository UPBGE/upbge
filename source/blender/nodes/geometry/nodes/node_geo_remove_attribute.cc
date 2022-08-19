/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_socket_search_link.hh"

namespace blender::nodes::node_geo_remove_attribute_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Geometry"));
  b.add_input<decl::String>(N_("Name")).is_attribute_name();
  b.add_output<decl::Geometry>(N_("Geometry"));
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const std::string name = params.extract_input<std::string>("Name");
  if (name.empty()) {
    params.set_output("Geometry", std::move(geometry_set));
    return;
  }
  if (!bke::allow_procedural_attribute_access(name)) {
    params.error_message_add(NodeWarningType::Info, TIP_(bke::no_procedural_access_message));
    params.set_output("Geometry", std::move(geometry_set));
    return;
  }

  std::atomic<bool> attribute_exists = false;
  std::atomic<bool> cannot_delete = false;

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    for (const GeometryComponentType type : {GEO_COMPONENT_TYPE_MESH,
                                             GEO_COMPONENT_TYPE_POINT_CLOUD,
                                             GEO_COMPONENT_TYPE_CURVE,
                                             GEO_COMPONENT_TYPE_INSTANCES}) {
      if (geometry_set.has(type)) {
        /* First check if the attribute exists before getting write access,
         * to avoid potentially expensive unnecessary copies. */
        const GeometryComponent &read_only_component = *geometry_set.get_component_for_read(type);
        if (read_only_component.attributes()->contains(name)) {
          attribute_exists = true;
        }
        else {
          continue;
        }

        GeometryComponent &component = geometry_set.get_component_for_write(type);
        if (!component.attributes_for_write()->remove(name)) {
          cannot_delete = true;
        }
      }
    }
  });

  if (attribute_exists && !cannot_delete) {
    params.used_named_attribute(name, eNamedAttrUsage::Remove);
  }

  if (!attribute_exists) {
    params.error_message_add(NodeWarningType::Info,
                             TIP_("Attribute does not exist: \"") + name + "\"");
  }
  if (cannot_delete) {
    params.error_message_add(NodeWarningType::Warning,
                             TIP_("Cannot delete built-in attribute with name \"") + name + "\"");
  }

  params.set_output("Geometry", std::move(geometry_set));
}

}  // namespace blender::nodes::node_geo_remove_attribute_cc

void register_node_type_geo_remove_attribute()
{
  namespace file_ns = blender::nodes::node_geo_remove_attribute_cc;

  static bNodeType ntype;

  geo_node_type_base(
      &ntype, GEO_NODE_REMOVE_ATTRIBUTE, "Remove Named Attribute", NODE_CLASS_ATTRIBUTE);
  ntype.declare = file_ns::node_declare;
  node_type_size(&ntype, 170, 100, 700);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  nodeRegisterType(&ntype);
}
