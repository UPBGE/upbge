/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_composite.hh"
#include "NOD_geometry.hh"
#include "NOD_register.hh"
#include "NOD_socket.hh"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

#include "BLT_translation.hh"

#include "UI_resources.hh"

static bool node_undefined_poll(const blender::bke::bNodeType * /*ntype*/,
                                const bNodeTree * /*nodetree*/,
                                const char ** /*r_disabled_hint*/)
{
  /* this type can not be added deliberately, it's just a placeholder */
  return false;
}

/* register fallback types used for undefined tree, nodes, sockets */
static void register_undefined_types()
{
  /* NOTE: these types are not registered in the type hashes,
   * they are just used as placeholders in case the actual types are not registered.
   */

  blender::bke::NodeTreeTypeUndefined.type = NTREE_UNDEFINED;
  blender::bke::NodeTreeTypeUndefined.idname = "NodeTreeUndefined";
  blender::bke::NodeTreeTypeUndefined.ui_name = N_("Undefined");
  blender::bke::NodeTreeTypeUndefined.ui_description = N_("Undefined Node Tree Type");

  blender::bke::node_type_base_custom(
      blender::bke::NodeTypeUndefined, "NodeUndefined", "Undefined", "UNDEFINED", 0);
  blender::bke::NodeTypeUndefined.poll = node_undefined_poll;

  blender::bke::NodeSocketTypeUndefined.idname = "NodeSocketUndefined";
  /* extra type info for standard socket types */
  blender::bke::NodeSocketTypeUndefined.type = SOCK_CUSTOM;
  blender::bke::NodeSocketTypeUndefined.subtype = PROP_NONE;

  blender::bke::NodeSocketTypeUndefined.use_link_limits_of_type = true;
  blender::bke::NodeSocketTypeUndefined.input_link_limit = 0xFFF;
  blender::bke::NodeSocketTypeUndefined.output_link_limit = 0xFFF;
}

class SimulationZoneType : public blender::bke::bNodeZoneType {
 public:
  SimulationZoneType()
  {
    this->input_idname = "GeometryNodeSimulationInput";
    this->output_idname = "GeometryNodeSimulationOutput";
    this->input_type = GEO_NODE_SIMULATION_INPUT;
    this->output_type = GEO_NODE_SIMULATION_OUTPUT;
    this->theme_id = TH_NODE_ZONE_SIMULATION;
  }

  const int &get_corresponding_output_id(const bNode &input_bnode) const override
  {
    BLI_assert(input_bnode.type_legacy == this->input_type);
    return static_cast<NodeGeometrySimulationInput *>(input_bnode.storage)->output_node_id;
  }
};

class RepeatZoneType : public blender::bke::bNodeZoneType {
 public:
  RepeatZoneType()
  {
    this->input_idname = "GeometryNodeRepeatInput";
    this->output_idname = "GeometryNodeRepeatOutput";
    this->input_type = GEO_NODE_REPEAT_INPUT;
    this->output_type = GEO_NODE_REPEAT_OUTPUT;
    this->theme_id = TH_NODE_ZONE_REPEAT;
  }

  const int &get_corresponding_output_id(const bNode &input_bnode) const override
  {
    BLI_assert(input_bnode.type_legacy == this->input_type);
    return static_cast<NodeGeometryRepeatInput *>(input_bnode.storage)->output_node_id;
  }
};

class ForeachGeometryElementZoneType : public blender::bke::bNodeZoneType {
 public:
  ForeachGeometryElementZoneType()
  {
    this->input_idname = "GeometryNodeForeachGeometryElementInput";
    this->output_idname = "GeometryNodeForeachGeometryElementOutput";
    this->input_type = GEO_NODE_FOREACH_GEOMETRY_ELEMENT_INPUT;
    this->output_type = GEO_NODE_FOREACH_GEOMETRY_ELEMENT_OUTPUT;
    this->theme_id = TH_NODE_ZONE_FOREACH_GEOMETRY_ELEMENT;
  }

  const int &get_corresponding_output_id(const bNode &input_bnode) const override
  {
    BLI_assert(input_bnode.type_legacy == this->input_type);
    return static_cast<NodeGeometryForeachGeometryElementInput *>(input_bnode.storage)
        ->output_node_id;
  }
};

class ClosureZoneType : public blender::bke::bNodeZoneType {
 public:
  ClosureZoneType()
  {
    this->input_idname = "NodeClosureInput";
    this->output_idname = "NodeClosureOutput";
    this->input_type = NODE_CLOSURE_INPUT;
    this->output_type = NODE_CLOSURE_OUTPUT;
    this->theme_id = TH_NODE_ZONE_CLOSURE;
  }

  const int &get_corresponding_output_id(const bNode &input_bnode) const override
  {
    BLI_assert(input_bnode.type_legacy == this->input_type);
    return static_cast<NodeClosureInput *>(input_bnode.storage)->output_node_id;
  }
};

static void register_zone_types()
{
  static SimulationZoneType simulation_zone_type;
  static RepeatZoneType repeat_zone_type;
  static ForeachGeometryElementZoneType foreach_geometry_element_zone_type;
  static ClosureZoneType closure_zone_type;
  blender::bke::register_node_zone_type(simulation_zone_type);
  blender::bke::register_node_zone_type(repeat_zone_type);
  blender::bke::register_node_zone_type(foreach_geometry_element_zone_type);
  blender::bke::register_node_zone_type(closure_zone_type);
}

void register_nodes()
{
  register_zone_types();

  register_undefined_types();

  register_standard_node_socket_types();

  register_node_tree_type_geo();
  register_node_tree_type_cmp();

  register_node_type_frame();
  register_node_type_reroute();
  register_node_type_group_input();
  register_node_type_group_output();

  register_compositor_nodes();
  register_shader_nodes();
  register_texture_nodes();
  register_geometry_nodes();
  register_function_nodes();
}
