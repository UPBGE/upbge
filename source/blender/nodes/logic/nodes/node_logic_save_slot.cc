/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Save Slot (2026-11-17): whether a Save Game slot has a saved game (a menu shows "Continue" or
 * an empty slot), and deleting one. The slot and Path as Save Game and Load Game name them.
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_save_slot_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Delete"_ustr)
      .description("Delete the slot's saved game (nothing happens for an empty slot)");
  b.add_input<decl::Int>("Slot"_ustr).default_value(0).min(0).description(
      "The slot, as Save Game and Load Game name it");
  b.add_input<decl::String>("Path"_ustr)
      .default_value("")
      .description("As Save Game and Load Game: the checkpoint file or directory (empty: the "
                   "scene's own)");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("When the deletion is done (after every Save Game of the slot asked before "
                   "it)");
  b.add_output<decl::Bool>("Success"_ustr)
      .description("At Done: the slot is empty now (it was already, or its file was deleted)");
  b.add_output<decl::Bool>("Exists"_ustr)
      .description("The slot holds a saved game (a Save Game asked for it counts at once, a "
                   "Delete asked for it too)");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSaveSlot"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_save_slot_cc
