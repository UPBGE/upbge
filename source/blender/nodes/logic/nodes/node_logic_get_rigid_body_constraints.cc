/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_get_rigid_body_constraints_cc {
namespace decl = blender::nodes::logic::decl;

enum MatchMode {
  Exact = 0,
  Contains,
  All,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("First"_ustr, "Object"_ustr).description(
      "First rigid body that owns the constraints; defaults to the owner");
  b.add_input<decl::String>("Name"_ustr).description(
      "Case-sensitive constraint name or substring to match");
  b.add_output<decl::Bool>("Found"_ustr).description(
      "Whether the selected match mode returned at least one constraint");
  b.add_output<decl::String>("First Match"_ustr, "Constraint"_ustr).description(
      "First matching constraint name in stable registry order, or an empty string");
  b.add_output<decl::List>("Matches"_ustr, "Constraints"_ustr).description(
      "Names of every constraint returned by the selected match mode");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "match_mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = MatchMode::Exact;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  if (!ELEM(node->custom1, MatchMode::Exact, MatchMode::Contains, MatchMode::All)) {
    node->custom1 = MatchMode::Exact;
  }
  if (bNodeSocket *name = node->input_by_identifier("Name"_ustr)) {
    bke::node_set_socket_availability(*ntree, *name, node->custom1 != MatchMode::All);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetRigidBodyConstraints"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_rigid_body_constraints_cc
