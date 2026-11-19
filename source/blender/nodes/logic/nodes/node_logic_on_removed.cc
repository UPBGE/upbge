/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * On Removed (2026-11-17): the object's last Logic + Physics step. An object whose trees hold
 * this node, removed while playing (Remove Object, its Life ending, End Object), is hidden and
 * out of physics at once, as any removed object, but deleted one step later: its trees run that
 * step with Out pulsing (drop loot, a death sound, add to a score). LN_Manager's On Removed holds.
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_on_removed_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Execution>("Out"_ustr)
      .description("Once, in the object's last step after it was removed: it is hidden and out "
                   "of physics then, still where it was, and deleted at the step's end");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeOnRemoved"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_on_removed_cc
