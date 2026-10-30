/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_dynamics_cc {
namespace decl = blender::nodes::logic::decl;

static constexpr int ghost_mode = 2;
static constexpr int restore_original_mode = 4;

static void node_update(bNodeTree *ntree, bNode *node);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr).description("Object to affect, or the owner when empty");
  b.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("Enable or disable Ghost while preserving the current physics type");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "dynamics_mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  /* Existing files retain their stored legacy value (0 = Restore Previous).
   * Newly created nodes should instead do the unambiguous game-start restore. */
  node->custom1 = restore_original_mode;
  node_update(ntree, node);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *enabled_socket = node->input_by_identifier("Enabled"_ustr);
  if (enabled_socket) {
    bke::node_set_socket_availability(*ntree, *enabled_socket, node->custom1 == ghost_mode);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetDynamics"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_dynamics_cc
