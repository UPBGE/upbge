/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Set Random Seed (2026-11-07): the game's random seed (LN_Random.h) from the Seed input, or a new
 * one from the clock (custom1 1). It takes effect after the tick it was set in, for every tree at
 * once. The Seed output is the game's seed now, whether the node ran or not.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_random_seed_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Int>("Seed"_ustr)
      .default_value(0)
      .description("The new seed: the same number gives the same random numbers again (a daily "
                   "challenge, a seed the player typed, a replay)");
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Int>("Seed"_ustr)
      .description("The game's random seed now (a new one shows from the next tick on)");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "seed_source", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  if (bNodeSocket *seed = node->input_by_identifier("Seed"_ustr)) {
    bke::node_set_socket_availability(*ntree, *seed, node->custom1 == 0);
  }
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = 0;
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetRandomSeed"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_random_seed_cc
