/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include <algorithm>

#include "BKE_node.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_while_paused_cc {
namespace decl = blender::nodes::logic::decl;

enum class PauseScope : int16_t {
  Owner = 0,
  ObjectHierarchy,
  Collection,
  Scene,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Root Object"_ustr, "Object"_ustr)
      .description("Root of the object hierarchy; an empty value uses the tree owner");
  b.add_input<decl::Collection>("Collection"_ustr)
      .description("Collection whose active runtime objects continue frame processing");
  b.add_output<decl::Execution>("Update"_ustr)
      .description("Once per rendered frame while the game is globally paused");
  b.add_output<decl::Float>("Delta Time"_ustr)
      .description("Unscaled wall-clock seconds represented by this paused frame");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "pause_scope", UI_ITEM_NONE, "Scope", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int16_t(PauseScope::Owner);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const PauseScope scope = PauseScope(std::clamp<int>(
      node->custom1, int(PauseScope::Owner), int(PauseScope::Scene)));
  node->custom1 = int16_t(scope);

  if (bNodeSocket *socket = node->input_by_identifier("Object"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, scope == PauseScope::ObjectHierarchy);
  }
  if (bNodeSocket *socket = node->input_by_identifier("Collection"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, scope == PauseScope::Collection);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeWhilePaused"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_while_paused_cc
