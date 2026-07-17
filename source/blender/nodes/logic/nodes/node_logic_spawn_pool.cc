/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_spawn_pool_cc {
namespace decl = blender::nodes::logic::decl;

static bool spawn_pool_is_simple(const int16_t spawn_type)
{
  return spawn_type == 0 || spawn_type == 3;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Create Pool"_ustr);
  b.add_input<decl::Execution>("Spawn"_ustr);
  b.add_input<decl::Object>("Spawner"_ustr);
  b.add_input<decl::Object>("Object Instance"_ustr);
  b.add_input<decl::Int>("Amount"_ustr).default_value(10).min(1);
  b.add_input<decl::Int>("Life"_ustr).default_value(3).min(1);
  b.add_input<decl::Float>("Speed"_ustr).default_value(75.0f).min(0.0f);
  b.add_input<decl::Int>("Bitmask"_ustr).default_value(65535).min(0);
  b.add_input<decl::Bool>("Visualize"_ustr);
  b.add_output<decl::Execution>("Pool Created"_ustr, "OUT"_ustr);
  b.add_output<decl::Execution>("Spawned"_ustr, "SPAWNED"_ustr);
  b.add_output<decl::Condition>("On Hit"_ustr, "ONHIT"_ustr);
  b.add_output<decl::Object>("Hit Object"_ustr, "HITOBJECT"_ustr);
  b.add_output<decl::Vector>("Hit Point"_ustr, "HITPOINT"_ustr);
  b.add_output<decl::Vector>("Hit Normal"_ustr, "HITNORMAL"_ustr);
  b.add_output<decl::Vector>("Hit Direction"_ustr, "HITDIR"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "create_on_init", UI_ITEM_NONE, "On Startup", ICON_NONE);
  layout.prop(ptr, "spawn_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 1;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const bool startup = (node->custom2 & 1) != 0;
  const bool simple = spawn_pool_is_simple(node->custom1);

  auto set_input = [&](const char *id, const bool visible) {
    if (bNodeSocket *socket = node->input_by_identifier(UString(id))) {
      bke::node_set_socket_availability(*ntree, *socket, visible);
    }
  };
  auto set_output = [&](const char *id, const bool visible) {
    if (bNodeSocket *socket = node->output_by_identifier(UString(id))) {
      bke::node_set_socket_availability(*ntree, *socket, visible);
    }
  };

  set_input("Create Pool", !startup);
  set_input("Speed", !simple);
  set_input("Bitmask", !simple);
  set_input("Visualize", !simple);

  set_output("OUT", !startup);
  set_output("ONHIT", !simple);
  set_output("HITOBJECT", !simple);
  set_output("HITPOINT", !simple);
  set_output("HITNORMAL", !simple);
  set_output("HITDIR", !simple);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSpawnPool"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_spawn_pool_cc
